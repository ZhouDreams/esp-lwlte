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
 * @brief 映射 Core 事件
 * @details Map Core event
 * @param[in] event_id Core 事件 ID
 * @return LTE 用户事件 ID
 */
static lwlte_event_id_t map_core_event(core_event_id_t event_id);

/**
 * @brief 映射 MQTT 状态
 * @details Map MQTT state
 * @param[in] state MQTT 状态
 * @return LTE MQTT 状态
 */
static lwlte_mqtt_state_t map_mqtt_state(mqtt_client_state_t state);

/**
 * @brief 映射 MQTT 事件
 * @details Map MQTT event
 * @param[in] event_id MQTT 事件 ID
 * @return LTE 用户事件 ID
 */
static lwlte_event_id_t map_mqtt_event(mqtt_client_event_id_t event_id);

/**
 * @brief 映射 Core 事件数据
 * @details Map Core event data
 * @param[in] core_data Core 事件数据，可能为 NULL
 * @param[out] lwlte_data LTE 用户事件数据
 */
static void map_core_event_data(const core_event_data_t *core_data,
                                lwlte_event_data_t *lwlte_data);

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
static esp_err_t begin_api_call(lwlte_t *me, bool require_core,
                                core_t **out_core);

/**
 * @brief 进入 MQTT 门面 API 调用
 * @details Begin MQTT facade API call
 * @param[in] me LTE 用户门面句柄
 * @param[out] out_mqtt MQTT 客户端输出指针
 * @return esp_err_t
 */
static esp_err_t begin_mqtt_api_call(lwlte_t *me, mqtt_client_t **out_mqtt);

/**
 * @brief 退出门面 API 调用
 * @details End facade API call
 * @param[in] me LTE 用户门面句柄
 */
static void end_api_call(lwlte_t *me);

/**
 * @brief 等待 API 调用空闲
 * @details Wait until API calls are idle
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: API 调用已空闲
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 内部状态无效
 */
static esp_err_t wait_api_calls_idle(lwlte_t *me);

/**
 * @brief 等待用户回调空闲
 * @details Wait until user callbacks are idle
 * @param[in] me LTE 用户门面句柄
 * @param[in] claim_waiter 是否登记回调等待者
 * @return
 *         - ESP_OK: 用户回调已空闲
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前任务正在执行回调或内部状态无效
 */
static esp_err_t wait_callbacks_idle(lwlte_t *me, bool claim_waiter);

/**
 * @brief 检查任务是否正在执行用户回调
 * @details Check whether a task is executing a user callback
 * @note 调用方必须持有 me->lock。
 * @param[in] me LTE 用户门面句柄
 * @param[in] task 任务句柄
 * @return true: 任务可能正在执行回调； false: 任务未执行回调
 */
static bool callback_task_active_locked(const lwlte_t *me, TaskHandle_t task);

/**
 * @brief 登记正在执行用户回调的任务
 * @details Add a task executing a user callback
 * @note 调用方必须持有 me->lock。
 * @param[in] me LTE 用户门面句柄
 * @param[in] task 任务句柄
 * @return true: 已精确登记； false: 使用溢出保守登记
 */
static bool add_callback_task_locked(lwlte_t *me, TaskHandle_t task);

/**
 * @brief 移除正在执行用户回调的任务
 * @details Remove a task executing a user callback
 * @note 调用方必须持有 me->lock。
 * @param[in] me LTE 用户门面句柄
 * @param[in] task 任务句柄
 */
static void remove_callback_task_locked(lwlte_t *me, TaskHandle_t task);

/**
 * @brief 唤醒所有 ready 等待者
 * @details Wake all ready waiters
 * @note 调用方必须持有 me->lock。
 * @param[in] me LTE 用户门面句柄
 */
static void wake_ready_waiters_locked(lwlte_t *me);

/**
 * @brief 恢复销毁失败后的门面状态
 * @details Restore facade state after destroy failure
 * @param[in] me LTE 用户门面句柄
 */
static void restore_after_destroy_failure(lwlte_t *me);

/**
 * @brief 销毁门面持有的资源
 * @details Destroy resources owned by facade
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 成功
 *         - other: 下层销毁错误
 */
static esp_err_t destroy_owned_resources(lwlte_t *me);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_create_empty(lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");

    *out_lte = NULL;
    lwlte_t *me = calloc(1, sizeof(lwlte_t));
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

    me->callback_done_sema = xSemaphoreCreateBinary();
    if (!me->callback_done_sema) {
        vSemaphoreDelete(me->api_done_sema);
        vSemaphoreDelete(me->ready_sema);
        vSemaphoreDelete(me->lock);
        free(me);
        return ESP_ERR_NO_MEM;
    }

    *out_lte = me;
    return ESP_OK;
}

esp_err_t lwlte_destroy(lwlte_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (me->callback_active > 0 &&
        callback_task_active_locked(me, xTaskGetCurrentTaskHandle())) {
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

    ret = wait_callbacks_idle(me, false);
    if (ret != ESP_OK) {
        restore_after_destroy_failure(me);
        return ret;
    }

    ret = destroy_owned_resources(me);
    if (ret != ESP_OK) {
        restore_after_destroy_failure(me);
        return ret;
    }

    if (me->callback_done_sema) {
        vSemaphoreDelete(me->callback_done_sema);
        me->callback_done_sema = NULL;
    }
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

esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx)
{
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    void *next_user_ctx = callback ? user_ctx : NULL;
    bool claimed_callback_waiter = false;

    while (true) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->destroying) {
            if (claimed_callback_waiter) {
                me->callback_waiting = false;
            }
            xSemaphoreGive(me->lock);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }

        bool changed = me->event_callback != callback ||
                       me->event_user_ctx != next_user_ctx;
        if (!changed) {
            if (claimed_callback_waiter) {
                me->callback_waiting = false;
            }
            xSemaphoreGive(me->lock);
            ret = ESP_OK;
            break;
        }
        if (me->callback_active == 0) {
            me->event_callback = callback;
            me->event_user_ctx = next_user_ctx;
            if (claimed_callback_waiter) {
                me->callback_waiting = false;
            }
            xSemaphoreGive(me->lock);
            ret = ESP_OK;
            break;
        }
        if (callback_task_active_locked(me, xTaskGetCurrentTaskHandle())) {
            if (claimed_callback_waiter) {
                me->callback_waiting = false;
            }
            xSemaphoreGive(me->lock);
            ret = ESP_ERR_INVALID_STATE;
            break;
        }
        xSemaphoreGive(me->lock);

        ret = wait_callbacks_idle(me, true);
        if (ret != ESP_OK) {
            break;
        }
        claimed_callback_waiter = true;
    }

    end_api_call(me);
    return ret;
}

esp_err_t lwlte_connect(lwlte_t *me)
{
    core_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    ret = core_connect(core);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_disconnect(lwlte_t *me)
{
    core_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    ret = core_disconnect(core);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_get_state(lwlte_t *me, lwlte_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    core_t *core = NULL;
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

esp_err_t lwlte_get_net_state(lwlte_t *me, lwlte_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    core_t *core = NULL;
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

esp_err_t lwlte_mqtt_start(lwlte_t *me)
{
    mqtt_client_t *mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_start(mqtt);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_stop(lwlte_t *me)
{
    mqtt_client_t *mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_stop(mqtt);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_get_state(lwlte_t *me, lwlte_mqtt_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    mqtt_client_t *mqtt = NULL;
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

esp_err_t lwlte_mqtt_subscribe(lwlte_t *me, const char *topic, uint8_t qos)
{
    mqtt_client_t *mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_subscribe(mqtt, topic, qos);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_unsubscribe(lwlte_t *me, const char *topic)
{
    mqtt_client_t *mqtt = NULL;
    esp_err_t ret = begin_mqtt_api_call(me, &mqtt);
    ESP_RETURN_ON_ERROR(ret, TAG, "MQTT facade not usable");

    ret = mqtt_client_unsubscribe(mqtt, topic);
    end_api_call(me);

    return ret;
}

esp_err_t lwlte_mqtt_publish(lwlte_t *me, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             uint8_t qos, bool retain)
{
    mqtt_client_t *mqtt = NULL;
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

esp_err_t lwlte_wait_ready(lwlte_t *me, uint32_t timeout_ms)
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

void lwlte_handle_core_event(core_t *core, core_event_id_t event_id,
                             const core_event_data_t *data, void *user_ctx)
{
    (void)core;

    lwlte_t *me = (lwlte_t *)user_ctx;
    if (!me || !me->lock) {
        return;
    }

    lwlte_event_callback_t callback = NULL;
    void *callback_ctx = NULL;
    lwlte_event_data_t lwlte_data = {0};
    map_core_event_data(data, &lwlte_data);

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return;
    }
    if (event_id == CORE_EVENT_READY) {
        me->ready = true;
        wake_ready_waiters_locked(me);
    } else if (event_id == CORE_EVENT_ERROR && !me->ready) {
        me->init_failed = true;
        me->init_error_code = data ? data->error_code : ESP_FAIL;
        wake_ready_waiters_locked(me);
    }
    if (!me->callback_waiting) {
        callback = me->event_callback;
        callback_ctx = me->event_user_ctx;
    }
    if (callback) {
        me->callback_active++;
        add_callback_task_locked(me, xTaskGetCurrentTaskHandle());
    }
    xSemaphoreGive(me->lock);

    if (callback) {
        callback(me, map_core_event(event_id), &lwlte_data, callback_ctx);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->callback_active > 0) {
            me->callback_active--;
        }
        remove_callback_task_locked(me, xTaskGetCurrentTaskHandle());
        bool callback_idle = me->callback_active == 0;
        SemaphoreHandle_t done_sema = me->callback_done_sema;
        xSemaphoreGive(me->lock);

        if (callback_idle && done_sema) {
            xSemaphoreGive(done_sema);
        }
    }
}

void lwlte_handle_mqtt_event(mqtt_client_t *mqtt,
                             mqtt_client_event_id_t event_id,
                             const mqtt_client_event_data_t *data,
                             void *user_ctx)
{
    lwlte_t *me = (lwlte_t *)user_ctx;
    if (!me || !me->lock) {
        return;
    }

    lwlte_event_callback_t callback = NULL;
    void *callback_ctx = NULL;
    lwlte_event_data_t lwlte_data = {0};
    if (data) {
        lwlte_data.mqtt_state = map_mqtt_state(data->state);
        lwlte_data.error_code = data->error_code;
        if (event_id == MQTT_CLIENT_EVENT_DATA) {
            lwlte_data.data.mqtt_msg.topic = data->data.msg.topic;
            lwlte_data.data.mqtt_msg.topic_len = data->data.msg.topic_len;
            lwlte_data.data.mqtt_msg.payload = data->data.msg.payload;
            lwlte_data.data.mqtt_msg.payload_len = data->data.msg.payload_len;
        }
    } else {
        mqtt_client_state_t mqtt_state = MQTT_CLIENT_STATE_STOPPED;
        if (mqtt) {
            (void)mqtt_client_get_state(mqtt, &mqtt_state);
        }
        lwlte_data.mqtt_state = map_mqtt_state(mqtt_state);
        lwlte_data.error_code = 0;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return;
    }
    if (!me->callback_waiting) {
        callback = me->event_callback;
        callback_ctx = me->event_user_ctx;
    }
    if (callback) {
        me->callback_active++;
        add_callback_task_locked(me, xTaskGetCurrentTaskHandle());
    }
    xSemaphoreGive(me->lock);

    if (callback) {
        callback(me, map_mqtt_event(event_id), &lwlte_data, callback_ctx);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->callback_active > 0) {
            me->callback_active--;
        }
        remove_callback_task_locked(me, xTaskGetCurrentTaskHandle());
        bool callback_idle = me->callback_active == 0;
        SemaphoreHandle_t done_sema = me->callback_done_sema;
        xSemaphoreGive(me->lock);

        if (callback_idle && done_sema) {
            xSemaphoreGive(done_sema);
        }
    }
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

static lwlte_event_id_t map_core_event(core_event_id_t event_id)
{
    switch (event_id) {
    case CORE_EVENT_STARTED:
        return LWLTE_EVENT_STARTED;
    case CORE_EVENT_READY:
        return LWLTE_EVENT_READY;
    case CORE_EVENT_NET_CONNECTING:
        return LWLTE_EVENT_NET_CONNECTING;
    case CORE_EVENT_NET_ONLINE:
        return LWLTE_EVENT_NET_ONLINE;
    case CORE_EVENT_NET_OFFLINE:
        return LWLTE_EVENT_NET_OFFLINE;
    case CORE_EVENT_NET_ERROR:
        return LWLTE_EVENT_NET_ERROR;
    case CORE_EVENT_STOPPED:
        return LWLTE_EVENT_STOPPED;
    case CORE_EVENT_ERROR:
        return LWLTE_EVENT_ERROR;
    default:
        return LWLTE_EVENT_ERROR;
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

static lwlte_event_id_t map_mqtt_event(mqtt_client_event_id_t event_id)
{
    switch (event_id) {
    case MQTT_CLIENT_EVENT_STARTED:
        return LWLTE_EVENT_MQTT_STARTED;
    case MQTT_CLIENT_EVENT_STOPPED:
        return LWLTE_EVENT_MQTT_STOPPED;
    case MQTT_CLIENT_EVENT_CONNECTING:
        return LWLTE_EVENT_MQTT_CONNECTING;
    case MQTT_CLIENT_EVENT_CONNECTED:
        return LWLTE_EVENT_MQTT_CONNECTED;
    case MQTT_CLIENT_EVENT_DISCONNECTED:
        return LWLTE_EVENT_MQTT_DISCONNECTED;
    case MQTT_CLIENT_EVENT_SUBSCRIBED:
        return LWLTE_EVENT_MQTT_SUBSCRIBED;
    case MQTT_CLIENT_EVENT_UNSUBSCRIBED:
        return LWLTE_EVENT_MQTT_UNSUBSCRIBED;
    case MQTT_CLIENT_EVENT_PUBLISHED:
        return LWLTE_EVENT_MQTT_PUBLISHED;
    case MQTT_CLIENT_EVENT_DATA:
        return LWLTE_EVENT_MQTT_DATA;
    case MQTT_CLIENT_EVENT_ERROR:
    default:
        return LWLTE_EVENT_MQTT_ERROR;
    }
}

static void map_core_event_data(const core_event_data_t *core_data,
                                lwlte_event_data_t *lwlte_data)
{
    if (!lwlte_data) {
        return;
    }
    if (!core_data) {
        lwlte_data->net_state = LWLTE_NET_STATE_OFFLINE;
        lwlte_data->error_code = 0;
        return;
    }

    lwlte_data->net_state = map_core_net_state(core_data->net_state);
    lwlte_data->error_code = core_data->error_code;
}

static esp_err_t begin_api_call(lwlte_t *me, bool require_core,
                                core_t **out_core)
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

static esp_err_t begin_mqtt_api_call(lwlte_t *me, mqtt_client_t **out_mqtt)
{
    ESP_RETURN_ON_FALSE(out_mqtt, ESP_ERR_INVALID_ARG, TAG,
                        "out_mqtt is NULL");
    *out_mqtt = NULL;

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_t *mqtt = me->mqtt;
    xSemaphoreGive(me->lock);

    if (!mqtt) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }
    *out_mqtt = mqtt;

    return ESP_OK;
}

static void end_api_call(lwlte_t *me)
{
    if (!me || !me->lock) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_api_calls > 0) {
        me->active_api_calls--;
    }
    bool api_idle = me->active_api_calls == 0;
    SemaphoreHandle_t done_sema = me->api_done_sema;
    xSemaphoreGive(me->lock);

    if (api_idle && done_sema) {
        xSemaphoreGive(done_sema);
    }
}

static esp_err_t wait_api_calls_idle(lwlte_t *me)
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

static esp_err_t wait_callbacks_idle(lwlte_t *me, bool claim_waiter)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    int active = me->callback_active;
    SemaphoreHandle_t done_sema = me->callback_done_sema;
    if (active == 0) {
        xSemaphoreGive(me->lock);
        return ESP_OK;
    }
    if (callback_task_active_locked(me, xTaskGetCurrentTaskHandle())) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!done_sema || (claim_waiter && me->callback_waiting)) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (claim_waiter) {
        me->callback_waiting = true;
    }
    xSemaphoreGive(me->lock);

    while (active > 0) {
        xSemaphoreTake(done_sema, portMAX_DELAY);

        xSemaphoreTake(me->lock, portMAX_DELAY);
        active = me->callback_active;
        done_sema = me->callback_done_sema;
        if (active > 0 && !done_sema) {
            if (claim_waiter) {
                me->callback_waiting = false;
            }
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(me->lock);
    }

    return ESP_OK;
}

static bool callback_task_active_locked(const lwlte_t *me, TaskHandle_t task)
{
    if (!me || !task) {
        return false;
    }

    for (int i = 0; i < LWLTE_CALLBACK_TASKS_MAX; i++) {
        if (me->callback_tasks[i] == task) {
            return true;
        }
    }

    return me->callback_task_overflow > 0;
}

static bool add_callback_task_locked(lwlte_t *me, TaskHandle_t task)
{
    if (!me || !task) {
        return false;
    }

    for (int i = 0; i < LWLTE_CALLBACK_TASKS_MAX; i++) {
        if (me->callback_tasks[i] == task) {
            me->callback_task_counts[i]++;
            return true;
        }
    }

    for (int i = 0; i < LWLTE_CALLBACK_TASKS_MAX; i++) {
        if (!me->callback_tasks[i]) {
            me->callback_tasks[i] = task;
            me->callback_task_counts[i] = 1;
            return true;
        }
    }

    me->callback_task_overflow++;
    return false;
}

static void remove_callback_task_locked(lwlte_t *me, TaskHandle_t task)
{
    if (!me || !task) {
        return;
    }

    for (int i = 0; i < LWLTE_CALLBACK_TASKS_MAX; i++) {
        if (me->callback_tasks[i] == task) {
            if (me->callback_task_counts[i] > 1) {
                me->callback_task_counts[i]--;
                return;
            }
            me->callback_task_counts[i] = 0;
            me->callback_tasks[i] = NULL;
            return;
        }
    }

    if (me->callback_task_overflow > 0) {
        me->callback_task_overflow--;
    }
}

static void wake_ready_waiters_locked(lwlte_t *me)
{
    if (!me || !me->ready_sema) {
        return;
    }

    int waiter_count = me->ready_waiter_count;
    for (int i = 0; i < waiter_count; i++) {
        xSemaphoreGive(me->ready_sema);
    }
}

static void restore_after_destroy_failure(lwlte_t *me)
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

static esp_err_t destroy_owned_resources(lwlte_t *me)
{
    esp_err_t ret = ESP_OK;

    if (me->mqtt) {
        ret = mqtt_client_destroy(me->mqtt);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy MQTT client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->mqtt = NULL;
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
