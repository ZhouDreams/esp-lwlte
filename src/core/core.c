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

/* Ensure core_net_state_t and lwlte_net_state_t have identical value
 * layouts — core_post_event casts between them directly. */
_Static_assert((int)CORE_NET_STATE_OFFLINE    == (int)LWLTE_NET_STATE_OFFLINE,
               "net state enum mismatch: OFFLINE");
_Static_assert((int)CORE_NET_STATE_ACTIVATING == (int)LWLTE_NET_STATE_ACTIVATING,
               "net state enum mismatch: ACTIVATING");
_Static_assert((int)CORE_NET_STATE_ONLINE     == (int)LWLTE_NET_STATE_ONLINE,
               "net state enum mismatch: ONLINE");
_Static_assert((int)CORE_NET_STATE_ERROR      == (int)LWLTE_NET_STATE_ERROR,
               "net state enum mismatch: ERROR");

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
static bool config_valid(const core_config_t *config, modem_handle_t *modem);

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
 * @brief 发送简单 FSM 信号
 * @details Send simple FSM signal
 * @param[in] me LTE 核心服务句柄
 * @param[in] sig_type FSM 信号类型
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_TIMEOUT: FSM 队列已满
 */
static esp_err_t send_simple_signal(core_handle_t *me,
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
static bool api_state_allows(core_handle_t *me, core_fsm_sig_type_t sig_type);

/**
 * @brief Modem 事件回调
 * @details Modem event callback
 * @param[in] modem 调制解调器句柄
 * @param[in] event Modem 事件
 * @param[in] user_ctx 用户上下文
 */
static void core_modem_event_cb(modem_handle_t *modem, const modem_event_t *event,
                                void *user_ctx);

/**
 * @brief 初始化 Core 内部资源
 * @details Initialize Core internal resources
 * @param[in] me LTE 核心服务句柄
 * @param[in] config LTE 核心服务配置
 * @param[in] modem 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - other: ESP Event 或内部组件错误码
 */
static esp_err_t core_init(core_handle_t *me, const core_config_t *config,
                           modem_handle_t *modem);

/**
 * @brief 反初始化 Core 内部资源
 * @details Deinitialize Core internal resources
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: ESP Event 错误码
 */
static esp_err_t core_deinit(core_handle_t *me);

/**
 * @brief 判断 Core 内部资源是否已完整反初始化
 * @details Check whether Core internal resources are fully deinitialized
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - true: 已完整反初始化
 *         - false: 仍有资源未释放
 */
static bool core_deinit_complete(const core_handle_t *me);

/**
 * @brief 注销 Modem 事件回调
 * @details Unregister Modem event callback
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: Modem 错误码
 */
static esp_err_t core_unregister_modem_callback(core_handle_t *me);
static core_cmd_t *clone_core_cmd(const core_cmd_t *cmd);
static void free_core_cmd(core_cmd_t *cmd);
static char *clone_optional_string(const char *value);
static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len);
static bool core_cmd_type_valid(core_cmd_type_t type);
static bool core_cmd_valid(const core_cmd_t *cmd);
static esp_err_t clone_modem_protocol_payload(modem_event_t *event);
static void release_modem_protocol_payload(modem_event_t *event);

/**********************
 *  STATIC VARIABLES
 **********************/
ESP_EVENT_DEFINE_BASE(LWLTE_EVENT);

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
core_handle_t *core_create(const core_config_t *config, modem_handle_t *modem)
{
    core_handle_t *me = calloc(1, sizeof(*me));
    if (!me) {
        ESP_LOGE(TAG, "calloc core failed");
        return NULL;
    }

    esp_err_t ret = core_init(me, config, modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init core failed: %s", esp_err_to_name(ret));
        if (core_deinit_complete(me)) {
            free(me);
        } else {
            ESP_LOGE(TAG, "core init cleanup failed; core left allocated");
        }
        return NULL;
    }

    return me;
}

esp_err_t core_destroy(core_handle_t *me)
{
    core_state_t previous_state = CORE_STATE_STOPPED;
    bool retry_destroy = false;

    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(!core_fsm_is_task(me), ESP_ERR_INVALID_STATE, TAG,
                        "destroy from FSM task is not allowed");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    previous_state = me->state;
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

    esp_err_t ret = core_unregister_modem_callback(me);
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

    ret = core_deinit(me);
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

esp_err_t core_start(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_START),
                        ESP_ERR_INVALID_STATE, TAG, "start not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_START), TAG,
                        "send start signal failed");

    return ESP_OK;
}

esp_err_t core_stop(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_STOP),
                        ESP_ERR_INVALID_STATE, TAG, "stop not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_STOP), TAG,
                        "send stop signal failed");

    return ESP_OK;
}

esp_err_t core_register_protocol_callback(core_handle_t *me,
                                          core_protocol_callback_t callback,
                                          void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->protocol_callback = callback;
    me->protocol_user_ctx = callback ? user_ctx : NULL;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->protocol_closed_callback = callback;
    me->protocol_closed_user_ctx = callback ? user_ctx : NULL;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t core_get_state(core_handle_t *me, core_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    *state = core_get_state_value(me);

    return ESP_OK;
}

esp_err_t core_get_net_state(core_handle_t *me, core_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    return net_mgr_get_state(me, state);
}

esp_err_t core_connect(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_ACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "connect not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_NET_ACTIVATE), TAG,
                        "send connect signal failed");

    return ESP_OK;
}

esp_err_t core_disconnect(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_DEACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "disconnect not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_NET_DEACTIVATE), TAG,
                        "send disconnect signal failed");

    return ESP_OK;
}

esp_err_t core_submit_cmd(core_handle_t *me, const core_cmd_t *cmd)
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

esp_err_t core_set_state(core_handle_t *me, core_state_t state)
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

core_state_t core_get_state_value(core_handle_t *me)
{
    if (!me || !me->lock) {
        return CORE_STATE_STOPPED;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    core_state_t state = me->state;
    xSemaphoreGive(me->lock);

    return state;
}

bool core_is_destroying(core_handle_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying || me->state == CORE_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    return destroying;
}

esp_err_t core_post_event(core_handle_t *me, lwlte_event_id_t event_id,
                          const lwlte_event_data_t *data)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    lwlte_event_data_t empty_data = {0};
    if (!data) {
        data = &empty_data;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;
    if (me->config.event_loop) {
        ret = esp_event_post_to(me->config.event_loop, LWLTE_EVENT,
                                event_id, data, sizeof(*data), 0);
    } else {
        ret = esp_event_post(LWLTE_EVENT, event_id, data, sizeof(*data), 0);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(me->lock);

    return ret;
}

void core_free_cmd(core_cmd_t *cmd)
{
    free_core_cmd(cmd);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool config_valid(const core_config_t *config, modem_handle_t *modem)
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

static esp_err_t send_simple_signal(core_handle_t *me,
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

static bool api_state_allows(core_handle_t *me, core_fsm_sig_type_t sig_type)
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

static void core_modem_event_cb(modem_handle_t *modem, const modem_event_t *event,
                                void *user_ctx)
{
    (void)modem;

    core_handle_t *me = (core_handle_t *)user_ctx;
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

static esp_err_t core_init(core_handle_t *me, const core_config_t *config,
                           modem_handle_t *modem)
{
    esp_err_t ret = ESP_OK;

    if (!config_valid(config, modem)) {
        ESP_LOGE(TAG, "invalid core config");
        ret = ESP_ERR_INVALID_ARG;
        goto err;
    }

    ret = normalize_config(config, &me->config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "normalize core config failed: %s", esp_err_to_name(ret));
        goto err;
    }
    me->modem = NULL;
    me->state = CORE_STATE_STOPPED;
    me->destroying = false;
    me->destroy_in_progress = false;
    me->protocol_callback = NULL;
    me->protocol_user_ctx = NULL;
    me->protocol_closed_callback = NULL;
    me->protocol_closed_user_ctx = NULL;

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        ESP_LOGE(TAG, "create lock failed");
        ret = ESP_ERR_NO_MEM;
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
    me->modem = modem;

    return ESP_OK;

err:
    esp_err_t cleanup_ret = core_deinit(me);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGE(TAG, "cleanup after init failure failed: %s",
                 esp_err_to_name(cleanup_ret));
    }
    return ret;
}

static esp_err_t core_deinit(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = core_unregister_modem_callback(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister modem callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    core_fsm_stop(me);
    ret = net_mgr_deinit(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "net manager deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = core_fsm_deinit(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "core FSM deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    me->protocol_callback = NULL;
    me->protocol_user_ctx = NULL;
    me->protocol_closed_callback = NULL;
    me->protocol_closed_user_ctx = NULL;
    free((void *)me->config.apn);
    me->config.apn = NULL;
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }

    return ESP_OK;
}

static bool core_deinit_complete(const core_handle_t *me)
{
    if (!me) {
        return true;
    }

    return !me->modem && !me->fsm.task &&
           !me->fsm.queue && !me->fsm.task_done_sema &&
           !me->net_mgr.reconnect_timer &&
           !me->net_mgr.reconnect_cb_done_sema &&
           !me->lock && !me->config.apn;
}

static esp_err_t core_unregister_modem_callback(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    if (!me->modem) {
        return ESP_OK;
    }

    modem_handle_t *modem = me->modem;
    esp_err_t ret = modem_register_event_callback(modem, NULL, NULL);
    if (ret == ESP_OK) {
        me->modem = NULL;
    }

    return ret;
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
    case CORE_CMD_MQTT_CONFIGURE:
        clone->data.mqtt_config.client_id = clone_optional_string(cmd->data.mqtt_config.client_id);
        clone->data.mqtt_config.username = clone_optional_string(cmd->data.mqtt_config.username);
        clone->data.mqtt_config.password = clone_optional_string(cmd->data.mqtt_config.password);
        clone->data.mqtt_config.host = clone_optional_string(cmd->data.mqtt_config.host);
        clone->data.mqtt_config.port = cmd->data.mqtt_config.port;
        clone->data.mqtt_config.clean_session = cmd->data.mqtt_config.clean_session;
        clone->data.mqtt_config.keepalive_s = cmd->data.mqtt_config.keepalive_s;
        if (!clone->data.mqtt_config.client_id ||
            (cmd->data.mqtt_config.username && !clone->data.mqtt_config.username) ||
            (cmd->data.mqtt_config.password && !clone->data.mqtt_config.password) ||
            !clone->data.mqtt_config.host) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_PING:
        clone->data.ping.host = clone_optional_string(cmd->data.ping.host);
        if (!clone->data.ping.host) {
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
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
    case CORE_CMD_MQTT_TCP_CONNECT:
    case CORE_CMD_MQTT_TCP_DISCONNECT:
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
    case CORE_CMD_MQTT_CONFIGURE:
        free((void *)cmd->data.mqtt_config.client_id);
        free((void *)cmd->data.mqtt_config.username);
        free((void *)cmd->data.mqtt_config.password);
        free((void *)cmd->data.mqtt_config.host);
        break;
    case CORE_CMD_PING:
        free((void *)cmd->data.ping.host);
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
    return type >= CORE_CMD_MQTT_CONFIGURE && type <= CORE_CMD_PING;
}

static bool core_cmd_valid(const core_cmd_t *cmd)
{
    if (!cmd || !core_cmd_type_valid(cmd->type)) {
        return false;
    }

    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIGURE:
        return cmd->data.mqtt_config.client_id &&
               cmd->data.mqtt_config.host &&
               cmd->data.mqtt_config.port > 0;
    case CORE_CMD_MQTT_TCP_CONNECT:
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        return true;
    case CORE_CMD_PING:
        return cmd->data.ping.host != NULL &&
               cmd->data.ping.host[0] != '\0' &&
               cmd->data.ping.count >= 1 &&
               cmd->data.ping.count <= 100 &&
               cmd->data.ping.data_len <= 1024 &&
               cmd->data.ping.timeout_100ms >= 1 &&
               cmd->data.ping.timeout_100ms <= 600 &&
               cmd->data.ping.ttl >= 1 &&
               cmd->data.ping.replies != NULL &&
               cmd->data.ping.max_replies >= cmd->data.ping.count;
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
