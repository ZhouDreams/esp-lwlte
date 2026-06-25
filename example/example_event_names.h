/**
 * @file example_event_names.h
 * @brief 示例事件名称辅助函数
 * @details Example event name helper functions
 * @author JovisDreams
 * @date 2026-06-25
 */
#ifndef EXAMPLE_EVENT_NAMES_H
#define EXAMPLE_EVENT_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lwlte.h"

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 获取 LTE 事件名称
 * @details Get LTE event name
 * @param[in] id LTE 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_event_name(lwlte_event_id_t id);

/**
 * @brief 获取 LTE 网络状态名称
 * @details Get LTE network state name
 * @param[in] state LTE 网络状态
 * @return 字符串常量名称
 */
const char *example_lwlte_net_state_name(lwlte_net_state_t state);

/**
 * @brief 获取 MQTT 事件名称
 * @details Get MQTT event name
 * @param[in] id MQTT 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id);

/**
 * @brief 获取 TCP 事件名称
 * @details Get TCP event name
 * @param[in] id TCP 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id);

/**
 * @brief 获取 TCP 连接状态名称
 * @details Get TCP connection state name
 * @param[in] state TCP 连接状态
 * @return 字符串常量名称
 */
const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EXAMPLE_EVENT_NAMES_H */
