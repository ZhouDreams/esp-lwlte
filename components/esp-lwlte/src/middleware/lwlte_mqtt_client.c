/*
    File: lwlte_mqtt_client.c
    Author: JovisDreams
    Date: 2026-01-01
    Description: esp-lwlte mqtt client source file
*/
#include "lwlte_mqtt.h"
#include "lwlte_mqtt_client.h"
#include "lwlte_sys_types.h"
#include "lwlte_err.h"
#include "lwlte_core.h"
#include "lwlte_sys_log.h"
#include "lwlte_sys_queue.h"
#include "lwlte_sys_mutex.h"
#include "lwlte_sys_thread.h"
#include "lwlte_sys_flags.h"
#include "lwlte_sys_mem.h"
#include <stddef.h>
#include <string.h>

#define AT_CMD_MAX_LENGTH 100

static const char* TAG = "lwlte_mqtt_client";

static struct {
    lwlte_mqtt_client_config_t config;
    lwlte_sys_flags_t flags;

} s_lwlte_mqtt_client_context;

static char* lwlte_string_dup(const char* str)
{
    if (str == NULL) {
        return NULL;
    }
    char* new_str = lwlte_sys_mem_malloc(strlen(str) + 1);
    strcpy(new_str, str);
    return new_str;
}

static lwlte_err_t lwlte_mqtt_client_config_copy(const lwlte_mqtt_client_config_t *config)
{
    /* Deep copy the client_t */
    if (config->client_t.client_id != NULL) {
        s_lwlte_mqtt_client_context.config.client_t.client_id = lwlte_string_dup(config->client_t.client_id);
    }
    if (config->client_t.username != NULL) {
        s_lwlte_mqtt_client_context.config.client_t.username = lwlte_string_dup(config->client_t.username);
    }
    if (config->client_t.password != NULL) {
        s_lwlte_mqtt_client_context.config.client_t.password = lwlte_string_dup(config->client_t.password);
    }
    if (config->client_t.will_qos != LWLTE_MQTT_CFG_UNSET_INT) {
        s_lwlte_mqtt_client_context.config.client_t.will_qos = config->client_t.will_qos;
    }
    if (config->client_t.will_retain != LWLTE_MQTT_CFG_UNSET_INT) {
        s_lwlte_mqtt_client_context.config.client_t.will_retain = config->client_t.will_retain;
    }
    if (config->client_t.will_topic != NULL) {
        s_lwlte_mqtt_client_context.config.client_t.will_topic = lwlte_string_dup(config->client_t.will_topic);
    }
    if (config->client_t.will_message != NULL) {
        s_lwlte_mqtt_client_context.config.client_t.will_message = lwlte_string_dup(config->client_t.will_message);
    }
    /* Deep copy the broker_t */
    if (config->broker_t.uri != NULL) {
        s_lwlte_mqtt_client_context.config.broker_t.uri = lwlte_string_dup(config->broker_t.uri);
    }
    if (config->broker_t.port != LWLTE_MQTT_CFG_UNSET_INT) {
        s_lwlte_mqtt_client_context.config.broker_t.port = config->broker_t.port;
    }
    if (config->broker_t.clean_session != LWLTE_MQTT_CFG_UNSET_INT) {
        s_lwlte_mqtt_client_context.config.broker_t.clean_session = config->broker_t.clean_session;
    }
    if (config->broker_t.keepalive != LWLTE_MQTT_CFG_UNSET_INT) {
        s_lwlte_mqtt_client_context.config.broker_t.keepalive = config->broker_t.keepalive;
    }
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_init_internal(const lwlte_mqtt_client_config_t *config, lwlte_base_type_t timeout_ms)
{
    LWLTE_LOGI(TAG, "Checking config and core status...");
    /* Check if the config is valid */
    if (config == NULL) {
        return LWLTE_INVALID_ARG;
    }
    if (config->client_t.client_id == NULL) {
        LWLTE_LOGE(TAG, "MQTT client ID is not set!");
        return LWLTE_INVALID_ARG;
    }
    if (config->broker_t.uri == NULL) {
        LWLTE_LOGE(TAG, "MQTT broker URI is not set!");
        return LWLTE_INVALID_ARG;
    }
    if (config->broker_t.port == LWLTE_MQTT_CFG_UNSET_INT) {
        LWLTE_LOGE(TAG, "MQTT broker port is not set!");
        return LWLTE_INVALID_ARG;
    }
    /* Wait until the module is ready or timeout */
    if (lwlte_core_wait_module_ready(timeout_ms) != LWLTE_OK) {
        LWLTE_LOGE(TAG, "Module is not ready!");
        return LWLTE_NOT_INITIALIZED;
    }
    LWLTE_LOGI(TAG, "Config and core status are ok, initializing MQTT client...");
    /* Create the flags */
    if (s_lwlte_mqtt_client_context.flags == NULL) {
        s_lwlte_mqtt_client_context.flags = lwlte_sys_flags_create();
        lwlte_sys_flags_clear(s_lwlte_mqtt_client_context.flags, LWLTE_FLAGS_ALL_BITS);
    }
    /* Deep copy the config */
    if (lwlte_mqtt_client_config_copy(config) != LWLTE_OK) {
        return LWLTE_ERROR;
    }
    /* Send the AT+MCONFIG command */
    char at_cmd_buf[AT_CMD_MAX_LENGTH];
    char response_buf[AT_CMD_MAX_LENGTH];
    response_buf[0] = '\0';
    bool has_will = s_lwlte_mqtt_client_context.config.client_t.will_topic != NULL && s_lwlte_mqtt_client_context.config.client_t.will_topic[0] != '\0' &&
    s_lwlte_mqtt_client_context.config.client_t.will_message != NULL && s_lwlte_mqtt_client_context.config.client_t.will_message[0] != '\0' &&
    s_lwlte_mqtt_client_context.config.client_t.will_qos != LWLTE_MQTT_CFG_UNSET_INT &&
    s_lwlte_mqtt_client_context.config.client_t.will_retain != LWLTE_MQTT_CFG_UNSET_INT;
    if (has_will) {
        snprintf(at_cmd_buf, AT_CMD_MAX_LENGTH, 
            "AT+MCONFIG=\"%s\",\"%s\",\"%s\",%d,%d,\"%s\",\"%s\"\r\n", 
            s_lwlte_mqtt_client_context.config.client_t.client_id, 
            s_lwlte_mqtt_client_context.config.client_t.username == NULL ? "" : s_lwlte_mqtt_client_context.config.client_t.username, 
            s_lwlte_mqtt_client_context.config.client_t.password == NULL ? "" : s_lwlte_mqtt_client_context.config.client_t.password, 
            s_lwlte_mqtt_client_context.config.client_t.will_qos, 
            s_lwlte_mqtt_client_context.config.client_t.will_retain, s_lwlte_mqtt_client_context.config.client_t.will_topic, 
            s_lwlte_mqtt_client_context.config.client_t.will_message);
    } else {
        snprintf(at_cmd_buf, AT_CMD_MAX_LENGTH, 
            "AT+MCONFIG=\"%s\",\"%s\",\"%s\"\r\n", 
            s_lwlte_mqtt_client_context.config.client_t.client_id, 
            s_lwlte_mqtt_client_context.config.client_t.username == NULL ? "" : s_lwlte_mqtt_client_context.config.client_t.username, 
            s_lwlte_mqtt_client_context.config.client_t.password == NULL ? "" : s_lwlte_mqtt_client_context.config.client_t.password
        );
    }
    lwlte_core_send_at_cmd_internal(at_cmd_buf, "OK", "ERROR", 10000, response_buf, AT_CMD_MAX_LENGTH);
    if (strstr(response_buf, "OK") == NULL) {
        LWLTE_LOGE(TAG, "Failed to set MQTT client config!");
        return LWLTE_ERROR;
    }
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_deinit_internal(void)
{
    if (s_lwlte_mqtt_client_context.config.client_t.client_id != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.client_t.client_id);
    }
    if (s_lwlte_mqtt_client_context.config.client_t.username != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.client_t.username);
    }
    if (s_lwlte_mqtt_client_context.config.client_t.password != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.client_t.password);
    }
    s_lwlte_mqtt_client_context.config.client_t.will_qos = LWLTE_MQTT_CFG_UNSET_INT;
    s_lwlte_mqtt_client_context.config.client_t.will_retain = LWLTE_MQTT_CFG_UNSET_INT;
    if (s_lwlte_mqtt_client_context.config.client_t.will_topic != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.client_t.will_topic);
    }
    if (s_lwlte_mqtt_client_context.config.client_t.will_message != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.client_t.will_message);
    }
    if (s_lwlte_mqtt_client_context.config.broker_t.uri != NULL) {
        free((void*)s_lwlte_mqtt_client_context.config.broker_t.uri);
    }
    s_lwlte_mqtt_client_context.config.broker_t.port = LWLTE_MQTT_CFG_UNSET_INT;
    s_lwlte_mqtt_client_context.config.broker_t.clean_session = LWLTE_MQTT_CFG_UNSET_INT;
    s_lwlte_mqtt_client_context.config.broker_t.keepalive = LWLTE_MQTT_CFG_UNSET_INT;
    lwlte_sys_flags_delete(s_lwlte_mqtt_client_context.flags);
    s_lwlte_mqtt_client_context.flags = NULL;

    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_connect_internal()
{
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_disconnect_internal()
{
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_subscribe_internal(const char* topic)
{
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_unsubscribe_internal(const char* topic)
{
    return LWLTE_OK;
}

lwlte_err_t lwlte_mqtt_client_publish_internal(const char* topic, const char* payload)
{
    return LWLTE_OK;
}