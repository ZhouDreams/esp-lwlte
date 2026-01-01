/*
    File: lwlte_sys_log.h
    Author: JovisDreams
    Date: 2025-12-30
    Description: System Log header file
    Platform: ESP-IDF
*/
#pragma once

#include "lwlte_err.h"
#include "esp_log.h"

#define LWLTE_LOGE(TAG, fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LWLTE_LOGW(TAG, fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LWLTE_LOGI(TAG, fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LWLTE_LOGD(TAG, fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
extern "C" {
#endif

lwlte_err_t lwlte_sys_log_error(const char* TAG, const char* format, ...);

lwlte_err_t lwlte_sys_log_warning(const char* TAG, const char* format, ...);

lwlte_err_t lwlte_sys_log_info(const char* TAG, const char* format, ...);

lwlte_err_t lwlte_sys_log_debug(const char* TAG, const char* format, ...);

#ifdef __cplusplus
}
#endif