/**
 * @file tcp_client_priv.h
 * @brief TCP 客户端服务内部接口
 * @details TCP client service internal interface
 * @author JovisDreams
 * @date 2026-06-18
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "tcp_client.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TCP_CLIENT_DEFAULT_MAX_CONNS          1
#define TCP_CLIENT_DEFAULT_SEND_QUEUE_SIZE    4
#define TCP_CLIENT_DEFAULT_MAX_TX_LEN         1460
#define TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN   730
#define TCP_CLIENT_DEFAULT_OPEN_TIMEOUT_MS    75000
#define TCP_CLIENT_DEFAULT_SEND_TIMEOUT_MS    50000
#define TCP_CLIENT_DEFAULT_CLOSE_TIMEOUT_MS   10000
#define TCP_CLIENT_DEFAULT_FSM_QUEUE_SIZE     16
#define TCP_CLIENT_DEFAULT_FSM_TASK_STACK     4096
#define TCP_CLIENT_DEFAULT_FSM_PRIORITY       8
#define TCP_CLIENT_FSM_WAIT_MS                100
/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    TCP_SIG_OPEN = 0,
    TCP_SIG_CLOSE,
    TCP_SIG_SEND_READY,
    TCP_SIG_CORE_CMD_DONE,
    TCP_SIG_PROTOCOL_DATA,
    TCP_SIG_PROTOCOL_CLOSED,
    TCP_SIG_NET_OFFLINE,
} tcp_fsm_sig_type_t;

typedef struct tcp_open_owned {
    char *host;
    uint16_t port;
} tcp_open_owned_t;

typedef struct tcp_send_item {
    uint8_t *data;
    size_t len;
} tcp_send_item_t;

typedef struct {
    bool active;
    core_cmd_type_t type;
    size_t send_len;
} tcp_pending_cmd_t;

typedef struct tcp_protocol_data_owned {
    uint8_t conn_id;
    uint8_t *payload;
    size_t payload_len;
    int reason;
    int modem_error_code;
} tcp_protocol_data_owned_t;

typedef struct {
    tcp_fsm_sig_type_t type;
    bool conn_scoped;
    uint32_t conn_generation;
    core_cmd_type_t core_cmd_type;
    core_cmd_result_t core_result;
    void *result_data;
    size_t result_size;
    int error_code;
    int modem_error_code;
    void *data;
    size_t data_size;
} tcp_fsm_sig_t;

struct tcp_client_handle {
    tcp_client_config_t config;
    core_handle_t *core;
    tcp_client_conn_t *conn;
    tcp_client_conn_t *deferred_destroy_conn;
    TaskHandle_t fsm_task;
    QueueHandle_t fsm_queue;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t fsm_task_done_sema;
    uint32_t next_conn_generation;
    bool destroying;
};

struct tcp_client_conn {
    tcp_client_handle_t *client;
    QueueHandle_t send_queue;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t send_queue_lock;
    SemaphoreHandle_t active_done_sema;
    tcp_conn_state_t state;
    tcp_pending_cmd_t pending_cmd;
    int active_refs;
    uint8_t conn_id;
    uint32_t generation;
    void *user_ctx;
    bool close_requested;
    bool recv_requested;
    int recv_reason;
    int recv_modem_error_code;
    bool terminal_event_pending;
    bool terminal_event_posted;
    int terminal_event_id;
    esp_err_t terminal_error_code;
    int terminal_modem_error_code;
    int terminal_reason;
    bool remote_closed;
    bool remote_closed_event_pending;
    bool remote_closed_event_posted;
    int remote_reason;
    int remote_modem_error_code;
    bool destroyed;
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
