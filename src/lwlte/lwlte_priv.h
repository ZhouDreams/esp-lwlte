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

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

struct lwlte {
    at_engine_t *at;
    modem_t *modem;
    core_t *core;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t ready_sema;
    SemaphoreHandle_t api_done_sema;
    SemaphoreHandle_t callback_done_sema;
    lwlte_event_callback_t event_callback;
    void *event_user_ctx;
    int init_error_code;
    int active_api_calls;
    int callback_active;
    int ready_waiter_count;
    TaskHandle_t callback_task;
    bool ready;
    bool init_failed;
    bool destroying;
    bool callback_waiting;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_create_empty(lwlte_t **out_lte);
esp_err_t lwlte_wait_ready(lwlte_t *me, uint32_t timeout_ms);
void lwlte_handle_core_event(core_t *core, core_event_id_t event_id,
                             const core_event_data_t *data, void *user_ctx);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
