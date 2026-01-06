/*
    File: lwlte_core.c
    Author: JovisDreams
    Date: 2025-12-28
    Description: esp-lwlte core source file
*/

#include "lwlte_core.h"
#include "lwlte_sys_types.h"
#include "lwlte_ll_hal.h"
#include "lwlte_err.h"
#include "lwlte_sys_flags.h"
#include "lwlte_sys_queue.h"
#include "lwlte_sys_mutex.h"
#include "lwlte_sys_thread.h"
#include "lwlte_sys_log.h"
#include "lwlte_sys_mem.h"
#include "string.h"
#include <stdbool.h>
#include <string.h>

#define ADD_TO_LINE(line, line_length, c) { line[line_length] = c; line_length++; line[line_length] = '\0'; }
#define RESET_LINE(line, line_length) { line_length = 0; line[0] = '\0'; }
#define GET_CSQ(response, data_pointer, csq) { data_pointer = strstr(response, "+CSQ: ") + 6; csq = *(data_pointer + 1) == ','?(*data_pointer - '0') : (*data_pointer - '0')*10 + (*(data_pointer+1) - '0'); }

static const char* TAG = "lwlte_core";

static struct {
    lwlte_config_t config; // config of lwlte_core
    lwlte_sys_flags_t flags;
    lwlte_sys_queue_t core_input_queue;
    lwlte_sys_thread_t network_activate_thread_handle;
    lwlte_sys_thread_t core_worker_thread_handle;
    char* core_input_buf;
    struct at_waiter_t {
        char *at_wait_string;
        char *at_error_string;
        char *at_response;
        lwlte_sys_semaphore_t done;
        lwlte_sys_mutex_t lock;
        bool response_ok;
        bool response_error;
    } at_waiter;
    lwlte_tick_t init_start_time_ms;

} s_lwlte_core_context;

lwlte_err_t lwlte_core_send_at_cmd_internal(const char* cmd, 
    const char* wait_str, 
    const char* error_str, 
    lwlte_base_type_t wait_time_ms, 
    char* response_buf, 
    lwlte_base_type_t response_buf_size)
{
    /* Check if the module is initialized */
    if (s_lwlte_core_context.flags == NULL || s_lwlte_core_context.core_input_queue == NULL) {
        return LWLTE_NOT_INITIALIZED;
    }
    /* Check if the core is initialized */
    if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_CORE_INITIALIZED)) {
        return LWLTE_NOT_INITIALIZED;
    }
    /* Check if the arguments are valid */
    if (cmd == NULL || wait_str == NULL || error_str == NULL || (response_buf_size < 0)) {
        return LWLTE_INVALID_ARG;
    }
    /* Check if the wait_time_ticks is valid */
    if (wait_time_ms <= 0) {
        return LWLTE_INVALID_ARG;
    }
    /* Lock the at_waiter */
    lwlte_sys_mutex_lock(s_lwlte_core_context.at_waiter.lock);
    lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_AT_CMD_IS_SENDING);
    s_lwlte_core_context.at_waiter.response_ok = false;
    s_lwlte_core_context.at_waiter.response_error = false;
    /* Reset the at_response */
    s_lwlte_core_context.at_waiter.at_response[0] = '\0';
    strcpy(s_lwlte_core_context.at_waiter.at_error_string, error_str);
    strcpy(s_lwlte_core_context.at_waiter.at_wait_string, wait_str);
    /* Send the AT command */
    lwlte_ll_uart_write(cmd, strlen(cmd));
    /* Find \n\r and replace \n with \0 , then log the command*/
    char *cmd_copy = lwlte_sys_mem_malloc(strlen(cmd) + 1);
    strcpy(cmd_copy, cmd);
    char *pos = strchr(cmd_copy, '\n');
    if (pos != NULL) {
        *pos = '\0';
    }
    LWLTE_LOGI(TAG, "TX:|%s", cmd_copy);
    free((void*)cmd_copy);
    /* Wait for the response */
    lwlte_sys_semaphore_wait(s_lwlte_core_context.at_waiter.done, wait_time_ms);
    /* If the response_buf is not NULL, copy the response to the response_buf */
    if (response_buf != NULL) {
        strncpy(response_buf, s_lwlte_core_context.at_waiter.at_response, 
            response_buf_size > strlen(s_lwlte_core_context.at_waiter.at_response) ? strlen(s_lwlte_core_context.at_waiter.at_response) : response_buf_size);
    }
    /* Unlock the at_waiter */
    lwlte_sys_flags_clear(s_lwlte_core_context.flags, LWLTE_FLAGS_AT_CMD_IS_SENDING);
    lwlte_sys_mutex_unlock(s_lwlte_core_context.at_waiter.lock);
    if (s_lwlte_core_context.at_waiter.response_ok) {
        return LWLTE_OK;
    }
    else if (s_lwlte_core_context.at_waiter.response_error) {
        return LWLTE_ERROR;
    }
    return LWLTE_TIMEOUT;
}

lwlte_err_t lwlte_core_input(char* input, lwlte_base_type_t input_size)
{
    /* Check if the module is initialized */
    if (s_lwlte_core_context.flags == NULL || s_lwlte_core_context.core_input_queue == NULL) {
        return LWLTE_NOT_INITIALIZED;
    }
    /* Check if the core is initialized */
    if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_CORE_INITIALIZED)) {
        return LWLTE_NOT_INITIALIZED;
    }
    /* Check if the arguments are valid */
    if (input == NULL || input_size == 0) {
        return LWLTE_INVALID_ARG;
    }
    /* Send the input to the core_input_queue */
    lwlte_sys_queue_send(s_lwlte_core_context.core_input_queue, input, UINT32_MAX);
    return LWLTE_OK;
}

static void handle_one_line(const char* line, int line_length)
{
    /* If the line contains "RDY" and the module is not ready, set the module ready flag */
    if (strstr(line, "RDY") != NULL) {
        if ((lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY) == 0))
        {
            lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY);
            LWLTE_LOGI(TAG, "Module reset is done.");
        }
        else if ((lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY) == 1))
        {
            LWLTE_LOGE(TAG, "Multiple \"RDY\" responses received, you may check if the power supply of LTE module is stable.");
        }
    }
    /* If the line contains "+CGEV: ME PDN ACT", it is a URC from the module that the PDN is activated */
    else if (strstr(line, "+CGEV: ME PDN ACT") != NULL && (lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_PDN_ACTIVATED) == 0)) {
        lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_PDN_ACTIVATED);
        LWLTE_LOGI(TAG, "PDN is activated.");
    }
    /* If the line contains +MSUB:, it is a message from mqtt subscription */
    else if (strstr(line, "+MSUB:") != NULL) {
        LWLTE_LOGI(TAG, "Received MSUB: %s", line);
    }
    /* If the LWLTE is sending an AT command, append the line to the response and check if the response contains the wait string or the error string */
    else if (lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_AT_CMD_IS_SENDING)) {
        strcat(s_lwlte_core_context.at_waiter.at_response, line);
        /* If the response contains the wait response or the error response, give the done semaphore */
        if (strstr(s_lwlte_core_context.at_waiter.at_response, s_lwlte_core_context.at_waiter.at_wait_string) != NULL) {
            s_lwlte_core_context.at_waiter.response_ok = true;
            lwlte_sys_semaphore_signal(s_lwlte_core_context.at_waiter.done);
        }
        else if (strstr(s_lwlte_core_context.at_waiter.at_response, s_lwlte_core_context.at_waiter.at_error_string) != NULL) {
            s_lwlte_core_context.at_waiter.response_error = true;
            lwlte_sys_semaphore_signal(s_lwlte_core_context.at_waiter.done);
        }
    }
}

static void core_worker_task(void *pvParameters)
{
    LWLTE_LOGI(TAG, "core_worker_task starts.");
    /* Allocate the core_input_buf */
    s_lwlte_core_context.core_input_buf = lwlte_sys_mem_malloc(s_lwlte_core_context.config.uart_buf_size);
    while (1) {
        /* Receive the input from the core_input_queue */
        lwlte_sys_queue_recv(s_lwlte_core_context.core_input_queue, 
            s_lwlte_core_context.core_input_buf, LWLTE_SYS_WAIT_FOREVER);
        /* Process the input line by line*/
        int line_length = 0;
        char line[s_lwlte_core_context.config.uart_buf_size];
        line[0] = '\0';
        for (int i = 0; i < s_lwlte_core_context.config.uart_buf_size; i++) {
            /* Append the character to the line */
            char c = (char)s_lwlte_core_context.core_input_buf[i];
            if (c == '\n') {
                /* We do this so that the log does not log an extra new line (not to log the '\n') */
                LWLTE_LOGI(TAG, "RX:|%s", line);
                ADD_TO_LINE(line, line_length, c);
                handle_one_line(line, line_length);
                /* Reset the line */
                RESET_LINE(line, line_length);
            }
            else if (c == '\0') {
                ADD_TO_LINE(line, line_length, c);
                break;
            }
            else {
                ADD_TO_LINE(line, line_length, c);
            }
        }
    }
    free(s_lwlte_core_context.core_input_buf);
}

static lwlte_err_t lwlte_core_create_worker_thread(void)
{
    /* Create the core_worker_thread */
    lwlte_sys_thread_cfg_t core_worker_thread_config = {
        .name = "core_worker_thread",
        .priority = tskIDLE_PRIORITY + 10,
        .stack_size = 4096,
        .arg = NULL
    };
    s_lwlte_core_context.core_worker_thread_handle = lwlte_sys_thread_create(core_worker_task, &core_worker_thread_config);
    return LWLTE_OK;
}

lwlte_err_t lwlte_core_init_internal(const lwlte_config_t* config)
{
    LWLTE_LOGI(TAG, "lwlte_core_init_internal starts.");
    /* Record the start time */
    s_lwlte_core_context.init_start_time_ms = lwlte_sys_time_get_ms();
    /* If the config is NULL, return an error */
    if (config == NULL) {
        return LWLTE_INVALID_ARG;
    }
    /* If the module is already initialized, return an error */
    if (s_lwlte_core_context.flags != NULL || s_lwlte_core_context.core_input_queue != NULL) {
        return LWLTE_ALREADY_INITIALIZED;
    }
    /* Copy the config */
    s_lwlte_core_context.config = *config;
    /* Create the flags and clear all the bits */
    s_lwlte_core_context.flags = lwlte_sys_flags_create();
    lwlte_sys_flags_clear(s_lwlte_core_context.flags, LWLTE_FLAGS_ALL_BITS);
    /* Set the initializing bit */
    lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_CORE_INITIALIZING);
    /* Create the core_input_queue */
    s_lwlte_core_context.core_input_queue = lwlte_sys_queue_create(s_lwlte_core_context.config.uart_buf_size, 10);
    /* Initialize the at_waiter */
    s_lwlte_core_context.at_waiter.done = lwlte_sys_semaphore_create();
    s_lwlte_core_context.at_waiter.lock = lwlte_sys_mutex_create();
    s_lwlte_core_context.at_waiter.at_response = lwlte_sys_mem_malloc(s_lwlte_core_context.config.uart_buf_size);
    s_lwlte_core_context.at_waiter.at_response[0] = '\0';
    s_lwlte_core_context.at_waiter.at_error_string = lwlte_sys_mem_malloc(s_lwlte_core_context.config.uart_buf_size);
    s_lwlte_core_context.at_waiter.at_error_string[0] = '\0';
    s_lwlte_core_context.at_waiter.at_wait_string = lwlte_sys_mem_malloc(s_lwlte_core_context.config.uart_buf_size);
    s_lwlte_core_context.at_waiter.at_wait_string[0] = '\0';
    /* Initialize the UART */
    lwlte_ll_uart_config_t uart_config = {
        .uart_num = s_lwlte_core_context.config.uart_num,
        .uart_tx_io_num = s_lwlte_core_context.config.uart_tx_io_num,
        .uart_rx_io_num = s_lwlte_core_context.config.uart_rx_io_num,
        .uart_buf_size = s_lwlte_core_context.config.uart_buf_size,
        .uart_config = {
            .baud_rate = config->uart_baudrate,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        },
    };
    if (lwlte_ll_uart_init(&uart_config) != LWLTE_OK) {
        return LWLTE_ERROR;
    }
    /* Create the core_worker_thread */
    if (lwlte_core_create_worker_thread() != LWLTE_OK) {
        return LWLTE_ERROR;
    }
    /* Initialize the GPIO */
    if (lwlte_ll_gpio_init(s_lwlte_core_context.config.gpio_en_num) != LWLTE_OK) {
        return LWLTE_ERROR;
    }
    /* Initialize the network activate task */
    if (lwlte_core_network_activate_internal() != LWLTE_OK) {
        return LWLTE_ERROR;
    }
    /* Set the initialized bit */
    lwlte_sys_flags_clear(s_lwlte_core_context.flags, LWLTE_FLAGS_CORE_INITIALIZING);
    lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_CORE_INITIALIZED);
    return LWLTE_OK;
}

lwlte_err_t lwlte_core_deinit_internal(void)
{
    return LWLTE_OK;
}

lwlte_base_type_t lwlte_core_get_signal_strength(void)
{
    if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY)) {
        LWLTE_LOGE(TAG, "LWLTE module is not ready! Please call lwlte_core_init() first.");
        return -1;
    }
    char response[s_lwlte_core_context.config.uart_buf_size];
    if (lwlte_core_send_at_cmd_internal(AT_CSQ, "OK", "ERROR", s_lwlte_core_context.config.at_wait_ticks, response, sizeof(response)) != LWLTE_OK) {
        return -1;
    }
    char *data_pointer = NULL;
    lwlte_base_type_t csq = 0;
    GET_CSQ(response, data_pointer, csq);
    return csq;
}

static void network_activate_task(void *pvParameters)
{
    LWLTE_LOGI(TAG, "network_activate_task starts.");
    lwlte_sys_flags_wait(s_lwlte_core_context.flags, 
        LWLTE_FLAGS_MODULE_READY, true, 
        false, LWLTE_SYS_WAIT_FOREVER);
    while (lwlte_sys_time_get_ms() - s_lwlte_core_context.init_start_time_ms < s_lwlte_core_context.config.init_max_time_ms) {
        lwlte_sys_thread_sleep(1000);
        /* Check if the SIM card is ready */
        if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIM_CARD_READY)) {
            char response[s_lwlte_core_context.config.uart_buf_size];
            lwlte_core_send_at_cmd_internal(AT_CPIN, "+CPIN: READY", "ERROR", s_lwlte_core_context.config.at_wait_ticks, response, sizeof(response));
            if (strstr(response, "+CPIN: READY") != NULL) {
                lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIM_CARD_READY);
                LWLTE_LOGI(TAG, "SIM card is ready");
            }
            else {
                LWLTE_LOGE(TAG, "Failed to initialize the LWLTE module: SIM card not ready");
                continue;
            }
        }
        lwlte_sys_thread_sleep(1000);
        /* Check if the signal is good */
        if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIGNAL_GOOD)) {
            lwlte_base_type_t csq = lwlte_core_get_signal_strength();
            if (csq == 99 ||csq > 9) {
                lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIGNAL_GOOD);
                LWLTE_LOGI(TAG, "Signal is good");
            }
            else {
                LWLTE_LOGE(TAG, "Failed to initialize the LWLTE module: Signal is not good");
                continue;
            }
        }
        lwlte_sys_thread_sleep(1000);
        /* Check if the PDN is activated */
        if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_PDN_ACTIVATED)) {
            LWLTE_LOGE(TAG, "Waiting for the PDN to be activated...");
            lwlte_sys_flags_wait(s_lwlte_core_context.flags, 
            LWLTE_FLAGS_MODULE_PDN_ACTIVATED, true, 
            false, s_lwlte_core_context.config.init_max_time_ms);
        }
        lwlte_sys_thread_sleep(1000);
        /* Check if the IP GPRS is activated */
        if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_IP_GPRS_ACTIVATED)) {
            char response[s_lwlte_core_context.config.uart_buf_size];
            lwlte_core_send_at_cmd_internal(AT_CSTT, "OK", "ERROR", s_lwlte_core_context.config.at_wait_ticks, response, sizeof(response));
            lwlte_core_send_at_cmd_internal(AT_CIICR, "OK", "ERROR", s_lwlte_core_context.config.at_wait_ticks, response, sizeof(response));
            if (strstr(response, "OK") != NULL) {
                lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_IP_GPRS_ACTIVATED);
                LWLTE_LOGI(TAG, "IP GPRS is activated.");
            }
            else {
                LWLTE_LOGE(TAG, "Failed to initialize the LWLTE module: IP GPRS not activated");
                continue;
            }
        }
        lwlte_sys_thread_sleep(1000);
        /* Check if the IP address is assigned */
        if (!lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_IP_ADDRESS_ASSIGNED)) {
            char response[s_lwlte_core_context.config.uart_buf_size];
            lwlte_core_send_at_cmd_internal(AT_CIFSR, "OK", "ERROR", s_lwlte_core_context.config.at_wait_ticks, response, sizeof(response));
            if (strstr(response, "ERROR") == NULL) {
                lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_IP_ADDRESS_ASSIGNED);
                LWLTE_LOGI(TAG, "IP address is assigned.");
            }
            else {
                LWLTE_LOGE(TAG, "Failed to initialize the LWLTE module: IP address not assigned");
                continue;
            }
        }
        lwlte_sys_flags_set(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_NETWORK_CONNECTED);
        LWLTE_LOGI(TAG, "The LTE Module has connected to the network.");
        s_lwlte_core_context.network_activate_thread_handle = NULL;
        lwlte_sys_thread_delete(NULL);
    }
    LWLTE_LOGE(TAG, "Network activation timed out");
    s_lwlte_core_context.network_activate_thread_handle = NULL;
    lwlte_sys_thread_delete(NULL);
}

lwlte_err_t lwlte_core_network_activate_internal(void)
{
    /* Check if the module is initialized */
    if (s_lwlte_core_context.flags == NULL || s_lwlte_core_context.core_input_queue == NULL) {
        return LWLTE_NOT_INITIALIZED;
    }
    /* Create the network activate task */
    lwlte_sys_thread_cfg_t network_activate_task_config = {
        .name = "network_activate_task",
        .priority = tskIDLE_PRIORITY + 1,
        .stack_size = 4096,
        .arg = NULL
    };
    lwlte_sys_thread_create(network_activate_task, &network_activate_task_config);
    return LWLTE_OK;
}

bool lwlte_core_get_module_ready_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY);
}

bool lwlte_core_get_module_sim_card_ready_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIM_CARD_READY);
}

bool lwlte_core_get_module_signal_good_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_SIGNAL_GOOD);
}

bool lwlte_core_get_module_pdn_activated_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_PDN_ACTIVATED);
}

bool lwlte_core_get_module_ip_gprs_activated_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_IP_GPRS_ACTIVATED);
}

bool lwlte_core_get_network_connected_internal(void)
{
    if (s_lwlte_core_context.flags == NULL) {
        return false;
    }
    return lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_NETWORK_CONNECTED);
}

lwlte_err_t lwlte_core_wait_module_ready(lwlte_base_type_t timeout_ms)
{
    if (s_lwlte_core_context.flags == NULL) {
        return LWLTE_NOT_INITIALIZED;
    }
    lwlte_sys_flags_wait(s_lwlte_core_context.flags, 
        LWLTE_FLAGS_MODULE_READY, true, 
        false, timeout_ms);
    if (lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_READY) == 0) {
        return LWLTE_TIMEOUT;
    }
    return LWLTE_OK;
}

lwlte_err_t lwlte_core_wait_network_connected(lwlte_base_type_t timeout_ms)
{
    if (s_lwlte_core_context.flags == NULL) {
        return LWLTE_NOT_INITIALIZED;
    }
    lwlte_sys_flags_wait(s_lwlte_core_context.flags, 
        LWLTE_FLAGS_MODULE_NETWORK_CONNECTED, true, 
        false, timeout_ms);
    if (lwlte_sys_flags_get_bit(s_lwlte_core_context.flags, LWLTE_FLAGS_MODULE_NETWORK_CONNECTED) == 0) {
        return LWLTE_TIMEOUT;
    }
    return LWLTE_OK;
}
