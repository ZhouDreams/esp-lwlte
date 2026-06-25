/**
 * @file example_event_names.c
 * @brief 示例事件名称辅助函数
 * @details Example event name helper functions
 * @author JovisDreams
 * @date 2026-06-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "example_event_names.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
const char *example_lwlte_event_name(lwlte_event_id_t id)
{
    switch (id) {
    case LWLTE_EVENT_STARTED:
        return "STARTED";
    case LWLTE_EVENT_READY:
        return "READY";
    case LWLTE_EVENT_NET_CONNECTING:
        return "NET_CONNECTING";
    case LWLTE_EVENT_NET_ONLINE:
        return "NET_ONLINE";
    case LWLTE_EVENT_NET_OFFLINE:
        return "NET_OFFLINE";
    case LWLTE_EVENT_NET_ERROR:
        return "NET_ERROR";
    case LWLTE_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_net_state_name(lwlte_net_state_t state)
{
    switch (state) {
    case LWLTE_NET_STATE_OFFLINE:
        return "OFFLINE";
    case LWLTE_NET_STATE_ACTIVATING:
        return "ACTIVATING";
    case LWLTE_NET_STATE_ONLINE:
        return "ONLINE";
    case LWLTE_NET_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id)
{
    switch (id) {
    case LWLTE_MQTT_EVENT_STARTED:
        return "STARTED";
    case LWLTE_MQTT_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_MQTT_EVENT_CONNECTING:
        return "CONNECTING";
    case LWLTE_MQTT_EVENT_CONNECTED:
        return "CONNECTED";
    case LWLTE_MQTT_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    case LWLTE_MQTT_EVENT_SUBSCRIBED:
        return "SUBSCRIBED";
    case LWLTE_MQTT_EVENT_UNSUBSCRIBED:
        return "UNSUBSCRIBED";
    case LWLTE_MQTT_EVENT_PUBLISHED:
        return "PUBLISHED";
    case LWLTE_MQTT_EVENT_DATA:
        return "DATA";
    case LWLTE_MQTT_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id)
{
    switch (id) {
    case LWLTE_TCP_EVENT_STARTED:
        return "STARTED";
    case LWLTE_TCP_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_TCP_EVENT_CONNECTED:
        return "CONNECTED";
    case LWLTE_TCP_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    case LWLTE_TCP_EVENT_SENT:
        return "SENT";
    case LWLTE_TCP_EVENT_DATA:
        return "DATA";
    case LWLTE_TCP_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state)
{
    switch (state) {
    case LWLTE_TCP_CONN_STATE_CREATED:
        return "CREATED";
    case LWLTE_TCP_CONN_STATE_CONNECTING:
        return "CONNECTING";
    case LWLTE_TCP_CONN_STATE_CONNECTED:
        return "CONNECTED";
    case LWLTE_TCP_CONN_STATE_CLOSING:
        return "CLOSING";
    case LWLTE_TCP_CONN_STATE_CLOSED:
        return "CLOSED";
    case LWLTE_TCP_CONN_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
