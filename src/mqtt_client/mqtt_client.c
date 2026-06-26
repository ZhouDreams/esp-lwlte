/**
 * @file mqtt_client.c
 * @brief MQTT 客户端服务实现
 * @details MQTT client service implementation
 * @author JovisDreams
 * @date 2026-05-27
 */

/*********************
 *      INCLUDES
 *********************/
#include "mqtt_client_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "mqtt_client"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool config_valid(const mqtt_client_config_t *config, core_handle_t *core);
static esp_err_t normalize_config(const mqtt_client_config_t *config,
                                  mqtt_client_config_t *normalized);
static char *clone_string(const char *value);
static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len);
static bool state_is(mqtt_client_handle_t *me, mqtt_client_state_t state);
static esp_err_t set_state(mqtt_client_handle_t *me, mqtt_client_state_t state);
static esp_err_t send_fsm_sig(mqtt_client_handle_t *me, const mqtt_fsm_sig_t *sig);
static esp_err_t send_simple_sig(mqtt_client_handle_t *me, mqtt_fsm_sig_type_t type);
static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void mqtt_protocol_data_cb(core_handle_t *me,
                                  const core_protocol_data_t *data,
                                  void *user_ctx);
static void mqtt_protocol_closed_cb(core_handle_t *me,
                                    core_protocol_t protocol,
                                    const core_protocol_data_t *data,
                                    void *user_ctx);
static void mqtt_core_cmd_done_cb(core_handle_t *core, core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data, void *user_ctx);
static void free_mqtt_fsm_sig_payload(mqtt_fsm_sig_t *sig);
static void drain_fsm_queue_payloads(mqtt_client_handle_t *me, QueueHandle_t queue);
static void cleanup_partial_client(mqtt_client_handle_t *me);
static esp_err_t wait_stop_before_destroy(mqtt_client_handle_t *me);
static lwlte_mqtt_state_t map_mqtt_state(mqtt_client_state_t state);
static esp_err_t post_mqtt_event(mqtt_client_handle_t *me,
                                 lwlte_mqtt_event_id_t event_id,
                                 const lwlte_mqtt_event_data_t *payload);
static void post_error_event(mqtt_client_handle_t *me, int error_code);
static esp_err_t submit_core_cmd(mqtt_client_handle_t *me, core_cmd_type_t type,
                                 mqtt_client_operation_t operation,
                                 const mqtt_fsm_sig_t *sig);
static esp_err_t begin_connect(mqtt_client_handle_t *me);
static void mqtt_fsm_task(void *arg);
static bool mqtt_fsm_should_stop(mqtt_client_handle_t *me);
static void handle_signal(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig);
static void handle_start(mqtt_client_handle_t *me);
static void handle_stop(mqtt_client_handle_t *me);
static void complete_stop(mqtt_client_handle_t *me);
static void request_stop_disconnect(mqtt_client_handle_t *me);
static void handle_core_cmd_done(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig);
static void handle_runtime_operation(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig,
                                     core_cmd_type_t cmd_type,
                                     mqtt_client_operation_t operation);
static void handle_protocol_data(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool config_valid(const mqtt_client_config_t *config, core_handle_t *core)
{
    return config && core && config->endpoint.host && config->endpoint.host[0] &&
           config->endpoint.port > 0 && config->auth.client_id &&
           config->auth.client_id[0] &&
           (config->endpoint.transport == MQTT_CLIENT_TRANSPORT_PLAIN_TCP ||
            config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS);
}

static esp_err_t normalize_config(const mqtt_client_config_t *config,
                                  mqtt_client_config_t *normalized)
{
    ESP_RETURN_ON_FALSE(config && normalized, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    *normalized = *config;
    normalized->endpoint.host = clone_string(config->endpoint.host);
    normalized->auth.client_id = clone_string(config->auth.client_id);
    normalized->auth.username = config->auth.username ? clone_string(config->auth.username) : NULL;
    normalized->auth.password = config->auth.password ? clone_string(config->auth.password) : NULL;
    if (!normalized->endpoint.host || !normalized->auth.client_id ||
        (config->auth.username && !normalized->auth.username) ||
        (config->auth.password && !normalized->auth.password)) {
        free((void *)normalized->endpoint.host);
        free((void *)normalized->auth.client_id);
        free((void *)normalized->auth.username);
        free((void *)normalized->auth.password);
        memset(normalized, 0, sizeof(*normalized));
        return ESP_ERR_NO_MEM;
    }
    if (normalized->session.keepalive_s == 0) {
        normalized->session.keepalive_s = MQTT_CLIENT_DEFAULT_KEEPALIVE_S;
    }
    if (normalized->fsm.queue_size <= 0) {
        normalized->fsm.queue_size = MQTT_CLIENT_DEFAULT_FSM_QUEUE_SIZE;
    }
    if (normalized->fsm.task_stack <= 0) {
        normalized->fsm.task_stack = MQTT_CLIENT_DEFAULT_FSM_TASK_STACK;
    }
    if (normalized->fsm.task_priority <= 0) {
        normalized->fsm.task_priority = MQTT_CLIENT_DEFAULT_FSM_PRIORITY;
    }

    return ESP_OK;
}

static char *clone_string(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t len = strlen(value) + 1;
    char *copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len);

    return copy;
}

static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return NULL;
    }

    uint8_t *copy = malloc(payload_len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, payload, payload_len);

    return copy;
}

static bool state_is(mqtt_client_handle_t *me, mqtt_client_state_t state)
{
    if (!me || !me->lock) {
        return false;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool is_state = me->state == state;
    xSemaphoreGive(me->lock);

    return is_state;
}

static esp_err_t set_state(mqtt_client_handle_t *me, mqtt_client_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->state = state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

static esp_err_t send_fsm_sig(mqtt_client_handle_t *me, const mqtt_fsm_sig_t *sig_ptr)
{
    ESP_RETURN_ON_FALSE(me && sig_ptr && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    mqtt_fsm_sig_t sig = *sig_ptr;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool can_send = !me->destroying && me->state != MQTT_CLIENT_STATE_DESTROYING &&
                    me->fsm_task && me->fsm_queue;
    QueueHandle_t queue = me->fsm_queue;
    BaseType_t send_ret = pdFALSE;
    if (can_send) {
        send_ret = xQueueSend(me->fsm_queue, &sig, 0);
    }
    xSemaphoreGive(me->lock);

    (void)queue;
    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }
    if (send_ret != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t send_simple_sig(mqtt_client_handle_t *me, mqtt_fsm_sig_type_t type)
{
    mqtt_fsm_sig_t sig = {
        .type = type,
    };

    return send_fsm_sig(me, &sig);
}

static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)event_data;
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)handler_arg;
    if (!me || event_base != LWLTE_EVENT) {
        return;
    }
    if (me->destroying) {
        return;
    }

    mqtt_fsm_sig_t sig = {0};
    switch ((lwlte_event_id_t)event_id) {
    case LWLTE_EVENT_NET_ONLINE:
        sig.type = MQTT_SIG_NET_ONLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        sig.type = MQTT_SIG_NET_OFFLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    default:
        break;
    }
}

static void mqtt_protocol_data_cb(core_handle_t *me,
                                  const core_protocol_data_t *data,
                                  void *user_ctx)
{
    (void)me;
    mqtt_client_handle_t *client = (mqtt_client_handle_t *)user_ctx;
    if (!client || !data || data->protocol != CORE_PROTOCOL_MQTT) {
        return;
    }

    mqtt_protocol_data_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) {
        ESP_LOGW(TAG, "protocol data cb: alloc owned failed");
        return;
    }
    owned->topic = clone_string(data->topic);
    owned->topic_len = data->topic_len;
    owned->payload = clone_payload(data->payload, data->payload_len);
    owned->payload_len = data->payload_len;
    if ((data->topic && !owned->topic) ||
        (data->payload && data->payload_len > 0 && !owned->payload)) {
        free(owned->topic);
        free(owned->payload);
        free(owned);
        ESP_LOGW(TAG, "protocol data cb: clone failed");
        return;
    }

    mqtt_fsm_sig_t sig = {
        .type      = MQTT_SIG_PROTOCOL_DATA,
        .data      = owned,
        .data_size = sizeof(*owned),
    };
    if (send_fsm_sig(client, &sig) != ESP_OK) {
        free(owned->topic);
        free(owned->payload);
        free(owned);
    }
}

static void mqtt_protocol_closed_cb(core_handle_t *me,
                                    core_protocol_t protocol,
                                    const core_protocol_data_t *data,
                                    void *user_ctx)
{
    (void)me;
    (void)data;
    mqtt_client_handle_t *client = (mqtt_client_handle_t *)user_ctx;
    if (!client || protocol != CORE_PROTOCOL_MQTT) {
        return;
    }
    mqtt_fsm_sig_t sig = { .type = MQTT_SIG_PROTOCOL_CLOSED };
    (void)send_fsm_sig(client, &sig);
}

static void mqtt_core_cmd_done_cb(core_handle_t *core, core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data, void *user_ctx)
{
    (void)core;
    (void)result_data;

    mqtt_client_handle_t *me = (mqtt_client_handle_t *)user_ctx;
    if (!me) {
        return;
    }

    mqtt_fsm_sig_t sig = {
        .type = MQTT_SIG_CORE_CMD_DONE,
        .core_cmd_type = type,
        .core_result = result,
    };
    (void)send_fsm_sig(me, &sig);
}

static void free_mqtt_fsm_sig_payload(mqtt_fsm_sig_t *sig)
{
    if (!sig) {
        return;
    }

    switch (sig->type) {
    case MQTT_SIG_SUBSCRIBE:
    case MQTT_SIG_UNSUBSCRIBE:
        free(sig->data);
        break;
    case MQTT_SIG_PUBLISH: {
        mqtt_client_publish_t *publish = (mqtt_client_publish_t *)sig->data;
        if (publish) {
            free((void *)publish->topic);
            free((void *)publish->payload);
            free(publish);
        }
        break;
    }
    case MQTT_SIG_PROTOCOL_DATA: {
        mqtt_protocol_data_owned_t *owned = (mqtt_protocol_data_owned_t *)sig->data;
        if (owned) {
            free(owned->topic);
            free(owned->payload);
            free(owned);
        }
        break;
    }
    default:
        break;
    }
    sig->data = NULL;
    sig->data_size = 0;
}

static void drain_fsm_queue_payloads(mqtt_client_handle_t *me, QueueHandle_t queue)
{
    (void)me;
    if (!queue) {
        return;
    }

    mqtt_fsm_sig_t sig = {0};
    while (xQueueReceive(queue, &sig, 0) == pdTRUE) {
        free_mqtt_fsm_sig_payload(&sig);
    }
}

static void cleanup_partial_client(mqtt_client_handle_t *me)
{
    if (!me) {
        return;
    }

    if (me->core) {
        (void)core_register_protocol_callback(me->core, CORE_PROTOCOL_MQTT,
                                              NULL, NULL);
        (void)core_register_protocol_closed_callback(me->core,
                                                     CORE_PROTOCOL_MQTT,
                                                     NULL, NULL);
    }
    if (me->config.event.loop) {
        (void)esp_event_handler_unregister_with(me->config.event.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_ONLINE,
                                                handle_lwlte_event);
        (void)esp_event_handler_unregister_with(me->config.event.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_OFFLINE,
                                                handle_lwlte_event);
    } else {
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_ONLINE,
                                           handle_lwlte_event);
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_OFFLINE,
                                           handle_lwlte_event);
    }

    if (me->fsm_task && me->fsm_task_done_sema) {
        xSemaphoreTake(me->fsm_task_done_sema, portMAX_DELAY);
        me->fsm_task = NULL;
    }
    if (me->fsm_queue) {
        drain_fsm_queue_payloads(me, me->fsm_queue);
        vQueueDelete(me->fsm_queue);
        me->fsm_queue = NULL;
    }
    if (me->fsm_task_done_sema) {
        vSemaphoreDelete(me->fsm_task_done_sema);
        me->fsm_task_done_sema = NULL;
    }
    if (me->stop_done_sema) {
        vSemaphoreDelete(me->stop_done_sema);
        me->stop_done_sema = NULL;
    }
    free((void *)me->config.endpoint.host);
    free((void *)me->config.auth.client_id);
    free((void *)me->config.auth.username);
    free((void *)me->config.auth.password);
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    free(me);
}

static esp_err_t wait_stop_before_destroy(mqtt_client_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->stop_done_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    if (state_is(me, MQTT_CLIENT_STATE_STOPPED)) {
        return ESP_OK;
    }

    SemaphoreHandle_t done_sema = me->stop_done_sema;
    while (xSemaphoreTake(done_sema, 0) == pdTRUE) {
    }

    esp_err_t ret = send_simple_sig(me, MQTT_SIG_STOP);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xSemaphoreTake(done_sema,
                       pdMS_TO_TICKS(MQTT_CLIENT_STOP_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return state_is(me, MQTT_CLIENT_STATE_STOPPED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static lwlte_mqtt_state_t map_mqtt_state(mqtt_client_state_t state)
{
    switch (state) {
    case MQTT_CLIENT_STATE_STOPPED:        return LWLTE_MQTT_STATE_STOPPED;
    case MQTT_CLIENT_STATE_WAITING_NET:    return LWLTE_MQTT_STATE_WAITING_NET;
    case MQTT_CLIENT_STATE_CONNECTING:     return LWLTE_MQTT_STATE_CONNECTING;
    case MQTT_CLIENT_STATE_CONNECTED:      return LWLTE_MQTT_STATE_CONNECTED;
    case MQTT_CLIENT_STATE_DISCONNECTING:  return LWLTE_MQTT_STATE_DISCONNECTING;
    case MQTT_CLIENT_STATE_ERROR:
    case MQTT_CLIENT_STATE_DESTROYING:
    default:                               return LWLTE_MQTT_STATE_ERROR;
    }
}

static esp_err_t post_mqtt_event(mqtt_client_handle_t *me,
                                 lwlte_mqtt_event_id_t event_id,
                                 const lwlte_mqtt_event_data_t *payload)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    lwlte_mqtt_event_data_t empty_payload = {0};
    if (!payload) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        empty_payload.mqtt_state = map_mqtt_state(me->state);
        xSemaphoreGive(me->lock);
        payload = &empty_payload;
    }

    esp_err_t ret;
    if (me->config.event.loop) {
        ret = esp_event_post_to(me->config.event.loop, LWLTE_MQTT_EVENT,
                                event_id, payload, sizeof(*payload), 0);
    } else {
        ret = esp_event_post(LWLTE_MQTT_EVENT, event_id, payload,
                             sizeof(*payload), 0);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post mqtt event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
    }
    return ret;
}

static void post_error_event(mqtt_client_handle_t *me, int error_code)
{
    lwlte_mqtt_event_data_t payload = {
        .mqtt_state = LWLTE_MQTT_STATE_ERROR,
        .error_code = error_code,
    };
    (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_ERROR, &payload);
}

static esp_err_t submit_core_cmd(mqtt_client_handle_t *me, core_cmd_type_t type,
                                 mqtt_client_operation_t operation,
                                 const mqtt_fsm_sig_t *sig)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    core_cmd_t cmd = {
        .type = type,
        .done_cb = mqtt_core_cmd_done_cb,
        .user_ctx = me,
        .timeout_ms = MQTT_CLIENT_CMD_TIMEOUT_MS,
    };

    switch (type) {
    case CORE_CMD_MQTT_CONFIGURE:
        cmd.data.mqtt_config.client_id = me->config.auth.client_id;
        cmd.data.mqtt_config.username = me->config.auth.username;
        cmd.data.mqtt_config.password = me->config.auth.password;
        cmd.data.mqtt_config.host = me->config.endpoint.host;
        cmd.data.mqtt_config.port = me->config.endpoint.port;
        cmd.data.mqtt_config.transport =
            me->config.endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS ?
            LWLTE_MQTT_TRANSPORT_TLS : LWLTE_MQTT_TRANSPORT_PLAIN_TCP;
        cmd.data.mqtt_config.ssl_context_id = me->config.endpoint.ssl_context_id;
        cmd.data.mqtt_config.clean_session = me->config.session.clean_session;
        cmd.data.mqtt_config.keepalive_s = me->config.session.keepalive_s;
        break;
    case CORE_CMD_MQTT_TCP_CONNECT:
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        break;
    case CORE_CMD_MQTT_SUBSCRIBE:
        cmd.data.mqtt_subscribe.topic = (const char *)sig->data;
        cmd.data.mqtt_subscribe.qos = (uint8_t)sig->error_code;
        break;
    case CORE_CMD_MQTT_UNSUBSCRIBE:
        cmd.data.mqtt_unsubscribe.topic = (const char *)sig->data;
        break;
    case CORE_CMD_MQTT_PUBLISH: {
        const mqtt_client_publish_t *publish = (const mqtt_client_publish_t *)sig->data;
        cmd.data.mqtt_publish.topic = publish->topic;
        cmd.data.mqtt_publish.payload = publish->payload;
        cmd.data.mqtt_publish.payload_len = publish->payload_len;
        cmd.data.mqtt_publish.qos = publish->qos;
        cmd.data.mqtt_publish.retain = publish->retain;
        break;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->pending_cmd.active = true;
        me->pending_cmd.type = type;
        me->pending_cmd.operation = operation;
        me->pending_cmd.timeout_ms = MQTT_CLIENT_CMD_TIMEOUT_MS;
        me->pending_cmd.started_ms = 0;
        xSemaphoreGive(me->lock);
    }

    return ret;
}

static esp_err_t begin_connect(mqtt_client_handle_t *me)
{
    set_state(me, MQTT_CLIENT_STATE_CONNECTING);
    me->connect_step = MQTT_CONNECT_STEP_CONFIGURE;
    lwlte_mqtt_event_data_t payload = {
        .mqtt_state = LWLTE_MQTT_STATE_CONNECTING,
    };
    (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_CONNECTING, &payload);

    return submit_core_cmd(me, CORE_CMD_MQTT_CONFIGURE,
                           MQTT_CLIENT_OPERATION_CONNECT, NULL);
}

static void mqtt_fsm_task(void *arg)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)arg;

    while (!mqtt_fsm_should_stop(me)) {
        mqtt_fsm_sig_t sig = {0};
        if (xQueueReceive(me->fsm_queue, &sig,
                          pdMS_TO_TICKS(MQTT_CLIENT_FSM_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (mqtt_fsm_should_stop(me)) {
            free_mqtt_fsm_sig_payload(&sig);
            break;
        }
        handle_signal(me, &sig);
    }

    drain_fsm_queue_payloads(me, me->fsm_queue);
    if (me && me->fsm_task_done_sema) {
        xSemaphoreGive(me->fsm_task_done_sema);
    }
    vTaskDelete(NULL);
}

static bool mqtt_fsm_should_stop(mqtt_client_handle_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool stop = me->destroying || me->state == MQTT_CLIENT_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    return stop;
}

static void handle_signal(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig)
{
    if (!me || !sig) {
        return;
    }

    switch (sig->type) {
    case MQTT_SIG_START:
        handle_start(me);
        break;
    case MQTT_SIG_STOP:
        handle_stop(me);
        break;
    case MQTT_SIG_NET_ONLINE:
        me->net_online = true;
        if (state_is(me, MQTT_CLIENT_STATE_WAITING_NET)) {
            (void)begin_connect(me);
        }
        break;
    case MQTT_SIG_NET_OFFLINE:
    case MQTT_SIG_PROTOCOL_CLOSED:
        me->pending_cmd.active = false;
        me->transport_open = false;
        me->session_connected = false;
        if (me->stop_requested) {
            complete_stop(me);
        } else {
            set_state(me, MQTT_CLIENT_STATE_WAITING_NET);
            (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_DISCONNECTED, NULL);
        }
        break;
    case MQTT_SIG_CORE_CMD_DONE:
        handle_core_cmd_done(me, sig);
        break;
    case MQTT_SIG_SUBSCRIBE:
        handle_runtime_operation(me, sig, CORE_CMD_MQTT_SUBSCRIBE,
                                 MQTT_CLIENT_OPERATION_SUBSCRIBE);
        break;
    case MQTT_SIG_UNSUBSCRIBE:
        handle_runtime_operation(me, sig, CORE_CMD_MQTT_UNSUBSCRIBE,
                                 MQTT_CLIENT_OPERATION_UNSUBSCRIBE);
        break;
    case MQTT_SIG_PUBLISH:
        handle_runtime_operation(me, sig, CORE_CMD_MQTT_PUBLISH,
                                 MQTT_CLIENT_OPERATION_PUBLISH);
        break;
    case MQTT_SIG_PROTOCOL_DATA:
        handle_protocol_data(me, sig);
        break;
    default:
        break;
    }
    free_mqtt_fsm_sig_payload(sig);
}

static void handle_start(mqtt_client_handle_t *me)
{
    core_net_state_t net_state = CORE_NET_STATE_OFFLINE;
    (void)core_get_net_state(me->core, &net_state);
    me->net_online = net_state == CORE_NET_STATE_ONLINE;

    if (me->net_online) {
        if (begin_connect(me) != ESP_OK) {
            set_state(me, MQTT_CLIENT_STATE_ERROR);
            post_error_event(me, ESP_FAIL);
        }
    } else {
        set_state(me, MQTT_CLIENT_STATE_WAITING_NET);
        (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_STARTED, NULL);
    }
}

static void handle_stop(mqtt_client_handle_t *me)
{
    me->stop_requested = true;
    if (me->pending_cmd.active) {
        return;
    }

    request_stop_disconnect(me);
}

static void complete_stop(mqtt_client_handle_t *me)
{
    me->pending_cmd.active = false;
    me->stop_requested = false;
    me->transport_open = false;
    me->session_connected = false;
    me->net_online = false;
    me->connect_step = MQTT_CONNECT_STEP_IDLE;
    me->stop_step = MQTT_STOP_STEP_IDLE;
    set_state(me, MQTT_CLIENT_STATE_STOPPED);
    (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_STOPPED, NULL);
    if (me->stop_done_sema) {
        xSemaphoreGive(me->stop_done_sema);
    }
}

static void request_stop_disconnect(mqtt_client_handle_t *me)
{
    mqtt_client_state_t state = MQTT_CLIENT_STATE_STOPPED;
    (void)mqtt_client_get_state(me, &state);

    if (me->session_connected && me->stop_step == MQTT_STOP_STEP_IDLE) {
        me->stop_step = MQTT_STOP_STEP_DISCONNECT;
        set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
        esp_err_t ret = submit_core_cmd(me, CORE_CMD_MQTT_DISCONNECT,
                                        MQTT_CLIENT_OPERATION_DISCONNECT, NULL);
        if (ret != ESP_OK) {
            complete_stop(me);
        }
        return;
    }

    if (me->transport_open) {
        me->stop_step = MQTT_STOP_STEP_TCP_DISCONNECT;
        set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
        esp_err_t ret = submit_core_cmd(me, CORE_CMD_MQTT_TCP_DISCONNECT,
                                        MQTT_CLIENT_OPERATION_DISCONNECT, NULL);
        if (ret != ESP_OK) {
            complete_stop(me);
        }
        return;
    }

    complete_stop(me);
}

static void handle_core_cmd_done(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig)
{
    if (!me->pending_cmd.active || sig->core_cmd_type != me->pending_cmd.type) {
        return;
    }

    mqtt_client_operation_t operation = me->pending_cmd.operation;
    me->pending_cmd.active = false;
    if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_CONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->transport_open = true;
    }
    if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_DISCONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->transport_open = false;
    }
    if (sig->core_cmd_type == CORE_CMD_MQTT_CONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->session_connected = true;
    }
    if (sig->core_cmd_type == CORE_CMD_MQTT_DISCONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->session_connected = false;
    }

    if (me->stop_requested) {
        if (sig->core_cmd_type == CORE_CMD_MQTT_CONNECT &&
            sig->core_result == CORE_CMD_RESULT_OK) {
            me->connect_step = MQTT_CONNECT_STEP_DONE;
            set_state(me, MQTT_CLIENT_STATE_CONNECTED);
        }
        if (sig->core_cmd_type == CORE_CMD_MQTT_DISCONNECT &&
            sig->core_result == CORE_CMD_RESULT_OK) {
            set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
            request_stop_disconnect(me);
        } else if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_DISCONNECT) {
            complete_stop(me);
        } else {
            request_stop_disconnect(me);
        }
        return;
    }

    if (sig->core_result != CORE_CMD_RESULT_OK) {
        me->connect_step = MQTT_CONNECT_STEP_ERROR;
        set_state(me, MQTT_CLIENT_STATE_ERROR);
        post_error_event(me, ESP_FAIL);
        return;
    }

    if (operation == MQTT_CLIENT_OPERATION_CONNECT) {
        if (me->connect_step == MQTT_CONNECT_STEP_CONFIGURE) {
            me->connect_step = MQTT_CONNECT_STEP_TCP_CONNECT;
            (void)submit_core_cmd(me, CORE_CMD_MQTT_TCP_CONNECT,
                                  MQTT_CLIENT_OPERATION_CONNECT, NULL);
        } else if (me->connect_step == MQTT_CONNECT_STEP_TCP_CONNECT) {
            me->connect_step = MQTT_CONNECT_STEP_CONNECT;
            (void)submit_core_cmd(me, CORE_CMD_MQTT_CONNECT,
                                  MQTT_CLIENT_OPERATION_CONNECT, NULL);
        } else if (me->connect_step == MQTT_CONNECT_STEP_CONNECT) {
            me->connect_step = MQTT_CONNECT_STEP_DONE;
            me->transport_open = true;
            set_state(me, MQTT_CLIENT_STATE_CONNECTED);
            (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_CONNECTED, NULL);
        }
        return;
    }

    lwlte_mqtt_event_data_t payload = {
        .mqtt_state = LWLTE_MQTT_STATE_CONNECTED,
    };
    if (operation == MQTT_CLIENT_OPERATION_SUBSCRIBE) {
        (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_SUBSCRIBED, &payload);
    } else if (operation == MQTT_CLIENT_OPERATION_UNSUBSCRIBE) {
        (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_UNSUBSCRIBED, &payload);
    } else if (operation == MQTT_CLIENT_OPERATION_PUBLISH) {
        (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_PUBLISHED, &payload);
    }
}

static void handle_runtime_operation(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig,
                                     core_cmd_type_t cmd_type,
                                     mqtt_client_operation_t operation)
{
    if (me->pending_cmd.active) {
        mqtt_fsm_sig_t requeue = *sig;
        sig->data = NULL;
        sig->data_size = 0;
        (void)send_fsm_sig(me, &requeue);
        return;
    }
    if (!state_is(me, MQTT_CLIENT_STATE_CONNECTED)) {
        post_error_event(me, ESP_ERR_INVALID_STATE);
        return;
    }

    esp_err_t ret = ESP_OK;
    if (cmd_type == CORE_CMD_MQTT_SUBSCRIBE) {
        ret = submit_core_cmd(me, CORE_CMD_MQTT_SUBSCRIBE, operation, sig);
    } else if (cmd_type == CORE_CMD_MQTT_UNSUBSCRIBE) {
        ret = submit_core_cmd(me, CORE_CMD_MQTT_UNSUBSCRIBE, operation, sig);
    } else if (cmd_type == CORE_CMD_MQTT_PUBLISH) {
        ret = submit_core_cmd(me, CORE_CMD_MQTT_PUBLISH, operation, sig);
    }
    if (ret != ESP_OK) {
        post_error_event(me, ret);
    }
}

static void handle_protocol_data(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig)
{
    mqtt_protocol_data_owned_t *owned = (mqtt_protocol_data_owned_t *)sig->data;
    if (!owned) {
        return;
    }

    char *topic_copy = clone_string(owned->topic);
    uint8_t *payload_copy = clone_payload(owned->payload, owned->payload_len);
    if ((owned->topic && !topic_copy) ||
        (owned->payload && owned->payload_len > 0 && !payload_copy)) {
        free(topic_copy);
        free(payload_copy);
        ESP_LOGW(TAG, "handle_protocol_data: clone for bus failed, dropping");
        return;
    }

    lwlte_mqtt_event_data_t payload = {
        .mqtt_state    = LWLTE_MQTT_STATE_CONNECTED,
        .error_code    = 0,
        .msg = {
            .topic       = topic_copy,
            .topic_len   = owned->topic_len,
            .payload     = payload_copy,
            .payload_len = owned->payload_len,
        },
        .owns_payload  = true,
    };
    if (post_mqtt_event(me, LWLTE_MQTT_EVENT_DATA, &payload) != ESP_OK) {
        free(topic_copy);
        free(payload_copy);
    }
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
mqtt_client_handle_t *mqtt_client_create(const mqtt_client_config_t *config,
                                  core_handle_t *core)
{
    if (!config_valid(config, core)) {
        return NULL;
    }

    mqtt_client_handle_t *me = calloc(1, sizeof(*me));
    if (!me) {
        return NULL;
    }

    esp_err_t ret = normalize_config(config, &me->config);
    if (ret != ESP_OK) {
        free(me);
        return NULL;
    }
    me->core = core;
    me->state = MQTT_CLIENT_STATE_STOPPED;

    me->lock = xSemaphoreCreateMutex();
    me->fsm_queue = xQueueCreate(me->config.fsm.queue_size, sizeof(mqtt_fsm_sig_t));
    me->fsm_task_done_sema = xSemaphoreCreateBinary();
    me->stop_done_sema = xSemaphoreCreateBinary();
    if (!me->lock || !me->fsm_queue || !me->fsm_task_done_sema ||
        !me->stop_done_sema) {
        cleanup_partial_client(me);
        return NULL;
    }

    ret = core_register_protocol_callback(core, CORE_PROTOCOL_MQTT,
                                          mqtt_protocol_data_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
    ret = core_register_protocol_closed_callback(core, CORE_PROTOCOL_MQTT,
                                                 mqtt_protocol_closed_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }

    if (me->config.event.loop) {
        ret = esp_event_handler_register_with(me->config.event.loop, LWLTE_EVENT,
                                              LWLTE_EVENT_NET_ONLINE,
                                              handle_lwlte_event, me);
    } else {
        ret = esp_event_handler_register(LWLTE_EVENT,
                                         LWLTE_EVENT_NET_ONLINE,
                                         handle_lwlte_event, me);
    }
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
    if (me->config.event.loop) {
        ret = esp_event_handler_register_with(me->config.event.loop, LWLTE_EVENT,
                                              LWLTE_EVENT_NET_OFFLINE,
                                              handle_lwlte_event, me);
    } else {
        ret = esp_event_handler_register(LWLTE_EVENT,
                                         LWLTE_EVENT_NET_OFFLINE,
                                         handle_lwlte_event, me);
    }
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }

    BaseType_t task_ret = xTaskCreate(mqtt_fsm_task, "mqtt_fsm",
                                      me->config.fsm.task_stack, me,
                                      me->config.fsm.task_priority,
                                      &me->fsm_task);
    if (task_ret != pdPASS) {
        mqtt_client_destroy(me);
        return NULL;
    }

    return me;
}

esp_err_t mqtt_client_destroy(mqtt_client_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(!(me->fsm_task == xTaskGetCurrentTaskHandle()),
                        ESP_ERR_INVALID_STATE, TAG,
                        "destroy from MQTT FSM task is not allowed");

    esp_err_t ret = wait_stop_before_destroy(me);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->destroying = true;
    me->state = MQTT_CLIENT_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    if (me->fsm_task && me->fsm_task_done_sema) {
        xSemaphoreTake(me->fsm_task_done_sema, portMAX_DELAY);
        me->fsm_task = NULL;
    }

    if (me->config.event.loop) {
        (void)esp_event_handler_unregister_with(me->config.event.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_ONLINE,
                                                handle_lwlte_event);
        (void)esp_event_handler_unregister_with(me->config.event.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_OFFLINE,
                                                handle_lwlte_event);
    } else {
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_ONLINE,
                                           handle_lwlte_event);
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_OFFLINE,
                                           handle_lwlte_event);
    }

    if (me->core) {
        (void)core_register_protocol_callback(me->core, CORE_PROTOCOL_MQTT,
                                              NULL, NULL);
        (void)core_register_protocol_closed_callback(me->core,
                                                     CORE_PROTOCOL_MQTT,
                                                     NULL, NULL);
    }

    if (me->fsm_queue) {
        drain_fsm_queue_payloads(me, me->fsm_queue);
        vQueueDelete(me->fsm_queue);
        me->fsm_queue = NULL;
    }
    if (me->fsm_task_done_sema) {
        vSemaphoreDelete(me->fsm_task_done_sema);
        me->fsm_task_done_sema = NULL;
    }
    if (me->stop_done_sema) {
        vSemaphoreDelete(me->stop_done_sema);
        me->stop_done_sema = NULL;
    }
    free((void *)me->config.endpoint.host);
    free((void *)me->config.auth.client_id);
    free((void *)me->config.auth.username);
    free((void *)me->config.auth.password);
    vSemaphoreDelete(me->lock);
    me->lock = NULL;
    free(me);

    return ESP_OK;
}

esp_err_t mqtt_client_start(mqtt_client_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    mqtt_client_state_t state = MQTT_CLIENT_STATE_STOPPED;
    mqtt_client_get_state(me, &state);
    ESP_RETURN_ON_FALSE(state == MQTT_CLIENT_STATE_STOPPED ||
                        state == MQTT_CLIENT_STATE_WAITING_NET ||
                        state == MQTT_CLIENT_STATE_ERROR,
                        ESP_ERR_INVALID_STATE, TAG, "start not allowed");

    return send_simple_sig(me, MQTT_SIG_START);
}

esp_err_t mqtt_client_stop(mqtt_client_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(!state_is(me, MQTT_CLIENT_STATE_DESTROYING),
                        ESP_ERR_INVALID_STATE, TAG, "destroying");

    return send_simple_sig(me, MQTT_SIG_STOP);
}

esp_err_t mqtt_client_get_state(mqtt_client_handle_t *me,
                                mqtt_client_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t mqtt_client_subscribe(mqtt_client_handle_t *me,
                                const char *topic,
                                uint8_t qos)
{
    ESP_RETURN_ON_FALSE(me && topic && topic[0] && qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "invalid subscribe args");
    ESP_RETURN_ON_FALSE(state_is(me, MQTT_CLIENT_STATE_CONNECTED),
                        ESP_ERR_INVALID_STATE, TAG, "not connected");

    char *topic_copy = clone_string(topic);
    ESP_RETURN_ON_FALSE(topic_copy, ESP_ERR_NO_MEM, TAG, "copy topic failed");
    mqtt_fsm_sig_t sig = {
        .type = MQTT_SIG_SUBSCRIBE,
        .data = topic_copy,
        .error_code = qos,
    };

    esp_err_t ret = send_fsm_sig(me, &sig);
    if (ret != ESP_OK) {
        free_mqtt_fsm_sig_payload(&sig);
    }
    return ret;
}

esp_err_t mqtt_client_unsubscribe(mqtt_client_handle_t *me,
                                  const char *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic[0], ESP_ERR_INVALID_ARG, TAG,
                        "invalid unsubscribe args");
    ESP_RETURN_ON_FALSE(state_is(me, MQTT_CLIENT_STATE_CONNECTED),
                        ESP_ERR_INVALID_STATE, TAG, "not connected");

    char *topic_copy = clone_string(topic);
    ESP_RETURN_ON_FALSE(topic_copy, ESP_ERR_NO_MEM, TAG, "copy topic failed");
    mqtt_fsm_sig_t sig = {
        .type = MQTT_SIG_UNSUBSCRIBE,
        .data = topic_copy,
    };

    esp_err_t ret = send_fsm_sig(me, &sig);
    if (ret != ESP_OK) {
        free_mqtt_fsm_sig_payload(&sig);
    }
    return ret;
}

esp_err_t mqtt_client_publish(mqtt_client_handle_t *me,
                              const mqtt_client_publish_t *request)
{
    ESP_RETURN_ON_FALSE(me && request && request->topic && request->topic[0] &&
                        request->payload && request->payload_len > 0 &&
                        request->qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "invalid publish args");
    ESP_RETURN_ON_FALSE(state_is(me, MQTT_CLIENT_STATE_CONNECTED),
                        ESP_ERR_INVALID_STATE, TAG, "not connected");

    mqtt_client_publish_t *publish = calloc(1, sizeof(*publish));
    ESP_RETURN_ON_FALSE(publish, ESP_ERR_NO_MEM, TAG, "alloc publish failed");
    publish->topic = clone_string(request->topic);
    publish->payload = clone_payload(request->payload, request->payload_len);
    publish->payload_len = request->payload_len;
    publish->qos = request->qos;
    publish->retain = request->retain;
    if (!publish->topic || !publish->payload) {
        free((void *)publish->topic);
        free((void *)publish->payload);
        free(publish);
        return ESP_ERR_NO_MEM;
    }

    mqtt_fsm_sig_t sig = {
        .type = MQTT_SIG_PUBLISH,
        .data = publish,
        .data_size = sizeof(*publish),
    };

    esp_err_t ret = send_fsm_sig(me, &sig);
    if (ret != ESP_OK) {
        free_mqtt_fsm_sig_payload(&sig);
    }
    return ret;
}
