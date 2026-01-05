/*
    File: lwlte_mqtt.c
    Author: JovisDreams
    Date: 2026-01-01
    Description: esp-lwlte mqtt source file
*/
#include "lwlte.h"
#include "lwlte_mqtt.h"
#include "lwlte_mqtt_client.h"
#include "lwlte_sys_types.h"
#include "lwlte_err.h"
#include "esp_err.h"

esp_err_t lwlte_mqtt_client_init(const lwlte_mqtt_client_config_t *config, lwlte_base_type_t timeout_ms)
{
    return lwlte_err_2_esp_err(lwlte_mqtt_client_init_internal(config, timeout_ms));
}

esp_err_t lwlte_mqtt_client_deinit(void)
{
    return lwlte_err_2_esp_err(lwlte_mqtt_client_deinit_internal());
}

esp_err_t lwlte_mqtt_client_connect(void)
{
    return lwlte_err_2_esp_err(lwlte_mqtt_client_connect_internal());
}

esp_err_t lwlte_mqtt_client_disconnect(void)
{
    return lwlte_err_2_esp_err(lwlte_mqtt_client_disconnect_internal());
}

esp_err_t lwlte_mqtt_client_subscribe(const char* topic)
{
    return lwlte_err_2_esp_err(lwlte_mqtt_client_subscribe_internal(topic));
}

esp_err_t lwlte_mqtt_client_unsubscribe(const char* topic)
{   
    return lwlte_err_2_esp_err(lwlte_mqtt_client_unsubscribe_internal(topic));
}