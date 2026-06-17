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
#define NET_MGR_WAIT_POLL_INTERVAL_MS 1000

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
 * @brief 进入网络激活流程
 * @details Enter network activation flow
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - other: 状态设置或事件发布失败
 */
static esp_err_t enter_activation(core_handle_t *me);

/**
 * @brief 执行网络激活阶段循环
 * @details Run network activation stage loop
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_TIMEOUT: 激活超时
 *         - other: 激活失败
 */
static esp_err_t run_activation_loop(core_handle_t *me,
                                     uint32_t activation_start_ms);

/**
 * @brief 执行当前网络激活阶段
 * @details Run current network activation stage
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 当前阶段完成
 *         - ESP_ERR_NOT_FINISHED: 当前阶段仍需等待
 *         - other: 当前阶段失败
 */
static esp_err_t run_activation_step(core_handle_t *me,
                                     uint32_t activation_start_ms);

/**
 * @brief 设置网络激活阶段
 * @details Set network activation stage
 * @param[in] me LTE 核心服务句柄
 * @param[in] step 网络激活阶段
 */
static void set_activation_step(core_handle_t *me, net_mgr_step_t step);

/**
 * @brief 等待下一次网络激活轮询
 * @details Wait for next network activation poll
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 可以继续
 *         - ESP_ERR_INVALID_STATE: Core 正在销毁
 *         - ESP_ERR_TIMEOUT: 网络激活已超时
 */
static esp_err_t wait_next_poll(core_handle_t *me, uint32_t activation_start_ms);

/**
 * @brief 完成网络激活
 * @details Complete network activation
 * @param[in] me LTE 核心服务句柄
 * @param[in] pdp PDP 上下文
 * @return
 *         - ESP_OK: 成功
 *         - other: 状态设置或事件发布失败
 */
static esp_err_t complete_activation(core_handle_t *me,
                                     const modem_pdp_context_t *pdp);

/**
 * @brief 重新分类 PDP 激活状态错误
 * @details Reclassify PDP activation invalid state
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_ERR_NOT_FINISHED: 前置条件需继续等待
 *         - ESP_ERR_INVALID_STATE: 前置条件终止失败或原始状态错误
 *         - ESP_ERR_TIMEOUT: 网络激活已超时
 *         - other: 前置条件查询失败
 */
static esp_err_t classify_pdp_activation_invalid_state(core_handle_t *me,
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
static bool activation_timed_out(core_handle_t *me, uint32_t activation_start_ms);

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
static esp_err_t check_activation_continue(core_handle_t *me,
                                            uint32_t activation_start_ms);

/**
 * @brief 处理网络激活失败
 * @details Handle network activation failure
 * @param[in] me LTE 核心服务句柄
 * @param[in] err 错误码
 * @return 传入的错误码
 */
static esp_err_t fail_activation(core_handle_t *me, esp_err_t err);

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
 * @brief 判断 SIM 状态是否为终止错误
 * @details Check whether SIM status is terminal failure
 * @param[in] status SIM 状态
 * @return
 *         - true: 终止错误
 *         - false: 可等待或已就绪
 */
static bool sim_status_fatal(modem_sim_status_t status);

/**
 * @brief 判断网络注册状态是否被拒绝
 * @details Check whether network registration is denied
 * @param[in] status 网络注册状态
 * @return
 *         - true: 注册被拒绝
 *         - false: 未被拒绝
 */
static bool registration_denied(modem_reg_status_t status);

/**
 * @brief 判断 PDP 上下文是否为主上下文
 * @details Check whether PDP context is primary
 * @param[in] me LTE 核心服务句柄
 * @param[in] pdp PDP 上下文
 * @return
 *         - true: 主 PDP 上下文
 *         - false: 非主 PDP 上下文
 */
static bool is_primary_pdp(core_handle_t *me, const modem_pdp_context_t *pdp);

/**
 * @brief 发布网络状态事件
 * @details Post network state event
 * @param[in] me LTE 核心服务句柄
 * @param[in] event_id LTE 用户事件 ID
 * @param[in] net_state LTE 网络状态
 * @param[in] error_code 错误码
 */
static void post_net_state(core_handle_t *me, lwlte_event_id_t event_id,
                           core_net_state_t net_state, int error_code);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t net_mgr_init(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->net_mgr.current_step = NET_STEP_IDLE;
    me->net_mgr.step_start_time_ms = 0;
    me->net_mgr.step_timeout_ms = me->config.network.net_activate_timeout_ms;
    me->net_mgr.retry_count = 0;
    me->net_mgr.max_retry = CORE_NET_MAX_RETRY;
    me->net_mgr.state = CORE_NET_STATE_OFFLINE;
    me->net_mgr.reconnect_enabled = false;
    me->net_mgr.reconnect_cb_active = 0;
    me->net_mgr.reconnect_cb_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->net_mgr.reconnect_cb_done_sema, ESP_ERR_NO_MEM, TAG,
                        "create reconnect_cb_done_sema failed");

    me->net_mgr.reconnect_timer = xTimerCreate("core_reconn",
                                                pdMS_TO_TICKS(me->config.network.reconnect_delay_ms),
                                                pdFALSE, me, reconnect_timer_cb);
    if (!me->net_mgr.reconnect_timer) {
        vSemaphoreDelete(me->net_mgr.reconnect_cb_done_sema);
        me->net_mgr.reconnect_cb_done_sema = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t net_mgr_deinit(core_handle_t *me)
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

void net_mgr_cancel_reconnect(core_handle_t *me)
{
    if (!me || !me->net_mgr.reconnect_timer) {
        return;
    }

    if (xTimerStop(me->net_mgr.reconnect_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "stop reconnect timer failed");
    }
}

void net_mgr_set_reconnect_enabled(core_handle_t *me, bool enabled)
{
    if (!me) {
        return;
    }

    me->net_mgr.reconnect_enabled = enabled;
}

esp_err_t net_mgr_get_state(core_handle_t *me, core_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->net_mgr.state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t net_mgr_set_state(core_handle_t *me, core_net_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(state >= CORE_NET_STATE_OFFLINE &&
                        state <= CORE_NET_STATE_ERROR,
                        ESP_ERR_INVALID_ARG, TAG, "invalid net state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if ((me->destroying || me->state == CORE_STATE_DESTROYING) &&
        state != CORE_NET_STATE_OFFLINE) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->net_mgr.state = state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t net_mgr_start_activation(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = true;
    me->net_mgr.retry_count = 0;

    const uint32_t activation_start_ms = now_ms();
    esp_err_t ret = enter_activation(me);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = run_activation_loop(me, activation_start_ms);
    if (ret == ESP_OK) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_STATE &&
        (core_is_destroying(me) || core_stop_pending(me))) {
        return ESP_ERR_INVALID_STATE;
    }

    return fail_activation(me, ret);
}

esp_err_t net_mgr_deactivate(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = false;

    core_net_state_t old_state = CORE_NET_STATE_OFFLINE;
    net_mgr_get_state(me, &old_state);
    if (old_state == CORE_NET_STATE_ONLINE ||
        old_state == CORE_NET_STATE_ACTIVATING ||
        old_state == CORE_NET_STATE_ERROR) {
        esp_err_t ret = modem_deactivate_pdp(me->modem, me->config.network.primary_cid);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "deactivate PDP failed: %s", esp_err_to_name(ret));
        }
    }

    pdp_mgr_set_active(&me->pdp_mgr, me->config.network.primary_cid, false);
    esp_err_t state_ret = net_mgr_set_state(me, CORE_NET_STATE_OFFLINE);
    if (state_ret != ESP_OK) {
        return state_ret;
    }
    if (old_state != CORE_NET_STATE_OFFLINE) {
        post_net_state(me, LWLTE_EVENT_NET_OFFLINE, CORE_NET_STATE_OFFLINE, 0);
    }

    return ESP_OK;
}

esp_err_t net_mgr_handle_pdp_activated(core_handle_t *me,
                                       const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    core_net_state_t old_state = CORE_NET_STATE_OFFLINE;
    esp_err_t ret = net_mgr_get_state(me, &old_state);
    ESP_RETURN_ON_ERROR(ret, TAG, "get net state failed");

    pdp_mgr_update(&me->pdp_mgr, pdp);
    if (old_state == CORE_NET_STATE_ONLINE) {
        return ESP_OK;
    }
    if (old_state != CORE_NET_STATE_ACTIVATING) {
        return ESP_OK;
    }
    if (!pdp->active || pdp->ip_addr[0] == '\0') {
        return ESP_OK;
    }

    return complete_activation(me, pdp);
}

esp_err_t net_mgr_handle_pdp_deactivated(core_handle_t *me,
                                          const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    core_net_state_t old_state = CORE_NET_STATE_OFFLINE;
    esp_err_t ret = net_mgr_get_state(me, &old_state);
    ESP_RETURN_ON_ERROR(ret, TAG, "get net state failed");

    pdp_mgr_update(&me->pdp_mgr, pdp);
    pdp_mgr_set_active(&me->pdp_mgr, me->config.network.primary_cid, false);
    if (old_state == CORE_NET_STATE_OFFLINE) {
        return ESP_OK;
    }

    ret = net_mgr_set_state(me, CORE_NET_STATE_OFFLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, CORE_STATE_READY);
    if (ret != ESP_OK) {
        return ret;
    }
    post_net_state(me, LWLTE_EVENT_NET_OFFLINE, CORE_NET_STATE_OFFLINE, 0);

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

static bool activation_timed_out(core_handle_t *me, uint32_t activation_start_ms)
{
    return me &&
           now_ms() - activation_start_ms >= me->config.network.net_activate_timeout_ms;
}

static esp_err_t check_activation_continue(core_handle_t *me,
                                            uint32_t activation_start_ms)
{
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core_stop_pending(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (activation_timed_out(me, activation_start_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    core_handle_t *core = (core_handle_t *)pvTimerGetTimerID(timer);
    if (!core) {
        return;
    }

    xSemaphoreTake(core->lock, portMAX_DELAY);
    core->net_mgr.reconnect_cb_active++;
    bool should_reconnect = !core->destroying &&
                            core->state != CORE_STATE_DESTROYING &&
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

static esp_err_t enter_activation(core_handle_t *me)
{
    esp_err_t ret = core_set_state(me, CORE_STATE_NET_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = net_mgr_set_state(me, CORE_NET_STATE_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }

    set_activation_step(me, NET_STEP_CHECK_SIM);
    post_net_state(me, LWLTE_EVENT_NET_CONNECTING,
                   CORE_NET_STATE_ACTIVATING, 0);

    return ESP_OK;
}

static esp_err_t run_activation_loop(core_handle_t *me,
                                     uint32_t activation_start_ms)
{
    while (true) {
        esp_err_t ret = check_activation_continue(me, activation_start_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = run_activation_step(me, activation_start_ms);
        if (ret == ESP_OK) {
            if (me->net_mgr.current_step == NET_STEP_DONE) {
                return ESP_OK;
            }
            continue;
        }
        if (ret == ESP_ERR_NOT_FINISHED) {
            ret = wait_next_poll(me, activation_start_ms);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }

        return ret;
    }
}

static esp_err_t run_activation_step(core_handle_t *me,
                                     uint32_t activation_start_ms)
{
    esp_err_t ret = ESP_OK;
    esp_err_t continue_ret = ESP_OK;
    switch (me->net_mgr.current_step) {
    case NET_STEP_CHECK_SIM: {
        modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
        ret = modem_get_sim_status(me->modem, &sim_status);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret == ESP_ERR_TIMEOUT) {
            return ESP_ERR_NOT_FINISHED;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (sim_status == MODEM_SIM_READY) {
            set_activation_step(me, NET_STEP_CHECK_SIGNAL);
            return ESP_OK;
        }
        if (sim_status_fatal(sim_status)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_CHECK_SIGNAL: {
        modem_signal_t signal = {0};
        ret = modem_get_signal(me->modem, &signal);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        set_activation_step(me, NET_STEP_WAIT_REGISTRATION);
        return ESP_OK;
    }

    case NET_STEP_WAIT_REGISTRATION: {
        modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
        ret = modem_get_registration(me->modem, &reg_status);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (registration_ready(reg_status)) {
            set_activation_step(me, NET_STEP_WAIT_PACKET_ATTACH);
            return ESP_OK;
        }
        if (registration_denied(reg_status)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_WAIT_PACKET_ATTACH: {
        bool attached = false;
        ret = modem_get_packet_attach_status(me->modem, &attached);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (attached) {
            set_activation_step(me, NET_STEP_SET_APN);
            return ESP_OK;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_SET_APN:
        if (me->config.network.apn[0] != '\0') {
            ret = modem_set_apn(me->modem, me->config.network.primary_cid,
                                me->config.network.apn);
            continue_ret = check_activation_continue(me, activation_start_ms);
            if (continue_ret != ESP_OK) {
                return continue_ret;
            }
            if (ret != ESP_OK) {
                return ret;
            }
        }
        set_activation_step(me, NET_STEP_ACTIVATE_PDP);
        return ESP_OK;

    case NET_STEP_ACTIVATE_PDP:
        ret = modem_activate_pdp(me->modem, me->config.network.primary_cid);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret == ESP_ERR_TIMEOUT) {
            esp_err_t cleanup_ret = modem_deactivate_pdp(me->modem,
                                                         me->config.network.primary_cid);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGW(TAG, "cleanup after PDP activation failed: %s",
                         esp_err_to_name(cleanup_ret));
            }
            return continue_ret;
        }
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret == ESP_ERR_INVALID_STATE) {
            return classify_pdp_activation_invalid_state(me,
                                                         activation_start_ms);
        }
        if (ret != ESP_OK) {
            esp_err_t cleanup_ret = modem_deactivate_pdp(me->modem,
                                                         me->config.network.primary_cid);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGW(TAG, "cleanup after PDP activation failed: %s",
                         esp_err_to_name(cleanup_ret));
            }
            continue_ret = check_activation_continue(me, activation_start_ms);
            if (continue_ret != ESP_OK) {
                return continue_ret;
            }
            return ret;
        }
        set_activation_step(me, NET_STEP_QUERY_IP);
        return ESP_OK;

    case NET_STEP_QUERY_IP: {
        modem_pdp_context_t pdp = {0};
        ret = modem_get_pdp_context(me->modem, me->config.network.primary_cid, &pdp);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (!pdp.active) {
            set_activation_step(me, NET_STEP_WAIT_PACKET_ATTACH);
            return ESP_ERR_NOT_FINISHED;
        }
        if (pdp.ip_addr[0] == '\0') {
            return ESP_ERR_NOT_FINISHED;
        }
        ret = check_activation_continue(me, activation_start_ms);
        if (ret != ESP_OK) {
            return ret;
        }
        return complete_activation(me, &pdp);
    }

    case NET_STEP_DONE:
        return ESP_OK;

    case NET_STEP_IDLE:
    case NET_STEP_ERROR:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

static void set_activation_step(core_handle_t *me, net_mgr_step_t step)
{
    me->net_mgr.current_step = step;
    me->net_mgr.step_start_time_ms = now_ms();
}

static esp_err_t wait_next_poll(core_handle_t *me, uint32_t activation_start_ms)
{
    esp_err_t ret = check_activation_continue(me, activation_start_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t timeout_ms = me->config.network.net_activate_timeout_ms;
    const uint32_t elapsed_ms = now_ms() - activation_start_ms;
    if (elapsed_ms >= timeout_ms) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t delay_ms = NET_MGR_WAIT_POLL_INTERVAL_MS;
    const uint32_t remaining_ms = timeout_ms - elapsed_ms;
    if (delay_ms > remaining_ms) {
        delay_ms = remaining_ms;
    }

    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    return check_activation_continue(me, activation_start_ms);
}

static esp_err_t classify_pdp_activation_invalid_state(core_handle_t *me,
                                                       uint32_t activation_start_ms)
{
    modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
    esp_err_t ret = modem_get_sim_status(me->modem, &sim_status);
    esp_err_t continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret == ESP_ERR_TIMEOUT) {
        set_activation_step(me, NET_STEP_CHECK_SIM);
        return ESP_ERR_NOT_FINISHED;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (sim_status_fatal(sim_status)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sim_status != MODEM_SIM_READY) {
        set_activation_step(me, NET_STEP_CHECK_SIM);
        return ESP_ERR_NOT_FINISHED;
    }

    modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
    ret = modem_get_registration(me->modem, &reg_status);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (registration_denied(reg_status)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!registration_ready(reg_status)) {
        set_activation_step(me, NET_STEP_WAIT_REGISTRATION);
        return ESP_ERR_NOT_FINISHED;
    }

    bool attached = false;
    ret = modem_get_packet_attach_status(me->modem, &attached);
    continue_ret = check_activation_continue(me, activation_start_ms);
    if (continue_ret != ESP_OK) {
        return continue_ret;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (!attached) {
        set_activation_step(me, NET_STEP_WAIT_PACKET_ATTACH);
        return ESP_ERR_NOT_FINISHED;
    }

    return ESP_ERR_INVALID_STATE;
}

static esp_err_t complete_activation(core_handle_t *me,
                                     const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(pdp && pdp->active && pdp->ip_addr[0] != '\0',
                        ESP_ERR_INVALID_STATE, TAG, "PDP is not ready");

    esp_err_t ret = net_mgr_set_state(me, CORE_NET_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, CORE_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }

    pdp_mgr_update(&me->pdp_mgr, pdp);
    set_activation_step(me, NET_STEP_DONE);
    post_net_state(me, LWLTE_EVENT_NET_ONLINE, CORE_NET_STATE_ONLINE, 0);

    return ESP_OK;
}

static esp_err_t fail_activation(core_handle_t *me, esp_err_t err)
{
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = net_mgr_set_state(me, CORE_NET_STATE_ERROR);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, CORE_STATE_ERROR);
    if (ret != ESP_OK) {
        return ret;
    }
    me->net_mgr.current_step = NET_STEP_ERROR;
    post_net_state(me, LWLTE_EVENT_NET_ERROR, CORE_NET_STATE_ERROR, err);

    return err;
}

static bool registration_ready(modem_reg_status_t status)
{
    return status == MODEM_REG_REGISTERED_HOME ||
           status == MODEM_REG_REGISTERED_ROAMING;
}

static bool sim_status_fatal(modem_sim_status_t status)
{
    return status == MODEM_SIM_PIN_REQUIRED ||
           status == MODEM_SIM_PUK_REQUIRED ||
           status == MODEM_SIM_NOT_INSERTED ||
           status == MODEM_SIM_ERROR;
}

static bool registration_denied(modem_reg_status_t status)
{
    return status == MODEM_REG_DENIED;
}

static bool is_primary_pdp(core_handle_t *me, const modem_pdp_context_t *pdp)
{
    return me && pdp && pdp->cid == me->config.network.primary_cid;
}

static void post_net_state(core_handle_t *me, lwlte_event_id_t event_id,
                           core_net_state_t net_state, int error_code)
{
    lwlte_event_data_t data = {
        .net_state = (lwlte_net_state_t)net_state,
        .error_code = error_code,
    };

    esp_err_t ret = core_post_event(me, event_id, &data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post net event failed: %s", esp_err_to_name(ret));
    }
}
