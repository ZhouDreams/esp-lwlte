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
    esp_event_loop_handle_t event_loop;
    int init_error_code;
    int active_api_calls;
    int ready_waiter_count;
    bool ready;
    bool init_failed;
    bool destroying;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_create_empty(lwlte_handle_t **out_lte);
esp_err_t lwlte_wait_ready(lwlte_handle_t *me, uint32_t timeout_ms);

/**
 * @brief 门面内部 READY/ERROR 事件处理器
 * @details Facade internal READY/ERROR event handler
 * @note 注册到共享事件总线，驱动 lwlte_wait_ready 同步。
 * @param[in] arg LTE 用户门面句柄
 * @param[in] base 事件 base
 * @param[in] id 事件 ID
 * @param[in] data 事件数据，指向 lwlte_event_data_t
 */
void facade_ready_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
