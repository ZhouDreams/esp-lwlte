/**
 * @file tcp_client.c
 * @brief TCP 客户端服务实现
 * @details TCP client service implementation
 * @author JovisDreams
 * @date 2026-06-18
 */

/*********************
 *      INCLUDES
 *********************/
#include "tcp_client_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "tcp_client"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool config_valid(const tcp_client_config_t *config, core_handle_t *core);
static esp_err_t normalize_config(const tcp_client_config_t *config,
                                  tcp_client_config_t *out);
static char *clone_string(const char *value);
static uint8_t *clone_payload(const uint8_t *data, size_t len);
static esp_err_t send_fsm_sig(tcp_client_handle_t *me, const tcp_fsm_sig_t *sig);
static esp_err_t send_fsm_sig_wait(tcp_client_handle_t *me,
                                   const tcp_fsm_sig_t *sig,
                                   TickType_t wait_ticks);
static bool acquire_conn(tcp_client_conn_t *conn);
static void release_conn(tcp_client_conn_t *conn);
static tcp_client_conn_t *acquire_current_conn(tcp_client_handle_t *me);
static bool signal_matches_current_conn(tcp_client_handle_t *me,
                                        const tcp_fsm_sig_t *sig,
                                        tcp_client_conn_t **out_conn);
static void latch_remote_closed(tcp_client_conn_t *conn, int reason,
                                int modem_error_code);
static void handle_remote_closed_if_latched(tcp_client_handle_t *me,
                                            tcp_client_conn_t *conn);
static esp_err_t set_conn_state(tcp_client_conn_t *conn, tcp_conn_state_t state);
static tcp_conn_state_t get_conn_state_value(tcp_client_conn_t *conn);
static bool conn_pending_command(tcp_client_conn_t *conn);
static bool conn_terminal_event_pending(tcp_client_conn_t *conn);
static bool conn_can_submit(tcp_client_conn_t *conn, core_cmd_type_t type);
static bool conn_terminal_or_destroyed(tcp_client_conn_t *conn);
static lwlte_tcp_conn_state_t map_conn_state(tcp_conn_state_t state);
static esp_err_t esp_err_from_core_result(core_cmd_result_t result);
static void tcp_protocol_data_cb(core_handle_t *core,
                                 const core_protocol_data_t *data,
                                 void *user_ctx);
static void tcp_protocol_closed_cb(core_handle_t *core,
                                   core_protocol_t protocol,
                                   const core_protocol_data_t *data,
                                   void *user_ctx);
static void tcp_core_cmd_done_cb(core_handle_t *core, core_cmd_type_t type,
                                 core_cmd_result_t result,
                                 const void *result_data, void *user_ctx);
static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void tcp_fsm_task(void *arg);
static bool tcp_fsm_should_stop(tcp_client_handle_t *me);
static void handle_signal(tcp_client_handle_t *me, tcp_fsm_sig_t *sig);
static void handle_open(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                        const tcp_open_owned_t *open);
static void handle_send_ready(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static void handle_close(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static void handle_core_cmd_done(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                                 tcp_fsm_sig_t *sig);
static void handle_protocol_data(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                                 tcp_fsm_sig_t *sig);
static void handle_protocol_closed(tcp_client_handle_t *me,
                                   tcp_client_conn_t *conn,
                                   const tcp_fsm_sig_t *sig);
static void handle_deferred_work(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static esp_err_t submit_socket_open(tcp_client_conn_t *conn, const char *host,
                                    uint16_t port);
static esp_err_t submit_socket_send(tcp_client_conn_t *conn,
                                    const tcp_send_item_t *item);
static esp_err_t submit_socket_recv(tcp_client_conn_t *conn);
static esp_err_t submit_socket_close(tcp_client_conn_t *conn);
static esp_err_t post_tcp_event(tcp_client_conn_t *conn,
                                lwlte_tcp_event_id_t event_id,
                                const lwlte_tcp_event_data_t *payload);
static esp_err_t post_tcp_event_with_ref(tcp_client_conn_t *conn,
                                         lwlte_tcp_event_id_t event_id,
                                         const lwlte_tcp_event_data_t *payload,
                                         bool ref_acquired,
                                         bool release_on_failure);
static void mark_terminal_event_pending(tcp_client_conn_t *conn,
                                        lwlte_tcp_event_id_t event_id,
                                        esp_err_t error_code,
                                        int modem_error_code,
                                        int reason);
static esp_err_t post_pending_terminal_event(tcp_client_conn_t *conn);
static void post_error_event(tcp_client_conn_t *conn, esp_err_t error_code,
                             int modem_error_code, int reason);
static void clear_send_queue(tcp_client_conn_t *conn);
static void free_fsm_sig_payload(tcp_fsm_sig_t *sig);
static void drain_fsm_queue_payloads(tcp_client_handle_t *me, QueueHandle_t queue);
static void cleanup_partial_client(tcp_client_handle_t *me);
static void cleanup_conn(tcp_client_conn_t *conn);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
tcp_client_handle_t *tcp_client_create(const tcp_client_config_t *config,
                                       core_handle_t *core)
{
    if (!config_valid(config, core)) {
        return NULL;
    }

    tcp_client_handle_t *me = calloc(1, sizeof(*me));
    if (!me) {
        return NULL;
    }

    esp_err_t ret = normalize_config(config, &me->config);
    if (ret != ESP_OK) {
        free(me);
        return NULL;
    }
    me->core = core;

    me->lock = xSemaphoreCreateMutex();
    me->fsm_queue = xQueueCreate(me->config.fsm_queue_size,
                                 sizeof(tcp_fsm_sig_t));
    me->fsm_task_done_sema = xSemaphoreCreateBinary();
    if (!me->lock || !me->fsm_queue || !me->fsm_task_done_sema) {
        cleanup_partial_client(me);
        return NULL;
    }

    ret = core_register_protocol_callback(core, CORE_PROTOCOL_TCP,
                                          tcp_protocol_data_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
    ret = core_register_protocol_closed_callback(core, CORE_PROTOCOL_TCP,
                                                 tcp_protocol_closed_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }

    if (me->config.loop) {
        ret = esp_event_handler_register_with(me->config.loop, LWLTE_EVENT,
                                              LWLTE_EVENT_NET_OFFLINE,
                                              handle_lwlte_event, me);
    } else {
        ret = esp_event_handler_register(LWLTE_EVENT, LWLTE_EVENT_NET_OFFLINE,
                                         handle_lwlte_event, me);
    }
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }

    BaseType_t task_ret = xTaskCreate(tcp_fsm_task, "tcp_fsm",
                                      me->config.fsm_task_stack, me,
                                      me->config.fsm_task_priority,
                                      &me->fsm_task);
    if (task_ret != pdPASS) {
        cleanup_partial_client(me);
        return NULL;
    }

    return me;
}

esp_err_t tcp_client_destroy(tcp_client_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(!(me->fsm_task == xTaskGetCurrentTaskHandle()),
                        ESP_ERR_INVALID_STATE, TAG,
                        "destroy from TCP FSM task is not allowed");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->conn) {
        xSemaphoreGive(me->lock);
        ESP_LOGW(TAG, "conn still exists");
        return ESP_ERR_INVALID_STATE;
    }
    if (me->deferred_destroy_conn) {
        xSemaphoreGive(me->lock);
        ESP_LOGW(TAG, "conn still exists");
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    xSemaphoreGive(me->lock);

    if (me->config.loop) {
        (void)esp_event_handler_unregister_with(me->config.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_OFFLINE,
                                                handle_lwlte_event);
    } else {
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_OFFLINE,
                                           handle_lwlte_event);
    }

    if (me->core) {
        (void)core_register_protocol_callback(me->core, CORE_PROTOCOL_TCP,
                                              NULL, NULL);
        (void)core_register_protocol_closed_callback(me->core,
                                                     CORE_PROTOCOL_TCP,
                                                     NULL, NULL);
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
    vSemaphoreDelete(me->lock);
    me->lock = NULL;
    free(me);

    return ESP_OK;
}

esp_err_t tcp_client_open(tcp_client_handle_t *me,
                          const tcp_client_open_config_t *config,
                          tcp_client_conn_t **out_conn)
{
    ESP_RETURN_ON_FALSE(me && me->lock && config && out_conn &&
                        config->host && config->host[0] && config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid open args");

    *out_conn = NULL;
    core_net_state_t net_state = CORE_NET_STATE_OFFLINE;
    ESP_RETURN_ON_ERROR(core_get_net_state(me->core, &net_state), TAG,
                        "get net state failed");
    ESP_RETURN_ON_FALSE(net_state == CORE_NET_STATE_ONLINE,
                        ESP_ERR_INVALID_STATE, TAG, "network is not online");

    tcp_client_conn_t *conn = calloc(1, sizeof(*conn));
    ESP_RETURN_ON_FALSE(conn, ESP_ERR_NO_MEM, TAG, "alloc TCP conn failed");
    conn->client = me;
    conn->conn_id = 0;
    conn->state = TCP_CONN_STATE_CONNECTING;
    conn->user_ctx = config->user_ctx;
    conn->send_queue = xQueueCreate(me->config.send_queue_size,
                                    sizeof(tcp_send_item_t));
    conn->lock = xSemaphoreCreateMutex();
    conn->send_queue_lock = xSemaphoreCreateMutex();
    conn->active_done_sema = xSemaphoreCreateBinary();
    if (!conn->send_queue || !conn->lock || !conn->send_queue_lock ||
        !conn->active_done_sema) {
        cleanup_conn(conn);
        return ESP_ERR_NO_MEM;
    }

    tcp_open_owned_t *open = calloc(1, sizeof(*open));
    if (!open) {
        cleanup_conn(conn);
        return ESP_ERR_NO_MEM;
    }
    open->host = clone_string(config->host);
    open->port = config->port;
    if (!open->host) {
        free(open);
        cleanup_conn(conn);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->conn || me->deferred_destroy_conn) {
        xSemaphoreGive(me->lock);
        free(open->host);
        free(open);
        cleanup_conn(conn);
        return ESP_ERR_INVALID_STATE;
    }
    conn->generation = ++me->next_conn_generation;
    me->conn = conn;
    xSemaphoreGive(me->lock);

    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_OPEN,
        .conn_scoped = true,
        .conn_generation = conn->generation,
        .data = open,
        .data_size = sizeof(*open),
    };
    esp_err_t ret = send_fsm_sig(me, &sig);
    if (ret != ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->conn == conn) {
            me->conn = NULL;
        }
        xSemaphoreGive(me->lock);
        free_fsm_sig_payload(&sig);
        cleanup_conn(conn);
        return ret;
    }

    *out_conn = conn;
    return ESP_OK;
}

esp_err_t tcp_client_send(tcp_client_conn_t *conn,
                          const uint8_t *data,
                          size_t len)
{
    ESP_RETURN_ON_FALSE(conn && conn->client && data && len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid send args");
    ESP_RETURN_ON_FALSE(acquire_conn(conn), ESP_ERR_INVALID_STATE, TAG,
                        "TCP conn is destroying");

    tcp_client_handle_t *client = conn->client;
    if (len > client->config.max_tx_len) {
        release_conn(conn);
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(client->lock, portMAX_DELAY);
    bool destroying = client->destroying;
    xSemaphoreGive(client->lock);
    if (destroying || !conn->send_queue || !conn->send_queue_lock) {
        release_conn(conn);
        return ESP_ERR_INVALID_STATE;
    }
    tcp_send_item_t item = {
        .data = clone_payload(data, len),
        .len = len,
    };
    if (!item.data) {
        release_conn(conn);
        return ESP_ERR_NO_MEM;
    }

    tcp_client_handle_t *me = conn->client;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    xSemaphoreTake(conn->send_queue_lock, portMAX_DELAY);
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (me->destroying || !me->fsm_task || !me->fsm_queue ||
        conn->state != TCP_CONN_STATE_CONNECTED) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(conn->send_queue_lock);
        xSemaphoreGive(me->lock);
        free(item.data);
        release_conn(conn);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t conn_generation = conn->generation;
    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_SEND_READY,
        .conn_scoped = true,
        .conn_generation = conn_generation,
    };
    BaseType_t send_ret = xQueueSend(me->fsm_queue, &sig, 0);
    if (send_ret != pdTRUE) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(conn->send_queue_lock);
        xSemaphoreGive(me->lock);
        free(item.data);
        release_conn(conn);
        return ESP_ERR_TIMEOUT;
    }
    if (xQueueSend(conn->send_queue, &item, 0) != pdTRUE) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(conn->send_queue_lock);
        xSemaphoreGive(me->lock);
        free(item.data);
        release_conn(conn);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreGive(conn->lock);
    xSemaphoreGive(conn->send_queue_lock);
    xSemaphoreGive(me->lock);
    release_conn(conn);

    return ESP_OK;
}

esp_err_t tcp_client_close(tcp_client_conn_t *conn)
{
    ESP_RETURN_ON_FALSE(conn && conn->client, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(acquire_conn(conn), ESP_ERR_INVALID_STATE, TAG,
                        "TCP conn is destroying");

    tcp_client_handle_t *me = conn->client;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->state != TCP_CONN_STATE_CONNECTED &&
        conn->state != TCP_CONN_STATE_ERROR) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(me->lock);
        release_conn(conn);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->destroying || !me->fsm_task || !me->fsm_queue) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(me->lock);
        release_conn(conn);
        return ESP_ERR_INVALID_STATE;
    }
    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_CLOSE,
        .conn_scoped = true,
        .conn_generation = conn->generation,
    };
    BaseType_t send_ret = xQueueSend(me->fsm_queue, &sig, 0);
    if (send_ret != pdTRUE) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(me->lock);
        release_conn(conn);
        return ESP_ERR_TIMEOUT;
    }
    conn->close_requested = true;
    if (conn->state != TCP_CONN_STATE_ERROR) {
        conn->state = TCP_CONN_STATE_CLOSING;
    }
    xSemaphoreGive(conn->lock);
    xSemaphoreGive(me->lock);

    release_conn(conn);
    return ESP_OK;
}

esp_err_t tcp_client_conn_get_state(tcp_client_conn_t *conn,
                                    tcp_conn_state_t *state)
{
    ESP_RETURN_ON_FALSE(conn && state && conn->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(acquire_conn(conn), ESP_ERR_INVALID_STATE, TAG,
                        "TCP conn is destroying");

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    *state = conn->state;
    xSemaphoreGive(conn->lock);
    release_conn(conn);

    return ESP_OK;
}

esp_err_t tcp_client_conn_destroy(tcp_client_conn_t *conn)
{
    ESP_RETURN_ON_FALSE(conn && conn->client && conn->lock,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    tcp_client_handle_t *client = conn->client;
    ESP_RETURN_ON_FALSE(client->lock, ESP_ERR_INVALID_ARG, TAG,
                        "invalid TCP client");
    xSemaphoreTake(client->lock, portMAX_DELAY);
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->destroyed) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(client->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (conn->state != TCP_CONN_STATE_CLOSED &&
        conn->state != TCP_CONN_STATE_ERROR) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(client->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (conn->pending_cmd.active) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(client->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (conn->terminal_event_pending ||
        (conn->remote_closed && !conn->remote_closed_event_posted)) {
        xSemaphoreGive(conn->lock);
        xSemaphoreGive(client->lock);
        return ESP_ERR_INVALID_STATE;
    }
    conn->destroyed = true;
    bool cleanup_now = conn->active_refs == 0;
    client->deferred_destroy_conn = cleanup_now ? NULL : conn;
    if (client->conn == conn) {
        client->conn = NULL;
    }
    xSemaphoreGive(conn->lock);
    xSemaphoreGive(client->lock);

    if (cleanup_now) {
        cleanup_conn(conn);
    }

    return ESP_OK;
}

void tcp_client_conn_release_event(tcp_client_conn_t *conn)
{
    release_conn(conn);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool config_valid(const tcp_client_config_t *config, core_handle_t *core)
{
    return config && core;
}

static esp_err_t normalize_config(const tcp_client_config_t *config,
                                  tcp_client_config_t *out)
{
    ESP_RETURN_ON_FALSE(config && out, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    if (config->max_conns > TCP_CLIENT_DEFAULT_MAX_CONNS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_FALSE(config->send_queue_size >= 0 &&
                        config->fsm_queue_size >= 0 &&
                        config->fsm_task_stack >= 0 &&
                        config->fsm_task_priority >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid TCP config");

    *out = *config;
    if (out->max_conns == 0) {
        out->max_conns = TCP_CLIENT_DEFAULT_MAX_CONNS;
    }
    if (out->send_queue_size == 0) {
        out->send_queue_size = TCP_CLIENT_DEFAULT_SEND_QUEUE_SIZE;
    }
    if (out->max_tx_len == 0) {
        out->max_tx_len = TCP_CLIENT_DEFAULT_MAX_TX_LEN;
    }
    if (out->max_rx_event_len == 0) {
        out->max_rx_event_len = TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN;
    }
    if (out->open_timeout_ms == 0) {
        out->open_timeout_ms = TCP_CLIENT_DEFAULT_OPEN_TIMEOUT_MS;
    }
    if (out->send_timeout_ms == 0) {
        out->send_timeout_ms = TCP_CLIENT_DEFAULT_SEND_TIMEOUT_MS;
    }
    if (out->close_timeout_ms == 0) {
        out->close_timeout_ms = TCP_CLIENT_DEFAULT_CLOSE_TIMEOUT_MS;
    }
    if (out->fsm_queue_size == 0) {
        out->fsm_queue_size = TCP_CLIENT_DEFAULT_FSM_QUEUE_SIZE;
    }
    if (out->fsm_task_stack == 0) {
        out->fsm_task_stack = TCP_CLIENT_DEFAULT_FSM_TASK_STACK;
    }
    if (out->fsm_task_priority == 0) {
        out->fsm_task_priority = TCP_CLIENT_DEFAULT_FSM_PRIORITY;
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

static uint8_t *clone_payload(const uint8_t *data, size_t len)
{
    if (!data || len == 0) {
        return NULL;
    }

    uint8_t *copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, data, len);

    return copy;
}

static esp_err_t send_fsm_sig(tcp_client_handle_t *me, const tcp_fsm_sig_t *sig)
{
    return send_fsm_sig_wait(me, sig, 0);
}

static esp_err_t send_fsm_sig_wait(tcp_client_handle_t *me,
                                   const tcp_fsm_sig_t *sig,
                                   TickType_t wait_ticks)
{
    ESP_RETURN_ON_FALSE(me && sig && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    QueueHandle_t queue = me->fsm_queue;
    bool can_send = !me->destroying && me->fsm_task && queue;
    BaseType_t send_ret = pdFALSE;
    xSemaphoreGive(me->lock);

    if (!can_send) {
        return ESP_ERR_INVALID_STATE;
    }
    send_ret = xQueueSend(queue, sig, wait_ticks);
    if (send_ret != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static bool acquire_conn(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return false;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->destroyed) {
        xSemaphoreGive(conn->lock);
        return false;
    }
    conn->active_refs++;
    xSemaphoreGive(conn->lock);

    return true;
}

static void release_conn(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return;
    }

    bool cleanup_now = false;
    tcp_client_handle_t *client = conn->client;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->active_refs > 0) {
        conn->active_refs--;
        cleanup_now = conn->destroyed && conn->active_refs == 0;
        if (conn->active_refs == 0) {
            xSemaphoreGive(conn->active_done_sema);
        }
    }
    xSemaphoreGive(conn->lock);

    if (cleanup_now && client && client->lock) {
        xSemaphoreTake(client->lock, portMAX_DELAY);
        if (client->deferred_destroy_conn == conn) {
            client->deferred_destroy_conn = NULL;
        }
        xSemaphoreGive(client->lock);
    }

    if (cleanup_now) {
        cleanup_conn(conn);
    }
}

static tcp_client_conn_t *acquire_current_conn(tcp_client_handle_t *me)
{
    if (!me || !me->lock) {
        return NULL;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    tcp_client_conn_t *conn = me->conn;
    if (conn && !acquire_conn(conn)) {
        conn = NULL;
    }
    xSemaphoreGive(me->lock);

    return conn;
}

static bool signal_matches_current_conn(tcp_client_handle_t *me,
                                        const tcp_fsm_sig_t *sig,
                                        tcp_client_conn_t **out_conn)
{
    if (out_conn) {
        *out_conn = NULL;
    }
    if (!me || !sig || !out_conn) {
        return false;
    }

    tcp_client_conn_t *conn = acquire_current_conn(me);
    if (!conn) {
        return !sig->conn_scoped;
    }
    if (sig->conn_scoped && conn->generation != sig->conn_generation) {
        release_conn(conn);
        return false;
    }

    *out_conn = conn;
    return true;
}

static void latch_remote_closed(tcp_client_conn_t *conn, int reason,
                                int modem_error_code)
{
    if (!conn || !conn->lock) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    conn->remote_closed = true;
    conn->remote_reason = reason;
    conn->remote_modem_error_code = modem_error_code;
    conn->close_requested = false;
    conn->recv_requested = false;
    if (conn->terminal_event_posted) {
        conn->remote_closed_event_posted = true;
        conn->remote_closed_event_pending = false;
    }
    mark_terminal_event_pending(conn, LWLTE_TCP_EVENT_DISCONNECTED, ESP_OK,
                                modem_error_code, reason);
    if (conn->state != TCP_CONN_STATE_CLOSED) {
        conn->state = TCP_CONN_STATE_CLOSED;
    }
    xSemaphoreGive(conn->lock);
}

static void handle_remote_closed_if_latched(tcp_client_handle_t *me,
                                            tcp_client_conn_t *conn)
{
    (void)me;
    if (!conn || !conn->lock) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->destroyed || !conn->remote_closed ||
        conn->remote_closed_event_posted) {
        xSemaphoreGive(conn->lock);
        return;
    }
    if (conn->pending_cmd.active) {
        conn->remote_closed_event_pending = true;
        xSemaphoreGive(conn->lock);
        return;
    }
    xSemaphoreGive(conn->lock);

    clear_send_queue(conn);
    if (post_pending_terminal_event(conn) != ESP_OK) {
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->remote_closed_event_pending = true;
        xSemaphoreGive(conn->lock);
        return;
    }
}

static esp_err_t set_conn_state(tcp_client_conn_t *conn, tcp_conn_state_t state)
{
    ESP_RETURN_ON_FALSE(conn && conn->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    conn->state = state;
    xSemaphoreGive(conn->lock);

    return ESP_OK;
}

static tcp_conn_state_t get_conn_state_value(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return TCP_CONN_STATE_ERROR;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    tcp_conn_state_t state = conn->state;
    xSemaphoreGive(conn->lock);

    return state;
}

static bool conn_pending_command(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return false;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool pending = conn->pending_cmd.active;
    xSemaphoreGive(conn->lock);

    return pending;
}

static bool conn_terminal_event_pending(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return false;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool pending = conn->terminal_event_pending;
    xSemaphoreGive(conn->lock);
    return pending;
}

static bool conn_can_submit(tcp_client_conn_t *conn, core_cmd_type_t type)
{
    /* Caller holds conn->lock so the guard and core submission are atomic. */
    if (!conn || !conn->lock) {
        return false;
    }
    if (conn->destroyed || conn->state == TCP_CONN_STATE_CLOSED ||
        conn->pending_cmd.active) {
        return false;
    }

    switch (type) {
    case CORE_CMD_SOCKET_OPEN:
        return conn->state == TCP_CONN_STATE_CONNECTING;
    case CORE_CMD_SOCKET_SEND:
    case CORE_CMD_SOCKET_RECV:
        return conn->state == TCP_CONN_STATE_CONNECTED;
    case CORE_CMD_SOCKET_CLOSE:
        return conn->close_requested &&
               (conn->state == TCP_CONN_STATE_CONNECTED ||
                conn->state == TCP_CONN_STATE_CLOSING ||
                conn->state == TCP_CONN_STATE_ERROR);
    default:
        return false;
    }
}

static bool conn_terminal_or_destroyed(tcp_client_conn_t *conn)
{
    if (!conn || !conn->lock) {
        return true;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool terminal = conn->destroyed || conn->state == TCP_CONN_STATE_CLOSED;
    xSemaphoreGive(conn->lock);

    return terminal;
}

static lwlte_tcp_conn_state_t map_conn_state(tcp_conn_state_t state)
{
    switch (state) {
    case TCP_CONN_STATE_CREATED:     return LWLTE_TCP_CONN_STATE_CREATED;
    case TCP_CONN_STATE_CONNECTING:  return LWLTE_TCP_CONN_STATE_CONNECTING;
    case TCP_CONN_STATE_CONNECTED:   return LWLTE_TCP_CONN_STATE_CONNECTED;
    case TCP_CONN_STATE_CLOSING:     return LWLTE_TCP_CONN_STATE_CLOSING;
    case TCP_CONN_STATE_CLOSED:      return LWLTE_TCP_CONN_STATE_CLOSED;
    case TCP_CONN_STATE_ERROR:
    default:                         return LWLTE_TCP_CONN_STATE_ERROR;
    }
}

static esp_err_t esp_err_from_core_result(core_cmd_result_t result)
{
    switch (result) {
    case CORE_CMD_RESULT_OK:
        return ESP_OK;
    case CORE_CMD_RESULT_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case CORE_CMD_RESULT_INVALID_RESPONSE:
        return ESP_ERR_INVALID_RESPONSE;
    case CORE_CMD_RESULT_ERROR:
    default:
        return ESP_FAIL;
    }
}

static void tcp_protocol_data_cb(core_handle_t *core,
                                 const core_protocol_data_t *data,
                                 void *user_ctx)
{
    (void)core;
    tcp_client_handle_t *me = (tcp_client_handle_t *)user_ctx;
    if (!me || !data || data->protocol != CORE_PROTOCOL_TCP) {
        return;
    }
    tcp_client_conn_t *conn = acquire_current_conn(me);
    if (!conn) {
        return;
    }
    if (data->conn_id != conn->conn_id) {
        release_conn(conn);
        return;
    }
    tcp_protocol_data_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) {
        release_conn(conn);
        return;
    }
    owned->conn_id = data->conn_id;
    owned->reason = data->reason;
    owned->modem_error_code = data->modem_error_code;
    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_PROTOCOL_DATA,
        .conn_scoped = true,
        .conn_generation = conn->generation,
        .data = owned,
        .data_size = sizeof(*owned),
    };
    if (send_fsm_sig_wait(me, &sig, portMAX_DELAY) != ESP_OK) {
        free(owned);
    }
    release_conn(conn);
}

static void tcp_protocol_closed_cb(core_handle_t *core,
                                   core_protocol_t protocol,
                                   const core_protocol_data_t *data,
                                   void *user_ctx)
{
    (void)core;
    tcp_client_handle_t *me = (tcp_client_handle_t *)user_ctx;
    if (!me || protocol != CORE_PROTOCOL_TCP) {
        return;
    }
    tcp_client_conn_t *conn = acquire_current_conn(me);
    if (conn) {
        tcp_fsm_sig_t sig = {
            .type = TCP_SIG_PROTOCOL_CLOSED,
            .conn_scoped = true,
            .conn_generation = conn->generation,
        };
        if (data) {
            sig.error_code = data->reason;
            sig.modem_error_code = data->modem_error_code;
        }
        (void)send_fsm_sig_wait(me, &sig, portMAX_DELAY);
        release_conn(conn);
    }
}

static void tcp_core_cmd_done_cb(core_handle_t *core, core_cmd_type_t type,
                                 core_cmd_result_t result,
                                 const void *result_data, void *user_ctx)
{
    (void)core;
    tcp_client_conn_t *conn = (tcp_client_conn_t *)user_ctx;
    if (!conn || !conn->lock) {
        if (type == CORE_CMD_SOCKET_RECV &&
            result == CORE_CMD_RESULT_OK && result_data) {
            const core_socket_recv_result_t *recv = result_data;
            free(recv->payload);
        }
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    tcp_client_handle_t *me = conn->client;
    bool destroyed = conn->destroyed;
    uint32_t conn_generation = conn->generation;
    xSemaphoreGive(conn->lock);

    if (destroyed || !me) {
        if (type == CORE_CMD_SOCKET_RECV &&
            result == CORE_CMD_RESULT_OK && result_data) {
            const core_socket_recv_result_t *recv = result_data;
            free(recv->payload);
        }
        release_conn(conn);
        return;
    }

    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_CORE_CMD_DONE,
        .conn_scoped = true,
        .conn_generation = conn_generation,
        .core_cmd_type = type,
        .core_result = result,
    };
    sig.error_code = esp_err_from_core_result(result);
    if (type == CORE_CMD_SOCKET_RECV && result == CORE_CMD_RESULT_OK && result_data) {
        const core_socket_recv_result_t *recv = result_data;
        core_socket_recv_result_t *owned = calloc(1, sizeof(*owned));
        if (!owned) {
            free(recv->payload);
            sig.core_result = CORE_CMD_RESULT_ERROR;
        } else {
            *owned = *recv;
            owned->payload = clone_payload(recv->payload, recv->payload_len);
            if (recv->payload && recv->payload_len > 0 && !owned->payload) {
                free(recv->payload);
                free(owned);
                sig.core_result = CORE_CMD_RESULT_ERROR;
            } else {
                free(recv->payload);
                sig.result_data = owned;
                sig.result_size = sizeof(*owned);
            }
        }
    } else if (result_data) {
        const core_socket_result_t *socket_result = result_data;
        sig.error_code = socket_result->error_code;
        sig.modem_error_code = socket_result->modem_error_code;
    }

    if (send_fsm_sig_wait(me, &sig, portMAX_DELAY) != ESP_OK) {
        free_fsm_sig_payload(&sig);
    }
    release_conn(conn);
}

static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)event_data;
    tcp_client_handle_t *me = (tcp_client_handle_t *)handler_arg;
    if (!me || event_base != LWLTE_EVENT) {
        return;
    }
    if ((lwlte_event_id_t)event_id == LWLTE_EVENT_NET_OFFLINE) {
        tcp_client_conn_t *conn = acquire_current_conn(me);
        if (!conn) {
            return;
        }
        tcp_fsm_sig_t sig = {
            .type = TCP_SIG_NET_OFFLINE,
            .conn_scoped = true,
            .conn_generation = conn->generation,
        };
        (void)send_fsm_sig_wait(me, &sig, portMAX_DELAY);
        release_conn(conn);
    }
}

static void tcp_fsm_task(void *arg)
{
    tcp_client_handle_t *me = (tcp_client_handle_t *)arg;

    while (!tcp_fsm_should_stop(me)) {
        tcp_fsm_sig_t sig = {0};
        if (xQueueReceive(me->fsm_queue, &sig,
                          pdMS_TO_TICKS(TCP_CLIENT_FSM_WAIT_MS)) != pdTRUE) {
            tcp_client_conn_t *conn = acquire_current_conn(me);
            handle_deferred_work(me, conn);
            if (conn) {
                release_conn(conn);
            }
            continue;
        }
        if (tcp_fsm_should_stop(me)) {
            free_fsm_sig_payload(&sig);
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

static bool tcp_fsm_should_stop(tcp_client_handle_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool stop = me->destroying;
    xSemaphoreGive(me->lock);

    return stop;
}

static void handle_signal(tcp_client_handle_t *me, tcp_fsm_sig_t *sig)
{
    if (!me || !sig) {
        return;
    }

    tcp_client_conn_t *conn = NULL;
    if (!signal_matches_current_conn(me, sig, &conn)) {
        free_fsm_sig_payload(sig);
        return;
    }
    switch (sig->type) {
    case TCP_SIG_OPEN:
        handle_open(me, conn, (const tcp_open_owned_t *)sig->data);
        break;
    case TCP_SIG_SEND_READY:
        handle_send_ready(me, conn);
        break;
    case TCP_SIG_CLOSE:
        handle_close(me, conn);
        break;
    case TCP_SIG_CORE_CMD_DONE:
        handle_core_cmd_done(me, conn, sig);
        break;
    case TCP_SIG_PROTOCOL_DATA:
        handle_protocol_data(me, conn, sig);
        break;
    case TCP_SIG_PROTOCOL_CLOSED:
    case TCP_SIG_NET_OFFLINE:
        handle_protocol_closed(me, conn, sig);
        break;
    default:
        break;
    }
    if (conn) {
        release_conn(conn);
    }
    free_fsm_sig_payload(sig);
}

static void handle_open(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                        const tcp_open_owned_t *open)
{
    if (!conn || !open || !open->host) {
        return;
    }
    if (submit_socket_open(conn, open->host, open->port) != ESP_OK) {
        if (conn_terminal_or_destroyed(conn)) {
            handle_remote_closed_if_latched(me, conn);
            return;
        }
        set_conn_state(conn, TCP_CONN_STATE_ERROR);
        post_error_event(conn, ESP_FAIL, 0, 0);
    }
}

static void handle_send_ready(tcp_client_handle_t *me, tcp_client_conn_t *conn)
{
    if (!conn) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool close_requested = conn->close_requested;
    tcp_conn_state_t state = conn->state;
    xSemaphoreGive(conn->lock);
    if (close_requested) {
        handle_close(me, conn);
        return;
    }
    if (state != TCP_CONN_STATE_CONNECTED) {
        return;
    }
    if (conn_pending_command(conn)) {
        return;
    }

    tcp_send_item_t item = {0};
    xSemaphoreTake(conn->send_queue_lock, portMAX_DELAY);
    if (xQueueReceive(conn->send_queue, &item, 0) != pdTRUE) {
        xSemaphoreGive(conn->send_queue_lock);
        return;
    }
    xSemaphoreGive(conn->send_queue_lock);
    esp_err_t ret = submit_socket_send(conn, &item);
    free(item.data);
    if (ret != ESP_OK) {
        if (conn_terminal_or_destroyed(conn)) {
            handle_remote_closed_if_latched(me, conn);
            return;
        }
        set_conn_state(conn, TCP_CONN_STATE_ERROR);
        clear_send_queue(conn);
        post_error_event(conn, ret, 0, 0);
    }
}

static void handle_close(tcp_client_handle_t *me, tcp_client_conn_t *conn)
{
    if (!conn) {
        return;
    }

    if (conn_pending_command(conn)) {
        return;
    }
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool close_requested = conn->close_requested;
    tcp_conn_state_t state = conn->state;
    bool destroyed = conn->destroyed;
    xSemaphoreGive(conn->lock);
    if (!close_requested || destroyed || state == TCP_CONN_STATE_CLOSED) {
        return;
    }
    if (submit_socket_close(conn) != ESP_OK) {
        if (conn_terminal_or_destroyed(conn)) {
            handle_remote_closed_if_latched(me, conn);
            return;
        }
        clear_send_queue(conn);
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->close_requested = false;
        xSemaphoreGive(conn->lock);
        set_conn_state(conn, TCP_CONN_STATE_ERROR);
        post_error_event(conn, ESP_FAIL, 0, 0);
    }
}

static void handle_core_cmd_done(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                                 tcp_fsm_sig_t *sig)
{
    (void)me;
    if (!conn || !sig) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (!conn->pending_cmd.active || sig->core_cmd_type != conn->pending_cmd.type) {
        xSemaphoreGive(conn->lock);
        return;
    }
    size_t send_len = conn->pending_cmd.send_len;
    conn->pending_cmd.active = false;
    conn->pending_cmd.type = 0;
    conn->pending_cmd.send_len = 0;
    xSemaphoreGive(conn->lock);

    tcp_conn_state_t current_state = get_conn_state_value(conn);
    if (current_state == TCP_CONN_STATE_CLOSED) {
        handle_remote_closed_if_latched(me, conn);
        return;
    }

    if (sig->core_result != CORE_CMD_RESULT_OK) {
        esp_err_t error_code = sig->error_code ?
                               sig->error_code :
                               esp_err_from_core_result(sig->core_result);
        if (sig->core_cmd_type == CORE_CMD_SOCKET_SEND ||
            sig->core_cmd_type == CORE_CMD_SOCKET_CLOSE) {
            clear_send_queue(conn);
        }
        if (sig->core_cmd_type == CORE_CMD_SOCKET_CLOSE) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->close_requested = false;
            xSemaphoreGive(conn->lock);
        }
        set_conn_state(conn, TCP_CONN_STATE_ERROR);
        post_error_event(conn, error_code, sig->modem_error_code, 0);
        return;
    }

    switch (sig->core_cmd_type) {
    case CORE_CMD_SOCKET_OPEN: {
        set_conn_state(conn, TCP_CONN_STATE_CONNECTED);
        lwlte_tcp_event_data_t payload = {
            .conn = (lwlte_tcp_conn_t *)conn,
            .user_ctx = conn->user_ctx,
            .conn_state = LWLTE_TCP_CONN_STATE_CONNECTED,
        };
        (void)post_tcp_event(conn, LWLTE_TCP_EVENT_CONNECTED, &payload);
        handle_deferred_work(me, conn);
        break;
    }
    case CORE_CMD_SOCKET_SEND: {
        lwlte_tcp_event_data_t payload = {
            .conn = (lwlte_tcp_conn_t *)conn,
            .user_ctx = conn->user_ctx,
            .conn_state = map_conn_state(get_conn_state_value(conn)),
            .sent_len = send_len,
        };
        (void)post_tcp_event(conn, LWLTE_TCP_EVENT_SENT, &payload);
        handle_deferred_work(me, conn);
        break;
    }
    case CORE_CMD_SOCKET_RECV: {
        core_socket_recv_result_t *recv = (core_socket_recv_result_t *)sig->result_data;
        if (!recv) {
            set_conn_state(conn, TCP_CONN_STATE_ERROR);
            post_error_event(conn, esp_err_from_core_result(CORE_CMD_RESULT_ERROR),
                             0, 0);
            break;
        }
        lwlte_tcp_event_data_t payload = {
            .conn = (lwlte_tcp_conn_t *)conn,
            .user_ctx = conn->user_ctx,
            .conn_state = map_conn_state(get_conn_state_value(conn)),
            .modem_error_code = recv->modem_error_code,
            .payload = recv->payload,
            .payload_len = recv->payload_len,
            .owns_payload = recv->payload != NULL,
        };
        if (recv->payload_len > 0) {
            if (post_tcp_event(conn, LWLTE_TCP_EVENT_DATA, &payload) == ESP_OK ||
                payload.owns_payload) {
                recv->payload = NULL;
                recv->payload_len = 0;
            }
        }
        if (recv->remaining_len > 0 &&
            get_conn_state_value(conn) == TCP_CONN_STATE_CONNECTED) {
            esp_err_t ret = submit_socket_recv(conn);
            if (ret != ESP_OK) {
                if (conn_terminal_or_destroyed(conn)) {
                    handle_remote_closed_if_latched(me, conn);
                    break;
                }
                set_conn_state(conn, TCP_CONN_STATE_ERROR);
                post_error_event(conn, ret, recv->modem_error_code, 0);
            }
        } else {
            handle_deferred_work(me, conn);
        }
        break;
    }
    case CORE_CMD_SOCKET_CLOSE: {
        clear_send_queue(conn);
        bool post_disconnected = false;
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->close_requested = false;
        conn->recv_requested = false;
        conn->state = TCP_CONN_STATE_CLOSED;
        if (!conn->remote_closed_event_posted) {
            mark_terminal_event_pending(conn, LWLTE_TCP_EVENT_DISCONNECTED,
                                        ESP_OK, 0, 0);
            post_disconnected = true;
        }
        xSemaphoreGive(conn->lock);
        if (post_disconnected) {
            (void)post_pending_terminal_event(conn);
        }
        break;
    }
    default:
        break;
    }
}

static void handle_protocol_data(tcp_client_handle_t *me, tcp_client_conn_t *conn,
                                 tcp_fsm_sig_t *sig)
{
    tcp_protocol_data_owned_t *owned = sig ? (tcp_protocol_data_owned_t *)sig->data : NULL;
    if (!conn || !owned || owned->conn_id != conn->conn_id ||
        get_conn_state_value(conn) != TCP_CONN_STATE_CONNECTED) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    conn->recv_requested = true;
    conn->recv_reason = owned->reason;
    conn->recv_modem_error_code = owned->modem_error_code;
    bool pending = conn->pending_cmd.active;
    xSemaphoreGive(conn->lock);
    if (pending) {
        return;
    }
    handle_deferred_work(me, conn);
}

static void handle_protocol_closed(tcp_client_handle_t *me,
                                   tcp_client_conn_t *conn,
                                   const tcp_fsm_sig_t *sig)
{
    if (!conn) {
        return;
    }
    latch_remote_closed(conn, sig->error_code, sig->modem_error_code);
    handle_remote_closed_if_latched(me, conn);
}

static void handle_deferred_work(tcp_client_handle_t *me, tcp_client_conn_t *conn)
{
    if (!me || !conn) {
        return;
    }

    handle_remote_closed_if_latched(me, conn);
    if (conn_terminal_event_pending(conn)) {
        (void)post_pending_terminal_event(conn);
        return;
    }
    if (conn_pending_command(conn)) {
        return;
    }

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    bool close_requested = conn->close_requested;
    bool recv_requested = conn->recv_requested;
    int recv_reason = conn->recv_reason;
    int recv_modem_error_code = conn->recv_modem_error_code;
    tcp_conn_state_t state = conn->state;
    xSemaphoreGive(conn->lock);

    if (close_requested && (state == TCP_CONN_STATE_CONNECTED ||
                            state == TCP_CONN_STATE_CLOSING ||
                            state == TCP_CONN_STATE_ERROR)) {
        handle_close(me, conn);
        return;
    }
    if (state != TCP_CONN_STATE_CONNECTED) {
        return;
    }
    if (recv_requested) {
        esp_err_t ret = submit_socket_recv(conn);
        if (ret == ESP_OK) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->recv_requested = false;
            xSemaphoreGive(conn->lock);
        } else {
            if (conn_terminal_or_destroyed(conn)) {
                handle_remote_closed_if_latched(me, conn);
                return;
            }
            set_conn_state(conn, TCP_CONN_STATE_ERROR);
            post_error_event(conn, ret, recv_modem_error_code, recv_reason);
        }
        return;
    }

    handle_send_ready(me, conn);
}

static esp_err_t submit_socket_open(tcp_client_conn_t *conn, const char *host,
                                    uint16_t port)
{
    ESP_RETURN_ON_FALSE(conn && conn->client && host, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket open args");

    tcp_client_handle_t *me = conn->client;
    core_cmd_t cmd = {
        .type = CORE_CMD_SOCKET_OPEN,
        .done_cb = tcp_core_cmd_done_cb,
        .user_ctx = conn,
        .timeout_ms = me->config.open_timeout_ms,
        .data.socket_open = {
            .proto = CORE_SOCKET_PROTO_TCP,
            .conn_id = conn->conn_id,
            .host = host,
            .port = port,
            .timeout_ms = me->config.open_timeout_ms,
        },
    };

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (!conn_can_submit(conn, CORE_CMD_SOCKET_OPEN)) {
        xSemaphoreGive(conn->lock);
        return ESP_ERR_INVALID_STATE;
    }

    conn->active_refs++;
    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret == ESP_OK) {
        conn->pending_cmd.active = true;
        conn->pending_cmd.type = CORE_CMD_SOCKET_OPEN;
        conn->pending_cmd.send_len = 0;
    } else if (conn->active_refs > 0) {
        conn->active_refs--;
    }
    xSemaphoreGive(conn->lock);
    return ret;
}

static esp_err_t submit_socket_send(tcp_client_conn_t *conn,
                                    const tcp_send_item_t *item)
{
    ESP_RETURN_ON_FALSE(conn && conn->client && item && item->data && item->len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket send args");

    tcp_client_handle_t *me = conn->client;
    core_cmd_t cmd = {
        .type = CORE_CMD_SOCKET_SEND,
        .done_cb = tcp_core_cmd_done_cb,
        .user_ctx = conn,
        .timeout_ms = me->config.send_timeout_ms,
        .data.socket_send = {
            .conn_id = conn->conn_id,
            .data = item->data,
            .len = item->len,
            .timeout_ms = me->config.send_timeout_ms,
        },
    };

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (!conn_can_submit(conn, CORE_CMD_SOCKET_SEND)) {
        xSemaphoreGive(conn->lock);
        return ESP_ERR_INVALID_STATE;
    }

    conn->active_refs++;
    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret == ESP_OK) {
        conn->pending_cmd.active = true;
        conn->pending_cmd.type = CORE_CMD_SOCKET_SEND;
        conn->pending_cmd.send_len = item->len;
    } else if (conn->active_refs > 0) {
        conn->active_refs--;
    }
    xSemaphoreGive(conn->lock);
    return ret;
}

static esp_err_t submit_socket_recv(tcp_client_conn_t *conn)
{
    ESP_RETURN_ON_FALSE(conn && conn->client, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket recv args");

    tcp_client_handle_t *me = conn->client;
    core_cmd_t cmd = {
        .type = CORE_CMD_SOCKET_RECV,
        .done_cb = tcp_core_cmd_done_cb,
        .user_ctx = conn,
        .timeout_ms = me->config.send_timeout_ms,
        .data.socket_recv = {
            .conn_id = conn->conn_id,
            .max_len = me->config.max_rx_event_len,
        },
    };

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (!conn_can_submit(conn, CORE_CMD_SOCKET_RECV)) {
        xSemaphoreGive(conn->lock);
        return ESP_ERR_INVALID_STATE;
    }

    conn->active_refs++;
    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret == ESP_OK) {
        conn->pending_cmd.active = true;
        conn->pending_cmd.type = CORE_CMD_SOCKET_RECV;
        conn->pending_cmd.send_len = 0;
    } else if (conn->active_refs > 0) {
        conn->active_refs--;
    }
    xSemaphoreGive(conn->lock);
    return ret;
}

static esp_err_t submit_socket_close(tcp_client_conn_t *conn)
{
    ESP_RETURN_ON_FALSE(conn && conn->client, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket close args");

    tcp_client_handle_t *me = conn->client;
    core_cmd_t cmd = {
        .type = CORE_CMD_SOCKET_CLOSE,
        .done_cb = tcp_core_cmd_done_cb,
        .user_ctx = conn,
        .timeout_ms = me->config.close_timeout_ms,
        .data.socket_close = {
            .conn_id = conn->conn_id,
            .timeout_ms = me->config.close_timeout_ms,
        },
    };

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (!conn_can_submit(conn, CORE_CMD_SOCKET_CLOSE)) {
        xSemaphoreGive(conn->lock);
        return ESP_ERR_INVALID_STATE;
    }

    conn->active_refs++;
    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret == ESP_OK) {
        conn->pending_cmd.active = true;
        conn->pending_cmd.type = CORE_CMD_SOCKET_CLOSE;
        conn->pending_cmd.send_len = 0;
    } else if (conn->active_refs > 0) {
        conn->active_refs--;
    }
    xSemaphoreGive(conn->lock);
    return ret;
}

static esp_err_t post_tcp_event(tcp_client_conn_t *conn,
                                lwlte_tcp_event_id_t event_id,
                                const lwlte_tcp_event_data_t *payload)
{
    return post_tcp_event_with_ref(conn, event_id, payload, false, true);
}

static esp_err_t post_tcp_event_with_ref(tcp_client_conn_t *conn,
                                         lwlte_tcp_event_id_t event_id,
                                         const lwlte_tcp_event_data_t *payload,
                                         bool ref_acquired,
                                         bool release_on_failure)
{
    ESP_RETURN_ON_FALSE(conn && conn->client, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    lwlte_tcp_event_data_t empty_payload = {0};
    if (!payload) {
        empty_payload.conn = (lwlte_tcp_conn_t *)conn;
        empty_payload.user_ctx = conn->user_ctx;
        empty_payload.conn_state = map_conn_state(get_conn_state_value(conn));
        payload = &empty_payload;
    }

    if (!ref_acquired && !acquire_conn(conn)) {
        if (payload->owns_payload) {
            free((void *)payload->payload);
        }
        return ESP_ERR_INVALID_STATE;
    }

    lwlte_tcp_event_data_t event_payload = *payload;
    event_payload.owns_event = true;

    tcp_client_handle_t *me = conn->client;
    esp_err_t ret;
    if (me->config.loop) {
        ret = esp_event_post_to(me->config.loop, LWLTE_TCP_EVENT, event_id,
                                &event_payload,
                                sizeof(lwlte_tcp_event_data_t), 0);
    } else {
        ret = esp_event_post(LWLTE_TCP_EVENT, event_id, &event_payload,
                             sizeof(lwlte_tcp_event_data_t), 0);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post tcp event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
        if (payload->owns_payload) {
            free((void *)payload->payload);
        }
        if (release_on_failure) {
            release_conn(conn);
        }
    }

    return ret;
}

static void mark_terminal_event_pending(tcp_client_conn_t *conn,
                                        lwlte_tcp_event_id_t event_id,
                                        esp_err_t error_code,
                                        int modem_error_code,
                                        int reason)
{
    if (!conn || !conn->lock) {
        return;
    }
    if (conn->terminal_event_posted) {
        return;
    }

    conn->terminal_event_pending = true;
    conn->terminal_event_posted = false;
    conn->terminal_event_id = event_id;
    conn->terminal_error_code = error_code;
    conn->terminal_modem_error_code = modem_error_code;
    conn->terminal_reason = reason;
}

static esp_err_t post_pending_terminal_event(tcp_client_conn_t *conn)
{
    ESP_RETURN_ON_FALSE(conn && conn->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(conn->lock, portMAX_DELAY);
    if (conn->destroyed || !conn->terminal_event_pending) {
        xSemaphoreGive(conn->lock);
        return ESP_ERR_INVALID_STATE;
    }
    conn->active_refs++;

    lwlte_tcp_event_id_t event_id = (lwlte_tcp_event_id_t)conn->terminal_event_id;
    lwlte_tcp_event_data_t payload = {
        .conn = (lwlte_tcp_conn_t *)conn,
        .user_ctx = conn->user_ctx,
        .conn_state = map_conn_state(conn->state),
        .error_code = conn->terminal_error_code,
        .modem_error_code = conn->terminal_modem_error_code,
        .reason = conn->terminal_reason,
    };
    conn->terminal_event_posted = true;
    conn->terminal_event_pending = false;
    if (conn->remote_closed && event_id == LWLTE_TCP_EVENT_DISCONNECTED) {
        conn->remote_closed_event_posted = true;
        conn->remote_closed_event_pending = false;
    }

    esp_err_t ret = post_tcp_event_with_ref(conn, event_id, &payload, true, false);

    if (ret != ESP_OK) {
        conn->terminal_event_posted = false;
        conn->terminal_event_pending = true;
        if (conn->remote_closed && event_id == LWLTE_TCP_EVENT_DISCONNECTED) {
            conn->remote_closed_event_posted = false;
            conn->remote_closed_event_pending = true;
        }
        xSemaphoreGive(conn->lock);
        release_conn(conn);
        return ret;
    }

    xSemaphoreGive(conn->lock);

    return ret;
}

static void post_error_event(tcp_client_conn_t *conn, esp_err_t error_code,
                             int modem_error_code, int reason)
{
    if (!conn) {
        return;
    }
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    mark_terminal_event_pending(conn, LWLTE_TCP_EVENT_ERROR, error_code,
                                modem_error_code, reason);
    xSemaphoreGive(conn->lock);
    (void)post_pending_terminal_event(conn);
}

static void clear_send_queue(tcp_client_conn_t *conn)
{
    if (!conn || !conn->send_queue || !conn->send_queue_lock) {
        return;
    }

    tcp_send_item_t item = {0};
    xSemaphoreTake(conn->send_queue_lock, portMAX_DELAY);
    while (xQueueReceive(conn->send_queue, &item, 0) == pdTRUE) {
        free(item.data);
    }
    xSemaphoreGive(conn->send_queue_lock);
}

static void free_fsm_sig_payload(tcp_fsm_sig_t *sig)
{
    if (!sig) {
        return;
    }

    switch (sig->type) {
    case TCP_SIG_OPEN: {
        tcp_open_owned_t *open = (tcp_open_owned_t *)sig->data;
        if (open) {
            free(open->host);
            free(open);
        }
        break;
    }
    case TCP_SIG_PROTOCOL_DATA: {
        tcp_protocol_data_owned_t *owned = (tcp_protocol_data_owned_t *)sig->data;
        if (owned) {
            free(owned->payload);
            free(owned);
        }
        break;
    }
    default:
        break;
    }
    if (sig->result_data) {
        core_socket_recv_result_t *recv = (core_socket_recv_result_t *)sig->result_data;
        free(recv->payload);
        free(recv);
    }
    sig->data = NULL;
    sig->data_size = 0;
    sig->result_data = NULL;
    sig->result_size = 0;
}

static void drain_fsm_queue_payloads(tcp_client_handle_t *me, QueueHandle_t queue)
{
    (void)me;
    if (!queue) {
        return;
    }

    tcp_fsm_sig_t sig = {0};
    while (xQueueReceive(queue, &sig, 0) == pdTRUE) {
        free_fsm_sig_payload(&sig);
    }
}

static void cleanup_partial_client(tcp_client_handle_t *me)
{
    if (!me) {
        return;
    }
    me->destroying = true;

    if (me->config.loop) {
        (void)esp_event_handler_unregister_with(me->config.loop, LWLTE_EVENT,
                                                LWLTE_EVENT_NET_OFFLINE,
                                                handle_lwlte_event);
    } else {
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_NET_OFFLINE,
                                           handle_lwlte_event);
    }
    if (me->core) {
        (void)core_register_protocol_callback(me->core, CORE_PROTOCOL_TCP,
                                              NULL, NULL);
        (void)core_register_protocol_closed_callback(me->core,
                                                     CORE_PROTOCOL_TCP,
                                                     NULL, NULL);
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
    if (me->conn) {
        cleanup_conn(me->conn);
        me->conn = NULL;
    }
    if (me->fsm_task_done_sema) {
        vSemaphoreDelete(me->fsm_task_done_sema);
        me->fsm_task_done_sema = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    free(me);
}

static void cleanup_conn(tcp_client_conn_t *conn)
{
    if (!conn) {
        return;
    }

    if (conn->lock) {
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->destroyed = true;
        xSemaphoreGive(conn->lock);
    }
    clear_send_queue(conn);
    if (conn->send_queue) {
        vQueueDelete(conn->send_queue);
        conn->send_queue = NULL;
    }
    if (conn->send_queue_lock) {
        vSemaphoreDelete(conn->send_queue_lock);
        conn->send_queue_lock = NULL;
    }
    if (conn->active_done_sema) {
        vSemaphoreDelete(conn->active_done_sema);
        conn->active_done_sema = NULL;
    }
    if (conn->lock) {
        vSemaphoreDelete(conn->lock);
        conn->lock = NULL;
    }
    free(conn);
}
