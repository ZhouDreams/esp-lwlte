/**
 * @file tcp_client.h
 * @brief TCP 客户端服务层间接口
 * @details TCP client service inter-layer interface
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
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "esp_err.h"
#include "esp_event.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct tcp_client_handle tcp_client_handle_t;
typedef struct tcp_client_conn tcp_client_conn_t;

typedef enum {
    TCP_CONN_STATE_CREATED = 0,
    TCP_CONN_STATE_CONNECTING,
    TCP_CONN_STATE_CONNECTED,
    TCP_CONN_STATE_CLOSING,
    TCP_CONN_STATE_CLOSED,
    TCP_CONN_STATE_ERROR,
} tcp_conn_state_t;

typedef struct {
    uint8_t max_conns;
    int send_queue_size;
    size_t max_tx_len;
    size_t max_rx_event_len;
    uint32_t open_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t close_timeout_ms;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
    esp_event_loop_handle_t loop;
} tcp_client_config_t;

typedef struct {
    const char *host;
    uint16_t port;
    void *user_ctx;
    core_socket_transport_t transport;    /**< 传输类型； Transport */
    uint8_t ssl_context_id;               /**< TLS SSL context ID； SSL context ID for TLS */
} tcp_client_open_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
tcp_client_handle_t *tcp_client_create(const tcp_client_config_t *config,
                                       core_handle_t *core);
esp_err_t tcp_client_destroy(tcp_client_handle_t *me);
esp_err_t tcp_client_open(tcp_client_handle_t *me,
                          const tcp_client_open_config_t *config,
                          tcp_client_conn_t **out_conn);
esp_err_t tcp_client_send(tcp_client_conn_t *conn,
                          const uint8_t *data,
                          size_t len);
esp_err_t tcp_client_close(tcp_client_conn_t *conn);
esp_err_t tcp_client_conn_get_state(tcp_client_conn_t *conn,
                                    tcp_conn_state_t *state);
esp_err_t tcp_client_conn_destroy(tcp_client_conn_t *conn);
void tcp_client_conn_release_event(tcp_client_conn_t *conn);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
