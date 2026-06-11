/**
 * @file core.h
 * @brief LTE 核心服务层间接口
 * @details LTE core service inter-layer interface
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
#include "esp_event.h"

#include "modem.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief LTE 核心服务句柄
 * @details LTE core service handle
 */
typedef struct core_handle core_handle_t;

/**
 * @brief LTE 核心服务配置
 * @details LTE core service configuration
 */
typedef struct {
    const char *apn;                     /**< APN； APN */
    uint8_t primary_cid;                 /**< 主 PDP 上下文 ID； Primary PDP context ID */
    uint32_t net_activate_timeout_ms;    /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;         /**< 重连延迟； Reconnect delay */
    int fsm_queue_size;                  /**< FSM 队列长度； FSM queue size */
    int fsm_task_stack;                  /**< FSM 任务栈大小； FSM task stack size */
    int fsm_task_priority;               /**< FSM 任务优先级； FSM task priority */
} core_config_t;

/**
 * @brief LTE 核心服务状态
 * @details LTE core service state
 */
typedef enum {
    CORE_STATE_STOPPED = 0,              /**< 已停止； Stopped */
    CORE_STATE_STARTING,                 /**< 启动中； Starting */
    CORE_STATE_READY,                    /**< 已就绪； Ready */
    CORE_STATE_NET_ACTIVATING,           /**< 网络激活中； Network activating */
    CORE_STATE_ONLINE,                   /**< 网络在线； Online */
    CORE_STATE_ERROR,                    /**< 错误； Error */
    CORE_STATE_DESTROYING,               /**< 销毁中； Destroying */
} core_state_t;

/**
 * @brief LTE 网络状态
 * @details LTE network state
 */
typedef enum {
    CORE_NET_STATE_OFFLINE = 0,          /**< 离线； Offline */
    CORE_NET_STATE_ACTIVATING,           /**< 激活中； Activating */
    CORE_NET_STATE_ONLINE,               /**< 在线； Online */
    CORE_NET_STATE_ERROR,                /**< 错误； Error */
} core_net_state_t;

/**
 * @brief LTE 核心服务事件基
 * @details LTE core service event base
 */
ESP_EVENT_DECLARE_BASE(CORE_EVENT);

/**
 * @brief LTE 核心服务事件 ID
 * @details LTE core service event ID
 */
typedef enum {
    CORE_EVENT_STARTED = 0,              /**< Core 已启动； Core started */
    CORE_EVENT_READY,                    /**< Core 已就绪； Core ready */
    CORE_EVENT_NET_CONNECTING,           /**< 网络连接中； Network connecting */
    CORE_EVENT_NET_ONLINE,               /**< 网络在线； Network online */
    CORE_EVENT_NET_OFFLINE,              /**< 网络离线； Network offline */
    CORE_EVENT_NET_ERROR,                /**< 网络错误； Network error */
    CORE_EVENT_STOPPED,                  /**< Core 已停止； Core stopped */
    CORE_EVENT_ERROR,                    /**< Core 错误； Core error */
    CORE_EVENT_PROTOCOL_DATA,            /**< 协议数据； Protocol data */
    CORE_EVENT_PROTOCOL_CLOSED,          /**< 协议关闭； Protocol closed */
} core_event_id_t;

/**
 * @brief Core 协议类型
 * @details Core protocol type
 */
typedef enum {
    CORE_PROTOCOL_MQTT = 0,              /**< MQTT 协议； MQTT protocol */
} core_protocol_t;

/**
 * @brief Core 协议数据
 * @details Core protocol data. For CORE_EVENT_PROTOCOL_DATA, topic/payload are
 * heap-owned by the protocol event consumer. The MQTT service must copy data it
 * needs and call core_release_event_payload() before its event callback returns.
 */
typedef struct {
    core_protocol_t protocol;            /**< 协议类型； Protocol type */
    const char *topic;                   /**< 主题； Topic */
    size_t topic_len;                    /**< 主题长度； Topic length */
    const uint8_t *payload;              /**< 负载； Payload */
    size_t payload_len;                  /**< 负载长度； Payload length */
} core_protocol_data_t;

/**
 * @brief Core 服务命令类型
 * @details Core service command type
 */
typedef enum {
    CORE_CMD_MQTT_CONFIGURE = 0,         /**< 配置 MQTT； Configure MQTT */
    CORE_CMD_MQTT_TCP_CONNECT,           /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    CORE_CMD_MQTT_CONNECT,               /**< 连接 MQTT； Connect MQTT */
    CORE_CMD_MQTT_DISCONNECT,            /**< 断开 MQTT； Disconnect MQTT */
    CORE_CMD_MQTT_TCP_DISCONNECT,        /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
    CORE_CMD_MQTT_SUBSCRIBE,             /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    CORE_CMD_MQTT_UNSUBSCRIBE,           /**< 退订 MQTT 主题； Unsubscribe MQTT topic */
    CORE_CMD_MQTT_PUBLISH,               /**< 发布 MQTT 消息； Publish MQTT message */
    CORE_CMD_PING,                       /**< 执行 Ping 诊断； Perform Ping diagnostic */
} core_cmd_type_t;

/**
 * @brief Core 服务命令结果
 * @details Core service command result
 */
typedef enum {
    CORE_CMD_RESULT_OK = 0,              /**< 成功； OK */
    CORE_CMD_RESULT_ERROR,               /**< 错误； Error */
    CORE_CMD_RESULT_TIMEOUT,             /**< 超时； Timeout */
    CORE_CMD_RESULT_INVALID_RESPONSE,    /**< 响应无效； Invalid response */
} core_cmd_result_t;

typedef struct {
    uint8_t seq;                         /**< 响应序号； Reply sequence */
    char ip[48];                         /**< 响应 IP； Reply IP */
    uint32_t time_ms;                    /**< 耗时毫秒； Time in milliseconds */
    uint8_t ttl;                         /**< 响应 TTL； Reply TTL */
    bool success;                        /**< 是否成功； Whether successful */
} core_ping_reply_t;

typedef struct {
    uint8_t sent;                        /**< 已发送数量； Sent count */
    uint8_t received;                    /**< 已收到数量； Received count */
    uint8_t lost;                        /**< 丢包数量； Lost count */
    uint32_t min_time_ms;                /**< 最小耗时； Minimum time */
    uint32_t max_time_ms;                /**< 最大耗时； Maximum time */
    uint32_t avg_time_ms;                /**< 平均耗时； Average time */
} core_ping_summary_t;

/**
 * @brief Core 服务命令完成回调
 * @details Core service command done callback
 */
typedef void (*core_cmd_done_callback_t)(core_handle_t *core,
                                         core_cmd_type_t type,
                                         core_cmd_result_t result,
                                         const void *result_data,
                                         void *user_ctx);

/**
 * @brief Core 服务命令
 * @details Core service command
 */
typedef struct {
    core_cmd_type_t type;                /**< 命令类型； Command type */
    core_cmd_done_callback_t done_cb;    /**< 完成回调； Done callback */
    void *user_ctx;                      /**< 用户上下文； User context */
    uint32_t timeout_ms;                 /**< 超时时间； Timeout */
    union {
        struct {
            const char *client_id;       /**< 客户端 ID； Client ID */
            const char *username;        /**< 用户名； Username */
            const char *password;        /**< 密码； Password */
            const char *host;            /**< 主机； Host */
            uint16_t port;               /**< 端口； Port */
            bool clean_session;          /**< 清理会话； Clean session */
            uint16_t keepalive_s;        /**< 保活秒数； Keepalive seconds */
        } mqtt_config;                   /**< MQTT 配置； MQTT config */
        struct {
            const char *topic;           /**< 主题； Topic */
            uint8_t qos;                 /**< QoS； QoS */
        } mqtt_subscribe;                /**< MQTT 订阅参数； MQTT subscribe args */
        struct {
            const char *topic;           /**< 主题； Topic */
        } mqtt_unsubscribe;              /**< MQTT 退订参数； MQTT unsubscribe args */
        struct {
            const char *topic;           /**< 主题； Topic */
            const uint8_t *payload;      /**< 负载； Payload */
            size_t payload_len;          /**< 负载长度； Payload length */
            uint8_t qos;                 /**< QoS； QoS */
            bool retain;                 /**< 保留消息； Retain */
        } mqtt_publish;                  /**< MQTT 发布参数； MQTT publish args */
        struct {
            const char *host;            /**< 主机； Host */
            uint8_t count;               /**< 发送次数； Request count */
            uint16_t data_len;           /**< 数据长度； Data length */
            uint16_t timeout_100ms;      /**< 单包超时，单位 100ms； Per-packet timeout in 100ms */
            uint8_t ttl;                 /**< TTL； TTL */
            core_ping_reply_t *replies;  /**< 响应输出数组； Reply output array */
            size_t max_replies;          /**< 响应数组容量； Reply array capacity */
            core_ping_summary_t *summary; /**< 可选汇总输出； Optional summary output */
        } ping;                          /**< Ping 参数； Ping args */
    } data;                              /**< 命令数据； Command data */
} core_cmd_t;

/**
 * @brief LTE 核心服务事件数据
 * @details LTE core service event data
 */
typedef struct {
    core_net_state_t net_state;          /**< 网络状态； Network state */
    int error_code;                      /**< 错误码； Error code */
    core_protocol_data_t protocol_data;  /**< 协议数据； Protocol data */
} core_event_data_t;

/**
 * @brief LTE 核心服务事件回调
 * @details LTE core service event callback
 * @param[in] core LTE 核心服务句柄
 * @param[in] event_id LTE 核心服务事件 ID
 * @param[in] data LTE 核心服务事件数据，可能为 NULL
 * @param[in] user_ctx 用户上下文
 */
typedef void (*core_event_callback_t)(core_handle_t *core,
                                      core_event_id_t event_id,
                                      const core_event_data_t *data,
                                      void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 LTE 核心服务
 * @details Create LTE core service
 * @note modem 由调用方持有，Core 仅借用，不会销毁该调制解调器。
 * @param[in] config LTE 核心服务配置
 * @param[in] modem 调制解调器句柄
 * @return
 *         - 非 NULL: 创建并初始化成功，返回 LTE 核心服务句柄
 *         - NULL: 参数无效、内存不足或初始化失败
 */
core_handle_t *core_create(const core_config_t *config, modem_handle_t *modem);

/**
 * @brief 销毁 LTE 核心服务
 * @details Destroy LTE core service
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许销毁
 *         - other: 下层资源清理错误码
 */
esp_err_t core_destroy(core_handle_t *me);

/**
 * @brief 启动 LTE 核心服务
 * @details Start LTE core service
 * @note 该函数异步提交启动请求；Core FSM 会调用 modem_start()，随后执行网络激活流程。
 * @note ESP_OK 仅表示请求已提交；最终 online 结果通过 CORE_EVENT_NET_ONLINE 上报。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t core_start(core_handle_t *me);

/**
 * @brief 停止 LTE 核心服务
 * @details Stop LTE core service
 * @note 该函数异步提交停止请求，返回 ESP_OK 表示请求已提交；最终状态通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t core_stop(core_handle_t *me);

/**
 * @brief 注册 LTE 核心服务事件回调
 * @details Register LTE core service event callback
 * @note Core 仅保存一个回调槽位，重复调用会覆盖之前的回调和用户上下文。
 * @param[in] me LTE 核心服务句柄
 * @param[in] callback 事件回调函数
 * @param[in] user_ctx 用户上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t core_register_event_callback(core_handle_t *me,
                                       core_event_callback_t callback,
                                       void *user_ctx);

/**
 * @brief 获取 LTE 核心服务事件循环
 * @details Get LTE core service event loop
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - 非 NULL: LTE 核心服务事件循环句柄
 *         - NULL: 参数无效或事件循环未创建
 */
esp_event_loop_handle_t core_get_event_loop(core_handle_t *me);

/**
 * @brief 获取 LTE 核心服务状态
 * @details Get LTE core service state
 * @param[in] me LTE 核心服务句柄
 * @param[out] state LTE 核心服务状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t core_get_state(core_handle_t *me, core_state_t *state);

/**
 * @brief 获取 LTE 网络状态
 * @details Get LTE network state
 * @param[in] me LTE 核心服务句柄
 * @param[out] state LTE 网络状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t core_get_net_state(core_handle_t *me, core_net_state_t *state);

/**
 * @brief 连接 LTE 网络
 * @details Connect LTE network
 * @note 该函数异步提交网络连接请求，返回 ESP_OK 表示请求已提交；最终结果通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t core_connect(core_handle_t *me);

/**
 * @brief 断开 LTE 网络
 * @details Disconnect LTE network
 * @note 该函数异步提交网络断开请求，返回 ESP_OK 表示请求已提交；最终结果通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t core_disconnect(core_handle_t *me);

/**
 * @brief 提交 Core 服务命令
 * @details Submit Core service command
 * @note 该函数异步提交服务命令，返回 ESP_OK 表示命令已入队。
 * @param[in] me LTE 核心服务句柄
 * @param[in] cmd Core 服务命令
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: FSM 队列已满
 */
esp_err_t core_submit_cmd(core_handle_t *me, const core_cmd_t *cmd);

/**
 * @brief 释放 Core 协议事件负载
 * @details Release heap-owned payload carried by CORE_EVENT_PROTOCOL_DATA.
 * @param[in,out] event_data Core 事件数据
 */
void core_release_event_payload(core_event_data_t *event_data);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
