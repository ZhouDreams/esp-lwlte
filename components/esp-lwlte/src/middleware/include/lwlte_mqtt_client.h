/*
    File: lwlte_mqtt_client.h
    Author: JovisDreams
    Date: 2026-01-01
    Description: esp-lwlte mqtt client header file
*/
#pragma once

#include "lwlte_mqtt.h"
#include "lwlte_sys_types.h"
#include "lwlte_err.h"

#ifdef __cplusplus
extern "C" {
#endif

lwlte_err_t lwlte_mqtt_client_init_internal(const lwlte_mqtt_client_config_t *config, lwlte_base_type_t timeout_ms);

lwlte_err_t lwlte_mqtt_client_deinit_internal(void);

lwlte_err_t lwlte_mqtt_client_connect_internal();

lwlte_err_t lwlte_mqtt_client_disconnect_internal();

lwlte_err_t lwlte_mqtt_client_subscribe_internal(const char* topic);

lwlte_err_t lwlte_mqtt_client_unsubscribe_internal(const char* topic);

lwlte_err_t lwlte_mqtt_client_publish_internal(const char* topic, const char* payload);




#ifdef __cplusplus
}
#endif