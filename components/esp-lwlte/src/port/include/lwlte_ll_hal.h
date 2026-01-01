/*
    File: lwlte_ll_uart.h
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer UART driver header file
    Platform: ESP-IDF
*/

#pragma once

#include "driver/uart.h"
#include "lwlte_sys_types.h"
#include "lwlte_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lwlte_base_type_t uart_num;
    lwlte_base_type_t uart_tx_io_num;
    lwlte_base_type_t uart_rx_io_num;
    lwlte_base_type_t uart_buf_size;
    lwlte_uart_config_t uart_config;
} lwlte_ll_uart_config_t;

lwlte_err_t lwlte_ll_uart_write(const char* data, size_t size);

lwlte_err_t lwlte_ll_uart_init(lwlte_ll_uart_config_t *config);

lwlte_err_t lwlte_ll_uart_deinit(lwlte_base_type_t uart_num);

lwlte_err_t lwlte_ll_gpio_init(lwlte_base_type_t gpio_num);

#ifdef __cplusplus
}
#endif