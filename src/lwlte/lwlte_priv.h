/**
 * @file lwlte_priv.h
 * @brief LTE 用户门面内部接口
 * @details LTE user facade internal interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "at_engine.h"
#include "core.h"
#include "lwlte.h"
#include "modem.h"
#include "mqtt_client.h"
#include "ping_client.h"

/*********************
 *      DEFINES
 *********************/
#define LWLTE_CALLBACK_TASKS_MAX 4

/**********************
 *      TYPEDEFS
 **********************/

struct lwlte_handle {
    at_engine_handle_t *at;
    modem_handle_t *modem;
    core_handle_t *core;
    mqtt_client_handle_t *mqtt;
    ping_client_handle_t *ping;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t ready_sema;
    SemaphoreHandle_t api_done_sema;
    SemaphoreHandle_t callback_done_sema;
    lwlte_event_callback_t event_callback;
    void *event_user_ctx;
    int init_error_code;
    int active_api_calls;
    int callback_active;
    int callback_task_overflow;
    int ready_waiter_count;
    TaskHandle_t callback_tasks[LWLTE_CALLBACK_TASKS_MAX];
    int callback_task_counts[LWLTE_CALLBACK_TASKS_MAX];
    bool ready;
    bool init_failed;
    bool destroying;
    bool callback_waiting;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_create_empty(lwlte_handle_t **out_lte);
esp_err_t lwlte_wait_ready(lwlte_handle_t *me, uint32_t timeout_ms);
void lwlte_handle_core_event(core_handle_t *core, core_event_id_t event_id,
                             const core_event_data_t *data, void *user_ctx);
void lwlte_handle_mqtt_event(mqtt_client_handle_t *mqtt,
                             mqtt_client_event_id_t event_id,
                             const mqtt_client_event_data_t *data,
                             void *user_ctx);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
