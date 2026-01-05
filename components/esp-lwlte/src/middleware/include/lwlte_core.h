/*
    File: lwlte_core.h
    Author: JovisDreams
    Date: 2025-12-28
    Description: esp-lwlte core header file
*/
#pragma once

#include "lwlte.h"
#include "lwlte_err.h"
#include "lwlte_sys_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* AT commands */
#define AT_CIMI "AT+CIMI\r\n"
#define AT_RESET "AT+RESET\r\n" //重启模块
#define AT_CPIN "AT+CPIN?\r\n" //查询SIM卡是否准备好
#define AT_CSQ "AT+CSQ\r\n" //查询信号强度
#define AT_CGATT "AT+CGATT?\r\n" //查询上网服务是否激活
#define AT_CIPSHUT "AT+CIPSHUT\r\n" //关闭移动场景
#define AT_CSTT "AT+CSTT\r\n" //启动任务并设置接入点 APN、用户名、密码
#define AT_CIICR "AT+CIICR\r\n" //激活移动场景(或发起 GPRS 或 CSD 无线连接)
#define AT_CIFSR "AT+CIFSR\r\n" //查询本地 IP 地址
/* Event Group Bits */
#define LWLTE_FLAGS_CORE_INITIALIZING BIT0 // module is initializing
#define LWLTE_FLAGS_CORE_INITIALIZED BIT1 // module is initialized
#define LWLTE_FLAGS_RX_TASK_RUNNING BIT2 // uart_rx_task is running
#define LWLTE_FLAGS_INIT_TASK_RUNNING BIT3 // init_task is running
#define LWLTE_FLAGS_MODULE_READY BIT4 // module sent "RDY" after reset
#define LWLTE_FLAGS_MODULE_SIM_CARD_READY BIT5 // SIM card is ready
#define LWLTE_FLAGS_MODULE_SIGNAL_GOOD BIT6 // signal is good
#define LWLTE_FLAGS_MODULE_PDN_ACTIVATED BIT7 // PDN is activated
#define LWLTE_FLAGS_MODULE_IP_GPRS_ACTIVATED BIT8 // IP GPRS is activated
#define LWLTE_FLAGS_MODULE_IP_ADDRESS_ASSIGNED BIT9 // IP address is assigned
#define LWLTE_FLAGS_MODULE_NETWORK_CONNECTED BIT10 // network is connected
#define LWLTE_FLAGS_AT_CMD_IS_SENDING BIT11 // AT command is sending

#ifdef __cplusplus
extern "C" {
#endif


lwlte_err_t lwlte_core_send_at_cmd_internal(const char* cmd, 
    const char* wait_str, 
    const char* error_str, 
    lwlte_base_type_t wait_time_ms, 
    char* response_buf, 
    lwlte_base_type_t response_buf_size
);

lwlte_err_t lwlte_core_input(char* input, lwlte_base_type_t input_size);

lwlte_err_t lwlte_core_init_internal(const lwlte_config_t* config);

lwlte_err_t lwlte_core_deinit_internal(void);

lwlte_err_t lwlte_core_network_activate_internal(void);

bool lwlte_core_get_module_ready_internal(void);

bool lwlte_core_get_module_sim_card_ready_internal(void);

bool lwlte_core_get_module_signal_good_internal(void);

bool lwlte_core_get_module_ip_gprs_activated_internal(void);

bool lwlte_core_get_module_pdn_activated_internal(void);

bool lwlte_core_get_network_connected_internal(void);

lwlte_err_t lwlte_core_wait_module_ready(lwlte_base_type_t timeout_ms);

lwlte_err_t lwlte_core_wait_network_connected(lwlte_base_type_t timeout_ms);   

#ifdef __cplusplus
}
#endif