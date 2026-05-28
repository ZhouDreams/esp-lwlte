/**
 * @file lwlte.h
 * @brief LTE 用户门面公共接口
 * @details LTE user facade public interface
 * @author JovisDreams
 * @date 2026-05-25
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

#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief LTE 用户门面句柄
 * @details LTE user facade handle
 */
typedef struct lwlte lwlte_t;

/**
 * @brief LTE 门面状态
 * @details LTE facade state
 */
typedef enum {
    LWLTE_STATE_STOPPED = 0,        /**< 已停止； Stopped */
    LWLTE_STATE_STARTING,           /**< 启动中； Starting */
    LWLTE_STATE_READY,              /**< 已就绪； Ready */
    LWLTE_STATE_NET_ACTIVATING,     /**< 网络激活中； Network activating */
    LWLTE_STATE_ONLINE,             /**< 网络在线； Online */
    LWLTE_STATE_ERROR,              /**< 错误； Error */
    LWLTE_STATE_DESTROYING,         /**< 销毁中； Destroying */
} lwlte_state_t;

/**
 * @brief LTE 网络状态
 * @details LTE network state
 */
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,    /**< 离线； Offline */
    LWLTE_NET_STATE_ACTIVATING,     /**< 激活中； Activating */
    LWLTE_NET_STATE_ONLINE,         /**< 在线； Online */
    LWLTE_NET_STATE_ERROR,          /**< 错误； Error */
} lwlte_net_state_t;

/**
 * @brief LTE MQTT 状态
 * @details LTE MQTT state
 */
typedef enum {
    LWLTE_MQTT_STATE_STOPPED = 0,    /**< 已停止； Stopped */
    LWLTE_MQTT_STATE_WAITING_NET,    /**< 等待网络； Waiting network */
    LWLTE_MQTT_STATE_CONNECTING,     /**< 连接中； Connecting */
    LWLTE_MQTT_STATE_CONNECTED,      /**< 已连接； Connected */
    LWLTE_MQTT_STATE_DISCONNECTING,  /**< 断开中； Disconnecting */
    LWLTE_MQTT_STATE_ERROR,          /**< 错误； Error */
} lwlte_mqtt_state_t;

/**
 * @brief LTE MQTT 消息
 * @details LTE MQTT message
 */
typedef struct {
    const char *topic;               /**< 主题指针； Topic pointer */
    size_t topic_len;                /**< 主题长度； Topic length */
    const uint8_t *payload;          /**< 载荷指针； Payload pointer */
    size_t payload_len;              /**< 载荷长度； Payload length */
} lwlte_mqtt_msg_t;

/**
 * @brief LTE Ping 请求
 * @details LTE Ping request
 */
typedef struct {
    const char *host;                /**< 目标主机或 IP； Target host or IP */
    uint8_t count;                   /**< 发送次数，1..100； Request count, 1..100 */
    uint16_t data_len;               /**< 数据长度，0..1024； Data length, 0..1024 */
    uint16_t timeout_100ms;          /**< 单包超时，单位 100ms； Per-packet timeout in 100ms */
    uint8_t ttl;                     /**< TTL，1..255； TTL, 1..255 */
    uint32_t total_timeout_ms;       /**< 总等待超时，0 使用派生默认值； Total timeout, 0 derives default */
} lwlte_ping_request_t;

/**
 * @brief LTE Ping 单包响应
 * @details LTE Ping single reply
 */
typedef struct {
    uint8_t seq;                     /**< 响应序号； Reply sequence */
    char ip[48];                     /**< 响应 IP； Reply IP */
    uint32_t time_ms;                /**< 耗时毫秒； Time in milliseconds */
    uint8_t ttl;                     /**< 响应 TTL； Reply TTL */
    bool success;                    /**< 是否成功； Whether successful */
} lwlte_ping_reply_t;

/**
 * @brief LTE Ping 汇总
 * @details LTE Ping summary
 */
typedef struct {
    uint8_t sent;                    /**< 已发送数量； Sent count */
    uint8_t received;                /**< 已收到数量； Received count */
    uint8_t lost;                    /**< 丢包数量； Lost count */
    uint32_t min_time_ms;            /**< 最小耗时； Minimum time */
    uint32_t max_time_ms;            /**< 最大耗时； Maximum time */
    uint32_t avg_time_ms;            /**< 平均耗时； Average time */
} lwlte_ping_summary_t;

/**
 * @brief LTE 用户事件 ID
 * @details LTE user event ID
 */
typedef enum {
    LWLTE_EVENT_STARTED = 0,        /**< 已启动； Started */
    LWLTE_EVENT_READY,              /**< 已就绪； Ready */
    LWLTE_EVENT_NET_CONNECTING,     /**< 网络连接中； Network connecting */
    LWLTE_EVENT_NET_ONLINE,         /**< 网络在线； Network online */
    LWLTE_EVENT_NET_OFFLINE,        /**< 网络离线； Network offline */
    LWLTE_EVENT_NET_ERROR,          /**< 网络错误； Network error */
    LWLTE_EVENT_STOPPED,            /**< 已停止； Stopped */
    LWLTE_EVENT_ERROR,              /**< 错误； Error */
    LWLTE_EVENT_MQTT_STARTED,        /**< MQTT 已启动； MQTT started */
    LWLTE_EVENT_MQTT_STOPPED,        /**< MQTT 已停止； MQTT stopped */
    LWLTE_EVENT_MQTT_CONNECTING,     /**< MQTT 连接中； MQTT connecting */
    LWLTE_EVENT_MQTT_CONNECTED,      /**< MQTT 已连接； MQTT connected */
    LWLTE_EVENT_MQTT_DISCONNECTED,   /**< MQTT 已断开； MQTT disconnected */
    LWLTE_EVENT_MQTT_SUBSCRIBED,     /**< MQTT 已订阅； MQTT subscribed */
    LWLTE_EVENT_MQTT_UNSUBSCRIBED,   /**< MQTT 已取消订阅； MQTT unsubscribed */
    LWLTE_EVENT_MQTT_PUBLISHED,      /**< MQTT 已发布； MQTT published */
    LWLTE_EVENT_MQTT_DATA,           /**< MQTT 数据； MQTT data */
    LWLTE_EVENT_MQTT_ERROR,          /**< MQTT 错误； MQTT error */
} lwlte_event_id_t;

/**
 * @brief LTE 用户事件数据
 * @details LTE user event data
 */
typedef struct {
    lwlte_net_state_t net_state;    /**< 网络状态； Network state */
    lwlte_mqtt_state_t mqtt_state;  /**< MQTT 状态； MQTT state */
    int error_code;                 /**< 错误码； Error code */
    union {
        lwlte_mqtt_msg_t mqtt_msg;  /**< MQTT 消息，仅在回调期间有效； MQTT message, callback-scoped */
    } data;
} lwlte_event_data_t;

/**
 * @brief LTE 用户事件回调
 * @details LTE user event callback
 * @note data 指针仅在回调执行期间有效；如需异步使用，调用方必须复制其中内容。
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据，可能为 NULL
 * @param[in] user_ctx 用户上下文
 */
typedef void (*lwlte_event_callback_t)(lwlte_t *lte,
                                       lwlte_event_id_t event_id,
                                       const lwlte_event_data_t *data,
                                       void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 销毁 LTE 用户门面
 * @details Destroy LTE user facade
 * @note me 必须是 lwlte_air780ep_init() 成功返回的句柄。ESP_OK 返回后句柄失效，调用方不得继续使用。
 * @note 销毁按门面持有资源的反向顺序执行。若清理失败，返回第一个下层错误，所有权保持保守状态，调用方不得假定资源已完全释放。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许销毁或销毁已在进行
 *         - ESP_FAIL: 下层清理失败
 *         - 其他 esp_err_t: 下层销毁或清理错误
 */
esp_err_t lwlte_destroy(lwlte_t *me);

/**
 * @brief 注册 LTE 用户事件回调
 * @details Register LTE user event callback
 * @note 门面仅保存一个用户回调槽位，重复调用会覆盖之前的回调和用户上下文。
 * @note callback 为 NULL 时注销当前用户回调；user_ctx 不被门面拥有，注册期间必须由调用方保持有效。
 * @param[in] me LTE 用户门面句柄
 * @param[in] callback 事件回调函数，NULL 表示注销
 * @param[in] user_ctx 用户上下文，原样传回回调
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx);

/**
 * @brief 连接 LTE 网络
 * @details Connect LTE network
 * @note 该函数异步提交网络连接请求，ESP_OK 仅表示请求已提交，不表示网络已上线。
 * @note 最终联网结果通过用户事件回调或 lwlte_get_net_state() 查询获得。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许连接或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层连接错误
 */
esp_err_t lwlte_connect(lwlte_t *me);

/**
 * @brief 断开 LTE 网络
 * @details Disconnect LTE network
 * @note 该函数异步提交网络断开请求，ESP_OK 仅表示请求已提交，最终结果通过用户事件回调或状态查询获得。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许断开或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层断开错误
 */
esp_err_t lwlte_disconnect(lwlte_t *me);

/**
 * @brief 获取 LTE 门面状态
 * @details Get LTE facade state
 * @param[in] me LTE 用户门面句柄
 * @param[out] state LTE 门面状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_get_state(lwlte_t *me, lwlte_state_t *state);

/**
 * @brief 获取 LTE 网络状态
 * @details Get LTE network state
 * @param[in] me LTE 用户门面句柄
 * @param[out] state LTE 网络状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_get_net_state(lwlte_t *me, lwlte_net_state_t *state);

/**
 * @brief 执行同步 Ping 诊断
 * @details Perform synchronous Ping diagnostic
 * @note replies 由调用方提供，max_replies 必须大于等于 request->count。
 * @note 该函数阻塞直到 Core command 完成；不应在时间敏感回调中调用。
 * @param[in] me LTE 用户门面句柄
 * @param[in] request Ping 请求
 * @param[out] replies 调用方提供的单包响应数组
 * @param[in] max_replies replies 数组容量
 * @param[out] summary 可选汇总结果，可为 NULL
 * @return
 *         - ESP_OK: 命令完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 网络未 online 或门面正在销毁
 *         - ESP_ERR_TIMEOUT: Ping 超时
 *         - ESP_ERR_INVALID_RESPONSE: 模块响应格式无效
 *         - other: 下层错误
 */
esp_err_t lwlte_ping(lwlte_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);

/**
 * @brief 启动 MQTT 客户端
 * @details Start MQTT client
 * @note 该函数异步提交 MQTT 启动请求，ESP_OK 仅表示请求已提交。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_start(lwlte_t *me);

/**
 * @brief 停止 MQTT 客户端
 * @details Stop MQTT client
 * @note 该函数异步提交 MQTT 停止请求，ESP_OK 仅表示请求已提交。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_stop(lwlte_t *me);

/**
 * @brief 获取 MQTT 状态
 * @details Get MQTT state
 * @param[in] me LTE 用户门面句柄
 * @param[out] state MQTT 状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_get_state(lwlte_t *me, lwlte_mqtt_state_t *state);

/**
 * @brief 订阅 MQTT 主题
 * @details Subscribe MQTT topic
 * @param[in] me LTE 用户门面句柄
 * @param[in] topic MQTT 主题
 * @param[in] qos QoS 等级
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用、未连接或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_subscribe(lwlte_t *me, const char *topic, uint8_t qos);

/**
 * @brief 取消订阅 MQTT 主题
 * @details Unsubscribe MQTT topic
 * @param[in] me LTE 用户门面句柄
 * @param[in] topic MQTT 主题
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用、未连接或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_unsubscribe(lwlte_t *me, const char *topic);

/**
 * @brief 发布 MQTT 消息
 * @details Publish MQTT message
 * @param[in] me LTE 用户门面句柄
 * @param[in] topic MQTT 主题
 * @param[in] payload 消息载荷
 * @param[in] payload_len 消息载荷长度
 * @param[in] qos QoS 等级
 * @param[in] retain retain 标志
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 服务未启用、未连接或门面正在销毁
 *         - 其他 esp_err_t: 下层 MQTT 服务错误
 */
esp_err_t lwlte_mqtt_publish(lwlte_t *me, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             uint8_t qos, bool retain);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
