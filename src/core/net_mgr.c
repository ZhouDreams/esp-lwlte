/**
 * @file net_mgr.c
 * @brief LTE 网络状态管理
 * @details LTE network state management
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/timers.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "net_mgr"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 重连定时器回调
 * @details Reconnect timer callback
 * @param[in] timer FreeRTOS 定时器句柄
 */
static void reconnect_timer_cb(TimerHandle_t timer);

/**
 * @brief 定时器服务屏障回调
 * @details Timer service barrier callback
 * @param[in] arg 完成信号量
 * @param[in] value 未使用
 */
static void timer_barrier_cb(void *arg, uint32_t value);

/**
 * @brief 等待定时器服务空闲
 * @details Wait until timer service has processed queued commands/callbacks
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_NO_MEM: 创建屏障信号量失败
 *         - ESP_ERR_INVALID_STATE: 无法等待定时器服务空闲
 */
static esp_err_t wait_timer_service_idle(void);

/**
 * @brief 获取当前时间
 * @details Get current time
 * @return 当前时间（毫秒）
 */
static uint32_t now_ms(void);

/**
 * @brief 执行一次网络激活流程
 * @details Run one network activation attempt
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_STATE: SIM 或网络注册状态未就绪
 *         - other: 调制解调器操作错误
 */
static esp_err_t run_activation_once(lwlte_core_t *me,
                                     uint32_t activation_start_ms);

/**
 * @brief 判断网络激活是否超时
 * @details Check whether network activation timed out
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - true: 已超时
 *         - false: 未超时
 */
static bool activation_timed_out(lwlte_core_t *me, uint32_t activation_start_ms);

/**
 * @brief 检查网络激活是否应继续
 * @details Check whether network activation should continue
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 可以继续
 *         - ESP_ERR_INVALID_STATE: Core 正在销毁
 *         - ESP_ERR_TIMEOUT: 网络激活已超时
 */
static esp_err_t check_activation_continue(lwlte_core_t *me,
                                           uint32_t activation_start_ms);

/**
 * @brief 处理网络激活失败
 * @details Handle network activation failure
 * @param[in] me LTE 核心服务句柄
 * @param[in] err 错误码
 * @return 传入的错误码
 */
static esp_err_t fail_activation(lwlte_core_t *me, esp_err_t err);

/**
 * @brief 判断网络注册状态是否就绪
 * @details Check whether network registration status is ready
 * @param[in] status 网络注册状态
 * @return
 *         - true: 已注册
 *         - false: 未注册
 */
static bool registration_ready(modem_reg_status_t status);

/**
 * @brief 判断 PDP 上下文是否为主上下文
 * @details Check whether PDP context is primary
 * @param[in] me LTE 核心服务句柄
 * @param[in] pdp PDP 上下文
 * @return
 *         - true: 主 PDP 上下文
 *         - false: 非主 PDP 上下文
 */
static bool is_primary_pdp(lwlte_core_t *me, const modem_pdp_context_t *pdp);

/**
 * @brief 发布网络状态事件
 * @details Post network state event
 * @param[in] me LTE 核心服务句柄
 * @param[in] event_id LTE 核心服务事件 ID
 * @param[in] net_state LTE 网络状态
 * @param[in] error_code 错误码
 */
static void post_net_state(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                           lwlte_net_state_t net_state, int error_code);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t net_mgr_init(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->net_mgr.current_step = NET_STEP_IDLE;
    me->net_mgr.step_start_time_ms = 0;
    me->net_mgr.step_timeout_ms = me->config.net_activate_timeout_ms;
    me->net_mgr.retry_count = 0;
    me->net_mgr.max_retry = CORE_NET_MAX_RETRY;
    me->net_mgr.state = LWLTE_NET_STATE_OFFLINE;
    me->net_mgr.reconnect_enabled = false;
    me->net_mgr.reconnect_cb_active = 0;
    me->net_mgr.reconnect_cb_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->net_mgr.reconnect_cb_done_sema, ESP_ERR_NO_MEM, TAG,
                        "create reconnect_cb_done_sema failed");

    me->net_mgr.reconnect_timer = xTimerCreate("core_reconn",
                                                pdMS_TO_TICKS(me->config.reconnect_delay_ms),
                                                pdFALSE, me, reconnect_timer_cb);
    if (!me->net_mgr.reconnect_timer) {
        vSemaphoreDelete(me->net_mgr.reconnect_cb_done_sema);
        me->net_mgr.reconnect_cb_done_sema = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t net_mgr_deinit(lwlte_core_t *me)
{
    if (!me) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    bool has_reconnect_resources = false;

    if (me->lock) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->net_mgr.reconnect_enabled = false;
        has_reconnect_resources = me->net_mgr.reconnect_timer ||
                                  me->net_mgr.reconnect_cb_done_sema ||
                                  me->net_mgr.reconnect_cb_active > 0;
        xSemaphoreGive(me->lock);
    } else {
        me->net_mgr.reconnect_enabled = false;
        has_reconnect_resources = me->net_mgr.reconnect_timer ||
                                  me->net_mgr.reconnect_cb_done_sema ||
                                  me->net_mgr.reconnect_cb_active > 0;
    }

    if (!has_reconnect_resources) {
        return ESP_OK;
    }

    net_mgr_cancel_reconnect(me);
    ret = wait_timer_service_idle();
    if (ret != ESP_OK) {
        return ret;
    }

    int active = 0;
    SemaphoreHandle_t done_sema = NULL;
    if (me->lock) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        active = me->net_mgr.reconnect_cb_active;
        done_sema = me->net_mgr.reconnect_cb_done_sema;
        xSemaphoreGive(me->lock);
    }
    while (active > 0 && done_sema) {
        xSemaphoreTake(done_sema, portMAX_DELAY);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        active = me->net_mgr.reconnect_cb_active;
        done_sema = me->net_mgr.reconnect_cb_done_sema;
        xSemaphoreGive(me->lock);
    }

    if (me->net_mgr.reconnect_timer) {
        if (xTimerDelete(me->net_mgr.reconnect_timer, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "delete reconnect timer failed");
            return ESP_ERR_INVALID_STATE;
        }
        me->net_mgr.reconnect_timer = NULL;
        ret = wait_timer_service_idle();
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (me->net_mgr.reconnect_cb_done_sema) {
        vSemaphoreDelete(me->net_mgr.reconnect_cb_done_sema);
        me->net_mgr.reconnect_cb_done_sema = NULL;
    }
    me->net_mgr.reconnect_cb_active = 0;

    return ESP_OK;
}

void net_mgr_cancel_reconnect(lwlte_core_t *me)
{
    if (!me || !me->net_mgr.reconnect_timer) {
        return;
    }

    if (xTimerStop(me->net_mgr.reconnect_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "stop reconnect timer failed");
    }
}

void net_mgr_set_reconnect_enabled(lwlte_core_t *me, bool enabled)
{
    if (!me) {
        return;
    }

    me->net_mgr.reconnect_enabled = enabled;
}

esp_err_t net_mgr_get_state(lwlte_core_t *me, lwlte_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->net_mgr.state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t net_mgr_set_state(lwlte_core_t *me, lwlte_net_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(state >= LWLTE_NET_STATE_OFFLINE &&
                        state <= LWLTE_NET_STATE_ERROR,
                        ESP_ERR_INVALID_ARG, TAG, "invalid net state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if ((me->destroying || me->state == LWLTE_CORE_STATE_DESTROYING) &&
        state != LWLTE_NET_STATE_OFFLINE) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->net_mgr.state = state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t net_mgr_start_activation(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = true;
    me->net_mgr.retry_count = 0;
    uint32_t activation_start_ms = now_ms();

    esp_err_t ret = ESP_FAIL;
    while (me->net_mgr.retry_count < me->net_mgr.max_retry) {
        esp_err_t continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret == ESP_ERR_INVALID_STATE) {
            return continue_ret;
        }
        if (continue_ret == ESP_ERR_TIMEOUT) {
            return fail_activation(me, ESP_ERR_TIMEOUT);
        }

        me->net_mgr.step_start_time_ms = now_ms();
        ret = run_activation_once(me, activation_start_ms);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        if (ret == ESP_ERR_INVALID_STATE && core_is_destroying(me)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            return fail_activation(me, ESP_ERR_TIMEOUT);
        }

        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret == ESP_ERR_INVALID_STATE) {
            return continue_ret;
        }
        if (continue_ret == ESP_ERR_TIMEOUT) {
            return fail_activation(me, ESP_ERR_TIMEOUT);
        }

        me->net_mgr.retry_count++;
        ESP_LOGW(TAG, "activation attempt %d failed: %s",
                 me->net_mgr.retry_count, esp_err_to_name(ret));
    }

    return fail_activation(me, ret);
}

esp_err_t net_mgr_deactivate(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = false;

    lwlte_net_state_t old_state = LWLTE_NET_STATE_OFFLINE;
    net_mgr_get_state(me, &old_state);
    if (old_state == LWLTE_NET_STATE_ONLINE ||
        old_state == LWLTE_NET_STATE_ACTIVATING ||
        old_state == LWLTE_NET_STATE_ERROR) {
        esp_err_t ret = modem_deactivate_pdp(me->modem, me->config.primary_cid);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "deactivate PDP failed: %s", esp_err_to_name(ret));
        }
    }

    pdp_mgr_set_active(&me->pdp_mgr, me->config.primary_cid, false);
    esp_err_t state_ret = net_mgr_set_state(me, LWLTE_NET_STATE_OFFLINE);
    if (state_ret != ESP_OK) {
        return state_ret;
    }
    if (old_state != LWLTE_NET_STATE_OFFLINE) {
        post_net_state(me, LWLTE_CORE_EVENT_NET_OFFLINE, LWLTE_NET_STATE_OFFLINE, 0);
    }

    return ESP_OK;
}

esp_err_t net_mgr_handle_pdp_activated(lwlte_core_t *me,
                                       const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    lwlte_net_state_t old_state = LWLTE_NET_STATE_OFFLINE;
    esp_err_t ret = net_mgr_get_state(me, &old_state);
    ESP_RETURN_ON_ERROR(ret, TAG, "get net state failed");

    if (old_state == LWLTE_NET_STATE_ONLINE) {
        pdp_mgr_update(&me->pdp_mgr, pdp);
        return ESP_OK;
    }

    ret = net_mgr_set_state(me, LWLTE_NET_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, LWLTE_CORE_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    pdp_mgr_update(&me->pdp_mgr, pdp);
    post_net_state(me, LWLTE_CORE_EVENT_NET_ONLINE, LWLTE_NET_STATE_ONLINE, 0);

    return ESP_OK;
}

esp_err_t net_mgr_handle_pdp_deactivated(lwlte_core_t *me,
                                         const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    lwlte_net_state_t old_state = LWLTE_NET_STATE_OFFLINE;
    esp_err_t ret = net_mgr_get_state(me, &old_state);
    ESP_RETURN_ON_ERROR(ret, TAG, "get net state failed");

    pdp_mgr_update(&me->pdp_mgr, pdp);
    pdp_mgr_set_active(&me->pdp_mgr, me->config.primary_cid, false);
    if (old_state == LWLTE_NET_STATE_OFFLINE) {
        return ESP_OK;
    }

    ret = net_mgr_set_state(me, LWLTE_NET_STATE_OFFLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, LWLTE_CORE_STATE_READY);
    if (ret != ESP_OK) {
        return ret;
    }
    post_net_state(me, LWLTE_CORE_EVENT_NET_OFFLINE, LWLTE_NET_STATE_OFFLINE, 0);

    if (me->net_mgr.reconnect_enabled && !core_is_destroying(me) &&
        me->net_mgr.reconnect_timer) {
        if (xTimerStart(me->net_mgr.reconnect_timer, 0) != pdPASS) {
            ESP_LOGW(TAG, "start reconnect timer failed");
        }
    }

    return ESP_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void timer_barrier_cb(void *arg, uint32_t value)
{
    (void)value;

    SemaphoreHandle_t done_sema = (SemaphoreHandle_t)arg;
    if (done_sema) {
        xSemaphoreGive(done_sema);
    }
}

static esp_err_t wait_timer_service_idle(void)
{
    if (xTimerGetTimerDaemonTaskHandle() == xTaskGetCurrentTaskHandle()) {
        ESP_LOGE(TAG, "timer barrier from timer service task is not allowed");
        return ESP_ERR_INVALID_STATE;
    }

    SemaphoreHandle_t done_sema = xSemaphoreCreateBinary();
    if (!done_sema) {
        ESP_LOGE(TAG, "create timer barrier semaphore failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTimerPendFunctionCall(timer_barrier_cb, done_sema, 0,
                               portMAX_DELAY) != pdPASS) {
        ESP_LOGE(TAG, "queue timer barrier failed");
        vSemaphoreDelete(done_sema);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(done_sema, portMAX_DELAY);
    vSemaphoreDelete(done_sema);

    return ESP_OK;
}

static bool activation_timed_out(lwlte_core_t *me, uint32_t activation_start_ms)
{
    return me &&
           now_ms() - activation_start_ms >= me->config.net_activate_timeout_ms;
}

static esp_err_t check_activation_continue(lwlte_core_t *me,
                                           uint32_t activation_start_ms)
{
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (activation_timed_out(me, activation_start_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    lwlte_core_t *core = (lwlte_core_t *)pvTimerGetTimerID(timer);
    if (!core) {
        return;
    }

    xSemaphoreTake(core->lock, portMAX_DELAY);
    core->net_mgr.reconnect_cb_active++;
    bool should_reconnect = !core->destroying &&
                            core->state != LWLTE_CORE_STATE_DESTROYING &&
                            core->net_mgr.reconnect_enabled;
    xSemaphoreGive(core->lock);

    if (should_reconnect) {
        core_fsm_sig_t sig = {
            .type = CORE_SIG_RECONNECT,
        };

        esp_err_t ret = core_fsm_send(core, &sig);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "send reconnect signal failed: %s", esp_err_to_name(ret));
        }
    }

    xSemaphoreTake(core->lock, portMAX_DELAY);
    if (core->net_mgr.reconnect_cb_active > 0) {
        core->net_mgr.reconnect_cb_active--;
    }
    bool callbacks_idle = core->net_mgr.reconnect_cb_active == 0;
    SemaphoreHandle_t done_sema = core->net_mgr.reconnect_cb_done_sema;
    xSemaphoreGive(core->lock);

    if (callbacks_idle && done_sema) {
        xSemaphoreGive(done_sema);
    }
}

static esp_err_t run_activation_once(lwlte_core_t *me,
                                     uint32_t activation_start_ms)
{
    modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
    modem_signal_t signal = {0};
    modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
    modem_pdp_context_t pdp = {0};
    esp_err_t ret = ESP_OK;

    me->net_mgr.current_step = NET_STEP_CHECK_SIM;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = core_set_state(me, LWLTE_CORE_STATE_NET_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = net_mgr_set_state(me, LWLTE_NET_STATE_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }
    post_net_state(me, LWLTE_CORE_EVENT_NET_CONNECTING,
                   LWLTE_NET_STATE_ACTIVATING, 0);

    ret = modem_get_sim_status(me->modem, &sim_status);
    esp_err_t continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (sim_status != MODEM_SIM_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    me->net_mgr.current_step = NET_STEP_CHECK_SIGNAL;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = modem_get_signal(me->modem, &signal);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    me->net_mgr.current_step = NET_STEP_CHECK_REGISTRATION;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = modem_get_registration(me->modem, &reg_status);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (!registration_ready(reg_status)) {
        return ESP_ERR_INVALID_STATE;
    }

    me->net_mgr.current_step = NET_STEP_SET_APN;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = modem_set_apn(me->modem, me->config.primary_cid, me->config.apn);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    me->net_mgr.current_step = NET_STEP_ACTIVATE_PDP;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = modem_activate_pdp(me->modem, me->config.primary_cid);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = modem_get_pdp_context(me->modem, me->config.primary_cid, &pdp);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }

    me->net_mgr.current_step = NET_STEP_DONE;
    me->net_mgr.step_start_time_ms = now_ms();
    ret = net_mgr_set_state(me, LWLTE_NET_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, LWLTE_CORE_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    pdp_mgr_update(&me->pdp_mgr, &pdp);
    post_net_state(me, LWLTE_CORE_EVENT_NET_ONLINE, LWLTE_NET_STATE_ONLINE, 0);

    return ESP_OK;
}

static esp_err_t fail_activation(lwlte_core_t *me, esp_err_t err)
{
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = net_mgr_set_state(me, LWLTE_NET_STATE_ERROR);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, LWLTE_CORE_STATE_ERROR);
    if (ret != ESP_OK) {
        return ret;
    }
    me->net_mgr.current_step = NET_STEP_ERROR;
    post_net_state(me, LWLTE_CORE_EVENT_NET_ERROR, LWLTE_NET_STATE_ERROR, err);

    return err;
}

static bool registration_ready(modem_reg_status_t status)
{
    return status == MODEM_REG_REGISTERED_HOME ||
           status == MODEM_REG_REGISTERED_ROAMING;
}

static bool is_primary_pdp(lwlte_core_t *me, const modem_pdp_context_t *pdp)
{
    return me && pdp && pdp->cid == me->config.primary_cid;
}

static void post_net_state(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                           lwlte_net_state_t net_state, int error_code)
{
    lwlte_core_event_data_t data = {
        .net_state = net_state,
        .error_code = error_code,
    };

    esp_err_t ret = core_post_event(me, event_id, &data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post net event failed: %s", esp_err_to_name(ret));
    }
}
