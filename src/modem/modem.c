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
static bool event_task_should_stop(modem_t *me);

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
static esp_err_t check_ready(modem_t *me, bool allow_created);

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
static esp_err_t call_no_arg(modem_t *me, esp_err_t (*fn)(modem_t *me));

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

esp_err_t modem_base_init(modem_t *me, const char *name, at_engine_t *at,
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
    modem_base_deinit(me);
    return ret;
}

void modem_base_deinit(modem_t *me)
{
    if (!me) {
        return;
    }

    esp_err_t ret = modem_base_stop_event_task(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "stop event task before deinit failed: %s", esp_err_to_name(ret));
        if (me->event_task) {
            return;
        }
    }

    if (me->event_queue) {
        vQueueDelete(me->event_queue);
        me->event_queue = NULL;
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
}

esp_err_t modem_base_stop_event_task(modem_t *me)
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

esp_err_t modem_post_event(modem_t *me, const modem_event_t *event)
{
    ESP_RETURN_ON_FALSE(me && event && me->lock && me->event_queue,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == MODEM_STATE_DESTROYING) {
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

esp_err_t modem_set_state(modem_t *me, modem_state_t state)
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

esp_err_t modem_destroy(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (me->event_task && xTaskGetCurrentTaskHandle() == me->event_task) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    modem_state_t state = me->state;
    bool allowed = (state == MODEM_STATE_CREATED ||
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

    modem_base_deinit(me);
    free(me);
    return ret;
}

esp_err_t modem_init(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->init,
                        ESP_ERR_NOT_SUPPORTED, TAG, "init not supported");

    return call_no_arg(me, me->ops->init);
}

esp_err_t modem_reset(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->reset,
                        ESP_ERR_NOT_SUPPORTED, TAG, "reset not supported");

    return call_no_arg(me, me->ops->reset);
}

esp_err_t modem_register_event_callback(modem_t *me,
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

esp_err_t modem_get_state(modem_t *me, modem_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->state;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t modem_get_info(modem_t *me, modem_info_t *info)
{
    ESP_RETURN_ON_FALSE(me && info, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_info,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_info not supported");

    return me->ops->get_info(me, info);
}

esp_err_t modem_get_sim_status(modem_t *me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_sim_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_sim_status not supported");

    return me->ops->get_sim_status(me, status);
}

esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_signal,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_signal not supported");

    return me->ops->get_signal(me, signal);
}

esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_registration,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_registration not supported");

    return me->ops->get_registration(me, status);
}

esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_packet_attach_status,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "get_packet_attach_status not supported");

    return me->ops->get_packet_attach_status(me, attached);
}

esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn)
{
    ESP_RETURN_ON_FALSE(me && apn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->set_apn,
                        ESP_ERR_NOT_SUPPORTED, TAG, "set_apn not supported");

    return me->ops->set_apn(me, cid, apn);
}

esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->activate_pdp,
                        ESP_ERR_NOT_SUPPORTED, TAG, "activate_pdp not supported");

    return me->ops->activate_pdp(me, cid);
}

esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->deactivate_pdp,
                        ESP_ERR_NOT_SUPPORTED, TAG, "deactivate_pdp not supported");

    return me->ops->deactivate_pdp(me, cid);
}

esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_pdp_context,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_pdp_context not supported");

    return me->ops->get_pdp_context(me, cid, pdp);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void event_task(void *arg)
{
    modem_t *me = (modem_t *)arg;

    while (!event_task_should_stop(me)) {
        modem_event_t event = {0};
        if (xQueueReceive(me->event_queue, &event,
                          pdMS_TO_TICKS(MODEM_EVENT_TASK_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (event_task_should_stop(me)) {
            break;
        }

        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->destroying || me->state == MODEM_STATE_DESTROYING) {
            xSemaphoreGive(me->lock);
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
        }
    }

    xSemaphoreGive(me->event_task_done_sema);
    vTaskDelete(NULL);
}

static bool event_task_should_stop(modem_t *me)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool should_stop = me->event_task_stop_requested || me->destroying ||
                       me->state == MODEM_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    return should_stop;
}

static esp_err_t check_ready(modem_t *me, bool allow_created)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    modem_state_t state = me->state;
    xSemaphoreGive(me->lock);

    if (destroying) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state == MODEM_STATE_CREATED && allow_created) {
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

static esp_err_t call_no_arg(modem_t *me, esp_err_t (*fn)(modem_t *me))
{
    ESP_RETURN_ON_FALSE(me && fn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    return fn(me);
}
