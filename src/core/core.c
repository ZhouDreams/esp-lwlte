/**
 * @file core.c
 * @brief LTE 核心服务门面实现
 * @details LTE core service facade implementation
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "core"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 检查 Core 配置是否有效
 * @details Check whether Core configuration is valid
 * @param[in] config LTE 核心服务配置
 * @param[in] modem 调制解调器句柄
 * @return
 *         - true: 配置有效
 *         - false: 配置无效
 */
static bool config_valid(const core_config_t *config, modem_t *modem);

/**
 * @brief 归一化 Core 配置默认值
 * @details Normalize Core configuration default values
 * @param[in] config LTE 核心服务配置
 * @param[out] normalized 归一化后的 LTE 核心服务配置
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 */
static esp_err_t normalize_config(const core_config_t *config,
                                  core_config_t *normalized);

/**
 * @brief 创建 Core 自有事件循环
 * @details Create Core-owned event loop
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: ESP Event 错误码
 */
static esp_err_t create_event_loop(core_t *me);

/**
 * @brief 销毁 Core 自有事件循环
 * @details Destroy Core-owned event loop
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: ESP Event 错误码
 */
static esp_err_t destroy_event_loop(core_t *me);

/**
 * @brief 发送简单 FSM 信号
 * @details Send simple FSM signal
 * @param[in] me LTE 核心服务句柄
 * @param[in] sig_type FSM 信号类型
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_TIMEOUT: FSM 队列已满
 */
static esp_err_t send_simple_signal(core_t *me,
                                    core_fsm_sig_type_t sig_type);

/**
 * @brief 判断当前状态是否允许 API 操作
 * @details Check whether current state allows API operation
 * @param[in] me LTE 核心服务句柄
 * @param[in] sig_type FSM 信号类型
 * @return
 *         - true: 允许操作
 *         - false: 不允许操作
 */
static bool api_state_allows(core_t *me, core_fsm_sig_type_t sig_type);

/**
 * @brief Modem 事件回调
 * @details Modem event callback
 * @param[in] modem 调制解调器句柄
 * @param[in] event Modem 事件
 * @param[in] user_ctx 用户上下文
 */
static void core_modem_event_cb(modem_t *modem, const modem_event_t *event,
                                void *user_ctx);

/**
 * @brief Core 事件适配器
 * @details Core event adapter
 * @param[in] handler_arg LTE 核心服务句柄
 * @param[in] event_base 事件基
 * @param[in] event_id 事件 ID
 * @param[in] event_data 事件数据
 */
static void core_event_adapter(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

/**
 * @brief 等待 Core 事件回调空闲
 * @details Wait until Core event callbacks are idle
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 回调已空闲
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前任务正在执行回调或内部状态无效
 */
static esp_err_t wait_event_callbacks_idle(core_t *me);

/**
 * @brief 清理 Core 内部资源
 * @details Clean Core internal resources
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: ESP Event 错误码
 */
static esp_err_t cleanup_core(core_t *me);
static core_cmd_t *clone_core_cmd(const core_cmd_t *cmd);
static void free_core_cmd(core_cmd_t *cmd);
static char *clone_optional_string(const char *value);
static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len);
static bool core_cmd_type_valid(core_cmd_type_t type);
static bool core_cmd_valid(const core_cmd_t *cmd);
static esp_err_t clone_protocol_data(core_event_data_t *event_data,
                                     const core_protocol_data_t *protocol_data);
static void release_core_event_payload(core_event_data_t *event_data);
static esp_err_t clone_modem_protocol_payload(modem_event_t *event);
static void release_modem_protocol_payload(modem_event_t *event);

/**********************
 *  STATIC VARIABLES
 **********************/
ESP_EVENT_DEFINE_BASE(CORE_EVENT);

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
core_t *core_create(const core_config_t *config, modem_t *modem)
{
    esp_err_t ret = ESP_OK;
    esp_err_t cleanup_ret = ESP_OK;

    if (!config_valid(config, modem)) {
        ESP_LOGE(TAG, "invalid core config");
        return NULL;
    }

    core_t *me = calloc(1, sizeof(core_t));
    if (!me) {
        ESP_LOGE(TAG, "calloc core failed");
        return NULL;
    }

    ret = normalize_config(config, &me->config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "normalize core config failed: %s", esp_err_to_name(ret));
        free(me);
        return NULL;
    }
    me->modem = modem;
    me->state = CORE_STATE_STOPPED;
    me->destroying = false;
    me->destroy_in_progress = false;
    me->event_loop_task = NULL;
    me->event_callback_done_sema = NULL;
    me->event_callback_task = NULL;
    me->event_callback_active = 0;
    me->event_callback_waiting = false;

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        ESP_LOGE(TAG, "create lock failed");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    me->event_callback_done_sema = xSemaphoreCreateBinary();
    if (!me->event_callback_done_sema) {
        ESP_LOGE(TAG, "create event_callback_done_sema failed");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    ret = create_event_loop(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create event loop failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = pdp_mgr_init(&me->pdp_mgr, me->config.primary_cid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init PDP manager failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = net_mgr_init(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init net manager failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = core_fsm_init(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init core FSM failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = modem_register_event_callback(modem, core_modem_event_cb, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register modem callback failed: %s", esp_err_to_name(ret));
        goto err;
    }

    return me;

err:
    cleanup_ret = cleanup_core(me);
    if (cleanup_ret == ESP_OK) {
        free(me);
    } else {
        ESP_LOGE(TAG, "cleanup after create failure failed: %s; core left allocated",
                 esp_err_to_name(cleanup_ret));
    }
    return NULL;
}

esp_err_t core_destroy(core_t *me)
{
    core_state_t previous_state = CORE_STATE_STOPPED;
    bool retry_destroy = false;

    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(!core_fsm_is_task(me), ESP_ERR_INVALID_STATE, TAG,
                        "destroy from FSM task is not allowed");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    previous_state = me->state;
    if (me->event_loop_task == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->event_callback_task == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->destroy_in_progress) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->destroying) {
        if (previous_state != CORE_STATE_DESTROYING) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        retry_destroy = true;
    } else if (previous_state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    me->state = CORE_STATE_DESTROYING;
    me->destroy_in_progress = true;
    xSemaphoreGive(me->lock);

    esp_err_t ret = modem_register_event_callback(me->modem, NULL, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister modem callback failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (!retry_destroy) {
            me->destroying = false;
            me->state = previous_state;
        }
        me->destroy_in_progress = false;
        xSemaphoreGive(me->lock);
        return ret;
    }

    ret = wait_event_callbacks_idle(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wait event callbacks idle failed: %s",
                 esp_err_to_name(ret));
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->destroy_in_progress = false;
        xSemaphoreGive(me->lock);
        return ret;
    }

    ret = cleanup_core(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cleanup core failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->destroy_in_progress = false;
        xSemaphoreGive(me->lock);
        return ret;
    }
    free(me);

    return ESP_OK;
}

esp_err_t core_start(core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_START),
                        ESP_ERR_INVALID_STATE, TAG, "start not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_START), TAG,
                        "send start signal failed");

    return ESP_OK;
}

esp_err_t core_stop(core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_STOP),
                        ESP_ERR_INVALID_STATE, TAG, "stop not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_STOP), TAG,
                        "send stop signal failed");

    return ESP_OK;
}

esp_err_t core_register_event_callback(core_t *me,
                                       core_event_callback_t callback,
                                       void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    void *next_user_ctx = callback ? user_ctx : NULL;

    while (true) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->destroying || me->state == CORE_STATE_DESTROYING) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        bool changed = me->event_callback != callback ||
                       me->event_user_ctx != next_user_ctx;
        if (!changed) {
            xSemaphoreGive(me->lock);
            return ESP_OK;
        }
        if (me->event_callback_active == 0) {
            me->event_callback = callback;
            me->event_user_ctx = next_user_ctx;
            xSemaphoreGive(me->lock);
            return ESP_OK;
        }
        if (me->event_callback_task == xTaskGetCurrentTaskHandle()) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(me->lock);

        ESP_RETURN_ON_ERROR(wait_event_callbacks_idle(me), TAG,
                            "wait event callbacks idle failed");
    }
}

esp_event_loop_handle_t core_get_event_loop(core_t *me)
{
    if (!me || !me->lock) {
        return NULL;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    esp_event_loop_handle_t event_loop = me->event_loop;
    xSemaphoreGive(me->lock);

    return event_loop;
}

esp_err_t core_get_state(core_t *me, core_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    *state = core_get_state_value(me);

    return ESP_OK;
}

esp_err_t core_get_net_state(core_t *me, core_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    return net_mgr_get_state(me, state);
}

esp_err_t core_connect(core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_ACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "connect not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_NET_ACTIVATE), TAG,
                        "send connect signal failed");

    return ESP_OK;
}

esp_err_t core_disconnect(core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_DEACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "disconnect not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_NET_DEACTIVATE), TAG,
                        "send disconnect signal failed");

    return ESP_OK;
}

esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd)
{
    ESP_RETURN_ON_FALSE(me && me->lock && cmd, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(core_cmd_valid(cmd), ESP_ERR_INVALID_ARG, TAG,
                        "invalid core command");
    ESP_RETURN_ON_FALSE(!core_is_destroying(me), ESP_ERR_INVALID_STATE, TAG,
                        "core is destroying");

    core_cmd_t *cloned_cmd = clone_core_cmd(cmd);
    ESP_RETURN_ON_FALSE(cloned_cmd, ESP_ERR_NO_MEM, TAG,
                        "clone core command failed");

    core_fsm_sig_t sig = {
        .type = CORE_SIG_SERVICE_CMD,
        .service_cmd = cloned_cmd,
    };

    esp_err_t ret = core_fsm_send(me, &sig);
    if (ret != ESP_OK) {
        core_free_cmd(cloned_cmd);
        return ret;
    }

    return ESP_OK;
}

esp_err_t core_set_state(core_t *me, core_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(state >= CORE_STATE_STOPPED &&
                        state <= CORE_STATE_DESTROYING,
                        ESP_ERR_INVALID_ARG, TAG, "invalid state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying && state != CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->state = state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

core_state_t core_get_state_value(core_t *me)
{
    if (!me || !me->lock) {
        return CORE_STATE_STOPPED;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    core_state_t state = me->state;
    xSemaphoreGive(me->lock);

    return state;
}

bool core_is_destroying(core_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying || me->state == CORE_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    return destroying;
}

esp_err_t core_post_event(core_t *me, core_event_id_t event_id,
                          const core_event_data_t *event_data)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->event_loop, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(event_id >= CORE_EVENT_STARTED &&
                        event_id <= CORE_EVENT_PROTOCOL_CLOSED,
                        ESP_ERR_INVALID_ARG, TAG, "invalid event id");

    core_event_data_t empty_data = {0};
    if (!event_data) {
        event_data = &empty_data;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_event_post_to(me->event_loop, CORE_EVENT, event_id,
                                      event_data, sizeof(*event_data), 0);
    xSemaphoreGive(me->lock);

    return ret;
}

void core_free_cmd(core_cmd_t *cmd)
{
    free_core_cmd(cmd);
}

esp_err_t core_post_protocol_data(core_t *me,
                                  const core_protocol_data_t *protocol_data)
{
    ESP_RETURN_ON_FALSE(me && protocol_data, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    core_event_data_t event_data = {0};
    esp_err_t ret = clone_protocol_data(&event_data, protocol_data);
    ESP_RETURN_ON_ERROR(ret, TAG, "clone protocol data failed");

    /* MQTT service must copy topic/payload during the event callback. The
     * current Core implementation keeps protocol payload heap-owned until the
     * MQTT handler returns through core_event_adapter(), then releases it. */
    ret = core_post_event(me, CORE_EVENT_PROTOCOL_DATA, &event_data);
    if (ret != ESP_OK) {
        release_core_event_payload(&event_data);
    }

    return ret;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool config_valid(const core_config_t *config, modem_t *modem)
{
    return config && modem && config->apn &&
           config->primary_cid <= CORE_MAX_PDP_CONTEXTS;
}

static esp_err_t normalize_config(const core_config_t *config,
                                  core_config_t *normalized)
{
    ESP_RETURN_ON_FALSE(config && normalized && config->apn,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    size_t apn_len = strlen(config->apn) + 1;
    char *apn = malloc(apn_len);
    ESP_RETURN_ON_FALSE(apn, ESP_ERR_NO_MEM, TAG, "copy APN failed");

    memcpy(apn, config->apn, apn_len);
    *normalized = *config;
    normalized->apn = apn;

    if (normalized->primary_cid == 0) {
        normalized->primary_cid = CORE_DEFAULT_PRIMARY_CID;
    }
    if (normalized->net_activate_timeout_ms == 0) {
        normalized->net_activate_timeout_ms = CORE_DEFAULT_NET_ACTIVATE_TIMEOUT_MS;
    }
    if (normalized->reconnect_delay_ms == 0) {
        normalized->reconnect_delay_ms = CORE_DEFAULT_RECONNECT_DELAY_MS;
    }
    if (normalized->fsm_queue_size <= 0) {
        normalized->fsm_queue_size = CORE_DEFAULT_FSM_QUEUE_SIZE;
    }
    if (normalized->fsm_task_stack <= 0) {
        normalized->fsm_task_stack = CORE_DEFAULT_FSM_TASK_STACK;
    }
    if (normalized->fsm_task_priority <= 0) {
        normalized->fsm_task_priority = CORE_DEFAULT_FSM_TASK_PRIORITY;
    }

    return ESP_OK;
}

static esp_err_t create_event_loop(core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(!me->event_loop, ESP_ERR_INVALID_STATE, TAG,
                        "event loop already exists");

    esp_event_loop_args_t args = {
        .queue_size = CORE_EVENT_QUEUE_SIZE,
        .task_name = "lwlte_evt",
        .task_priority = CORE_EVENT_TASK_PRIORITY,
        .task_stack_size = CORE_EVENT_TASK_STACK,
        .task_core_id = tskNO_AFFINITY,
    };

    esp_err_t ret = esp_event_loop_create(&args, &me->event_loop);
    ESP_RETURN_ON_ERROR(ret, TAG, "create event loop failed");

    ret = esp_event_handler_register_with(me->event_loop, ESP_EVENT_ANY_BASE,
                                          ESP_EVENT_ANY_ID, core_event_adapter,
                                          me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register event adapter failed: %s", esp_err_to_name(ret));
        esp_err_t delete_ret = esp_event_loop_delete(me->event_loop);
        if (delete_ret != ESP_OK) {
            ESP_LOGW(TAG, "delete event loop after register failure failed: %s",
                     esp_err_to_name(delete_ret));
        }
        me->event_loop = NULL;
        return ret;
    }

    return ESP_OK;
}

static esp_err_t destroy_event_loop(core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    if (!me->event_loop) {
        return ESP_OK;
    }

    esp_err_t unregister_ret = esp_event_handler_unregister_with(me->event_loop,
                                                                 ESP_EVENT_ANY_BASE,
                                                                 ESP_EVENT_ANY_ID,
                                                                 core_event_adapter);
    if (unregister_ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister event adapter failed: %s",
                 esp_err_to_name(unregister_ret));
    }

    esp_err_t ret = esp_event_loop_delete(me->event_loop);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "delete event loop failed: %s", esp_err_to_name(ret));
        return ret;
    }
    me->event_loop = NULL;

    return ESP_OK;
}

static esp_err_t send_simple_signal(core_t *me,
                                    core_fsm_sig_type_t sig_type)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(sig_type == CORE_SIG_START ||
                        sig_type == CORE_SIG_STOP ||
                        sig_type == CORE_SIG_NET_ACTIVATE ||
                        sig_type == CORE_SIG_NET_DEACTIVATE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid simple signal");

    core_fsm_sig_t sig = {
        .type = sig_type,
    };

    return core_fsm_send(me, &sig);
}

static bool api_state_allows(core_t *me, core_fsm_sig_type_t sig_type)
{
    if (!me || !me->lock) {
        return false;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    core_state_t state = me->state;
    bool destroying = me->destroying;
    xSemaphoreGive(me->lock);

    if (destroying || state == CORE_STATE_DESTROYING) {
        return false;
    }

    switch (sig_type) {
    case CORE_SIG_START:
        return state == CORE_STATE_STOPPED;
    case CORE_SIG_STOP:
        return state != CORE_STATE_STOPPED;
    case CORE_SIG_NET_ACTIVATE:
        return state == CORE_STATE_READY ||
               state == CORE_STATE_ERROR;
    case CORE_SIG_NET_DEACTIVATE:
        return state == CORE_STATE_NET_ACTIVATING ||
               state == CORE_STATE_ONLINE ||
               state == CORE_STATE_ERROR;
    default:
        return false;
    }
}

static void core_modem_event_cb(modem_t *modem, const modem_event_t *event,
                                void *user_ctx)
{
    (void)modem;

    core_t *me = (core_t *)user_ctx;
    if (!me || !event) {
        return;
    }
    if (core_is_destroying(me)) {
        return;
    }

    core_fsm_sig_t sig = {
        .type = CORE_SIG_MODEM_EVENT,
        .modem_event = *event,
    };

    if (event->id == MODEM_EVENT_PROTOCOL_DATA) {
        esp_err_t clone_ret = clone_modem_protocol_payload(&sig.modem_event);
        if (clone_ret != ESP_OK) {
            ESP_LOGW(TAG, "clone modem protocol payload failed: %s",
                     esp_err_to_name(clone_ret));
            return;
        }
    }

    esp_err_t ret = core_fsm_send(me, &sig);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send modem event signal failed: %s", esp_err_to_name(ret));
        release_modem_protocol_payload(&sig.modem_event);
    }
}

static void core_event_adapter(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    core_t *me = (core_t *)handler_arg;
    if (!me) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->event_loop_task = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(me->lock);

    if (event_base != CORE_EVENT) {
        return;
    }
    if (event_id < CORE_EVENT_STARTED || event_id > CORE_EVENT_PROTOCOL_CLOSED) {
        return;
    }

    core_event_callback_t callback = NULL;
    void *user_ctx = NULL;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        release_core_event_payload((core_event_data_t *)event_data);
        return;
    }
    callback = me->event_callback;
    user_ctx = me->event_user_ctx;
    if (callback) {
        me->event_callback_active++;
        me->event_callback_task = xTaskGetCurrentTaskHandle();
    }
    xSemaphoreGive(me->lock);

    if (callback) {
        callback(me, (core_event_id_t)event_id,
                 (const core_event_data_t *)event_data, user_ctx);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->event_callback_active > 0) {
            me->event_callback_active--;
        }
        bool callbacks_idle = me->event_callback_active == 0;
        SemaphoreHandle_t done_sema = me->event_callback_done_sema;
        if (callbacks_idle) {
            me->event_callback_task = NULL;
        }
        xSemaphoreGive(me->lock);

        if (callbacks_idle && done_sema) {
            xSemaphoreGive(done_sema);
        }
    }

    release_core_event_payload((core_event_data_t *)event_data);
}

static esp_err_t wait_event_callbacks_idle(core_t *me)
{
    if (!me || !me->lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    int active = me->event_callback_active;
    SemaphoreHandle_t done_sema = me->event_callback_done_sema;
    if (me->event_callback_waiting) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (active == 0) {
        xSemaphoreGive(me->lock);
        return ESP_OK;
    }
    if (active > 0 && me->event_callback_task == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!done_sema) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->event_callback_waiting = true;
    xSemaphoreGive(me->lock);

    while (active > 0) {
        xSemaphoreTake(done_sema, portMAX_DELAY);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        active = me->event_callback_active;
        done_sema = me->event_callback_done_sema;
        if (active > 0 && !done_sema) {
            me->event_callback_waiting = false;
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(me->lock);
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->event_callback_waiting = false;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

static esp_err_t cleanup_core(core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    core_fsm_stop(me);
    esp_err_t ret = net_mgr_deinit(me);
    if (ret != ESP_OK) {
        return ret;
    }
    core_fsm_deinit(me);
    ret = destroy_event_loop(me);
    if (ret != ESP_OK) {
        return ret;
    }
    if (me->event_callback_done_sema) {
        vSemaphoreDelete(me->event_callback_done_sema);
        me->event_callback_done_sema = NULL;
    }
    me->event_callback_task = NULL;
    me->event_callback_active = 0;
    me->event_callback_waiting = false;
    me->event_loop_task = NULL;
    free((void *)me->config.apn);
    me->config.apn = NULL;
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }

    return ESP_OK;
}

static core_cmd_t *clone_core_cmd(const core_cmd_t *cmd)
{
    if (!core_cmd_valid(cmd)) {
        return NULL;
    }

    core_cmd_t *clone = calloc(1, sizeof(core_cmd_t));
    if (!clone) {
        return NULL;
    }
    *clone = *cmd;

    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIG:
        clone->data.mqtt_config.client_id = clone_optional_string(cmd->data.mqtt_config.client_id);
        clone->data.mqtt_config.username = clone_optional_string(cmd->data.mqtt_config.username);
        clone->data.mqtt_config.password = clone_optional_string(cmd->data.mqtt_config.password);
        if (!clone->data.mqtt_config.client_id ||
            (cmd->data.mqtt_config.username && !clone->data.mqtt_config.username) ||
            (cmd->data.mqtt_config.password && !clone->data.mqtt_config.password)) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_OPEN:
        clone->data.mqtt_open.host = clone_optional_string(cmd->data.mqtt_open.host);
        if (!clone->data.mqtt_open.host) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_SUBSCRIBE:
        clone->data.mqtt_subscribe.topic = clone_optional_string(cmd->data.mqtt_subscribe.topic);
        if (!clone->data.mqtt_subscribe.topic) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_UNSUBSCRIBE:
        clone->data.mqtt_unsubscribe.topic = clone_optional_string(cmd->data.mqtt_unsubscribe.topic);
        if (!clone->data.mqtt_unsubscribe.topic) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_PUBLISH:
        clone->data.mqtt_publish.topic = clone_optional_string(cmd->data.mqtt_publish.topic);
        clone->data.mqtt_publish.payload = clone_payload(cmd->data.mqtt_publish.payload,
                                                         cmd->data.mqtt_publish.payload_len);
        if (!clone->data.mqtt_publish.topic || !clone->data.mqtt_publish.payload) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_LOGIN:
    case CORE_CMD_MQTT_DISCONNECT:
        break;
    default:
        free_core_cmd(clone);
        return NULL;
    }

    return clone;
}

static void free_core_cmd(core_cmd_t *cmd)
{
    if (!cmd) {
        return;
    }

    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIG:
        free((void *)cmd->data.mqtt_config.client_id);
        free((void *)cmd->data.mqtt_config.username);
        free((void *)cmd->data.mqtt_config.password);
        break;
    case CORE_CMD_MQTT_OPEN:
        free((void *)cmd->data.mqtt_open.host);
        break;
    case CORE_CMD_MQTT_SUBSCRIBE:
        free((void *)cmd->data.mqtt_subscribe.topic);
        break;
    case CORE_CMD_MQTT_UNSUBSCRIBE:
        free((void *)cmd->data.mqtt_unsubscribe.topic);
        break;
    case CORE_CMD_MQTT_PUBLISH:
        free((void *)cmd->data.mqtt_publish.topic);
        free((void *)cmd->data.mqtt_publish.payload);
        break;
    default:
        break;
    }
    free(cmd);
}

static char *clone_optional_string(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t len = strlen(value) + 1;
    char *clone = malloc(len);
    if (!clone) {
        return NULL;
    }
    memcpy(clone, value, len);

    return clone;
}

static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return NULL;
    }

    uint8_t *clone = malloc(payload_len);
    if (!clone) {
        return NULL;
    }
    memcpy(clone, payload, payload_len);

    return clone;
}

static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_MQTT_CONFIG && type <= CORE_CMD_MQTT_PUBLISH;
}

static bool core_cmd_valid(const core_cmd_t *cmd)
{
    if (!cmd || !core_cmd_type_valid(cmd->type)) {
        return false;
    }

    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIG:
        return cmd->data.mqtt_config.client_id != NULL;
    case CORE_CMD_MQTT_OPEN:
        return cmd->data.mqtt_open.host != NULL && cmd->data.mqtt_open.port > 0;
    case CORE_CMD_MQTT_LOGIN:
        return true;
    case CORE_CMD_MQTT_DISCONNECT:
        return true;
    case CORE_CMD_MQTT_SUBSCRIBE:
        return cmd->data.mqtt_subscribe.topic != NULL &&
               cmd->data.mqtt_subscribe.qos <= 2;
    case CORE_CMD_MQTT_UNSUBSCRIBE:
        return cmd->data.mqtt_unsubscribe.topic != NULL;
    case CORE_CMD_MQTT_PUBLISH:
        return cmd->data.mqtt_publish.topic != NULL &&
               cmd->data.mqtt_publish.payload != NULL &&
               cmd->data.mqtt_publish.payload_len > 0 &&
               cmd->data.mqtt_publish.qos <= 2;
    default:
        return false;
    }
}

static esp_err_t clone_protocol_data(core_event_data_t *event_data,
                                     const core_protocol_data_t *protocol_data)
{
    ESP_RETURN_ON_FALSE(event_data && protocol_data, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    event_data->protocol_data = *protocol_data;
    event_data->protocol_data.topic = clone_optional_string(protocol_data->topic);
    event_data->protocol_data.payload = clone_payload(protocol_data->payload,
                                                      protocol_data->payload_len);
    if ((protocol_data->topic && !event_data->protocol_data.topic) ||
        (protocol_data->payload && protocol_data->payload_len > 0 &&
         !event_data->protocol_data.payload)) {
        release_core_event_payload(event_data);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void release_core_event_payload(core_event_data_t *event_data)
{
    if (!event_data) {
        return;
    }

    free((void *)event_data->protocol_data.topic);
    free((void *)event_data->protocol_data.payload);
    event_data->protocol_data.topic = NULL;
    event_data->protocol_data.payload = NULL;
    event_data->protocol_data.topic_len = 0;
    event_data->protocol_data.payload_len = 0;
}

static esp_err_t clone_modem_protocol_payload(modem_event_t *event)
{
    if (!event || event->id != MODEM_EVENT_PROTOCOL_DATA) {
        return ESP_OK;
    }

    const modem_protocol_data_t original = event->data.protocol_data;
    event->data.protocol_data.topic = clone_optional_string(original.topic);
    event->data.protocol_data.payload = clone_payload(original.payload,
                                                      original.payload_len);
    if ((original.topic && !event->data.protocol_data.topic) ||
        (original.payload && original.payload_len > 0 &&
         !event->data.protocol_data.payload)) {
        release_modem_protocol_payload(event);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void release_modem_protocol_payload(modem_event_t *event)
{
    if (!event || event->id != MODEM_EVENT_PROTOCOL_DATA) {
        return;
    }

    free((void *)event->data.protocol_data.topic);
    free((void *)event->data.protocol_data.payload);
    event->data.protocol_data.topic = NULL;
    event->data.protocol_data.payload = NULL;
    event->data.protocol_data.topic_len = 0;
    event->data.protocol_data.payload_len = 0;
}
