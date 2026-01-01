/*
    File: lwlte_sys_log.h
    Author: JovisDreams
    Date: 2025-12-30
    Description: System Log header file
    Platform: ESP-IDF
*/

#include "esp_log_level.h"
#include "lwlte_err.h"
#include "lwlte_sys_types.h"
#include "esp_log.h"
#include <string.h>

lwlte_err_t lwlte_sys_log_error(const char* TAG, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    char* log_message = (char*)malloc((strlen(TAG) + strlen(format) + 64)* sizeof(char));
    sprintf(log_message, "%s: %s\n", TAG, format);
    esp_log_writev(ESP_LOG_ERROR, TAG, log_message, ap);
    va_end(ap);
    free(log_message);
    return LWLTE_OK;
}

lwlte_err_t lwlte_sys_log_warning(const char* TAG, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    char* log_message = (char*)malloc((strlen(TAG) + strlen(format) + 64)* sizeof(char));
    sprintf(log_message, "%s: %s\n", TAG, format);
    esp_log_writev(ESP_LOG_WARN, TAG, log_message, ap);
    va_end(ap);
    free(log_message);
    return LWLTE_OK;
}

lwlte_err_t lwlte_sys_log_info(const char* TAG, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    char* log_message = (char*)malloc((strlen(TAG) + strlen(format) + 64)* sizeof(char));
    sprintf(log_message, "%s: %s\n", TAG, format);
    esp_log_writev(ESP_LOG_INFO, TAG, log_message, ap);
    va_end(ap);
    free(log_message);
    return LWLTE_OK;
}

lwlte_err_t lwlte_sys_log_debug(const char* TAG, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    char* log_message = (char*)malloc((strlen(TAG) + strlen(format) + 64)* sizeof(char));
    sprintf(log_message, "%s: %s\n", TAG, format);
    esp_log_writev(ESP_LOG_DEBUG, TAG, log_message, ap);
    va_end(ap);
    free(log_message);
    return LWLTE_OK;
}