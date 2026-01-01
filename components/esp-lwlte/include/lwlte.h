/*
    File: lwlte.h
    Author: JovisDreams
    Date: 2025-12-31
    Description: esp-lwlte api header file
    Platform: ESP-IDF
*/
#pragma once

#include "lwlte_sys_types.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    lwlte_base_type_t gpio_en_num; // GPIO number of the EN pin
    lwlte_uart_num_t uart_num; // UART number
    lwlte_base_type_t uart_tx_io_num; // UART TX IO number
    lwlte_base_type_t uart_rx_io_num; // UART RX IO number
    lwlte_base_type_t uart_buf_size; // UART buffer size
    lwlte_base_type_t uart_baudrate; // UART baudrate
    lwlte_tick_t at_wait_ticks; // AT command wait time
    lwlte_base_type_t init_max_time_ms; // Initialization maximum time
} lwlte_config_t;

esp_err_t lwlte_core_init(const lwlte_config_t* config);


#ifdef __cplusplus
}
#endif