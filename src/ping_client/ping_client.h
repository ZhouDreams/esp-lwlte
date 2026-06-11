/**
 * @file ping_client.h
 * @brief Ping 诊断服务层间接口
 * @details Ping diagnostic service inter-layer interface
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
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct ping_client_handle ping_client_handle_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} ping_client_request_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
ping_client_handle_t *ping_client_create(core_handle_t *core);
esp_err_t ping_client_destroy(ping_client_handle_t *me);
esp_err_t ping_client_ping(ping_client_handle_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
