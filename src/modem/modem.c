/**
 * @file modem.c
 * @brief 调制解调器通用实现
 * @details Modem common implementation
 * @author JovisDreams
 * @date 2026-05-23
 */

/*********************
 *      INCLUDES
 *********************/
#include "modem_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "modem"
#define MODEM_DEFAULT_EVENT_QUEUE_SIZE     8
#define MODEM_DEFAULT_EVENT_TASK_STACK     4096
#define MODEM_DEFAULT_EVENT_TASK_PRIORITY  9
#define MODEM_EVENT_TASK_WAIT_MS           100

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 事件任务
 * @details Event task
 * @param[in] arg 调制解调器句柄
 */
static void event_task(void *arg);

/**
 * @brief 检查事件任务是否应停止
 * @details Check whether event task should stop
 * @param[in] me 调制解调器句柄
 * @return
 *         - true: 应停止
 *         - false: 继续运行
 */
static bool event_task_should_stop(modem_handle_t *me);

/**
 * @brief 检查调制解调器是否可操作
 * @details Check whether modem can operate
 * @param[in] me 调制解调器句柄
 * @param[in] allow_created 是否允许 CREATED 状态
 * @return
 *         - ESP_OK: 可以操作
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 */
static esp_err_t check_ready(modem_handle_t *me, bool allow_created);

/**
 * @brief 检查 SSL 证书材料是否匹配认证模式
 * @details Check whether SSL credentials match authentication mode
 * @param[in] auth SSL 认证模式
 * @param[in] credentials SSL 证书材料
 * @return
 *         - true: 有效
 *         - false: 无效
 */
static bool ssl_credentials_valid(modem_ssl_auth_mode_t auth,
                                  const modem_ssl_credentials_t *credentials);

/**
 * @brief 调用无额外参数的 ops 方法
 * @details Call ops method without extra arguments
 * @param[in] me 调制解调器句柄
 * @param[in] fn ops 方法
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: ops 方法返回值
 */
static esp_err_t call_no_arg(modem_handle_t *me, modem_no_arg_fn fn);

/**
 * @brief 释放协议数据事件负载
 * @details Release PROTOCOL_DATA event payload (topic/payload buffers)
 * @param[in,out] event 调制解调器事件
 */
static void release_event_payload(modem_event_t *event);

/**
 * @brief 排空事件队列中的负载
 * @details Drain event queue and release all PROTOCOL_DATA payloads
 * @param[in] me 调制解调器句柄
 */
static void drain_event_queue_payloads(modem_handle_t *me);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

esp_err_t modem_base_init(modem_handle_t *me, const char *name, at_engine_handle_t *at,
                          const modem_ops_t *ops, int event_queue_size,
                          int event_task_stack, int event_task_priority)
{
    ESP_RETURN_ON_FALSE(me && name && at && ops, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = ESP_OK;

    me->lock = NULL;
    me->event_queue = NULL;
    me->event_task = NULL;
    me->event_task_done_sema = NULL;
    me->event_cb_done_sema = NULL;
    me->event_cb_active = 0;
    me->event_task_stop_requested = false;

    if (event_queue_size <= 0) {
        event_queue_size = MODEM_DEFAULT_EVENT_QUEUE_SIZE;
    }
    if (event_task_stack <= 0) {
        event_task_stack = MODEM_DEFAULT_EVENT_TASK_STACK;
    }
    if (event_task_priority <= 0) {
        event_task_priority = MODEM_DEFAULT_EVENT_TASK_PRIORITY;
    }

    me->ops = ops;
    me->at = at;
    me->name = name;
    me->state = MODEM_STATE_CREATED;
    me->destroying = false;
    me->event_cb = NULL;
    me->event_user_ctx = NULL;
    me->event_cb_active = 0;

    me->lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(me->lock, ESP_ERR_NO_MEM, err, TAG, "create lock failed");

    me->event_queue = xQueueCreate(event_queue_size, sizeof(modem_event_t));
    ESP_GOTO_ON_FALSE(me->event_queue, ESP_ERR_NO_MEM, err, TAG,
                      "create event_queue failed");

    me->event_task_done_sema = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(me->event_task_done_sema, ESP_ERR_NO_MEM, err, TAG,
                      "create event_task_done_sema failed");

    me->event_cb_done_sema = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(me->event_cb_done_sema, ESP_ERR_NO_MEM, err, TAG,
                      "create event_cb_done_sema failed");

    BaseType_t task_ret = xTaskCreate(event_task, "modem_evt", event_task_stack,
                                      me, event_task_priority, &me->event_task);
    ESP_GOTO_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, err, TAG,
                      "create event task failed");

    return ESP_OK;

err:
    esp_err_t cleanup_ret = modem_base_deinit(me);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "cleanup after base init failure failed: %s",
                 esp_err_to_name(cleanup_ret));
    }
    return ret;
}

esp_err_t modem_base_deinit(modem_handle_t *me)
{
    if (!me) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = modem_base_stop_event_task(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stop event task before deinit failed: %s", esp_err_to_name(ret));
        if (me->event_task) {
            return ret;
        }
    }

    if (me->event_queue) {
        QueueHandle_t event_queue = me->event_queue;
        if (me->lock) {
            xSemaphoreTake(me->lock, portMAX_DELAY);
            event_queue = me->event_queue;
            drain_event_queue_payloads(me);
            me->event_queue = NULL;
            xSemaphoreGive(me->lock);
        } else {
            drain_event_queue_payloads(me);
            me->event_queue = NULL;
        }
        vQueueDelete(event_queue);
    }
    if (me->event_task_done_sema) {
        vSemaphoreDelete(me->event_task_done_sema);
        me->event_task_done_sema = NULL;
    }
    if (me->event_cb_done_sema) {
        vSemaphoreDelete(me->event_cb_done_sema);
        me->event_cb_done_sema = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }

    me->ops = NULL;
    me->at = NULL;
    me->event_task = NULL;
    me->event_cb = NULL;
    me->event_user_ctx = NULL;
    me->event_cb_active = 0;
    me->name = NULL;

    return ret;
}

esp_err_t modem_base_stop_event_task(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    if (!me->lock) {
        return me->event_task ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    if (me->event_task && xTaskGetCurrentTaskHandle() == me->event_task) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->event_task_stop_requested = true;
    xSemaphoreGive(me->lock);

    if (me->event_task) {
        xSemaphoreTake(me->event_task_done_sema, portMAX_DELAY);
        me->event_task = NULL;
    }

    return ESP_OK;
}

esp_err_t modem_post_event(modem_handle_t *me, const modem_event_t *event)
{
    ESP_RETURN_ON_FALSE(me && event && me->lock,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == MODEM_STATE_DESTROYING ||
        me->event_task_stop_requested || !me->event_task || !me->event_queue) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t send_ret = xQueueSend(me->event_queue, event, 0);
    xSemaphoreGive(me->lock);

    if (send_ret != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, drop event %d", event->id);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t modem_set_state(modem_handle_t *me, modem_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(state >= MODEM_STATE_CREATED && state <= MODEM_STATE_DESTROYING,
                        ESP_ERR_INVALID_ARG, TAG, "invalid state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying && state != MODEM_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->state = state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t modem_destroy(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (me->event_task && xTaskGetCurrentTaskHandle() == me->event_task) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    modem_state_t state = me->state;
    bool allowed = (state == MODEM_STATE_CREATED ||
                    state == MODEM_STATE_OFF ||
                    state == MODEM_STATE_READY ||
                    state == MODEM_STATE_REGISTERING ||
                    state == MODEM_STATE_REGISTERED ||
                    state == MODEM_STATE_PDP_ACTIVE ||
                    state == MODEM_STATE_ERROR);
    if (!allowed || me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->state = MODEM_STATE_DESTROYING;
    me->destroying = true;
    xSemaphoreGive(me->lock);

    esp_err_t ret = modem_base_stop_event_task(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stop event task failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (me->ops && me->ops->destroy) {
        esp_err_t destroy_ret = me->ops->destroy(me);
        if (destroy_ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem failed: %s", esp_err_to_name(destroy_ret));
            xSemaphoreTake(me->lock, portMAX_DELAY);
            me->destroying = false;
            me->state = (state >= MODEM_STATE_CREATED && state <= MODEM_STATE_ERROR) ?
                        state : MODEM_STATE_ERROR;
            me->event_task_stop_requested = false;
            xSemaphoreGive(me->lock);
            /* Event task is already stopped; retry can still run subclass destroy/base deinit. */
            return destroy_ret;
        }
    }

    ret = modem_base_deinit(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "base deinit failed: %s", esp_err_to_name(ret));
        return ret;
    }
    free(me);
    return ESP_OK;
}

esp_err_t modem_start(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->start,
                        ESP_ERR_NOT_SUPPORTED, TAG, "start not supported");

    return call_no_arg(me, me->ops->start);
}

esp_err_t modem_stop(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying || me->state == MODEM_STATE_DESTROYING;
    xSemaphoreGive(me->lock);
    ESP_RETURN_ON_FALSE(!destroying, ESP_ERR_INVALID_STATE, TAG, "modem destroying");

    ESP_RETURN_ON_FALSE(me->ops && me->ops->stop,
                        ESP_ERR_NOT_SUPPORTED, TAG, "stop not supported");

    return call_no_arg(me, me->ops->stop);
}

esp_err_t modem_reset(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->reset,
                        ESP_ERR_NOT_SUPPORTED, TAG, "reset not supported");

    return call_no_arg(me, me->ops->reset);
}

esp_err_t modem_register_event_callback(modem_handle_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (!callback && me->event_task && xTaskGetCurrentTaskHandle() == me->event_task) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (callback && me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    me->event_cb = callback;
    me->event_user_ctx = callback ? user_ctx : NULL;

    if (callback) {
        xSemaphoreGive(me->lock);
        return ESP_OK;
    }

    int active = me->event_cb_active;
    SemaphoreHandle_t done_sema = me->event_cb_done_sema;
    xSemaphoreGive(me->lock);

    while (active > 0) {
        if (!done_sema) {
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreTake(done_sema, portMAX_DELAY);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        active = me->event_cb_active;
        done_sema = me->event_cb_done_sema;
        xSemaphoreGive(me->lock);
    }

    return ESP_OK;
}

esp_err_t modem_get_state(modem_handle_t *me, modem_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t modem_get_info(modem_handle_t *me, modem_info_t *info)
{
    ESP_RETURN_ON_FALSE(me && info, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_info,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_info not supported");

    return me->ops->get_info(me, info);
}

esp_err_t modem_get_sim_status(modem_handle_t *me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_sim_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_sim_status not supported");

    return me->ops->get_sim_status(me, status);
}

esp_err_t modem_get_signal(modem_handle_t *me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_signal,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_signal not supported");

    return me->ops->get_signal(me, signal);
}

esp_err_t modem_get_registration(modem_handle_t *me, modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_registration,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_registration not supported");

    return me->ops->get_registration(me, status);
}

esp_err_t modem_get_packet_attach_status(modem_handle_t *me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_packet_attach_status,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "get_packet_attach_status not supported");

    return me->ops->get_packet_attach_status(me, attached);
}

esp_err_t modem_set_apn(modem_handle_t *me, uint8_t cid, const char *apn)
{
    ESP_RETURN_ON_FALSE(me && apn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->set_apn,
                        ESP_ERR_NOT_SUPPORTED, TAG, "set_apn not supported");

    return me->ops->set_apn(me, cid, apn);
}

esp_err_t modem_activate_pdp(modem_handle_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->activate_pdp,
                        ESP_ERR_NOT_SUPPORTED, TAG, "activate_pdp not supported");

    return me->ops->activate_pdp(me, cid);
}

esp_err_t modem_deactivate_pdp(modem_handle_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->deactivate_pdp,
                        ESP_ERR_NOT_SUPPORTED, TAG, "deactivate_pdp not supported");

    return me->ops->deactivate_pdp(me, cid);
}

esp_err_t modem_get_pdp_context(modem_handle_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_pdp_context,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_pdp_context not supported");

    return me->ops->get_pdp_context(me, cid, pdp);
}

esp_err_t modem_ssl_provision(modem_handle_t *me,
                              const modem_ssl_context_config_t *config,
                              const modem_ssl_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(me && config && ssl_credentials_valid(config->auth_mode, credentials),
                        ESP_ERR_INVALID_ARG, TAG, "invalid SSL provision args");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ssl_provision,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ssl_provision not supported");
    return me->ops->ssl_provision(me, config, credentials);
}

esp_err_t modem_ssl_get_context_status(modem_handle_t *me,
                                       uint8_t context_id,
                                       modem_ssl_context_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    memset(status, 0, sizeof(*status));
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ssl_get_context_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ssl_get_context_status not supported");
    return me->ops->ssl_get_context_status(me, context_id, status);
}

esp_err_t modem_mqtt_configure(modem_handle_t *me,
                               const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id && config->host && config->port > 0 &&
                        (config->transport == MODEM_MQTT_TRANSPORT_PLAIN_TCP ||
                         config->transport == MODEM_MQTT_TRANSPORT_TLS),
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT config");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_configure,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_configure not supported");
    return me->ops->mqtt_configure(me, config);
}

esp_err_t modem_mqtt_tcp_connect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_tcp_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_tcp_connect not supported");
    return me->ops->mqtt_tcp_connect(me);
}

esp_err_t modem_mqtt_connect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_connect not supported");
    return me->ops->mqtt_connect(me);
}

esp_err_t modem_mqtt_disconnect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_disconnect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_disconnect not supported");

    return me->ops->mqtt_disconnect(me);
}

esp_err_t modem_mqtt_tcp_disconnect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_tcp_disconnect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_tcp_disconnect not supported");
    return me->ops->mqtt_tcp_disconnect(me);
}

esp_err_t modem_mqtt_subscribe(modem_handle_t *me,
                               const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic && topic->qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_subscribe,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_subscribe not supported");

    return me->ops->mqtt_subscribe(me, topic);
}

esp_err_t modem_mqtt_unsubscribe(modem_handle_t *me,
                                 const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_unsubscribe,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "mqtt_unsubscribe not supported");

    return me->ops->mqtt_unsubscribe(me, topic);
}

esp_err_t modem_mqtt_publish(modem_handle_t *me,
                             const modem_mqtt_publish_t *publish)
{
    ESP_RETURN_ON_FALSE(me && publish && publish->topic && publish->payload &&
                        publish->payload_len > 0 && publish->qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_publish,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_publish not supported");

    return me->ops->mqtt_publish(me, publish);
}

esp_err_t modem_mqtt_get_status(modem_handle_t *me, modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_get_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_get_status not supported");
    return me->ops->mqtt_get_status(me, status);
}

esp_err_t modem_socket_open(modem_handle_t *me,
                            const modem_socket_open_t *open)
{
    ESP_RETURN_ON_FALSE(me && open && open->proto == MODEM_SOCKET_PROTO_TCP &&
                        open->host && open->host[0] && open->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket open args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_open,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_open not supported");

    return me->ops->socket_open(me, open);
}

esp_err_t modem_socket_send(modem_handle_t *me,
                            const modem_socket_send_t *send)
{
    ESP_RETURN_ON_FALSE(me && send && send->data && send->len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket send args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_send,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_send not supported");

    return me->ops->socket_send(me, send);
}

esp_err_t modem_socket_recv(modem_handle_t *me,
                            const modem_socket_recv_t *recv,
                            modem_socket_recv_result_t *result)
{
    ESP_RETURN_ON_FALSE(me && recv && result && recv->max_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket recv args");

    memset(result, 0, sizeof(*result));
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_recv,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_recv not supported");

    return me->ops->socket_recv(me, recv, result);
}

esp_err_t modem_socket_close(modem_handle_t *me,
                             const modem_socket_close_t *close)
{
    ESP_RETURN_ON_FALSE(me && close, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket close args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_close,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_close not supported");

    return me->ops->socket_close(me, close);
}

esp_err_t modem_ping(modem_handle_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && request && request->host && request->host[0] &&
                        replies && request->count >= 1 && request->count <= 100 &&
                        request->data_len <= 1024 &&
                        request->timeout_100ms >= 1 &&
                        request->timeout_100ms <= 600 &&
                        request->ttl >= 1 &&
                        max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping request");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ping,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ping not supported");

    return me->ops->ping(me, request, replies, max_replies, summary);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void event_task(void *arg)
{
    modem_handle_t *me = (modem_handle_t *)arg;

    while (!event_task_should_stop(me)) {
        modem_event_t event = {0};
        if (xQueueReceive(me->event_queue, &event,
                          pdMS_TO_TICKS(MODEM_EVENT_TASK_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (event_task_should_stop(me)) {
            release_event_payload(&event);
            break;
        }

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->destroying || me->state == MODEM_STATE_DESTROYING) {
            xSemaphoreGive(me->lock);
            release_event_payload(&event);
            break;
        }
        modem_event_callback_t cb = me->event_cb;
        void *user_ctx = me->event_user_ctx;
        if (cb) {
            me->event_cb_active++;
        }
        xSemaphoreGive(me->lock);

        if (cb) {
            cb(me, &event, user_ctx);
            release_event_payload(&event);

            xSemaphoreTake(me->lock, portMAX_DELAY);
            if (me->event_cb_active > 0) {
                me->event_cb_active--;
            }
            bool cb_done = me->event_cb_active == 0;
            SemaphoreHandle_t done_sema = me->event_cb_done_sema;
            xSemaphoreGive(me->lock);

            if (cb_done && done_sema) {
                xSemaphoreGive(done_sema);
            }
        } else {
            release_event_payload(&event);
        }
    }

    drain_event_queue_payloads(me);

    xSemaphoreGive(me->event_task_done_sema);
    vTaskDelete(NULL);
}

static bool event_task_should_stop(modem_handle_t *me)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool should_stop = me->event_task_stop_requested || me->destroying ||
                       me->state == MODEM_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    return should_stop;
}

static bool ssl_credentials_valid(modem_ssl_auth_mode_t auth,
                                  const modem_ssl_credentials_t *credentials)
{
    int auth_value = (int)auth;
    if (!credentials || auth_value < (int)MODEM_SSL_AUTH_NONE ||
        auth_value > (int)MODEM_SSL_AUTH_MUTUAL) {
        return false;
    }
    bool ca_pair = (!credentials->ca_cert_pem && credentials->ca_cert_len == 0) ||
                   (credentials->ca_cert_pem && credentials->ca_cert_len > 0);
    bool cert_pair = (!credentials->client_cert_pem && credentials->client_cert_len == 0) ||
                     (credentials->client_cert_pem && credentials->client_cert_len > 0);
    bool key_pair = (!credentials->client_key_pem && credentials->client_key_len == 0) ||
                    (credentials->client_key_pem && credentials->client_key_len > 0);
    if (!ca_pair || !cert_pair || !key_pair) {
        return false;
    }
    if (auth == MODEM_SSL_AUTH_SERVER) {
        return credentials->ca_cert_pem && credentials->ca_cert_len > 0;
    }
    if (auth == MODEM_SSL_AUTH_MUTUAL) {
        return credentials->ca_cert_pem && credentials->ca_cert_len > 0 &&
               credentials->client_cert_pem && credentials->client_cert_len > 0 &&
               credentials->client_key_pem && credentials->client_key_len > 0;
    }
    return true;
}

static esp_err_t check_ready(modem_handle_t *me, bool allow_created)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    modem_state_t state = me->state;
    xSemaphoreGive(me->lock);

    if (destroying) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((state == MODEM_STATE_CREATED || state == MODEM_STATE_OFF) && allow_created) {
        return ESP_OK;
    }
    if (state == MODEM_STATE_READY ||
        state == MODEM_STATE_REGISTERING ||
        state == MODEM_STATE_REGISTERED ||
        state == MODEM_STATE_PDP_ACTIVE) {
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

static esp_err_t call_no_arg(modem_handle_t *me, modem_no_arg_fn fn)
{
    ESP_RETURN_ON_FALSE(me && fn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    return fn(me);
}

static void drain_event_queue_payloads(modem_handle_t *me)
{
    if (!me || !me->event_queue) {
        return;
    }

    modem_event_t event = {0};
    while (xQueueReceive(me->event_queue, &event, 0) == pdTRUE) {
        release_event_payload(&event);
    }
}

static void release_event_payload(modem_event_t *event)
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
