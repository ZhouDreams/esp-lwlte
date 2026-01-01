/*
    File: lwlte_sys_types.h
    Author: JovisDreams
    Date: 2025-12-29
    Description: System Types encapsulation header file
    Platform: ESP-IDF
*/
#pragma once

#include "freertos/FreeRTOS.h"
#include "driver/uart.h"

#define LWLTE_TICK_FOREVER portMAX_DELAY
#define LWLTE_SYS_WAIT_FOREVER  (UINT32_MAX)

#ifdef __cplusplus
extern "C" {
#endif

typedef BaseType_t lwlte_base_type_t;
typedef TickType_t lwlte_tick_t;
typedef uart_config_t lwlte_uart_config_t;
typedef uart_port_t lwlte_uart_num_t;


#ifdef __cplusplus
}
#endif