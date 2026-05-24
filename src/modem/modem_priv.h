/**
 * @file modem_priv.h
 * @brief 调制解调器内部接口
 * @details Modem internal interface
 * @author JovisDreams
 * @date 2026-05-23
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stddef.h>

#include "at_engine.h"
#include "modem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define MODEM_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/**********************
 *      TYPEDEFS
 **********************/
typedef struct modem_ops {
    esp_err_t (*destroy)(modem_t *me);
    esp_err_t (*init)(modem_t *me);
    esp_err_t (*reset)(modem_t *me);
    esp_err_t (*get_info)(modem_t *me, modem_info_t *info);
    esp_err_t (*get_sim_status)(modem_t *me, modem_sim_status_t *status);
    esp_err_t (*get_signal)(modem_t *me, modem_signal_t *signal);
    esp_err_t (*get_registration)(modem_t *me, modem_reg_status_t *status);
    esp_err_t (*set_apn)(modem_t *me, uint8_t cid, const char *apn);
    esp_err_t (*activate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*deactivate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*get_pdp_context)(modem_t *me, uint8_t cid,
                                  modem_pdp_context_t *pdp);
} modem_ops_t;

struct modem {
    const modem_ops_t *ops;
    at_engine_t *at;
    SemaphoreHandle_t lock;
    QueueHandle_t event_queue;
    TaskHandle_t event_task;
    SemaphoreHandle_t event_task_done_sema;
    SemaphoreHandle_t event_cb_done_sema;
    modem_event_callback_t event_cb;
    void *event_user_ctx;
    int event_cb_active;
    modem_state_t state;
    bool destroying;
    bool event_task_stop_requested;
    const char *name;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/
esp_err_t modem_base_init(modem_t *me, const char *name, at_engine_t *at,
                          const modem_ops_t *ops, int event_queue_size,
                          int event_task_stack, int event_task_priority);
void modem_base_deinit(modem_t *me);
esp_err_t modem_base_stop_event_task(modem_t *me);
esp_err_t modem_post_event(modem_t *me, const modem_event_t *event);
esp_err_t modem_set_state(modem_t *me, modem_state_t state);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
