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
#include "lwlte.h"

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
 * @brief LTE 核心服务事件配置
 * @details LTE core service event configuration
 */
typedef struct {
    esp_event_loop_handle_t loop;         /**< 共享事件总线（借用）； Shared event bus (borrowed) */
} core_event_config_t;

/**
 * @brief LTE 核心服务网络配置
 * @details LTE core service network configuration
 */
typedef struct {
    const char *apn;                      /**< APN； APN */
    uint8_t primary_cid;                  /**< 主 PDP 上下文 ID； Primary PDP context ID */
    uint32_t net_activate_timeout_ms;     /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;          /**< 重连延迟； Reconnect delay */
} core_network_config_t;

/**
 * @brief LTE 核心服务 FSM 配置
 * @details LTE core service FSM configuration
 */
typedef struct {
    int queue_size;                       /**< FSM 队列长度； FSM queue size */
    int task_stack;                       /**< FSM 任务栈大小； FSM task stack size */
    int task_priority;                    /**< FSM 任务优先级； FSM task priority */
} core_fsm_config_t;

/**
 * @brief LTE 核心服务配置
 * @details LTE core service configuration
 */
typedef struct {
    core_event_config_t event;            /**< 事件总线； Event bus */
    core_network_config_t network;        /**< 网络策略； Network policy */
    core_fsm_config_t fsm;                /**< FSM 资源； FSM resources */
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
 * @brief Core 协议类型
 * @details Core protocol type
 */
typedef enum {
    CORE_PROTOCOL_MQTT = 0,              /**< MQTT 协议； MQTT protocol */
    CORE_PROTOCOL_TCP,                   /**< TCP 协议； TCP protocol */
    CORE_PROTOCOL_MAX,                   /**< 协议数量； Protocol count */
} core_protocol_t;

/**
 * @brief Core 协议数据
 * @details Core protocol data. topic/payload are borrowed and only valid during
 * the core_protocol_callback_t invocation; the consumer must copy data it needs
 * before returning.
 */
typedef struct {
    core_protocol_t protocol;            /**< 协议类型； Protocol type */
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const char *topic;                   /**< 主题，MQTT 使用； Topic, used by MQTT */
    size_t topic_len;                    /**< 主题长度； Topic length */
    const uint8_t *payload;              /**< 负载； Payload */
    size_t payload_len;                  /**< 负载长度； Payload length */
    int reason;                          /**< 事件原因； Event reason */
    int modem_error_code;                /**< 模块原始错误码； Raw modem error code */
} core_protocol_data_t;

/**
 * @brief Core 协议数据回调（私有，service-service 内部）
 * @details Core protocol data callback (private, service-service internal)
 * @note core FSM 同步调用；callback 必须只做轻量操作（入队、memcpy），不得阻塞。
 */
typedef void (*core_protocol_callback_t)(core_handle_t *me,
                                         const core_protocol_data_t *data,
                                         void *user_ctx);

/**
 * @brief Core 协议通道关闭回调（私有）
 * @details Core protocol closed callback (private)
 */
typedef void (*core_protocol_closed_callback_t)(core_handle_t *me,
                                                core_protocol_t protocol,
                                                const core_protocol_data_t *data,
                                                void *user_ctx);

/**
 * @brief 注册 Core 协议数据回调
 * @details Register Core protocol data callback
 * @note 回调在 Core FSM 任务上同步执行，必须非阻塞；只做轻量操作（入队、memcpy）。
 * @note callback 为 NULL 时注销当前回调；user_ctx 不被 Core 拥有，注册期间须由调用方保持有效。
 * @note Core 按协议保存回调槽位，重复注册同一协议会覆盖之前的回调和用户上下文。
 * @param[in] me LTE 核心服务句柄
 * @param[in] protocol 协议类型
 * @param[in] callback 协议数据回调，NULL 表示注销
 * @param[in] user_ctx 用户上下文，原样传回回调
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: Core 正在销毁
 */
esp_err_t core_register_protocol_callback(core_handle_t *me,
                                          core_protocol_t protocol,
                                          core_protocol_callback_t callback,
                                          void *user_ctx);

/**
 * @brief 注册 Core 协议通道关闭回调
 * @details Register Core protocol closed callback
 * @note 回调在 Core FSM 任务上同步执行，必须非阻塞。
 * @note callback 为 NULL 时注销当前回调；user_ctx 不被 Core 拥有，注册期间须由调用方保持有效。
 * @note Core 按协议保存回调槽位，重复注册同一协议会覆盖之前的回调和用户上下文。
 * @param[in] me LTE 核心服务句柄
 * @param[in] protocol 协议类型
 * @param[in] callback 协议通道关闭回调，NULL 表示注销
 * @param[in] user_ctx 用户上下文，原样传回回调
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: Core 正在销毁
 */
esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                 core_protocol_t protocol,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx);

typedef enum {
    CORE_SOCKET_PROTO_TCP = 0,           /**< TCP socket； TCP socket */
} core_socket_proto_t;

/**
 * @brief Socket 传输类型
 * @details Socket transport type
 */
typedef enum {
    CORE_SOCKET_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    CORE_SOCKET_TRANSPORT_TLS,            /**< TLS； TLS */
} core_socket_transport_t;

typedef struct {
    core_socket_proto_t proto;           /**< Socket 协议； Socket protocol */
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const char *host;                    /**< 主机； Host */
    uint16_t port;                       /**< 端口； Port */
    uint32_t timeout_ms;                 /**< 打开超时； Open timeout */
    core_socket_transport_t transport;   /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;              /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
} core_socket_open_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const uint8_t *data;                 /**< 发送数据； Send data */
    size_t len;                          /**< 发送长度； Send length */
    uint32_t timeout_ms;                 /**< 发送超时； Send timeout */
} core_socket_send_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    size_t max_len;                      /**< 最大读取长度； Maximum read length */
} core_socket_recv_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    uint8_t *payload;                    /**< 堆负载，接收方拥有； Heap payload, receiver owns */
    size_t payload_len;                  /**< 负载长度； Payload length */
    size_t remaining_len;                /**< 模块缓存剩余长度； Remaining cached length */
    int modem_error_code;                /**< 模块错误码； Modem error code */
} core_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    uint32_t timeout_ms;                 /**< 关闭超时； Close timeout */
} core_socket_close_t;

typedef struct {
    esp_err_t error_code;                /**< ESP 错误码； ESP error code */
    int modem_error_code;                /**< 模块原始错误码； Raw modem error code */
} core_socket_result_t;

typedef struct {
    uint8_t context_id;                  /**< SSL context ID； SSL context ID */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
    uint8_t tls_version;                 /**< TLS 版本； TLS version */
    uint32_t negotiate_timeout_s;        /**< 协商超时秒数； Negotiation timeout seconds */
    bool ignore_cert_time;               /**< 是否忽略证书时间； Whether to ignore certificate time */
    const char *hostname;                /**< 主机名/SNI； Hostname/SNI */
} core_ssl_context_config_t;

typedef struct {
    const uint8_t *ca_cert_pem;          /**< CA 证书 PEM； CA certificate PEM */
    size_t ca_cert_len;                  /**< CA 证书长度； CA certificate length */
    const uint8_t *client_cert_pem;      /**< 客户端证书 PEM； Client certificate PEM */
    size_t client_cert_len;              /**< 客户端证书长度； Client certificate length */
    const uint8_t *client_key_pem;       /**< 客户端私钥 PEM； Client private key PEM */
    size_t client_key_len;               /**< 客户端私钥长度； Client private key length */
} core_ssl_credentials_t;

typedef struct {
    bool provisioned;                    /**< 必需对象是否已存在； Whether required objects exist */
    bool ca_cert_present;                /**< CA 证书是否存在； Whether CA certificate exists */
    bool client_cert_present;            /**< 客户端证书是否存在； Whether client certificate exists */
    bool client_key_present;             /**< 客户端私钥是否存在； Whether client key exists */
    bool check_valid;                    /**< 模块校验是否通过； Whether module check passed */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
} core_ssl_context_status_t;

/**
 * @brief Core 服务命令类型
 * @details Core service command type
 */
typedef enum {
    CORE_CMD_SSL_PROVISION = 0,          /**< 写入并配置 SSL context； Provision SSL context */
    CORE_CMD_SSL_GET_CONTEXT_STATUS,     /**< 查询 SSL context 状态； Query SSL context status */
    CORE_CMD_MQTT_CONFIGURE,             /**< 配置 MQTT； Configure MQTT */
    CORE_CMD_MQTT_TCP_CONNECT,           /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    CORE_CMD_MQTT_CONNECT,               /**< 连接 MQTT； Connect MQTT */
    CORE_CMD_MQTT_DISCONNECT,            /**< 断开 MQTT； Disconnect MQTT */
    CORE_CMD_MQTT_TCP_DISCONNECT,        /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
    CORE_CMD_MQTT_SUBSCRIBE,             /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    CORE_CMD_MQTT_UNSUBSCRIBE,           /**< 退订 MQTT 主题； Unsubscribe MQTT topic */
    CORE_CMD_MQTT_PUBLISH,               /**< 发布 MQTT 消息； Publish MQTT message */
    CORE_CMD_PING,                       /**< 执行 Ping 诊断； Perform Ping diagnostic */
    CORE_CMD_SOCKET_OPEN,                /**< 打开 socket； Open socket */
    CORE_CMD_SOCKET_SEND,                /**< 发送 socket 数据； Send socket data */
    CORE_CMD_SOCKET_RECV,                /**< 接收 socket 数据； Receive socket data */
    CORE_CMD_SOCKET_CLOSE,               /**< 关闭 socket； Close socket */
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
            lwlte_mqtt_transport_t transport; /**< MQTT 传输； MQTT transport */
            uint8_t ssl_context_id;      /**< SSL context ID； SSL context ID */
        } mqtt_config;                   /**< MQTT 配置； MQTT config */
        struct {
            core_ssl_context_config_t config;
            core_ssl_credentials_t credentials;
        } ssl_provision;                 /**< SSL provision 参数； SSL provision args */
        struct {
            uint8_t context_id;          /**< SSL context ID； SSL context ID */
            core_ssl_context_status_t *status; /**< 状态输出； Status output */
        } ssl_get_context_status;        /**< SSL status 参数； SSL status args */
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
        core_socket_open_t socket_open;  /**< Socket 打开参数； Socket open args */
        core_socket_send_t socket_send;  /**< Socket 发送参数； Socket send args */
        core_socket_recv_t socket_recv;  /**< Socket 接收参数； Socket receive args */
        core_socket_close_t socket_close; /**< Socket 关闭参数； Socket close args */
    } data;                              /**< 命令数据； Command data */
} core_cmd_t;

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
 * @note ESP_OK 仅表示请求已提交；最终 online 结果通过 LWLTE_EVENT_NET_ONLINE 上报。
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

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
