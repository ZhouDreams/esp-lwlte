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

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct mqtt_client mqtt_client_t;

typedef enum {
    MQTT_CLIENT_TRANSPORT_PLAIN_TCP = 0,
    MQTT_CLIENT_TRANSPORT_TLS,
} mqtt_client_transport_t;

typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keepalive_s;
    bool clean_session;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
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

ESP_EVENT_DECLARE_BASE(MQTT_CLIENT_EVENT);

typedef enum {
    MQTT_CLIENT_EVENT_STARTED = 0,
    MQTT_CLIENT_EVENT_STOPPED,
    MQTT_CLIENT_EVENT_CONNECTING,
    MQTT_CLIENT_EVENT_CONNECTED,
    MQTT_CLIENT_EVENT_DISCONNECTED,
    MQTT_CLIENT_EVENT_SUBSCRIBED,
    MQTT_CLIENT_EVENT_UNSUBSCRIBED,
    MQTT_CLIENT_EVENT_PUBLISHED,
    MQTT_CLIENT_EVENT_DATA,
    MQTT_CLIENT_EVENT_ERROR,
} mqtt_client_event_id_t;

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

typedef struct {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} mqtt_client_msg_t;

typedef struct {
    mqtt_client_state_t state;
    int error_code;
    union {
        mqtt_client_operation_t operation;
        mqtt_client_msg_t msg;
    } data;
} mqtt_client_event_data_t;

typedef void (*mqtt_client_event_callback_t)(mqtt_client_t *client,
                                             mqtt_client_event_id_t event_id,
                                             const mqtt_client_event_data_t *data,
                                             void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/
mqtt_client_t *mqtt_client_create(const mqtt_client_config_t *config,
                                  core_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_t *me);
esp_err_t mqtt_client_start(mqtt_client_t *me);
esp_err_t mqtt_client_stop(mqtt_client_t *me);
esp_err_t mqtt_client_register_event_callback(mqtt_client_t *me,
                                              mqtt_client_event_callback_t callback,
                                              void *user_ctx);
esp_event_loop_handle_t mqtt_client_get_event_loop(mqtt_client_t *me);
esp_err_t mqtt_client_get_state(mqtt_client_t *me,
                                mqtt_client_state_t *state);
esp_err_t mqtt_client_subscribe(mqtt_client_t *me,
                                const char *topic,
                                uint8_t qos);
esp_err_t mqtt_client_unsubscribe(mqtt_client_t *me,
                                  const char *topic);
esp_err_t mqtt_client_publish(mqtt_client_t *me,
                              const mqtt_client_publish_t *request);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
