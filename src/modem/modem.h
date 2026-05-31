/**
 * @file modem.h
 * @brief 调制解调器公共接口
 * @details Modem public interface
 * @author JovisDreams
 * @date 2026-05-23
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

/**
 * @brief 调制解调器字符串最大长度
 * @details Modem string maximum lengths
 */
#define MODEM_IMEI_MAX_LEN      16      /**< IMEI 最大长度； IMEI maximum length */
#define MODEM_IMSI_MAX_LEN      16      /**< IMSI 最大长度； IMSI maximum length */
#define MODEM_ICCID_MAX_LEN     24      /**< ICCID 最大长度； ICCID maximum length */
#define MODEM_MODEL_MAX_LEN     32      /**< 型号最大长度； Model maximum length */
#define MODEM_FW_REV_MAX_LEN    64      /**< 固件版本最大长度； Firmware revision maximum length */
#define MODEM_APN_MAX_LEN       64      /**< APN 最大长度； APN maximum length */
#define MODEM_PDP_TYPE_MAX_LEN  8       /**< PDP 类型最大长度； PDP type maximum length */
#define MODEM_IP_ADDR_MAX_LEN   48      /**< IP 地址最大长度； IP address maximum length */

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief 调制解调器句柄
 * @details Modem handle
 */
typedef struct modem modem_t;

/**
 * @brief 调制解调器状态
 * @details Modem state
 */
typedef enum {
    MODEM_STATE_CREATED = 0,        /**< 已创建； Created */
    MODEM_STATE_INITIALIZING,       /**< 初始化中； Initializing */
    MODEM_STATE_READY,              /**< 就绪； Ready */
    MODEM_STATE_REGISTERING,        /**< 注册中； Registering */
    MODEM_STATE_REGISTERED,         /**< 已注册； Registered */
    MODEM_STATE_PDP_ACTIVE,         /**< PDP 已激活； PDP active */
    MODEM_STATE_ERROR,              /**< 错误； Error */
    MODEM_STATE_DESTROYING,         /**< 销毁中； Destroying */
} modem_state_t;

/**
 * @brief 网络注册状态
 * @details Network registration status
 */
typedef enum {
    MODEM_REG_NOT_REGISTERED = 0,   /**< 未注册； Not registered */
    MODEM_REG_REGISTERED_HOME,      /**< 本网注册； Registered home */
    MODEM_REG_SEARCHING,            /**< 搜网中； Searching */
    MODEM_REG_DENIED,               /**< 注册被拒绝； Registration denied */
    MODEM_REG_UNKNOWN,              /**< 未知； Unknown */
    MODEM_REG_REGISTERED_ROAMING,   /**< 漫游注册； Registered roaming */
} modem_reg_status_t;

/**
 * @brief SIM 卡状态
 * @details SIM card status
 */
typedef enum {
    MODEM_SIM_UNKNOWN = 0,          /**< 未知； Unknown */
    MODEM_SIM_READY,                /**< 已就绪； Ready */
    MODEM_SIM_PIN_REQUIRED,         /**< 需要 PIN； PIN required */
    MODEM_SIM_PUK_REQUIRED,         /**< 需要 PUK； PUK required */
    MODEM_SIM_NOT_INSERTED,         /**< 未插卡； Not inserted */
    MODEM_SIM_ERROR,                /**< SIM 错误； SIM error */
} modem_sim_status_t;

/**
 * @brief 调制解调器信息
 * @details Modem information
 */
typedef struct {
    char imei[MODEM_IMEI_MAX_LEN];              /**< IMEI； IMEI */
    char imsi[MODEM_IMSI_MAX_LEN];              /**< IMSI； IMSI */
    char iccid[MODEM_ICCID_MAX_LEN];            /**< ICCID； ICCID */
    char model[MODEM_MODEL_MAX_LEN];            /**< 型号； Model */
    char fw_revision[MODEM_FW_REV_MAX_LEN];     /**< 固件版本； Firmware revision */
} modem_info_t;

/**
 * @brief 信号质量
 * @details Signal quality
 */
typedef struct {
    int rssi;                       /**< 原始 RSSI； Raw RSSI */
    int ber;                        /**< 误码率； Bit error rate */
    int rssi_dbm;                   /**< RSSI dBm 值； RSSI dBm value */
    bool rssi_dbm_valid;            /**< RSSI dBm 是否有效； Whether RSSI dBm is valid */
} modem_signal_t;

/**
 * @brief PDP 上下文
 * @details PDP context
 */
typedef struct {
    uint8_t cid;                                /**< 上下文 ID； Context ID */
    char apn[MODEM_APN_MAX_LEN];               /**< APN； APN */
    char pdp_type[MODEM_PDP_TYPE_MAX_LEN];     /**< PDP 类型； PDP type */
    bool active;                               /**< 是否激活； Whether active */
    char ip_addr[MODEM_IP_ADDR_MAX_LEN];       /**< IP 地址； IP address */
} modem_pdp_context_t;

/**
 * @brief MQTT 配置参数
 * @details MQTT configuration parameters
 */
typedef struct {
    const char *client_id;        /**< 客户端 ID； Client ID */
    const char *username;         /**< 用户名，可为 NULL； Username, can be NULL */
    const char *password;         /**< 密码，可为 NULL； Password, can be NULL */
    const char *host;             /**< Broker 主机名或 IP； Broker host name or IP */
    uint16_t port;                /**< Broker 端口号； Broker port */
    bool clean_session;           /**< 是否使用 clean session； Whether to use clean session */
    uint16_t keepalive_s;         /**< 保活时间（秒）； Keepalive in seconds */
} modem_mqtt_config_t;

/**
 * @brief MQTT 主题参数
 * @details MQTT topic parameters
 */
typedef struct {
    const char *topic;            /**< 主题字符串； Topic string */
    uint8_t qos;                  /**< QoS 等级； QoS level */
} modem_mqtt_topic_t;

/**
 * @brief MQTT 发布参数
 * @details MQTT publish parameters
 */
typedef struct {
    const char *topic;            /**< 主题字符串； Topic string */
    const uint8_t *payload;       /**< 负载数据； Payload data */
    size_t payload_len;           /**< 负载长度； Payload length */
    uint8_t qos;                  /**< QoS 等级； QoS level */
    bool retain;                  /**< 是否保留消息； Whether to retain message */
} modem_mqtt_publish_t;

/**
 * @brief Ping 请求参数
 * @details Ping request parameters
 */
typedef struct {
    const char *host;             /**< 目标主机名或 IP； Target host name or IP */
    uint8_t count;                /**< Ping 次数； Ping count */
    uint16_t data_len;            /**< 单包数据长度； Per-packet data length */
    uint16_t timeout_100ms;       /**< 单包超时，单位 100ms； Per-packet timeout in 100ms units */
    uint8_t ttl;                  /**< TTL 值； TTL value */
    uint32_t total_timeout_ms;    /**< 总超时，0 表示自动计算； Total timeout, 0 for automatic calculation */
} modem_ping_request_t;

/**
 * @brief Ping 单包响应
 * @details Ping single reply
 */
typedef struct {
    uint8_t seq;                  /**< 响应序号； Reply sequence */
    char ip[48];                  /**< 目标 IP 地址； Target IP address */
    uint32_t time_ms;             /**< 响应耗时（毫秒）； Reply time in milliseconds */
    uint8_t ttl;                  /**< 响应 TTL； Reply TTL */
    bool success;                 /**< 是否成功； Whether successful */
} modem_ping_reply_t;

/**
 * @brief Ping 汇总结果
 * @details Ping summary result
 */
typedef struct {
    uint8_t sent;                 /**< 发送数量； Sent count */
    uint8_t received;             /**< 接收数量； Received count */
    uint8_t lost;                 /**< 丢包数量； Lost count */
    uint32_t min_time_ms;         /**< 最小耗时（毫秒）； Minimum time in milliseconds */
    uint32_t max_time_ms;         /**< 最大耗时（毫秒）； Maximum time in milliseconds */
    uint32_t avg_time_ms;         /**< 平均耗时（毫秒）； Average time in milliseconds */
} modem_ping_summary_t;

/**
 * @brief 协议类型
 * @details Protocol type
 */
typedef enum {
    MODEM_PROTOCOL_MQTT = 0,      /**< MQTT 协议； MQTT protocol */
} modem_protocol_t;

/**
 * @brief 协议数据事件负载
 * @details For MODEM_EVENT_PROTOCOL_DATA, topic and payload must be heap-owned
 * buffers when passed to modem_post_event(). If modem_post_event() succeeds,
 * Modem owns and frees them after the callback returns; on failure, caller keeps ownership.
 * Callback pointers are valid only during modem_event_callback_t;
 * consumers must copy topic/payload if they need to retain them.
 * @note `topic` 和 `payload` 指针只在回调期间有效；需要保留时调用方必须复制。
 */
typedef struct {
    modem_protocol_t protocol;    /**< 协议类型； Protocol type */
    const char *topic;            /**< 主题指针； Topic pointer */
    size_t topic_len;             /**< 主题长度； Topic length */
    const uint8_t *payload;       /**< 负载指针； Payload pointer */
    size_t payload_len;           /**< 负载长度； Payload length */
} modem_protocol_data_t;

/**
 * @brief 调制解调器事件 ID
 * @details Modem event ID
 */
typedef enum {
    MODEM_EVENT_READY = 0,          /**< 就绪事件； Ready event */
    MODEM_EVENT_SIM_CHANGED,        /**< SIM 状态变化； SIM status changed */
    MODEM_EVENT_REG_CHANGED,        /**< 注册状态变化； Registration status changed */
    MODEM_EVENT_PDP_ACTIVATED,      /**< PDP 已激活； PDP activated */
    MODEM_EVENT_PDP_DEACTIVATED,    /**< PDP 已去激活； PDP deactivated */
    MODEM_EVENT_SIGNAL_CHANGED,     /**< 信号变化； Signal changed */
    MODEM_EVENT_ERROR,              /**< 错误事件； Error event */
    MODEM_EVENT_PROTOCOL_DATA,      /**< 协议数据事件； Protocol data event */
    MODEM_EVENT_PROTOCOL_CLOSED,    /**< 协议连接关闭； Protocol connection closed */
} modem_event_id_t;

/**
 * @brief 调制解调器事件
 * @details Modem event
 */
typedef struct {
    modem_event_id_t id;            /**< 事件 ID； Event ID */
    union {
        modem_sim_status_t sim_status;      /**< SIM 状态； SIM status */
        modem_reg_status_t reg_status;      /**< 注册状态； Registration status */
        modem_pdp_context_t pdp;            /**< PDP 上下文； PDP context */
        modem_signal_t signal;              /**< 信号质量； Signal quality */
        modem_protocol_data_t protocol_data;     /**< 协议数据； Protocol data */
        int error_code;                     /**< 错误码； Error code */
    } data;                         /**< 事件数据； Event data */
} modem_event_t;

/**
 * @brief 调制解调器事件回调
 * @details Modem event callback
 * @param[in] modem 调制解调器句柄
 * @param[in] event 调制解调器事件
 * @param[in] user_ctx 用户上下文
 */
typedef void (*modem_event_callback_t)(modem_t *modem,
                                       const modem_event_t *event,
                                       void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 销毁调制解调器
 * @details Destroy modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t modem_destroy(modem_t *me);

/**
 * @brief 初始化调制解调器
 * @details Initialize modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 初始化失败
 */
esp_err_t modem_init(modem_t *me);

/**
 * @brief 复位调制解调器
 * @details Reset modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 复位失败
 */
esp_err_t modem_reset(modem_t *me);

/**
 * @brief 注册事件回调
 * @details Register event callback
 * @param[in] me 调制解调器句柄
 * @param[in] callback 事件回调函数
 * @param[in] user_ctx 用户上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t modem_register_event_callback(modem_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

/**
 * @brief 获取调制解调器状态
 * @details Get modem state
 * @param[in] me 调制解调器句柄
 * @param[out] state 调制解调器状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t modem_get_state(modem_t *me, modem_state_t *state);

/**
 * @brief 获取调制解调器信息
 * @details Get modem information
 * @param[in] me 调制解调器句柄
 * @param[out] info 调制解调器信息
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_info(modem_t *me, modem_info_t *info);

/**
 * @brief 获取 SIM 卡状态
 * @details Get SIM card status
 * @param[in] me 调制解调器句柄
 * @param[out] status SIM 卡状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_sim_status(modem_t *me, modem_sim_status_t *status);

/**
 * @brief 获取信号质量
 * @details Get signal quality
 * @param[in] me 调制解调器句柄
 * @param[out] signal 信号质量
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal);

/**
 * @brief 获取网络注册状态
 * @details Get network registration status
 * @param[in] me 调制解调器句柄
 * @param[out] status 网络注册状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status);

/**
 * @brief 获取分组域附着状态
 * @details Get packet domain attach status
 * @param[in] me 调制解调器句柄
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached);

/**
 * @brief 设置 APN
 * @details Set APN
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @param[in] apn APN 字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 设置失败
 */
esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn);

/**
 * @brief 激活 PDP 上下文
 * @details Activate PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 激活失败
 */
esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid);

/**
 * @brief 去激活 PDP 上下文
 * @details Deactivate PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 去激活失败
 */
esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid);

/**
 * @brief 获取 PDP 上下文
 * @details Get PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @param[out] pdp PDP 上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp);

/**
 * @brief 配置 MQTT 参数
 * @details Configure MQTT parameters
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT 配置参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 配置失败
 */
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config);

/**
 * @brief 建立 MQTT TCP 通道
 * @details Connect MQTT TCP channel
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 连接失败
 */
esp_err_t modem_mqtt_tcp_connect(modem_t *me);

/**
 * @brief 连接 MQTT Broker
 * @details Connect to MQTT broker
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 连接失败
 */
esp_err_t modem_mqtt_connect(modem_t *me);

/**
 * @brief 断开 MQTT Broker 连接
 * @details Disconnect from MQTT broker
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 断开失败
 */
esp_err_t modem_mqtt_disconnect(modem_t *me);

/**
 * @brief 断开 MQTT TCP 通道
 * @details Disconnect MQTT TCP channel
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 断开失败
 */
esp_err_t modem_mqtt_tcp_disconnect(modem_t *me);

/**
 * @brief 订阅 MQTT 主题
 * @details Subscribe MQTT topic
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 订阅失败
 */
esp_err_t modem_mqtt_subscribe(modem_t *me,
                               const modem_mqtt_topic_t *topic);

/**
 * @brief 取消订阅 MQTT 主题
 * @details Unsubscribe MQTT topic
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 取消订阅失败
 */
esp_err_t modem_mqtt_unsubscribe(modem_t *me,
                                 const modem_mqtt_topic_t *topic);

/**
 * @brief 发布 MQTT 消息
 * @details Publish MQTT message
 * @param[in] me 调制解调器句柄
 * @param[in] publish MQTT 发布参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 发布失败
 */
esp_err_t modem_mqtt_publish(modem_t *me,
                             const modem_mqtt_publish_t *publish);

/**
 * @brief 执行 Ping 诊断
 * @details Execute ping diagnostic
 * @param[in] me 调制解调器句柄
 * @param[in] request Ping 请求参数
 * @param[out] replies Ping 单包响应数组
 * @param[in] max_replies Ping 单包响应数组容量
 * @param[out] summary Ping 汇总结果，可为 NULL
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - ESP_FAIL: Ping 失败
 */
esp_err_t modem_ping(modem_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
