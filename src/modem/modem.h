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

typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
} modem_mqtt_config_t;

typedef struct {
    const char *host;
    uint16_t port;
} modem_mqtt_open_t;

typedef struct {
    bool clean_session;
    uint16_t keepalive_s;
} modem_mqtt_login_t;

typedef struct {
    const char *topic;
    uint8_t qos;
} modem_mqtt_topic_t;

typedef struct {
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} modem_mqtt_publish_t;

typedef enum {
    MODEM_PROTOCOL_MQTT = 0,
} modem_protocol_t;

typedef struct {
    modem_protocol_t protocol;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
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
    MODEM_EVENT_PROTOCOL_DATA,      /**< 协议数据事件； Protocol data event */
    MODEM_EVENT_PROTOCOL_CLOSED,    /**< 协议连接关闭； Protocol connection closed */
    MODEM_EVENT_ERROR,              /**< 错误事件； Error event */
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

esp_err_t modem_mqtt_config(modem_t *me,
                            const modem_mqtt_config_t *config);
esp_err_t modem_mqtt_open(modem_t *me,
                          const modem_mqtt_open_t *open);
esp_err_t modem_mqtt_login(modem_t *me,
                           const modem_mqtt_login_t *login);
esp_err_t modem_mqtt_disconnect(modem_t *me);
esp_err_t modem_mqtt_subscribe(modem_t *me,
                               const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_unsubscribe(modem_t *me,
                                 const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_publish(modem_t *me,
                             const modem_mqtt_publish_t *publish);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
