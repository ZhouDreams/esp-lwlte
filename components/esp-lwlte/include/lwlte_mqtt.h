/*
    File: lwlte_mqtt.h
    Author: JovisDreams
    Date: 2026-01-01
    Description: esp-lwlte mqtt header file
*/
#pragma once

#include "lwlte_mqtt.h"
#include "lwlte_sys_types.h"
#include "lwlte_err.h"

#define LWLTE_MQTT_CFG_UNSET_INT -1
#define LWLTE_MQTT_CFG_UNSET_BOOL -1

#define LWLTE_MQTT_CLIENT_CONFIG_DEFAULT() (lwlte_mqtt_client_config_t){ \
    .client_t = { \
        .client_id = NULL, \
        .username = NULL, \
        .password = NULL, \
        .will_qos = LWLTE_MQTT_CFG_UNSET_INT, \
        .will_retain = LWLTE_MQTT_CFG_UNSET_INT, \
        .will_topic = NULL, \
        .will_message = NULL, \
    }, \
    .broker_t = { \
        .uri = NULL, \
        .port = LWLTE_MQTT_CFG_UNSET_INT, \
        .clean_session = LWLTE_MQTT_CFG_UNSET_INT, \
        .keepalive = LWLTE_MQTT_CFG_UNSET_INT \
    } \
}

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    struct {
        const char* client_id; // MQTT client ID, Necessary
        const char* username; // MQTT username, Optional
        const char* password; // MQTT password, Optional
        /* attention: will_qos, will_retain, will_topic, will_message must be set or unset at the same time,
        if one of them is set, the others must be set, otherwise the will configuration will be ignored*/
        lwlte_base_type_t will_qos; // MQTT will QoS, Optional
        lwlte_base_type_t will_retain; // MQTT will retain, Optional
        const char* will_topic; // MQTT will topic, Optional
        const char* will_message; // MQTT will message, Optional
    } client_t;
    struct {
        const char* uri; // MQTT broker URI, Necessary
        lwlte_base_type_t port; // MQTT broker port, Necessary
        lwlte_base_type_t clean_session; // MQTT clean session, Optional
        lwlte_base_type_t keepalive; // MQTT keepalive, Optional
    } broker_t;
} lwlte_mqtt_client_config_t;

esp_err_t lwlte_mqtt_client_init(const lwlte_mqtt_client_config_t *config, lwlte_base_type_t timeout_ms);

esp_err_t lwlte_mqtt_client_deinit(void);

esp_err_t lwlte_mqtt_client_connect(void);

esp_err_t lwlte_mqtt_client_disconnect(void);

esp_err_t lwlte_mqtt_client_subscribe(const char* topic);

esp_err_t lwlte_mqtt_client_unsubscribe(const char* topic);

esp_err_t lwlte_mqtt_client_publish(const char* topic, const char* payload);

#ifdef __cplusplus
}
#endif
