/**
 * @file ping_client_priv.h
 * @brief Ping 诊断服务内部接口
 * @details Ping diagnostic service internal interface
 * @author JovisDreams
 * @date 2026-05-28
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>

#include "ping_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*********************
 *      DEFINES
 *********************/
#define PING_CLIENT_MAX_COUNT              100U
#define PING_CLIENT_MAX_DATA_LEN           1024U
#define PING_CLIENT_MAX_TIMEOUT_100MS      600U
#define PING_CLIENT_DEFAULT_OVERHEAD_MS    5000U

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    SemaphoreHandle_t done_sema;
    core_cmd_result_t core_result;
    esp_err_t esp_result;
    bool completed;
} ping_wait_ctx_t;

struct ping_client_handle {
    core_handle_t *core;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t active_done_sema;
    size_t active_calls;
    bool destroying;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
