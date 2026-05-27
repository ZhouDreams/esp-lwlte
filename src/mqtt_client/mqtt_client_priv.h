/**
 * @file mqtt_client_priv.h
 * @brief MQTT 客户端服务内部接口
 * @details MQTT client service internal interface
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
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define MQTT_CLIENT_DEFAULT_KEEPALIVE_S      300
#define MQTT_CLIENT_DEFAULT_FSM_QUEUE_SIZE   16
#define MQTT_CLIENT_DEFAULT_FSM_TASK_STACK   4096
#define MQTT_CLIENT_DEFAULT_FSM_PRIORITY     8
#define MQTT_CLIENT_FSM_WAIT_MS              100
#define MQTT_CLIENT_CMD_TIMEOUT_MS           60000
#define MQTT_CLIENT_STOP_WAIT_MS             (MQTT_CLIENT_CMD_TIMEOUT_MS * 2)

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    MQTT_SIG_START = 0,
    MQTT_SIG_STOP,
    MQTT_SIG_NET_ONLINE,
    MQTT_SIG_NET_OFFLINE,
    MQTT_SIG_CORE_CMD_DONE,
    MQTT_SIG_SUBSCRIBE,
    MQTT_SIG_UNSUBSCRIBE,
    MQTT_SIG_PUBLISH,
    MQTT_SIG_PROTOCOL_DATA,
    MQTT_SIG_PROTOCOL_CLOSED,
} mqtt_fsm_sig_type_t;

typedef enum {
    MQTT_CONNECT_STEP_IDLE = 0,
    MQTT_CONNECT_STEP_CONFIG,
    MQTT_CONNECT_STEP_OPEN,
    MQTT_CONNECT_STEP_LOGIN,
    MQTT_CONNECT_STEP_DONE,
    MQTT_CONNECT_STEP_ERROR,
} mqtt_connect_step_t;

typedef struct {
    bool active;
    core_cmd_type_t type;
    mqtt_client_operation_t operation;
    uint32_t started_ms;
    uint32_t timeout_ms;
} mqtt_pending_cmd_t;

typedef struct mqtt_protocol_data_owned {
    char *topic;
    size_t topic_len;
    uint8_t *payload;
    size_t payload_len;
} mqtt_protocol_data_owned_t;

typedef struct {
    mqtt_fsm_sig_type_t type;
    core_cmd_type_t core_cmd_type;
    core_cmd_result_t core_result;
    int error_code;
    void *data;
    size_t data_size;
} mqtt_fsm_sig_t;

struct mqtt_client {
    mqtt_client_config_t config;
    core_t *core;
    esp_event_loop_handle_t event_loop;
    mqtt_client_event_callback_t event_callback;
    void *event_user_ctx;
    TaskHandle_t fsm_task;
    QueueHandle_t fsm_queue;
    SemaphoreHandle_t fsm_task_done_sema;
    SemaphoreHandle_t stop_done_sema;
    SemaphoreHandle_t lock;
    mqtt_client_state_t state;
    mqtt_connect_step_t connect_step;
    mqtt_pending_cmd_t pending_cmd;
    bool destroying;
    bool started;
    bool net_online;
    bool stop_requested;
    bool transport_open;
    SemaphoreHandle_t event_callback_done_sema;
    TaskHandle_t event_callback_task;
    int event_callback_active;
    bool event_callback_waiting;
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
