/**
 * @file mqtt_client.h
 * @brief MQTT 客户端服务层间接口
 * @details MQTT client service inter-layer interface
 * @author JovisDreams
 * @date 2026-05-27
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "esp_err.h"
#include "esp_event.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct mqtt_client_handle mqtt_client_handle_t;

typedef enum {
    MQTT_CLIENT_TRANSPORT_PLAIN_TCP = 0,
    MQTT_CLIENT_TRANSPORT_TLS,
} mqtt_client_transport_t;

typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
    uint8_t ssl_context_id;
} mqtt_client_endpoint_config_t;

typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
} mqtt_client_auth_config_t;

typedef struct {
    uint16_t keepalive_s;
    bool clean_session;
} mqtt_client_session_config_t;

typedef struct {
    int queue_size;
    int task_stack;
    int task_priority;
} mqtt_client_fsm_config_t;

typedef struct {
    esp_event_loop_handle_t loop;        /**< 共享事件总线（借用）； Shared event bus (borrowed) */
} mqtt_client_event_config_t;

typedef struct {
    mqtt_client_endpoint_config_t endpoint;
    mqtt_client_auth_config_t auth;
    mqtt_client_session_config_t session;
    mqtt_client_fsm_config_t fsm;
    mqtt_client_event_config_t event;
} mqtt_client_config_t;

typedef enum {
    MQTT_CLIENT_STATE_STOPPED = 0,
    MQTT_CLIENT_STATE_WAITING_NET,
    MQTT_CLIENT_STATE_CONNECTING,
    MQTT_CLIENT_STATE_CONNECTED,
    MQTT_CLIENT_STATE_DISCONNECTING,
    MQTT_CLIENT_STATE_ERROR,
    MQTT_CLIENT_STATE_DESTROYING,
} mqtt_client_state_t;

typedef enum {
    MQTT_CLIENT_OPERATION_CONNECT = 0,
    MQTT_CLIENT_OPERATION_DISCONNECT,
    MQTT_CLIENT_OPERATION_SUBSCRIBE,
    MQTT_CLIENT_OPERATION_UNSUBSCRIBE,
    MQTT_CLIENT_OPERATION_PUBLISH,
} mqtt_client_operation_t;

typedef struct {
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} mqtt_client_publish_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
mqtt_client_handle_t *mqtt_client_create(const mqtt_client_config_t *config,
                                         core_handle_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_handle_t *me);
esp_err_t mqtt_client_start(mqtt_client_handle_t *me);
esp_err_t mqtt_client_stop(mqtt_client_handle_t *me);
esp_err_t mqtt_client_get_state(mqtt_client_handle_t *me,
                                 mqtt_client_state_t *state);
esp_err_t mqtt_client_subscribe(mqtt_client_handle_t *me,
                                 const char *topic,
                                 uint8_t qos);
esp_err_t mqtt_client_unsubscribe(mqtt_client_handle_t *me,
                                   const char *topic);
esp_err_t mqtt_client_publish(mqtt_client_handle_t *me,
                               const mqtt_client_publish_t *request);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
