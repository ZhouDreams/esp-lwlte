/*
    File: lwlte.c
    Author: JovisDreams
    Date: 2025-12-31
    Description: esp-lwlte api source file
    Platform: ESP-IDF
*/
#include "lwlte.h"
#include "lwlte_core.h"
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

static esp_err_t lwlte_err_2_esp_err(lwlte_err_t err)
{
    switch (err)
    {
        case LWLTE_OK:
            return ESP_OK;
        case LWLTE_ERROR:
            return ESP_FAIL;
        case LWLTE_TIMEOUT:
            return ESP_ERR_TIMEOUT;
        case LWLTE_INVALID_ARG:
            return ESP_ERR_INVALID_ARG;
        case LWLTE_NOT_SUPPORTED:
            return ESP_ERR_NOT_SUPPORTED;
        case LWLTE_NOT_INITIALIZED:
            return ESP_ERR_NOT_ALLOWED;
        case LWLTE_ALREADY_INITIALIZED:
            return ESP_ERR_NOT_ALLOWED;
        default:
            return ESP_FAIL;
    }
    return ESP_FAIL;
}

esp_err_t lwlte_core_init(const lwlte_config_t* config)
{
    return lwlte_err_2_esp_err(lwlte_core_init_internal(config));
}