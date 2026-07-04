/**
 * @file lwlte.c
 * @brief LTE 用户门面通用实现
 * @details LTE user facade common implementation
 * @author JovisDreams
 * @date 2026-05-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "lwlte_priv.h"

#include <string.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "lwlte"
#define LWLTE_READY_SEMA_MAX_COUNT  32767U

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    SemaphoreHandle_t done;
    core_cmd_result_t result;
    esp_err_t error_code;
} lwlte_sync_cmd_ctx_t;

typedef struct {
    SemaphoreHandle_t done;
    core_cmd_result_t result;
    esp_err_t error_code;
    int status_code;
    uint8_t *body;
    size_t body_len;
    int modem_error_code;
} lwlte_http_cmd_ctx_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 映射 Core 状态
 * @details Map Core state
 * @param[in] state Core 状态
 * @return LTE 门面状态
 */
static lwlte_state_t map_core_state(core_state_t state);

/**
 * @brief 映射 Core 网络状态
 * @details Map Core network state
 * @param[in] state Core 网络状态
 * @return LTE 网络状态
 */
static lwlte_net_state_t map_core_net_state(core_net_state_t state);

/**
 * @brief 映射 MQTT 状态
 * @details Map MQTT state
 * @param[in] state MQTT 状态
 * @return LTE MQTT 状态
 */
static lwlte_mqtt_state_t map_mqtt_state(mqtt_client_state_t state);

/**
 * @brief 映射 TCP 连接状态
 * @details Map TCP connection state
 * @param[in] state TCP 连接状态
 * @return LTE TCP 连接状态
 */
static lwlte_tcp_conn_state_t map_tcp_conn_state(tcp_conn_state_t state);

/**
 * @brief 处理同步 Core 命令完成回调
 * @details Handle synchronous Core command done callback
 * @param[in] core Core 句柄
 * @param[in] type Core 命令类型
 * @param[in] result Core 命令结果
 * @param[in] result_data 结果数据
 * @param[in] user_ctx 用户上下文
 */
static void facade_core_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx);

/**
 * @brief 处理同步 HTTP 命令完成回调
 * @details Handle synchronous HTTP command done callback
 * @param[in] core Core 句柄
 * @param[in] type Core 命令类型
 * @param[in] result Core 命令结果
 * @param[in] result_data 结果数据
 * @param[in] user_ctx 用户上下文
 */
static void facade_http_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx);

/**
 * @brief 进入门面 API 调用
 * @details Begin facade API call
 * @param[in] me LTE 用户门面句柄
 * @param[in] require_core 是否要求 Core 已创建
 * @param[out] out_core Core 句柄输出指针，可为 NULL
 * @return
 *         - ESP_OK: 可以操作
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态无效
 */
static esp_err_t begin_api_call(lwlte_handle_t me, bool require_core,
                                core_handle_t *out_core);

/**
 * @brief 进入 MQTT 门面 API 调用
 * @details Begin MQTT facade API call
 * @param[in] me LTE 用户门面句柄
 * @param[out] out_mqtt MQTT 客户端输出指针
 * @return esp_err_t
 */
static esp_err_t begin_mqtt_api_call(lwlte_handle_t me, mqtt_client_handle_t *out_mqtt);

/**
 * @brief 退出门面 API 调用
 * @details End facade API call
 * @param[in] me LTE 用户门面句柄
 */
static void end_api_call(lwlte_handle_t me);

/**
 * @brief 检查整数是否非负
 * @details Check whether integer is non-negative
 * @param[in] value 整数值
 * @return
 *         - true: 非负
 *         - false: 负数
 */
static bool non_negative_int(int value);

/**
 * @brief 等待 API 调用空闲
 * @details Wait until API calls are idle
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: API 调用已空闲
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 内部状态无效
 */
static esp_err_t wait_api_calls_idle(lwlte_handle_t me);

/**
 * @brief 唤醒所有 ready 等待者
 * @details Wake all ready waiters
 * @note 调用方必须持有 me->lock。
 * @param[in] me LTE 用户门面句柄
 */
static void wake_ready_waiters_locked(lwlte_handle_t me);

/**
 * @brief 恢复销毁失败后的门面状态
 * @details Restore facade state after destroy failure
 * @param[in] me LTE 用户门面句柄
 */
static void restore_after_destroy_failure(lwlte_handle_t me);

/**
 * @brief 销毁门面持有的资源
 * @details Destroy resources owned by facade
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 成功
 *         - other: 下层销毁错误
 */
static esp_err_t destroy_owned_resources(lwlte_handle_t me);

/**********************
 *  STATIC VARIABLES
 **********************/
ESP_EVENT_DEFINE_BASE(LWLTE_TCP_EVENT);
ESP_EVENT_DEFINE_BASE(LWLTE_MQTT_EVENT);

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void lwlte_mqtt_event_data_release(lwlte_mqtt_event_data_t *data)
{
    if (!data || !data->owns_payload) {
        return;
    }
    free((void *)data->msg.topic);
    free((void *)data->msg.payload);
    data->msg.topic = NULL;
    data->msg.payload = NULL;
    data->msg.topic_len = 0;
    data->msg.payload_len = 0;
    data->owns_payload = false;
}

void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data)
{
    if (!data) {
        return;
    }
    if (data->owns_payload) {
        free((void *)data->payload);
        data->payload = NULL;
        data->payload_len = 0;
        data->owns_payload = false;
    }
    if (data->owns_event) {
        tcp_client_conn_release_event((tcp_client_conn_t)data->conn);
        data->owns_event = false;
    }
}

void lwlte_http_response_release(lwlte_http_response_t *response)
{
    if (!response) {
        return;
    }
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
}

esp_err_t lwlte_create_empty(lwlte_handle_t *out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");

    *out_lte = NULL;
    lwlte_handle_t me = calloc(1, sizeof(*me));
    ESP_RETURN_ON_FALSE(me, ESP_ERR_NO_MEM, TAG, "calloc lwlte failed");

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        free(me);
        return ESP_ERR_NO_MEM;
    }

    me->ready_sema = xSemaphoreCreateCounting(LWLTE_READY_SEMA_MAX_COUNT, 0);
    if (!me->ready_sema) {
        vSemaphoreDelete(me->lock);
        free(me);
        return ESP_ERR_NO_MEM;
    }

    me->api_done_sema = xSemaphoreCreateBinary();
    if (!me->api_done_sema) {
        vSemaphoreDelete(me->ready_sema);
        vSemaphoreDelete(me->lock);
        free(me);
        return ESP_ERR_NO_MEM;
    }

    *out_lte = me;
    return ESP_OK;
}

esp_err_t lwlte_destroy(lwlte_handle_t me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    wake_ready_waiters_locked(me);
    xSemaphoreGive(me->lock);

    esp_err_t ret = wait_api_calls_idle(me);
    if (ret != ESP_OK) {
        restore_after_destroy_failure(me);
        return ret;
    }

    ret = destroy_owned_resources(me);
    if (ret != ESP_OK) {
        restore_after_destroy_failure(me);
        return ret;
    }

    /* Unregister internal bus handler (after resources destroyed) */
    if (me->event_loop) {
        (void)esp_event_handler_unregister_with(me->event_loop, LWLTE_EVENT,
                                                LWLTE_EVENT_READY,
                                                facade_ready_handler);
        (void)esp_event_handler_unregister_with(me->event_loop, LWLTE_EVENT,
                                                LWLTE_EVENT_ERROR,
                                                facade_ready_handler);
    } else {
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_READY,
                                           facade_ready_handler);
        (void)esp_event_handler_unregister(LWLTE_EVENT,
                                           LWLTE_EVENT_ERROR,
                                           facade_ready_handler);
    }

    /* Ensure any in-flight handler invocation has completed before freeing.
     * After unregister, no new dispatch can start. Acquiring the lock guarantees
     * the last in-flight handler has released it and is about to return. */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    xSemaphoreGive(me->lock);

    if (me->api_done_sema) {
        vSemaphoreDelete(me->api_done_sema);
        me->api_done_sema = NULL;
    }
    if (me->ready_sema) {
        vSemaphoreDelete(me->ready_sema);
        me->ready_sema = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    free(me);

    return ESP_OK;
}

esp_err_t lwlte_start(lwlte_handle_t me)
{
    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    ret = core_start(core);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_stop(lwlte_handle_t me)
{
    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    esp_err_t mqtt_ret = ESP_OK;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t mqtt = me->mqtt;
    if (mqtt) {
        mqtt_ret = mqtt_client_stop(mqtt);
        if (mqtt_ret != ESP_OK) {
            ESP_LOGW(TAG, "stop MQTT during lwlte_stop failed: %s",
                     esp_err_to_name(mqtt_ret));
        }
    }
    xSemaphoreGive(me->lock);

    ret = core_stop(core);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_get_state(lwlte_handle_t me, lwlte_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    core_state_t core_state = CORE_STATE_STOPPED;
    ret = core_get_state(core, &core_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get core state failed: %s", esp_err_to_name(ret));
        end_api_call(me);
        return ret;
    }
    *state = map_core_state(core_state);

    end_api_call(me);

    return ESP_OK;
}

esp_err_t lwlte_get_net_state(lwlte_handle_t me, lwlte_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    core_net_state_t core_state = CORE_NET_STATE_OFFLINE;
    ret = core_get_net_state(core, &core_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get core net state failed: %s", esp_err_to_name(ret));
        end_api_call(me);
        return ret;
    }
    *state = map_core_net_state(core_state);

    end_api_call(me);

    return ESP_OK;
}

esp_err_t lwlte_ping(lwlte_handle_t me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(request && replies && request->host && request->host[0] &&
                        request->count >= 1 && request->count <= 100 &&
                        request->data_len <= 1024 &&
                        request->timeout_100ms >= 1 &&
                        request->timeout_100ms <= 600 &&
                        request->ttl >= 1 &&
                        max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping request");

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    ping_client_handle_t ping = me->ping;
    xSemaphoreGive(me->lock);
    if (!ping) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    core_ping_reply_t *core_replies = calloc(request->count,
                                             sizeof(core_ping_reply_t));
    if (!core_replies) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_ping_summary_t core_summary = {0};
    ping_client_request_t ping_request = {
        .host = request->host,
        .count = request->count,
        .data_len = request->data_len,
        .timeout_100ms = request->timeout_100ms,
        .ttl = request->ttl,
        .total_timeout_ms = request->total_timeout_ms,
    };

    ret = ping_client_ping(ping, &ping_request, core_replies, request->count,
                           summary ? &core_summary : NULL);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < request->count; i++) {
            replies[i].seq = core_replies[i].seq;
            strlcpy(replies[i].ip, core_replies[i].ip, sizeof(replies[i].ip));
            replies[i].time_ms = core_replies[i].time_ms;
            replies[i].ttl = core_replies[i].ttl;
            replies[i].success = core_replies[i].success;
        }
        if (summary) {
            summary->sent = core_summary.sent;
            summary->received = core_summary.received;
            summary->lost = core_summary.lost;
            summary->min_time_ms = core_summary.min_time_ms;
            summary->max_time_ms = core_summary.max_time_ms;
            summary->avg_time_ms = core_summary.avg_time_ms;
        }
    }

    free(core_replies);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_ssl_provision(lwlte_handle_t me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(config && credentials, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_sync_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_cmd_t cmd = {
        .type = CORE_CMD_SSL_PROVISION,
        .done_cb = facade_core_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = 60000,
        .data.ssl_provision = {
            .config = {
                .context_id = config->context_id,
                .auth_mode = config->auth_mode,
                .tls_version = config->tls_version,
                .negotiate_timeout_s = config->negotiate_timeout_s,
                .ignore_cert_time = config->ignore_cert_time,
                .hostname = config->hostname,
            },
            .credentials = {
                .ca_cert_pem = credentials->ca_cert_pem,
                .ca_cert_len = credentials->ca_cert_len,
                .client_cert_pem = credentials->client_cert_pem,
                .client_cert_len = credentials->client_cert_len,
                .client_key_pem = credentials->client_key_pem,
                .client_key_len = credentials->client_key_len,
            },
        },
    };

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
    }

    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_sync_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_ssl_context_status_t core_status = {0};
    core_cmd_t cmd = {
        .type = CORE_CMD_SSL_GET_CONTEXT_STATUS,
        .done_cb = facade_core_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = 30000,
        .data.ssl_get_context_status = {
            .context_id = context_id,
            .status = &core_status,
        },
    };

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
    }

    if (ret == ESP_OK) {
        status->provisioned = core_status.provisioned;
        status->ca_cert_present = core_status.ca_cert_present;
        status->client_cert_present = core_status.client_cert_present;
        status->client_key_present = core_status.client_key_present;
        status->check_valid = core_status.check_valid;
        status->auth_mode = core_status.auth_mode;
    }

    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_http_request(lwlte_handle_t me,
                             const lwlte_http_request_t *request,
                             lwlte_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(request && response, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(request->url && request->url[0], ESP_ERR_INVALID_ARG,
                        TAG, "HTTP url is required");
    ESP_RETURN_ON_FALSE(request->method == LWLTE_HTTP_METHOD_GET ||
                        request->method == LWLTE_HTTP_METHOD_POST,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HTTP method");
    ESP_RETURN_ON_FALSE(request->transport == LWLTE_HTTP_TRANSPORT_HTTP ||
                        request->transport == LWLTE_HTTP_TRANSPORT_HTTPS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HTTP transport");
    ESP_RETURN_ON_FALSE(request->method != LWLTE_HTTP_METHOD_POST ||
                        (request->body && request->body_len > 0),
                        ESP_ERR_INVALID_ARG, TAG,
                        "POST requires non-empty body");

    /* Zero-init response so release() is safe on any return path */
    response->status_code = 0;
    response->body = NULL;
    response->body_len = 0;
    response->modem_error_code = 0;

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_http_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    uint32_t timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 120000;
    core_cmd_t cmd = {
        .type = CORE_CMD_HTTP_REQUEST,
        .done_cb = facade_http_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = timeout_ms,
        .data.http_request = {
            .method = request->method,
            .url = request->url,
            .transport = request->transport,
            .ssl_context_id = request->ssl_context_id,
            .content_type = request->content_type,
            .body = request->body,
            .body_len = request->body_len,
        },
    };

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
        response->status_code = ctx.status_code;
        response->body = ctx.body;
        response->body_len = ctx.body_len;
        response->modem_error_code = ctx.modem_error_code;
    }

    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_mqtt_init(lwlte_handle_t me, const lwlte_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(config->host && config->host[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT host is required");
    ESP_RETURN_ON_FALSE(config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "MQTT port is required");
    ESP_RETURN_ON_FALSE(config->client_id && config->client_id[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT client_id is required");
    ESP_RETURN_ON_FALSE((int)config->transport == LWLTE_MQTT_TRANSPORT_PLAIN_TCP ||
                        (int)config->transport == LWLTE_MQTT_TRANSPORT_TLS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT transport");
    ESP_RETURN_ON_FALSE(non_negative_int(config->fsm_queue_size) &&
                        non_negative_int(config->fsm_task_stack) &&
                        non_negative_int(config->fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "MQTT task fields must be non-negative");

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool already_initialized = (me->mqtt != NULL);
    xSemaphoreGive(me->lock);
    if (already_initialized) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_client_config_t mqtt_config = {
        .endpoint = {
            .transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
            .host = config->host,
            .port = config->port,
            .ssl_context_id = config->ssl_context_id,
        },
        .auth = {
            .client_id = config->client_id,
            .username = config->username,
            .password = config->password,
        },
        .session = {
            .keepalive_s = config->keepalive_s,
            .clean_session = config->clean_session,
        },
        .fsm = {
            .queue_size = config->fsm_queue_size,
            .task_stack = config->fsm_task_stack,
            .task_priority = config->fsm_task_priority,
        },
        .event = {
            .loop = me->event_loop,
        },
    };
    if (config->transport == LWLTE_MQTT_TRANSPORT_TLS) {
        mqtt_config.endpoint.transport = MQTT_CLIENT_TRANSPORT_TLS;
    }
    mqtt_client_handle_t mqtt = mqtt_client_create(&mqtt_config, core);
    if (!mqtt) {
        end_api_call(me);
        ESP_LOGE(TAG, "create MQTT client failed");
        return ESP_FAIL;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool lost_race = (me->mqtt != NULL);
    if (!lost_race) {
        me->mqtt = mqtt;
    }
    xSemaphoreGive(me->lock);

    if (lost_race) {
        mqtt_client_destroy(mqtt);
        end_api_call(me);
        ESP_LOGW(TAG, "MQTT client already initialized by a concurrent call");
        return ESP_ERR_INVALID_STATE;
    }

    end_api_call(me);
    return ESP_OK;
}

esp_err_t lwlte_mqtt_destroy(lwlte_handle_t me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t mqtt = me->mqtt;
    me->mqtt = NULL;
    xSemaphoreGive(me->lock);

    if (!mqtt) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    ret = mqtt_client_destroy(mqtt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "destroy MQTT client failed: %s", esp_err_to_name(ret));
    }
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_tcp_init(lwlte_handle_t me, const lwlte_tcp_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(non_negative_int(config->send_queue_size) &&
                        non_negative_int(config->fsm_queue_size) &&
                        non_negative_int(config->fsm_task_stack) &&
                        non_negative_int(config->fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "TCP task fields must be non-negative");

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->tcp) {
        xSemaphoreGive(me->lock);
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    const tcp_client_config_t tcp_config = {
        .max_conns = config->max_conns,
        .send_queue_size = config->send_queue_size,
        .max_tx_len = config->max_tx_len,
        .max_rx_event_len = config->max_rx_event_len,
        .open_timeout_ms = config->open_timeout_ms,
        .send_timeout_ms = config->send_timeout_ms,
        .close_timeout_ms = config->close_timeout_ms,
        .fsm_queue_size = config->fsm_queue_size,
        .fsm_task_stack = config->fsm_task_stack,
        .fsm_task_priority = config->fsm_task_priority,
        .loop = me->event_loop,
    };
    tcp_client_handle_t tcp = tcp_client_create(&tcp_config, core);
    if (!tcp) {
        xSemaphoreGive(me->lock);
        end_api_call(me);
        return config->max_conns > 1 ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }
    me->tcp = tcp;
    xSemaphoreGive(me->lock);

    end_api_call(me);
    return ESP_OK;
}

esp_err_t lwlte_tcp_destroy(lwlte_handle_t me)
{
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    tcp_client_handle_t tcp = me->tcp;
    if (!tcp) {
        xSemaphoreGive(me->lock);
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }
    ret = tcp_client_destroy(tcp);
    if (ret == ESP_OK) {
        me->tcp = NULL;
    }
    xSemaphoreGive(me->lock);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_tcp_open(lwlte_handle_t me,
                         const lwlte_tcp_open_config_t *config,
                         lwlte_tcp_conn_t *out_conn)
{
    ESP_RETURN_ON_FALSE(config && out_conn, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(config->transport == LWLTE_TCP_TRANSPORT_PLAIN_TCP ||
                        config->transport == LWLTE_TCP_TRANSPORT_TLS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid transport");
    tcp_client_handle_t tcp = NULL;
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    tcp = me->tcp;
    if (!tcp) {
        xSemaphoreGive(me->lock);
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    const tcp_client_open_config_t open_config = {
        .host = config->host,
        .port = config->port,
        .user_ctx = config->user_ctx,
        .transport = (config->transport == LWLTE_TCP_TRANSPORT_TLS)
                         ? CORE_SOCKET_TRANSPORT_TLS
                         : CORE_SOCKET_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = config->ssl_context_id,
    };
    ret = tcp_client_open(tcp, &open_config, (tcp_client_conn_t *)out_conn);
    xSemaphoreGive(me->lock);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t conn, const uint8_t *data, size_t len)
{
    return tcp_client_send((tcp_client_conn_t)conn, data, len);
}

esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t conn)
{
    return tcp_client_close((tcp_client_conn_t)conn);
}

esp_err_t lwlte_tcp_conn_get_state(lwlte_tcp_conn_t conn,
                                   lwlte_tcp_conn_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");

    tcp_conn_state_t tcp_state = TCP_CONN_STATE_ERROR;
    esp_err_t ret = tcp_client_conn_get_state((tcp_client_conn_t)conn,
                                              &tcp_state);
    if (ret == ESP_OK) {
        *state = map_tcp_conn_state(tcp_state);
    }
    return ret;
}

esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t conn)
{
    return tcp_client_conn_destroy((tcp_client_conn_t)conn);
}

esp_err_t lwlte_mqtt_start(lwlte_handle_t me)
{
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_start(mqtt);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_stop(lwlte_handle_t me)
{
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_stop(mqtt);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_get_state(lwlte_handle_t me, lwlte_mqtt_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    mqtt_client_state_t mqtt_state = MQTT_CLIENT_STATE_STOPPED;
    ret = mqtt_client_get_state(mqtt, &mqtt_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "get MQTT state failed: %s", esp_err_to_name(ret));
        end_api_call(me);
        return ret;
    }
    *state = map_mqtt_state(mqtt_state);

    end_api_call(me);

    return ESP_OK;
}

esp_err_t lwlte_mqtt_subscribe(lwlte_handle_t me, const char *topic, uint8_t qos)
{
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_subscribe(mqtt, topic, qos);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_unsubscribe(lwlte_handle_t me, const char *topic)
{
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_unsubscribe(mqtt, topic);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_publish(lwlte_handle_t me, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             uint8_t qos, bool retain)
{
    mqtt_client_handle_t mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    const mqtt_client_publish_t request = {
        .topic = topic,
        .payload = payload,
        .payload_len = payload_len,
        .qos = qos,
        .retain = retain,
    };
    ret = mqtt_client_publish(mqtt, &request);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_wait_ready(lwlte_handle_t me, uint32_t timeout_ms)
{
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool ready = me->ready;
    bool failed = me->init_failed;
    bool destroying = me->destroying;
    int error_code = me->init_error_code;
    bool should_wait = !ready && !failed && !destroying;
    if (should_wait) {
        me->ready_waiter_count++;
    }
    xSemaphoreGive(me->lock);

    BaseType_t wait_ret = pdFALSE;
    if (should_wait) {
        TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
        wait_ret = xSemaphoreTake(me->ready_sema, ticks);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->ready_waiter_count > 0) {
            me->ready_waiter_count--;
        }
        ready = me->ready;
        failed = me->init_failed;
        destroying = me->destroying;
        error_code = me->init_error_code;
        xSemaphoreGive(me->lock);
    }

    ret = ESP_ERR_INVALID_STATE;
    if (destroying) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (ready) {
        ret = ESP_OK;
    } else if (failed) {
        ret = error_code ? (esp_err_t)error_code : ESP_FAIL;
    } else if (should_wait && wait_ret != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
    }

    end_api_call(me);
    return ret;
}

void facade_ready_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    lwlte_handle_t me = (lwlte_handle_t)arg;
    if (!me || !me->lock || base != LWLTE_EVENT) {
        return;
    }
    const lwlte_event_data_t *ev = data;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return;
    }
    if ((lwlte_event_id_t)id == LWLTE_EVENT_READY) {
        me->ready = true;
        wake_ready_waiters_locked(me);
    } else if ((lwlte_event_id_t)id == LWLTE_EVENT_ERROR && !me->ready) {
        me->init_failed = true;
        me->init_error_code = ev ? ev->error_code : ESP_FAIL;
        wake_ready_waiters_locked(me);
    }
    xSemaphoreGive(me->lock);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static lwlte_state_t map_core_state(core_state_t state)
{
    switch (state) {
    case CORE_STATE_STOPPED:
        return LWLTE_STATE_STOPPED;
    case CORE_STATE_STARTING:
        return LWLTE_STATE_STARTING;
    case CORE_STATE_READY:
        return LWLTE_STATE_READY;
    case CORE_STATE_NET_ACTIVATING:
        return LWLTE_STATE_NET_ACTIVATING;
    case CORE_STATE_ONLINE:
        return LWLTE_STATE_ONLINE;
    case CORE_STATE_ERROR:
        return LWLTE_STATE_ERROR;
    case CORE_STATE_DESTROYING:
        return LWLTE_STATE_DESTROYING;
    default:
        return LWLTE_STATE_ERROR;
    }
}

static lwlte_net_state_t map_core_net_state(core_net_state_t state)
{
    switch (state) {
    case CORE_NET_STATE_OFFLINE:
        return LWLTE_NET_STATE_OFFLINE;
    case CORE_NET_STATE_ACTIVATING:
        return LWLTE_NET_STATE_ACTIVATING;
    case CORE_NET_STATE_ONLINE:
        return LWLTE_NET_STATE_ONLINE;
    case CORE_NET_STATE_ERROR:
        return LWLTE_NET_STATE_ERROR;
    default:
        return LWLTE_NET_STATE_ERROR;
    }
}

static lwlte_mqtt_state_t map_mqtt_state(mqtt_client_state_t state)
{
    switch (state) {
    case MQTT_CLIENT_STATE_STOPPED:
        return LWLTE_MQTT_STATE_STOPPED;
    case MQTT_CLIENT_STATE_WAITING_NET:
        return LWLTE_MQTT_STATE_WAITING_NET;
    case MQTT_CLIENT_STATE_CONNECTING:
        return LWLTE_MQTT_STATE_CONNECTING;
    case MQTT_CLIENT_STATE_CONNECTED:
        return LWLTE_MQTT_STATE_CONNECTED;
    case MQTT_CLIENT_STATE_DISCONNECTING:
        return LWLTE_MQTT_STATE_DISCONNECTING;
    case MQTT_CLIENT_STATE_ERROR:
    case MQTT_CLIENT_STATE_DESTROYING:
    default:
        return LWLTE_MQTT_STATE_ERROR;
    }
}

static lwlte_tcp_conn_state_t map_tcp_conn_state(tcp_conn_state_t state)
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

static void facade_core_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx)
{
    (void)core;
    (void)type;
    lwlte_sync_cmd_ctx_t *ctx = (lwlte_sync_cmd_ctx_t *)user_ctx;
    if (!ctx) {
        return;
    }
    ctx->result = result;
    if (result == CORE_CMD_RESULT_OK) {
        ctx->error_code = ESP_OK;
    } else if (result_data) {
        ctx->error_code = *(const esp_err_t *)result_data;
    } else {
        ctx->error_code = ESP_FAIL;
    }
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}

static void facade_http_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx)
{
    (void)core;
    (void)type;
    lwlte_http_cmd_ctx_t *ctx = (lwlte_http_cmd_ctx_t *)user_ctx;
    if (!ctx) {
        return;
    }
    ctx->result = result;
    if (result == CORE_CMD_RESULT_OK && result_data) {
        const core_http_result_t *hr = (const core_http_result_t *)result_data;
        ctx->error_code = hr->error_code;
        ctx->status_code = hr->status_code;
        ctx->body = hr->body;
        ctx->body_len = hr->body_len;
        ctx->modem_error_code = hr->modem_error_code;
    } else {
        ctx->error_code = ESP_FAIL;
        ctx->status_code = 0;
        ctx->body = NULL;
        ctx->body_len = 0;
        ctx->modem_error_code = 0;
        if (result_data) {
            const core_http_result_t *hr = (const core_http_result_t *)result_data;
            ctx->error_code = hr->error_code;
            ctx->modem_error_code = hr->modem_error_code;
            free(hr->body);
        }
    }
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}

static esp_err_t begin_api_call(lwlte_handle_t me, bool require_core,
                                core_handle_t *out_core)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->api_done_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    if (destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (require_core && !me->core) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (out_core) {
        *out_core = me->core;
    }
    me->active_api_calls++;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

static esp_err_t begin_mqtt_api_call(lwlte_handle_t me, mqtt_client_handle_t *out_mqtt)
{
    ESP_RETURN_ON_FALSE(out_mqtt, ESP_ERR_INVALID_ARG, TAG,
                        "out_mqtt is NULL");
    *out_mqtt = NULL;

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t mqtt = me->mqtt;
    xSemaphoreGive(me->lock);

    if (!mqtt) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }
    *out_mqtt = mqtt;

    return ESP_OK;
}

static void end_api_call(lwlte_handle_t me)
{
    if (!me || !me->lock) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_api_calls > 0) {
        me->active_api_calls--;
    }
    if (me->active_api_calls == 0 && me->api_done_sema) {
        xSemaphoreGive(me->api_done_sema);
    }
    xSemaphoreGive(me->lock);
}

static bool non_negative_int(int value)
{
    return value >= 0;
}

static esp_err_t wait_api_calls_idle(lwlte_handle_t me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    while (true) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        int active = me->active_api_calls;
        SemaphoreHandle_t done_sema = me->api_done_sema;
        xSemaphoreGive(me->lock);

        if (active == 0) {
            return ESP_OK;
        }
        if (!done_sema) {
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(done_sema, portMAX_DELAY);
    }
}

static void wake_ready_waiters_locked(lwlte_handle_t me)
{
    if (!me || !me->ready_sema) {
        return;
    }

    int waiter_count = me->ready_waiter_count;
    for (int i = 0; i < waiter_count; i++) {
        xSemaphoreGive(me->ready_sema);
    }
}

static void restore_after_destroy_failure(lwlte_handle_t me)
{
    if (!me || !me->lock) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->destroying = false;
    if (me->ready_sema && !me->ready && !me->init_failed) {
        while (xSemaphoreTake(me->ready_sema, 0) == pdTRUE) {
        }
    }
    xSemaphoreGive(me->lock);
}

static esp_err_t destroy_owned_resources(lwlte_handle_t me)
{
    esp_err_t ret = ESP_OK;

    if (me->tcp) {
        ret = tcp_client_destroy(me->tcp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy TCP client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->tcp = NULL;
    }

    if (me->mqtt) {
        ret = mqtt_client_destroy(me->mqtt);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy MQTT client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->mqtt = NULL;
    }

    if (me->ping) {
        ret = ping_client_destroy(me->ping);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy Ping client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->ping = NULL;
    }

    if (me->core) {
        ret = core_destroy(me->core);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy core failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->core = NULL;
    }

    if (me->modem) {
        ret = modem_destroy(me->modem);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->modem = NULL;
    }

    if (me->at) {
        ret = at_engine_destroy(me->at);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy AT engine failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->at = NULL;
    }

    return ESP_OK;
}
