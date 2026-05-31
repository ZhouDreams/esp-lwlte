/**
 * @file core_fsm.c
 * @brief LTE Core FSM 任务
 * @details LTE Core FSM task
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
#define TAG "core_fsm"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief FSM 任务入口
 * @details FSM task entry
 * @param[in] arg LTE 核心服务句柄
 */
static void fsm_task(void *arg);

/**
 * @brief 判断 FSM 任务是否应停止
 * @details Check whether FSM task should stop
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - true: 应停止
 *         - false: 继续运行
 */
static bool fsm_should_stop(core_t *me);

/**
 * @brief 分发 FSM 信号
 * @details Dispatch FSM signal
 * @param[in] me LTE 核心服务句柄
 * @param[in] sig FSM 信号
 */
static void handle_signal(core_t *me, core_fsm_sig_t *sig);

/**
 * @brief 处理启动信号
 * @details Handle start signal
 * @param[in] me LTE 核心服务句柄
 */
static void handle_start(core_t *me);

/**
 * @brief 处理停止信号
 * @details Handle stop signal
 * @param[in] me LTE 核心服务句柄
 */
static void handle_stop(core_t *me);

/**
 * @brief 处理 Modem 事件
 * @details Handle Modem event
 * @param[in] me LTE 核心服务句柄
 * @param[in] event Modem 事件
 */
static void handle_modem_event(core_t *me, const modem_event_t *event);
static void handle_service_cmd(core_t *me, core_cmd_t *cmd);
static void handle_ping_cmd(core_t *me, core_cmd_t *cmd);
static void copy_core_ping_replies(core_ping_reply_t *dst,
                                   const modem_ping_reply_t *src,
                                   size_t count);
static void copy_core_ping_summary(core_ping_summary_t *dst,
                                   const modem_ping_summary_t *src);
static core_cmd_result_t result_from_esp_err(esp_err_t err);
static void finish_service_cmd(core_t *me, core_cmd_t *cmd,
                               core_cmd_result_t result,
                               const void *result_data);
static void release_modem_protocol_payload(modem_event_t *event);
static void release_fsm_signal_payload(core_t *me, core_fsm_sig_t *sig);
static void drain_fsm_queue_payloads(core_t *me, QueueHandle_t queue);

/**
 * @brief 处理 Modem 就绪状态
 * @details Handle Modem ready state
 * @param[in] me LTE 核心服务句柄
 */
static void handle_ready(core_t *me);

/**
 * @brief 处理 Core 错误
 * @details Handle Core error
 * @param[in] me LTE 核心服务句柄
 * @param[in] error_code 错误码
 */
static void handle_core_error(core_t *me, int error_code);

/**
 * @brief 判断 Modem 状态是否已就绪
 * @details Check whether Modem state is ready or beyond
 * @param[in] state Modem 状态
 * @return
 *         - true: 已就绪或更高状态
 *         - false: 未就绪
 */
static bool modem_state_ready(modem_state_t state);

/**
 * @brief 发布 Core 事件并记录失败
 * @details Post Core event and log failure
 * @param[in] me LTE 核心服务句柄
 * @param[in] event_id LTE 核心服务事件 ID
 * @param[in] data LTE 核心服务事件数据，可能为 NULL
 */
static void post_event_checked(core_t *me,
                               core_event_id_t event_id,
                               const core_event_data_t *data);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t core_fsm_init(core_t *me)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->fsm.task = NULL;
    me->fsm.queue = NULL;
    me->fsm.task_done_sema = NULL;
    me->fsm.running = false;
    me->fsm.stop_requested = false;

    me->fsm.queue = xQueueCreate(me->config.fsm_queue_size,
                                 sizeof(core_fsm_sig_t));
    ESP_GOTO_ON_FALSE(me->fsm.queue, ESP_ERR_NO_MEM, err, TAG,
                      "create fsm queue failed");

    me->fsm.task_done_sema = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(me->fsm.task_done_sema, ESP_ERR_NO_MEM, err, TAG,
                      "create task_done_sema failed");

    BaseType_t task_ret = xTaskCreate(fsm_task, "lwlte_fsm",
                                      me->config.fsm_task_stack, me,
                                      me->config.fsm_task_priority,
                                      &me->fsm.task);
    ESP_GOTO_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, err, TAG,
                      "create fsm task failed");

    me->fsm.running = true;

    return ESP_OK;

err:
    if (me->fsm.task_done_sema) {
        vSemaphoreDelete(me->fsm.task_done_sema);
        me->fsm.task_done_sema = NULL;
    }
    if (me->fsm.queue) {
        drain_fsm_queue_payloads(me, me->fsm.queue);
        vQueueDelete(me->fsm.queue);
        me->fsm.queue = NULL;
    }
    me->fsm.task = NULL;
    me->fsm.running = false;
    me->fsm.stop_requested = false;

    return ret;
}

void core_fsm_stop(core_t *me)
{
    if (!me) {
        return;
    }
    if (core_fsm_is_task(me)) {
        return;
    }

    if (me->lock) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->fsm.stop_requested = true;
        xSemaphoreGive(me->lock);
    } else {
        me->fsm.stop_requested = true;
    }

    if (me->fsm.queue) {
        core_fsm_sig_t sig = {
            .type = CORE_SIG_STOP,
        };
        (void)xQueueSend(me->fsm.queue, &sig, 0);
    }

    if (me->fsm.task && me->fsm.task_done_sema) {
        xSemaphoreTake(me->fsm.task_done_sema, portMAX_DELAY);
        me->fsm.task = NULL;
    } else if (me->fsm.task) {
        ESP_LOGW(TAG, "fsm task exists without task_done_sema");
        return;
    }

    me->fsm.running = false;
}

void core_fsm_deinit(core_t *me)
{
    if (!me) {
        return;
    }
    if (core_fsm_is_task(me)) {
        return;
    }

    core_fsm_stop(me);
    if (me->fsm.task) {
        return;
    }

    if (me->fsm.queue) {
        QueueHandle_t queue = me->fsm.queue;
        if (me->lock) {
            xSemaphoreTake(me->lock, portMAX_DELAY);
            queue = me->fsm.queue;
            me->fsm.queue = NULL;
            xSemaphoreGive(me->lock);
        } else {
            me->fsm.queue = NULL;
        }
        drain_fsm_queue_payloads(me, queue);
        vQueueDelete(queue);
    }
    if (me->fsm.task_done_sema) {
        vSemaphoreDelete(me->fsm.task_done_sema);
        me->fsm.task_done_sema = NULL;
    }
    me->fsm.running = false;
    me->fsm.stop_requested = false;
}

esp_err_t core_fsm_send(core_t *me, const core_fsm_sig_t *sig)
{
    ESP_RETURN_ON_FALSE(me && sig && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->fsm.stop_requested || !me->fsm.running ||
        !me->fsm.task || !me->fsm.queue) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t send_ret = xQueueSend(me->fsm.queue, sig, 0);
    xSemaphoreGive(me->lock);

    if (send_ret != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool core_fsm_is_task(core_t *me)
{
    return me && me->fsm.task && xTaskGetCurrentTaskHandle() == me->fsm.task;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void fsm_task(void *arg)
{
    core_t *me = (core_t *)arg;

    while (!fsm_should_stop(me)) {
        core_fsm_sig_t sig = {0};

        if (xQueueReceive(me->fsm.queue, &sig,
                          pdMS_TO_TICKS(CORE_FSM_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (fsm_should_stop(me)) {
            release_fsm_signal_payload(me, &sig);
            break;
        }
        handle_signal(me, &sig);
    }

    drain_fsm_queue_payloads(me, me->fsm.queue);

    if (me && me->lock) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->fsm.running = false;
        xSemaphoreGive(me->lock);
    }
    if (me && me->fsm.task_done_sema) {
        xSemaphoreGive(me->fsm.task_done_sema);
    }
    vTaskDelete(NULL);
}

static bool fsm_should_stop(core_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool stop = me->fsm.stop_requested || me->destroying;
    xSemaphoreGive(me->lock);

    return stop;
}

static void handle_signal(core_t *me, core_fsm_sig_t *sig)
{
    if (!me || !sig) {
        return;
    }
    if (core_is_destroying(me)) {
        release_fsm_signal_payload(me, sig);
        return;
    }

    switch (sig->type) {
    case CORE_SIG_START:
        handle_start(me);
        break;
    case CORE_SIG_STOP:
        handle_stop(me);
        break;
    case CORE_SIG_NET_ACTIVATE: {
        core_state_t state = core_get_state_value(me);
        if (state == CORE_STATE_READY || state == CORE_STATE_ERROR) {
            net_mgr_start_activation(me);
        }
        break;
    }
    case CORE_SIG_NET_DEACTIVATE: {
        core_state_t state = core_get_state_value(me);
        if (state == CORE_STATE_NET_ACTIVATING ||
            state == CORE_STATE_ONLINE ||
            state == CORE_STATE_ERROR) {
            net_mgr_deactivate(me);
            core_set_state(me, CORE_STATE_READY);
        }
        break;
    }
    case CORE_SIG_MODEM_EVENT:
        handle_modem_event(me, &sig->modem_event);
        release_modem_protocol_payload(&sig->modem_event);
        break;
    case CORE_SIG_SERVICE_CMD:
        handle_service_cmd(me, sig->service_cmd);
        sig->service_cmd = NULL;
        break;
    case CORE_SIG_RECONNECT: {
        core_state_t state = core_get_state_value(me);
        if (!core_is_destroying(me) &&
            (state == CORE_STATE_READY || state == CORE_STATE_ERROR)) {
            net_mgr_start_activation(me);
        }
        break;
    }
    case CORE_SIG_NET_STEP_DONE:
    case CORE_SIG_NET_STEP_TIMEOUT:
        break;
    default:
        ESP_LOGW(TAG, "unknown signal %d", sig->type);
        break;
    }
}

static void handle_start(core_t *me)
{
    modem_state_t modem_state = MODEM_STATE_CREATED;
    core_state_t state = core_get_state_value(me);

    if (state != CORE_STATE_STOPPED) {
        return;
    }

    core_set_state(me, CORE_STATE_STARTING);
    post_event_checked(me, CORE_EVENT_STARTED, NULL);

    esp_err_t ret = modem_get_state(me->modem, &modem_state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "get modem state failed: %s", esp_err_to_name(ret));
        return;
    }
    if (modem_state_ready(modem_state)) {
        handle_ready(me);
    }
}

static void handle_stop(core_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state == CORE_STATE_STOPPED ||
        state == CORE_STATE_DESTROYING) {
        return;
    }

    net_mgr_set_reconnect_enabled(me, false);
    net_mgr_cancel_reconnect(me);
    net_mgr_deactivate(me);
    core_set_state(me, CORE_STATE_STOPPED);
    post_event_checked(me, CORE_EVENT_STOPPED, NULL);
}

static void handle_modem_event(core_t *me, const modem_event_t *event)
{
    if (!me || !event) {
        return;
    }
    core_state_t state = core_get_state_value(me);
    if (core_is_destroying(me) || state == CORE_STATE_STOPPED ||
        state == CORE_STATE_DESTROYING) {
        return;
    }

    switch (event->id) {
    case MODEM_EVENT_READY:
        handle_ready(me);
        break;
    case MODEM_EVENT_PDP_ACTIVATED:
        net_mgr_handle_pdp_activated(me, &event->data.pdp);
        break;
    case MODEM_EVENT_PDP_DEACTIVATED:
        net_mgr_handle_pdp_deactivated(me, &event->data.pdp);
        break;
    case MODEM_EVENT_ERROR:
        handle_core_error(me, event->data.error_code);
        break;
    case MODEM_EVENT_PROTOCOL_DATA: {
        core_protocol_data_t protocol_data = {
            .protocol = (core_protocol_t)event->data.protocol_data.protocol,
            .topic = event->data.protocol_data.topic,
            .topic_len = event->data.protocol_data.topic_len,
            .payload = event->data.protocol_data.payload,
            .payload_len = event->data.protocol_data.payload_len,
        };
        (void)core_post_protocol_data(me, &protocol_data);
        break;
    }
    case MODEM_EVENT_PROTOCOL_CLOSED:
        post_event_checked(me, CORE_EVENT_PROTOCOL_CLOSED, NULL);
        break;
    case MODEM_EVENT_SIM_CHANGED:
    case MODEM_EVENT_REG_CHANGED:
    case MODEM_EVENT_SIGNAL_CHANGED:
        break;
    default:
        ESP_LOGW(TAG, "unknown modem event %d", event->id);
        break;
    }
}

static void handle_ready(core_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state == CORE_STATE_ONLINE ||
        state == CORE_STATE_NET_ACTIVATING ||
        state == CORE_STATE_READY) {
        return;
    }

    core_set_state(me, CORE_STATE_READY);
    post_event_checked(me, CORE_EVENT_READY, NULL);

    if (me->config.auto_connect) {
        net_mgr_start_activation(me);
    }
}

static void handle_core_error(core_t *me, int error_code)
{
    core_event_data_t data = {
        .net_state = CORE_NET_STATE_ERROR,
        .error_code = error_code,
    };

    core_set_state(me, CORE_STATE_ERROR);
    post_event_checked(me, CORE_EVENT_ERROR, &data);
}

static bool modem_state_ready(modem_state_t state)
{
    return state == MODEM_STATE_READY ||
           state == MODEM_STATE_REGISTERING ||
           state == MODEM_STATE_REGISTERED ||
           state == MODEM_STATE_PDP_ACTIVE;
}

static void post_event_checked(core_t *me,
                               core_event_id_t event_id,
                               const core_event_data_t *data)
{
    esp_err_t ret = core_post_event(me, event_id, data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post core event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
    }
}

static void handle_service_cmd(core_t *me, core_cmd_t *cmd)
{
    if (!me || !cmd) {
        core_free_cmd(cmd);
        return;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIGURE: {
        modem_mqtt_config_t config = {
            .client_id = cmd->data.mqtt_config.client_id,
            .username = cmd->data.mqtt_config.username,
            .password = cmd->data.mqtt_config.password,
            .host = cmd->data.mqtt_config.host,
            .port = cmd->data.mqtt_config.port,
            .clean_session = cmd->data.mqtt_config.clean_session,
            .keepalive_s = cmd->data.mqtt_config.keepalive_s,
        };
        ret = modem_mqtt_configure(me->modem, &config);
        break;
    }
    case CORE_CMD_MQTT_TCP_CONNECT:
        ret = modem_mqtt_tcp_connect(me->modem);
        break;
    case CORE_CMD_MQTT_CONNECT:
        ret = modem_mqtt_connect(me->modem);
        break;
    case CORE_CMD_MQTT_DISCONNECT:
        ret = modem_mqtt_disconnect(me->modem);
        break;
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        ret = modem_mqtt_tcp_disconnect(me->modem);
        break;
    case CORE_CMD_MQTT_SUBSCRIBE: {
        modem_mqtt_topic_t topic = {
            .topic = cmd->data.mqtt_subscribe.topic,
            .qos = cmd->data.mqtt_subscribe.qos,
        };
        ret = modem_mqtt_subscribe(me->modem, &topic);
        break;
    }
    case CORE_CMD_MQTT_UNSUBSCRIBE: {
        modem_mqtt_topic_t topic = {
            .topic = cmd->data.mqtt_unsubscribe.topic,
            .qos = 0,
        };
        ret = modem_mqtt_unsubscribe(me->modem, &topic);
        break;
    }
    case CORE_CMD_MQTT_PUBLISH: {
        modem_mqtt_publish_t publish = {
            .topic = cmd->data.mqtt_publish.topic,
            .payload = cmd->data.mqtt_publish.payload,
            .payload_len = cmd->data.mqtt_publish.payload_len,
            .qos = cmd->data.mqtt_publish.qos,
            .retain = cmd->data.mqtt_publish.retain,
        };
        ret = modem_mqtt_publish(me->modem, &publish);
        break;
    }
    case CORE_CMD_PING:
        handle_ping_cmd(me, cmd);
        return;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    finish_service_cmd(me, cmd, result_from_esp_err(ret), NULL);
}

static void handle_ping_cmd(core_t *me, core_cmd_t *cmd)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!me || !cmd) {
        finish_service_cmd(me, cmd, CORE_CMD_RESULT_ERROR, &ret);
        return;
    }

    core_net_state_t net_state = CORE_NET_STATE_OFFLINE;
    ret = core_get_net_state(me, &net_state);
    if (ret == ESP_OK && net_state != CORE_NET_STATE_ONLINE) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK) {
        finish_service_cmd(me, cmd, result_from_esp_err(ret), &ret);
        return;
    }

    modem_ping_reply_t *modem_replies = calloc(cmd->data.ping.count,
                                               sizeof(modem_ping_reply_t));
    if (!modem_replies) {
        ret = ESP_ERR_NO_MEM;
        finish_service_cmd(me, cmd, CORE_CMD_RESULT_ERROR, &ret);
        return;
    }

    modem_ping_summary_t modem_summary = {0};
    modem_ping_request_t request = {
        .host = cmd->data.ping.host,
        .count = cmd->data.ping.count,
        .data_len = cmd->data.ping.data_len,
        .timeout_100ms = cmd->data.ping.timeout_100ms,
        .ttl = cmd->data.ping.ttl,
        .total_timeout_ms = cmd->timeout_ms,
    };

    ret = modem_ping(me->modem, &request, modem_replies,
                     cmd->data.ping.count,
                     cmd->data.ping.summary ? &modem_summary : NULL);
    if (ret == ESP_OK) {
        copy_core_ping_replies(cmd->data.ping.replies, modem_replies,
                               cmd->data.ping.count);
        copy_core_ping_summary(cmd->data.ping.summary, &modem_summary);
    }

    free(modem_replies);
    finish_service_cmd(me, cmd, result_from_esp_err(ret), &ret);
}

static void copy_core_ping_replies(core_ping_reply_t *dst,
                                   const modem_ping_reply_t *src,
                                   size_t count)
{
    if (!dst || !src) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        dst[i].seq = src[i].seq;
        strlcpy(dst[i].ip, src[i].ip, sizeof(dst[i].ip));
        dst[i].time_ms = src[i].time_ms;
        dst[i].ttl = src[i].ttl;
        dst[i].success = src[i].success;
    }
}

static void copy_core_ping_summary(core_ping_summary_t *dst,
                                   const modem_ping_summary_t *src)
{
    if (!dst || !src) {
        return;
    }

    const modem_ping_summary_t modem_summary = *src;

    dst->sent = modem_summary.sent;
    dst->received = modem_summary.received;
    dst->lost = modem_summary.lost;
    dst->min_time_ms = modem_summary.min_time_ms;
    dst->max_time_ms = modem_summary.max_time_ms;
    dst->avg_time_ms = modem_summary.avg_time_ms;
}

static core_cmd_result_t result_from_esp_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return CORE_CMD_RESULT_OK;
    case ESP_ERR_TIMEOUT:
        return CORE_CMD_RESULT_TIMEOUT;
    case ESP_ERR_INVALID_RESPONSE:
        return CORE_CMD_RESULT_INVALID_RESPONSE;
    default:
        return CORE_CMD_RESULT_ERROR;
    }
}

static void finish_service_cmd(core_t *me, core_cmd_t *cmd,
                               core_cmd_result_t result,
                               const void *result_data)
{
    if (!cmd) {
        return;
    }
    if (cmd->done_cb) {
        cmd->done_cb(me, cmd->type, result, result_data, cmd->user_ctx);
    }
    core_free_cmd(cmd);
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

static void release_fsm_signal_payload(core_t *me, core_fsm_sig_t *sig)
{
    if (!sig) {
        return;
    }

    switch (sig->type) {
    case CORE_SIG_MODEM_EVENT:
        release_modem_protocol_payload(&sig->modem_event);
        break;
    case CORE_SIG_SERVICE_CMD:
        finish_service_cmd(me, sig->service_cmd, CORE_CMD_RESULT_ERROR, NULL);
        sig->service_cmd = NULL;
        break;
    default:
        break;
    }
}

static void drain_fsm_queue_payloads(core_t *me, QueueHandle_t queue)
{
    if (!me || !queue) {
        return;
    }

    core_fsm_sig_t sig = {0};
    while (xQueueReceive(queue, &sig, 0) == pdTRUE) {
        release_fsm_signal_payload(me, &sig);
    }
}
