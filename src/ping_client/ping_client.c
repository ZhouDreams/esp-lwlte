/**
 * @file ping_client.c
 * @brief Ping 诊断服务实现
 * @details Ping diagnostic service implementation
 * @author JovisDreams
 * @date 2026-05-28
 */

/*********************
 *      INCLUDES
 *********************/
#include "ping_client_priv.h"

#include <stdlib.h>

#include "esp_check.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "ping_client"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
/**
 * @brief 校验 Ping 请求参数
 * @details Validate Ping request arguments
 * @param[in] request Ping 请求
 * @param[out] replies 响应数组
 * @param[in] max_replies 响应数组容量
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
static esp_err_t validate_request(const ping_client_request_t *request,
                                  const core_ping_reply_t *replies,
                                  size_t max_replies);

/**
 * @brief 开始一次已接受的 Ping 调用
 * @details Begin one accepted Ping call
 * @param[in] me Ping 服务句柄
 * @param[out] out_core 借用的 Core 句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 服务正在销毁
 */
static esp_err_t begin_ping_call(ping_client_t *me, core_t **out_core);

/**
 * @brief 结束一次已接受的 Ping 调用
 * @details End one accepted Ping call
 * @param[in] me Ping 服务句柄
 */
static void end_ping_call(ping_client_t *me);

/**
 * @brief 等待已接受的 Ping 调用结束
 * @details Wait until accepted Ping calls are idle
 * @param[in] me Ping 服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
static esp_err_t wait_active_calls_idle(ping_client_t *me);

/**
 * @brief 计算 Core 命令超时
 * @details Calculate Core command timeout
 * @param[in] request Ping 请求
 * @return Core 命令超时毫秒
 */
static uint32_t derive_timeout_ms(const ping_client_request_t *request);

/**
 * @brief 映射 Core 命令结果
 * @details Map Core command result
 * @param[in] result Core 命令结果
 * @param[in] esp_result 下层错误码
 * @return ESP-IDF 错误码
 */
static esp_err_t map_core_result(core_cmd_result_t result, esp_err_t esp_result);

/**
 * @brief 处理 Core 命令完成
 * @details Handle Core command completion
 * @param[in] core Core 句柄
 * @param[in] type 命令类型
 * @param[in] result 命令结果
 * @param[in] result_data 结果数据
 * @param[in] user_ctx 用户上下文
 */
static void ping_core_cmd_done_cb(core_t *core, core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data, void *user_ctx);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
ping_client_t *ping_client_create(core_t *core)
{
    if (!core) {
        return NULL;
    }

    ping_client_t *me = calloc(1, sizeof(*me));
    if (!me) {
        return NULL;
    }

    me->lock = xSemaphoreCreateMutex();
    me->active_done_sema = xSemaphoreCreateBinary();
    if (!me->lock || !me->active_done_sema) {
        if (me->active_done_sema) {
            vSemaphoreDelete(me->active_done_sema);
        }
        if (me->lock) {
            vSemaphoreDelete(me->lock);
        }
        free(me);
        return NULL;
    }
    me->core = core;

    return me;
}

esp_err_t ping_client_destroy(ping_client_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->active_done_sema,
                        ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    xSemaphoreGive(me->lock);

    ESP_RETURN_ON_ERROR(wait_active_calls_idle(me), TAG,
                        "wait active ping calls idle failed");

    SemaphoreHandle_t lock = me->lock;
    SemaphoreHandle_t active_done_sema = me->active_done_sema;
    me->core = NULL;
    me->active_done_sema = NULL;
    me->lock = NULL;
    vSemaphoreDelete(active_done_sema);
    vSemaphoreDelete(lock);
    free(me);

    return ESP_OK;
}

esp_err_t ping_client_ping(ping_client_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_ERROR(validate_request(request, replies, max_replies), TAG,
                        "invalid ping request");

    core_t *core = NULL;
    ESP_RETURN_ON_ERROR(begin_ping_call(me, &core), TAG,
                        "begin ping call failed");

    SemaphoreHandle_t done_sema = xSemaphoreCreateBinary();
    if (!done_sema) {
        end_ping_call(me);
        return ESP_ERR_NO_MEM;
    }

    ping_wait_ctx_t wait_ctx = {
        .done_sema = done_sema,
        .core_result = CORE_CMD_RESULT_ERROR,
        .esp_result = ESP_FAIL,
    };
    core_cmd_t cmd = {
        .type = CORE_CMD_PING,
        .done_cb = ping_core_cmd_done_cb,
        .user_ctx = &wait_ctx,
        .timeout_ms = derive_timeout_ms(request),
        .data.ping = {
            .host = request->host,
            .count = request->count,
            .data_len = request->data_len,
            .timeout_100ms = request->timeout_100ms,
            .ttl = request->ttl,
            .replies = replies,
            .max_replies = max_replies,
            .summary = summary,
        },
    };

    esp_err_t ret = core_submit_cmd(core, &cmd);
    if (ret != ESP_OK) {
        vSemaphoreDelete(done_sema);
        end_ping_call(me);
        return ret;
    }

    xSemaphoreTake(done_sema, portMAX_DELAY);
    ret = wait_ctx.completed ? map_core_result(wait_ctx.core_result,
                                               wait_ctx.esp_result) : ESP_FAIL;
    vSemaphoreDelete(done_sema);
    end_ping_call(me);

    return ret;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t validate_request(const ping_client_request_t *request,
                                  const core_ping_reply_t *replies,
                                  size_t max_replies)
{
    ESP_RETURN_ON_FALSE(request && request->host && request->host[0] && replies,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(request->count >= 1 &&
                        request->count <= PING_CLIENT_MAX_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid count");
    ESP_RETURN_ON_FALSE(request->data_len <= PING_CLIENT_MAX_DATA_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid data length");
    ESP_RETURN_ON_FALSE(request->timeout_100ms >= 1 &&
                        request->timeout_100ms <= PING_CLIENT_MAX_TIMEOUT_100MS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid timeout");
    ESP_RETURN_ON_FALSE(request->ttl >= 1, ESP_ERR_INVALID_ARG, TAG,
                        "invalid ttl");
    ESP_RETURN_ON_FALSE(max_replies >= request->count, ESP_ERR_INVALID_ARG,
                        TAG, "reply buffer too small");

    return ESP_OK;
}

static esp_err_t begin_ping_call(ping_client_t *me, core_t **out_core)
{
    ESP_RETURN_ON_FALSE(me && me->lock && out_core, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || !me->core) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    me->active_calls++;
    *out_core = me->core;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

static void end_ping_call(ping_client_t *me)
{
    if (!me || !me->lock) {
        return;
    }

    SemaphoreHandle_t active_done_sema = NULL;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_calls > 0) {
        me->active_calls--;
        if (me->active_calls == 0) {
            active_done_sema = me->active_done_sema;
        }
    }
    xSemaphoreGive(me->lock);

    if (active_done_sema) {
        xSemaphoreGive(active_done_sema);
    }
}

static esp_err_t wait_active_calls_idle(ping_client_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->active_done_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    while (true) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        size_t active_calls = me->active_calls;
        SemaphoreHandle_t active_done_sema = me->active_done_sema;
        xSemaphoreGive(me->lock);

        if (active_calls == 0) {
            return ESP_OK;
        }
        xSemaphoreTake(active_done_sema, portMAX_DELAY);
    }
}

static uint32_t derive_timeout_ms(const ping_client_request_t *request)
{
    if (request->total_timeout_ms > 0) {
        return request->total_timeout_ms;
    }

    return (uint32_t)request->count * (uint32_t)request->timeout_100ms * 100U +
           PING_CLIENT_DEFAULT_OVERHEAD_MS;
}

static esp_err_t map_core_result(core_cmd_result_t result, esp_err_t esp_result)
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
        return esp_result != ESP_OK ? esp_result : ESP_FAIL;
    }
}

static void ping_core_cmd_done_cb(core_t *core, core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data, void *user_ctx)
{
    (void)core;

    ping_wait_ctx_t *wait_ctx = (ping_wait_ctx_t *)user_ctx;
    if (!wait_ctx || !wait_ctx->done_sema || type != CORE_CMD_PING) {
        return;
    }

    wait_ctx->core_result = result;
    wait_ctx->esp_result = result_data ? *(const esp_err_t *)result_data : ESP_FAIL;
    if (result == CORE_CMD_RESULT_OK) {
        wait_ctx->esp_result = ESP_OK;
    }
    wait_ctx->completed = true;
    xSemaphoreGive(wait_ctx->done_sema);
}
