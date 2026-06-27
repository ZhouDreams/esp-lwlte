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

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"

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
typedef struct lwlte_handle lwlte_handle_t;

/**
 * @brief LTE TCP 连接句柄
 * @details LTE TCP connection handle
 */
typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;

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
 * @brief LTE TCP 连接状态
 * @details LTE TCP connection state
 */
typedef enum {
    LWLTE_TCP_CONN_STATE_CREATED = 0,     /**< 已创建； Created */
    LWLTE_TCP_CONN_STATE_CONNECTING,      /**< 连接中； Connecting */
    LWLTE_TCP_CONN_STATE_CONNECTED,       /**< 已连接； Connected */
    LWLTE_TCP_CONN_STATE_CLOSING,         /**< 关闭中； Closing */
    LWLTE_TCP_CONN_STATE_CLOSED,          /**< 已关闭； Closed */
    LWLTE_TCP_CONN_STATE_ERROR,           /**< 错误； Error */
} lwlte_tcp_conn_state_t;

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
 * @brief LTE 用户事件 base
 * @details LTE user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_EVENT);

/**
 * @brief LTE MQTT 用户事件 base
 * @details LTE MQTT user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT);

/**
 * @brief LTE TCP 用户事件 base
 * @details LTE TCP user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_TCP_EVENT);

/**
 * @brief LTE 用户事件 ID
 * @details LTE user event ID
 * @note 投递到共享事件总线 LWLTE_EVENT。
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
} lwlte_event_id_t;

/**
 * @brief LTE MQTT 用户事件 ID
 * @details LTE MQTT user event ID
 * @note 投递到共享事件总线 LWLTE_MQTT_EVENT。
 */
typedef enum {
    LWLTE_MQTT_EVENT_STARTED = 0,   /**< MQTT 已启动； MQTT started */
    LWLTE_MQTT_EVENT_STOPPED,       /**< MQTT 已停止； MQTT stopped */
    LWLTE_MQTT_EVENT_CONNECTING,    /**< MQTT 连接中； MQTT connecting */
    LWLTE_MQTT_EVENT_CONNECTED,     /**< MQTT 已连接； MQTT connected */
    LWLTE_MQTT_EVENT_DISCONNECTED,  /**< MQTT 已断开； MQTT disconnected */
    LWLTE_MQTT_EVENT_SUBSCRIBED,    /**< MQTT 已订阅； MQTT subscribed */
    LWLTE_MQTT_EVENT_UNSUBSCRIBED,  /**< MQTT 已取消订阅； MQTT unsubscribed */
    LWLTE_MQTT_EVENT_PUBLISHED,     /**< MQTT 已发布； MQTT published */
    LWLTE_MQTT_EVENT_DATA,          /**< MQTT 数据； MQTT data */
    LWLTE_MQTT_EVENT_ERROR,         /**< MQTT 错误； MQTT error */
} lwlte_mqtt_event_id_t;

/**
 * @brief LTE TCP 用户事件 ID
 * @details LTE TCP user event ID
 * @note 投递到共享事件总线 LWLTE_TCP_EVENT。
 */
typedef enum {
    LWLTE_TCP_EVENT_STARTED = 0,          /**< TCP 服务已启动； TCP service started */
    LWLTE_TCP_EVENT_STOPPED,              /**< TCP 服务已停止； TCP service stopped */
    LWLTE_TCP_EVENT_CONNECTED,            /**< TCP 已连接； TCP connected */
    LWLTE_TCP_EVENT_DISCONNECTED,         /**< TCP 已断开； TCP disconnected */
    LWLTE_TCP_EVENT_SENT,                 /**< TCP 数据已送入模块协议栈； TCP data accepted by module stack */
    LWLTE_TCP_EVENT_DATA,                 /**< TCP 数据； TCP data */
    LWLTE_TCP_EVENT_ERROR,                /**< TCP 错误； TCP error */
} lwlte_tcp_event_id_t;

/**
 * @brief LTE 用户事件数据
 * @details LTE user event data
 */
typedef struct {
    lwlte_net_state_t net_state;    /**< 网络状态； Network state */
    int error_code;                 /**< 诊断错误码； Diagnostic error code */
} lwlte_event_data_t;

/**
 * @brief LTE MQTT 用户事件数据
 * @details LTE MQTT user event data
 */
typedef struct {
    lwlte_mqtt_state_t mqtt_state;  /**< MQTT 状态； MQTT state */
    int error_code;                 /**< 诊断错误码； Diagnostic error code */
    lwlte_mqtt_msg_t msg;           /**< MQTT 消息，仅 LWLTE_MQTT_EVENT_DATA 有效 */
    bool owns_payload;              /**< DATA 事件为 true，其余为 false */
} lwlte_mqtt_event_data_t;

/**
 * @brief TCP 客户端配置
 * @details TCP client configuration
 * @note 0 值使用默认值。v1 仅支持 max_conns 为 0 或 1；大于 1 返回 ESP_ERR_NOT_SUPPORTED。
 */
typedef struct {
    uint8_t max_conns;                    /**< 最大连接数，0 使用默认值 1； Maximum connections, 0 uses default 1 */
    int send_queue_size;                  /**< 发送队列长度，0 使用默认值； Send queue size, 0 uses default */
    size_t max_tx_len;                    /**< 单次发送最大长度，0 使用默认值； Maximum TX length, 0 uses default */
    size_t max_rx_event_len;              /**< 单个 DATA 事件最大 RX 长度，0 使用默认值； Maximum RX event length, 0 uses default */
    uint32_t open_timeout_ms;             /**< 打开超时，0 使用默认值； Open timeout, 0 uses default */
    uint32_t send_timeout_ms;             /**< 发送超时，0 使用默认值； Send timeout, 0 uses default */
    uint32_t close_timeout_ms;            /**< 关闭超时，0 使用默认值； Close timeout, 0 uses default */
    int fsm_queue_size;                   /**< TCP FSM 队列长度，0 使用默认值； TCP FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< TCP FSM 任务栈大小，0 使用默认值； TCP FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< TCP FSM 任务优先级，0 使用默认值； TCP FSM task priority, 0 uses default */
} lwlte_tcp_config_t;

/**
 * @brief TCP 传输类型
 * @details TCP transport type
 */
typedef enum {
    LWLTE_TCP_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    LWLTE_TCP_TRANSPORT_TLS,            /**< TLS； TLS */
} lwlte_tcp_transport_t;

/**
 * @brief TCP 打开连接配置
 * @details TCP open connection configuration
 */
typedef struct {
    const char           *host;           /**< 目标主机或 IP； Target host or IP */
    uint16_t              port;           /**< 目标端口； Target port */
    lwlte_tcp_transport_t transport;      /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t               ssl_context_id; /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
    void                 *user_ctx;       /**< 用户上下文，事件中原样返回； User context returned in events */
} lwlte_tcp_open_config_t;

/**
 * @brief LTE TCP 用户事件数据
 * @details LTE TCP user event data
 */
typedef struct {
    lwlte_tcp_conn_t *conn;               /**< TCP 连接句柄； TCP connection handle */
    void *user_ctx;                       /**< 用户上下文； User context */
    lwlte_tcp_conn_state_t conn_state;    /**< 连接状态； Connection state */
    esp_err_t error_code;                 /**< ESP 错误码； ESP error code */
    int modem_error_code;                 /**< 模块原始错误码； Raw modem error code */
    int reason;                           /**< 断开或错误原因； Disconnect or error reason */
    size_t sent_len;                      /**< SENT 事件已接受长度； Accepted length for SENT event */
    const uint8_t *payload;               /**< DATA 事件负载； DATA event payload */
    size_t payload_len;                   /**< DATA 事件负载长度； DATA event payload length */
    bool owns_payload;                    /**< DATA 事件为 true，其余为 false； True for DATA events */
    bool owns_event;                      /**< 事件引用待释放； Event reference to release */
} lwlte_tcp_event_data_t;

/**
 * @brief LTE SSL 认证模式
 * @details LTE SSL authentication mode
 */
typedef enum {
    LWLTE_SSL_AUTH_NONE = 0,        /**< 不认证； No authentication */
    LWLTE_SSL_AUTH_SERVER,          /**< 服务器认证； Server authentication */
    LWLTE_SSL_AUTH_MUTUAL,          /**< 双向认证； Mutual authentication */
} lwlte_ssl_auth_mode_t;

/**
 * @brief LTE MQTT 传输类型
 * @details LTE MQTT transport type
 */
typedef enum {
    LWLTE_MQTT_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    LWLTE_MQTT_TRANSPORT_TLS,            /**< TLS； TLS */
} lwlte_mqtt_transport_t;

/**
 * @brief LTE SSL context 配置
 * @details LTE SSL context configuration
 * @note tls_version 为 0 时使用模块默认版本；非 0 值为模块特定编码。
 * @note ignore_cert_time 会写入模块 SSL context 状态，可能影响后续使用同一 context 的连接。
 * @note hostname 可为 NULL；非 NULL 时作为可选主机名/SNI 传递给下层模块适配器。
 */
typedef struct {
    uint8_t context_id;                  /**< SSL context ID； SSL context ID */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
    uint8_t tls_version;                 /**< TLS 版本，0 使用模块默认； TLS version, 0 uses module default */
    uint32_t negotiate_timeout_s;        /**< 协商超时秒数，0 使用模块默认； Negotiation timeout seconds, 0 uses module default */
    bool ignore_cert_time;               /**< 是否忽略证书时间； Whether to ignore certificate time */
    const char *hostname;                /**< 可选主机名/SNI； Optional hostname/SNI */
} lwlte_ssl_context_config_t;

/**
 * @brief LTE SSL 证书材料
 * @details LTE SSL credential material
 * @note LWLTE_SSL_AUTH_NONE 不需要 PEM 数据，但 lwlte_ssl_provision() 的 credentials 指针仍必须非 NULL。
 * @note LWLTE_SSL_AUTH_SERVER 需要 CA PEM 指针和长度；LWLTE_SSL_AUTH_MUTUAL 还需要客户端证书和私钥指针及长度。
 * @note 可选证书材料的指针/长度必须同时存在或同时为空；仅提供指针或仅提供长度均为无效参数。
 * @note PEM 缓冲仅在 lwlte_ssl_provision() 调用期间借用；下层会在异步执行前复制需要的数据。
 */
typedef struct {
    const uint8_t *ca_cert_pem;          /**< CA 证书 PEM； CA certificate PEM */
    size_t ca_cert_len;                  /**< CA 证书长度； CA certificate length */
    const uint8_t *client_cert_pem;      /**< 客户端证书 PEM； Client certificate PEM */
    size_t client_cert_len;              /**< 客户端证书长度； Client certificate length */
    const uint8_t *client_key_pem;       /**< 客户端私钥 PEM； Client private key PEM */
    size_t client_key_len;               /**< 客户端私钥长度； Client private key length */
} lwlte_ssl_credentials_t;

/**
 * @brief LTE SSL context 状态
 * @details LTE SSL context status
 */
typedef struct {
    bool provisioned;                    /**< 必需对象是否已存在； Whether required objects exist */
    bool ca_cert_present;                /**< CA 证书是否存在； Whether CA certificate exists */
    bool client_cert_present;            /**< 客户端证书是否存在； Whether client certificate exists */
    bool client_key_present;             /**< 客户端私钥是否存在； Whether client key exists */
    bool check_valid;                    /**< 模块校验是否通过； Whether module check passed */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 查询到的认证模式； Queried authentication mode */
} lwlte_ssl_context_status_t;

/**
 * @brief MQTT 客户端配置
 * @details MQTT client configuration
 * @note host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 * @note config 及其字符串指针由调用方拥有，在 lwlte_mqtt_init() 返回前必须保持有效。
 */
typedef struct {
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    lwlte_mqtt_transport_t transport;     /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;               /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_mqtt_config_t;

/**
 * @brief UART 硬件配置
 * @details UART hardware configuration
 */
typedef struct {
    uart_port_t num;        /**< 必填 UART 端口号； Required UART port number */
    gpio_num_t  tx_pin;     /**< 必填 UART TX GPIO，不能为 GPIO_NUM_NC； Required UART TX GPIO, not GPIO_NUM_NC */
    gpio_num_t  rx_pin;     /**< 必填 UART RX GPIO，不能为 GPIO_NUM_NC； Required UART RX GPIO, not GPIO_NUM_NC */
    int         baud_rate;  /**< 必填 UART 波特率，必须大于 0； Required UART baud rate, must be > 0 */
} lwlte_uart_config_t;

/**
 * @brief AT 引擎调优配置
 * @details AT engine tuning configuration
 * @note 所有字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    int rx_buf_size;            /**< AT RX 缓冲大小，0 使用默认值； AT RX buffer size, 0 uses default */
    int rx_task_stack;          /**< AT RX 任务栈大小，0 使用默认值； AT RX task stack, 0 uses default */
    int rx_task_priority;       /**< AT RX 任务优先级，0 使用默认值； AT RX task priority, 0 uses default */
    int rx_line_buf_size;       /**< AT 单行缓冲大小，0 使用默认值； AT line buffer size, 0 uses default */
    int cmd_default_timeout_ms; /**< AT 默认命令超时，0 使用默认值； AT default command timeout, 0 uses default */
    int max_response_lines;     /**< AT 最大响应行数，0 使用默认值； AT maximum response lines, 0 uses default */
} lwlte_at_engine_config_t;

/**
 * @brief 调制解调器配置
 * @details Modem configuration
 * @note en_pin 可设为 GPIO_NUM_NC 以禁用门面对 EN GPIO 的控制。
 * @note ready_timeout_ms 为 0 时使用下层默认值；该值为硬复位后等待 AT OK 的总超时。
 * @note 有符号的队列、任务字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
    gpio_num_t en_pin;                 /**< 可选模块 EN GPIO，GPIO_NUM_NC 表示不控制； Optional module EN GPIO, GPIO_NUM_NC disables control */
    uint32_t   reset_pulse_ms;         /**< Modem 复位脉冲(EN 拉低保持)时长，0 表示不额外等待； Modem reset pulse (EN low hold) length, 0 skips extra wait */
    uint32_t   ready_timeout_ms;       /**< 启动 AT OK 等待总超时，0 使用下层默认值； Startup AT OK wait timeout, 0 uses lower-layer default */
    uint32_t   default_cmd_timeout_ms; /**< Modem 默认命令超时，0 使用默认值； Modem default command timeout, 0 uses default */
    int        event_queue_size;       /**< Modem 事件队列长度，0 使用默认值； Modem event queue size, 0 uses default */
    int        event_task_stack;       /**< Modem 事件任务栈大小，0 使用默认值； Modem event task stack, 0 uses default */
    int        event_task_priority;    /**< Modem 事件任务优先级，0 使用默认值； Modem event task priority, 0 uses default */
} lwlte_modem_config_t;

/**
 * @brief Core 网络/PDP 与状态机配置
 * @details Core network/PDP and FSM configuration
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note primary_cid 当前仅支持 1。
 * @note 有符号的队列、任务字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
    const char *apn;                     /**< 可选 APN，NULL/空表示门面不配置； Optional APN, NULL/empty means facade does not configure it */
    uint8_t     primary_cid;             /**< 必填主 PDP 上下文 ID，当前仅支持 1； Required primary PDP context ID, currently supports 1 only */
    uint32_t    net_activate_timeout_ms; /**< 网络激活总超时，0 使用 Core 默认值； Network activation timeout, 0 uses Core default */
    uint32_t    reconnect_delay_ms;      /**< 重连延迟，0 使用 Core 默认值； Reconnect delay, 0 uses Core default */
    int         fsm_queue_size;          /**< Core FSM 队列长度，0 使用默认值； Core FSM queue size, 0 uses default */
    int         fsm_task_stack;          /**< Core FSM 任务栈大小，0 使用默认值； Core FSM task stack, 0 uses default */
    int         fsm_task_priority;       /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
} lwlte_core_config_t;

/**
 * @brief 事件总线配置
 * @details Event bus configuration
 */
typedef struct {
    esp_event_loop_handle_t loop;        /**< 可选事件总线，NULL 使用 default loop； Optional event loop, NULL uses default */
} lwlte_event_config_t;

/**
 * @brief LTE 公共基础配置
 * @details LTE common base configuration
 */
typedef struct {
    lwlte_uart_config_t      uart;      /**< UART 硬件； UART hardware */
    lwlte_at_engine_config_t at_engine; /**< AT 引擎调优； AT engine tuning */
    lwlte_modem_config_t     modem;     /**< 调制解调器； Modem */
    lwlte_core_config_t      core;      /**< Core 网络/状态机； Core network/FSM */
    lwlte_event_config_t     event;     /**< 事件总线； Event bus */
} lwlte_base_config_t;

/**
 * @brief Air780EP LTE 初始化配置
 * @details Air780EP LTE initialization configuration
 * @note base 为公共基础配置；UART 端口必须满足 UART_NUM_0 <= base.uart.num < UART_NUM_MAX，TX/RX 必须是有效 GPIO 且不能为 GPIO_NUM_NC，base.uart.baud_rate 必须大于 0。
 * @note Air780EP 启动在硬复位后通过 AT OK 探测就绪；base.modem.ready_timeout_ms 为该阶段总超时。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_air780ep_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* Air780EP 特有字段：暂无，预留； Air780EP-specific fields: none yet, reserved */
} lwlte_air780ep_config_t;

/**
 * @brief ML307R LTE 初始化配置
 * @details ML307R LTE initialization configuration
 * @note base 为公共基础配置；校验约束与 Air780EP 相同。
 * @note ML307R 启动不等待 +MATREADY，硬复位后重复发送 AT 并等待 OK；base.modem.ready_timeout_ms 为该阶段总超时。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_ml307r_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* ML307R 特有字段：暂无，预留； ML307R-specific fields: none yet, reserved */
} lwlte_ml307r_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 初始化 Air780EP LTE 用户门面
 * @details Initialize Air780EP LTE user facade
 * @note 该函数只创建 LTE 用户门面及内部对象，不启动模块、不等待 AT ready、不激活 PDP。
 * @note ESP_OK 返回时 *out_lte 为可用句柄，所有权转移给调用方，必须通过 lwlte_destroy() 释放。
 * @note 调用方应注册事件处理函数后调用 lwlte_start()；最终 online 结果通过 LWLTE_EVENT_NET_ONLINE 上报。
 * @note 非 ESP_OK 返回时不会转移句柄所有权，门面会尽力释放已创建的内部资源。
 * @note config 及其 apn 字符串指针由调用方拥有，在函数返回前必须保持有效。
 * @param[in] config Air780EP LTE 初始化配置
 * @param[out] out_lte LTE 用户门面句柄输出指针
 * @return
 *         - ESP_OK: 初始化成功，门面句柄可用
 *         - ESP_ERR_INVALID_ARG: 参数无效、必填字段缺失或字段超出有效范围
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_INVALID_STATE: 下层状态错误
 *         - ESP_FAIL: GPIO、UART、Modem 或 Core 创建失败
 *         - 其他 esp_err_t: 下层创建、初始化或清理错误
 */
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_handle_t **out_lte);

/**
 * @brief 初始化 ML307R LTE 用户门面
 * @details Initialize ML307R LTE user facade
 * @note 该函数只创建 LTE 用户门面及内部对象，不启动模块、不等待 AT ready、不激活 PDP。
 * @note ML307R 启动阶段由 lwlte_start() 触发，modem_start() 使用 AT OK 探测，不等待 +MATREADY。
 * @param[in] config ML307R LTE 初始化配置
 * @param[out] out_lte LTE 用户门面句柄输出指针
 * @return
 *         - ESP_OK: 初始化成功，门面句柄可用
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: GPIO、UART、Modem 或 Core 创建失败
 */
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_handle_t **out_lte);

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
esp_err_t lwlte_destroy(lwlte_handle_t *me);

/**
 * @brief 启动 LTE 并异步联网
 * @details Start LTE and connect network asynchronously
 * @note 该函数异步提交启动请求，ESP_OK 仅表示请求已提交，不表示模块 ready 或网络 online。
 * @note 成功联网通过 LWLTE_EVENT_NET_ONLINE 上报，也可通过 lwlte_get_net_state() 查询。
 * @note 建议在调用本函数前先用 esp_event_handler_register() 注册 LWLTE_EVENT 事件处理函数。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许启动或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层请求提交错误
 */
esp_err_t lwlte_start(lwlte_handle_t *me);

/**
 * @brief 停止 LTE 并对模块断电（硬件关机）
 * @details Stop LTE and power off the module
 * @note 该函数异步提交停机请求：去激活网络、停止 MQTT、对模块 EN 断电，Core 回到 STOPPED。
 * @note ESP_OK 仅表示请求已提交；完成通过 LWLTE_EVENT_STOPPED 上报，或用 lwlte_get_state() 查询。
 * @note 停机后可再次 lwlte_start() 重新上电联网；重启前应等待状态变为 LWLTE_STATE_STOPPED。
 * @note en_pin 为 GPIO_NUM_NC 时无法物理断电，降级为逻辑停机（模块仍上电）。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许停止或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层停止错误
 */
esp_err_t lwlte_stop(lwlte_handle_t *me);

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
esp_err_t lwlte_get_state(lwlte_handle_t *me, lwlte_state_t *state);

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
esp_err_t lwlte_get_net_state(lwlte_handle_t *me, lwlte_net_state_t *state);

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
esp_err_t lwlte_ping(lwlte_handle_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);

/**
 * @brief 写入并配置 LTE SSL context
 * @details Provision and configure LTE SSL context
 * @note credentials 不能为 NULL；LWLTE_SSL_AUTH_NONE 使用空 credentials 结构体表示不提供 PEM 数据。
 * @note 证书材料要求由 config->auth_mode 决定；认证模式无效或缺少必需 PEM 数据时返回 ESP_ERR_INVALID_ARG。
 * @note PEM 数据超过模块或适配器限制时返回 ESP_ERR_INVALID_SIZE；当前模块不支持 SSL provisioning 时返回 ESP_ERR_NOT_SUPPORTED。
 * @param[in] me LTE 用户门面句柄
 * @param[in] config SSL context 配置
 * @param[in] credentials SSL 证书材料
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_SIZE: PEM 数据超过限制
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许操作或门面正在销毁
 *         - ESP_ERR_NOT_SUPPORTED: 当前模块不支持该操作
 *         - 其他 esp_err_t: 下层错误
 */
esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials);

/**
 * @brief 查询 LTE SSL context 状态
 * @details Query LTE SSL context status
 * @param[in] me LTE 用户门面句柄
 * @param[in] context_id SSL context ID
 * @param[out] status SSL context 状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许操作或门面正在销毁
 *         - 其他 esp_err_t: 下层错误
 */
esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status);

/**
 * @brief 释放 MQTT_DATA 事件的堆缓冲
 * @details Release heap buffers carried by LWLTE_MQTT_EVENT_DATA
 * @note 处理 LWLTE_MQTT_EVENT_DATA 的 handler 必须在返回前调用。
 * @note 如果注册了多个 LWLTE_MQTT_EVENT_DATA handler，只有最后一个调 release() 的才真正释放缓冲；建议单消费者模式，额外观察者应自行拷贝数据。
 * @param[in] data 事件数据指针，可为 NULL
 */
void lwlte_mqtt_event_data_release(lwlte_mqtt_event_data_t *data);

/**
 * @brief 释放 TCP 事件资源
 * @details Release resources carried by LWLTE_TCP_EVENT
 * @note 处理 LWLTE_TCP_EVENT 的 handler 必须在返回前调用。
 * @note 当 owns_payload 为 true 时释放 payload；当 owns_event 为 true 时释放事件连接引用。
 * @note 如果注册了多个 LWLTE_TCP_EVENT handler，建议单消费者模式；额外观察者应自行拷贝数据。
 * @param[in] data 事件数据指针，可为 NULL
 */
void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data);

/**
 * @brief 初始化 TCP 客户端服务
 * @details Initialize TCP client service
 * @note 该函数只创建 TCP 客户端服务对象，不打开 TCP 连接；连接由 lwlte_tcp_open() 触发。
 * @note 须在 lwlte_air780ep_init()/lwlte_ml307r_init() 返回句柄之后、lwlte_destroy() 之前调用。
 * @note 同一句柄只能初始化一次，重复调用返回 ESP_ERR_INVALID_STATE；要更换配置须先 lwlte_tcp_destroy()。
 * @note config 由调用方拥有，仅在该函数执行期间被借用；函数返回后调用方可释放或复用。
 * @param[in] me LTE 用户门面句柄
 * @param[in] config TCP 客户端配置
 * @return
 *         - ESP_OK: 初始化成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 已初始化或门面正在销毁
 *         - ESP_ERR_NOT_SUPPORTED: 配置包含 v1 不支持的能力
 *         - ESP_FAIL: 下层创建失败
 */
esp_err_t lwlte_tcp_init(lwlte_handle_t *me, const lwlte_tcp_config_t *config);

/**
 * @brief 销毁 TCP 客户端服务
 * @details Destroy TCP client service
 * @note 未初始化时返回 ESP_ERR_INVALID_STATE。
 * @note v1 中仍存在 CONNECTING、CONNECTED 或 CLOSING 连接对象时返回 ESP_ERR_INVALID_STATE；应用应先关闭并销毁连接。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 销毁成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未初始化、门面正在销毁或仍有活动连接
 *         - 其他 esp_err_t: 下层销毁错误
 */
esp_err_t lwlte_tcp_destroy(lwlte_handle_t *me);

/**
 * @brief 异步打开 TCP 连接
 * @details Open TCP connection asynchronously
 * @note 调用前 TCP 服务必须已初始化且 LTE 网络必须 online。
 * @note ESP_OK 返回时 *out_conn 为新建连接句柄，所有权转移给调用方；连接结果通过 LWLTE_TCP_EVENT_CONNECTED 或 LWLTE_TCP_EVENT_ERROR 上报。
 * @note user_ctx 从 config 捕获，并在该连接后续所有 TCP 事件中原样返回。
 * @param[in] me LTE 用户门面句柄
 * @param[in] config TCP 打开连接配置
 * @param[out] out_conn TCP 连接句柄输出指针
 * @return
 *         - ESP_OK: 打开请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: TCP 服务未初始化、网络未 online 或已有连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_tcp_open(lwlte_handle_t *me,
                         const lwlte_tcp_open_config_t *config,
                         lwlte_tcp_conn_t **out_conn);

/**
 * @brief 异步发送 TCP 数据
 * @details Send TCP data asynchronously
 * @note data 会在函数返回前复制到 TCP 服务内部 FIFO；调用方可在返回后立即复用或释放原缓冲。
 * @note v1 仅在连接状态为 CONNECTED 时接受发送；发送完成通过 LWLTE_TCP_EVENT_SENT 上报。
 * @param[in] conn TCP 连接句柄
 * @param[in] data 发送数据指针
 * @param[in] len 发送长度，必须大于 0 且不超过配置 max_tx_len
 * @return
 *         - ESP_OK: 发送请求已入队
 *         - ESP_ERR_INVALID_ARG: 参数无效或长度无效
 *         - ESP_ERR_INVALID_STATE: 连接未处于 CONNECTED 状态
 *         - ESP_ERR_TIMEOUT: 发送 FIFO 已满
 *         - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn,
                         const uint8_t *data,
                         size_t len);

/**
 * @brief 异步关闭 TCP 连接
 * @details Close TCP connection asynchronously
 * @note v1 接受 CONNECTED 或 ERROR 状态下关闭；完成后通过 LWLTE_TCP_EVENT_DISCONNECTED 或 LWLTE_TCP_EVENT_ERROR 上报。
 * @note 关闭完成不会自动释放连接对象；应用仍需调用 lwlte_tcp_conn_destroy()。
 * @param[in] conn TCP 连接句柄
 * @return
 *         - ESP_OK: 关闭请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前连接状态不允许关闭
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t *conn);

/**
 * @brief 获取 TCP 连接状态
 * @details Get TCP connection state
 * @param[in] conn TCP 连接句柄
 * @param[out] state TCP 连接状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t lwlte_tcp_conn_get_state(lwlte_tcp_conn_t *conn,
                                   lwlte_tcp_conn_state_t *state);

/**
 * @brief 销毁 TCP 连接对象
 * @details Destroy TCP connection object
 * @note 仅 CLOSED 或 ERROR 状态可销毁；CONNECTING、CONNECTED 或 CLOSING 状态返回 ESP_ERR_INVALID_STATE。
 * @note 销毁后 conn 失效，调用方不得继续使用该指针。
 * @param[in] conn TCP 连接句柄
 * @return
 *         - ESP_OK: 销毁成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前连接状态不允许销毁
 */
esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t *conn);

/**
 * @brief 初始化 MQTT 客户端
 * @details Initialize MQTT client
 * @note 该函数只创建 MQTT 客户端对象，不启动连接；连接由 lwlte_mqtt_start() 触发。
 * @note 须在 lwlte_air780ep_init()/lwlte_ml307r_init() 返回句柄之后、lwlte_destroy() 之前调用；与 lwlte_start() 的先后顺序无要求。
 * @note 同一句柄只能初始化一次，重复调用返回 ESP_ERR_INVALID_STATE；要更换配置须先 lwlte_mqtt_destroy()。
 * @note config 及其字符串字段由调用方拥有，仅在该函数执行期间被借用；函数返回后调用方可释放或复用。
 * @note ESP_OK 返回时 MQTT 客户端可用，最终须通过 lwlte_mqtt_destroy() 或 lwlte_destroy() 释放。
 * @param[in] me LTE 用户门面句柄
 * @param[in] config MQTT 客户端配置
 * @return
 *         - ESP_OK: 初始化成功
 *         - ESP_ERR_INVALID_ARG: 参数无效或必填字段缺失
 *         - ESP_ERR_INVALID_STATE: 已初始化或门面正在销毁
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 下层创建失败
 */
esp_err_t lwlte_mqtt_init(lwlte_handle_t *me, const lwlte_mqtt_config_t *config);

/**
 * @brief 销毁 MQTT 客户端
 * @details Destroy MQTT client
 * @note 该函数从任何 FSM 状态安全调用：若 MQTT 仍在运行（CONNECTED/CONNECTING/...），下层会先自动停止。
 * @note 重复调用或未初始化时返回 ESP_ERR_INVALID_STATE。
 * @note 若应用层未手动调用本函数，lwlte_destroy() 会作为兜底清理 MQTT 客户端。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 销毁成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未初始化或门面正在销毁
 *         - 其他 esp_err_t: 下层销毁错误（已记录日志）
 */
esp_err_t lwlte_mqtt_destroy(lwlte_handle_t *me);

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
esp_err_t lwlte_mqtt_start(lwlte_handle_t *me);

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
esp_err_t lwlte_mqtt_stop(lwlte_handle_t *me);

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
esp_err_t lwlte_mqtt_get_state(lwlte_handle_t *me, lwlte_mqtt_state_t *state);

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
esp_err_t lwlte_mqtt_subscribe(lwlte_handle_t *me, const char *topic, uint8_t qos);

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
esp_err_t lwlte_mqtt_unsubscribe(lwlte_handle_t *me, const char *topic);

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
esp_err_t lwlte_mqtt_publish(lwlte_handle_t *me, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             uint8_t qos, bool retain);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
