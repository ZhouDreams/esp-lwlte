/**
 * @file modem_air780ep.c
 * @brief Air780EP 调制解调器实现
 * @details Air780EP modem implementation
 * @author JovisDreams
 * @date 2026-05-23
 */

/*********************
 *      INCLUDES
 *********************/
#include "modem_air780ep.h"
#include "modem_priv.h"

#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "modem_air780ep"
#define AIR780EP_MAX_PDP_CONTEXTS        4
#define AIR780EP_MAX_RESPONSE_LINES      101
#define AIR780EP_PARSE_BUF_SIZE          128
#define AIR780EP_DEFAULT_CMD_TIMEOUT_MS  9000
#define AIR780EP_DEFAULT_READY_TIMEOUT_MS 30000
#define AIR780EP_CSTT_TIMEOUT_MS         60000
#define AIR780EP_CIICR_TIMEOUT_MS        90000
#define AIR780EP_CIPSHUT_TIMEOUT_MS      90000
#define AIR780EP_AT_READY_PROBE_TIMEOUT_MS 1000
#define AIR780EP_INIT_RETRY_DELAY_MS       500
#define AIR780EP_INIT_CMD_MAX_ATTEMPTS     3
#define AIR780EP_SIM_READY_TIMEOUT_MS     10000
#define AIR780EP_SIM_READY_POLL_INTERVAL_MS 1000
#define AIR780EP_CME_SIM_NOT_INSERTED     10
#define AIR780EP_CME_SIM_PIN_REQUIRED     11
#define AIR780EP_CME_SIM_PUK_REQUIRED     12
#define AIR780EP_CME_SIM_FAILURE          13
#define AIR780EP_CME_SIM_BUSY             14
#define AIR780EP_CME_SIM_WRONG            15
#define AIR780EP_URC_CPIN                "+CPIN:"
#define AIR780EP_URC_CREG                "+CREG:"
#define AIR780EP_URC_CEREG               "+CEREG:"
#define AIR780EP_URC_CGREG               "+CGREG:"
#define AIR780EP_URC_CGEV                "+CGEV:"
#define AIR780EP_URC_PDP_DEACT           "+PDP DEACT"
#define AIR780EP_URC_PDP_COLON_DEACT     "+PDP:DEACT"
#define AIR780EP_URC_MSUB                "+MSUB:"
#define AIR780EP_MQTT_CMD_TIMEOUT_MS     9000
#define AIR780EP_MQTT_CONNECT_TIMEOUT_MS 60000
#define AIR780EP_MQTT_PAYLOAD_PROMPT     ">"
#define AIR780EP_SSL_MQTT_CONTEXT_ID       88
#define AIR780EP_SSL_TCP_CONTEXT_ID        0   /**< TCP SSL socket 固定 context 0； TCP SSL socket fixed ctx 0 */
#define AIR780EP_SSL_MAX_PEM_LEN           10240U
#define AIR780EP_SSL_CMD_TIMEOUT_MS        9000
#define AIR780EP_SSL_WRITE_TIMEOUT_S       30U
#define AIR780EP_SSL_CONTEXT_BITMAP_BITS   256U
#define AIR780EP_TCP_CONN_ID                 0
#define AIR780EP_TCP_MAX_HEX_READ_BYTES      730
#define AIR780EP_TCP_PAYLOAD_PROMPT          ">"
#define AIR780EP_CIPRXGET_READY_PREFIX       "+CIPRXGET: 1"
#define AIR780EP_CIPRXGET_READY_COMPACT_PREFIX "+CIPRXGET:1"
#define AIR780EP_TCP_ERROR_PREFIX            "TCP ERROR:"
#define AIR780EP_CIPPING_PREFIX         "+CIPPING:"
#define AIR780EP_CIPPING_MAX_COUNT      100
#define AIR780EP_CIPPING_CMD_OVERHEAD_MS 5000U
#define AIR780EP_HTTP_SSL_CONTEXT_ID    153
#define AIR780EP_HTTP_CMD_TIMEOUT_MS    9000
#define AIR780EP_HTTP_ACTION_TIMEOUT_MS 120000
#define AIR780EP_HTTPDATA_PROMPT_MS     10000
#define AIR780EP_HTTPDATA_BODY_MAX      3356
#define AIR780EP_HTTPREAD_BODY_MAX      3356

_Static_assert(AIR780EP_MAX_RESPONSE_LINES >= AIR780EP_CIPPING_MAX_COUNT + 1,
               "Air780EP CIPPING response storage must hold replies plus final status");

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief Air780EP 命令上下文
 * @details Air780EP command context
 */
typedef struct {
    char *lines[AIR780EP_MAX_RESPONSE_LINES];
    at_response_t response;
} air780ep_cmd_ctx_t;

/**
 * @brief Air780EP 调制解调器实例
 * @details Air780EP modem instance
 */
typedef struct {
    struct modem_t base;
    modem_air780ep_config_t config;
    at_urc_handler_t cpin_handler;
    at_urc_handler_t creg_handler;
    at_urc_handler_t cereg_handler;
    at_urc_handler_t cgreg_handler;
    at_urc_handler_t cgev_handler;
    at_urc_handler_t pdp_deact_handler;
    at_urc_handler_t pdp_colon_deact_handler;
    at_urc_handler_t msub_handler;
    at_urc_handler_t tcp_readable_handler;
    at_urc_handler_t tcp_readable_compact_handler;
    at_urc_handler_t tcp_closed_handler;
    at_urc_handler_t tcp_error_handler;
    modem_info_t cached_info;
    modem_sim_status_t last_sim_status;
    modem_reg_status_t last_reg_status;
    modem_signal_t last_signal;
    modem_pdp_context_t pdp[AIR780EP_MAX_PDP_CONTEXTS];
    modem_mqtt_config_t mqtt_config;
    uint32_t ssl_provisioned_bitmap[(AIR780EP_SSL_CONTEXT_BITMAP_BITS + 31U) / 32U];
    modem_ssl_auth_mode_t ssl_auth_modes[AIR780EP_SSL_CONTEXT_BITMAP_BITS];
    modem_mqtt_transport_t mqtt_transport;
    uint8_t mqtt_ssl_context_id;
    bool urc_registered;
    bool initialized;
    bool mqtt_configured;
    bool mqtt_tcp_connected;
    bool mqtt_session_connected;
    bool mqtt_data_enabled;
} modem_air780ep_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 销毁 Air780EP 子类资源
 * @details Destroy Air780EP subclass resources
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: URC 注销错误
 */
static esp_err_t air780ep_destroy(modem_handle_t me);

/**
 * @brief 启动 Air780EP 调制解调器
 * @details Start Air780EP modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: 启动失败
 */
static esp_err_t air780ep_start(modem_handle_t me);

/**
 * @brief 停止 Air780EP 调制解调器
 * @details Stop Air780EP modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 停止失败
 */
static esp_err_t air780ep_stop(modem_handle_t me);

/**
 * @brief 复位 Air780EP 调制解调器
 * @details Reset Air780EP modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: 复位失败
 */
static esp_err_t air780ep_reset(modem_handle_t me);

/**
 * @brief 获取 Air780EP 调制解调器信息
 * @details Get Air780EP modem information
 * @param[in] me 调制解调器句柄
 * @param[out] info 调制解调器信息
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_info(modem_handle_t me, modem_info_t *info);

/**
 * @brief 获取 Air780EP SIM 卡状态
 * @details Get Air780EP SIM card status
 * @param[in] me 调制解调器句柄
 * @param[out] status SIM 卡状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_sim_status(modem_handle_t me, modem_sim_status_t *status);

/**
 * @brief 获取 Air780EP 信号质量
 * @details Get Air780EP signal quality
 * @param[in] me 调制解调器句柄
 * @param[out] signal 信号质量
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_signal(modem_handle_t me, modem_signal_t *signal);

/**
 * @brief 获取 Air780EP 网络注册状态
 * @details Get Air780EP network registration status
 * @param[in] me 调制解调器句柄
 * @param[out] status 网络注册状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_registration(modem_handle_t me, modem_reg_status_t *status);

/**
 * @brief 获取 Air780EP 分组域附着状态
 * @details Get Air780EP packet domain attach status
 * @param[in] me 调制解调器句柄
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_packet_attach_status(modem_handle_t me, bool *attached);

/**
 * @brief 设置 Air780EP APN
 * @details Set Air780EP APN
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @param[in] apn APN 字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_set_apn(modem_handle_t me, uint8_t cid, const char *apn);

/**
 * @brief 激活 Air780EP PDP 上下文
 * @details Activate Air780EP PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_activate_pdp(modem_handle_t me, uint8_t cid);

/**
 * @brief 去激活 Air780EP PDP 上下文
 * @details Deactivate Air780EP PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_deactivate_pdp(modem_handle_t me, uint8_t cid);

/**
 * @brief 获取 Air780EP PDP 上下文
 * @details Get Air780EP PDP context
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID
 * @param[out] pdp PDP 上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
static esp_err_t air780ep_get_pdp_context(modem_handle_t me, uint8_t cid,
                                             modem_pdp_context_t *pdp);

/**
 * @brief 打开 Air780EP TCP Socket
 * @details Open Air780EP TCP socket
 * @param[in] me 调制解调器句柄
 * @param[in] open Socket 打开参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_socket_open(modem_handle_t me,
                                      const modem_socket_open_t *open);

/**
 * @brief 发送 Air780EP TCP Socket 数据
 * @details Send Air780EP TCP socket data
 * @param[in] me 调制解调器句柄
 * @param[in] send Socket 发送参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_socket_send(modem_handle_t me,
                                      const modem_socket_send_t *send);

/**
 * @brief 接收 Air780EP TCP Socket 数据
 * @details Receive Air780EP TCP socket data
 * @param[in] me 调制解调器句柄
 * @param[in] recv Socket 接收参数
 * @param[out] result Socket 接收结果
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_socket_recv(modem_handle_t me,
                                      const modem_socket_recv_t *recv,
                                      modem_socket_recv_result_t *result);

/**
 * @brief 关闭 Air780EP TCP Socket
 * @details Close Air780EP TCP socket
 * @param[in] me 调制解调器句柄
 * @param[in] close Socket 关闭参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_socket_close(modem_handle_t me,
                                       const modem_socket_close_t *close);

/**
 * @brief 准备 Air780EP TCP Socket 模式
 * @details Prepare Air780EP TCP socket mode
 * @param[in] self Air780EP 调制解调器实例
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_socket_prepare(modem_air780ep_t *self);

/**
 * @brief 投递 TCP 可读事件
 * @details Post TCP readable event
 * @param[in] self Air780EP 调制解调器实例
 */
static void air780ep_post_tcp_readable(modem_air780ep_t *self);

/**
 * @brief 投递 TCP 关闭事件
 * @details Post TCP closed event
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] reason 关闭原因
 * @param[in] modem_error_code 模块错误码
 */
static void air780ep_post_tcp_closed(modem_air780ep_t *self, int reason,
                                     int modem_error_code);

/**
 * @brief 解码十六进制负载
 * @details Decode hex payload into heap buffer
 * @param[in] hex 十六进制字符串
 * @param[out] out_payload 堆负载输出
 * @param[out] out_len 负载长度输出
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t decode_hex_payload(const char *hex, uint8_t **out_payload,
                                    size_t *out_len);

/**
 * @brief 检查响应是否包含文本
 * @details Check whether response contains text
 * @param[in] response AT 响应
 * @param[in] needle 查找文本
 * @return true: 包含； false: 不包含
 */
static bool response_contains(const at_response_t *response, const char *needle);

/**
 * @brief 查找 +CIPRXGET 十六进制数据行
 * @details Find +CIPRXGET hex payload line
 * @param[in] response AT 响应
 * @param[out] out_remaining_len 剩余长度输出
 * @return 十六进制负载字符串或 NULL
 */
static const char *find_ciprxget_hex_line(const at_response_t *response,
                                          size_t *out_remaining_len);

/**
 * @brief 解析十六进制半字节
 * @details Parse hex nibble
 * @param[in] c 十六进制字符
 * @return 0-15 表示成功，-1 表示无效
 */
static int hex_nibble(char c);

/**
 * @brief 深拷贝 MQTT 配置
 * @details Deep-copy MQTT config; frees any existing dst fields
 * @param[out] dst 目标 MQTT 配置
 * @param[in] src 源 MQTT 配置
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 */
static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src);
/**
 * @brief 克隆 MQTT 字符串
 * @details Malloc'd copy of a string; NULL input yields NULL output
 * @param[in] value 源字符串，可为 NULL
 * @return 克隆字符串或 NULL
 */
static char *clone_mqtt_string(const char *value);
/**
 * @brief 释放 MQTT 配置
 * @details Free all strings inside config and zero the struct
 * @param[in,out] config MQTT 配置
 */
static void free_mqtt_config(modem_mqtt_config_t *config);
/**
 * @brief 清空 MQTT 状态
 * @details Reset MQTT flags to false and free stored config
 * @param[in] self Air780EP 调制解调器实例
 */
static void clear_mqtt_state(modem_air780ep_t *self);
/**
 * @brief 配置 Air780EP MQTT
 * @details Configure Air780EP MQTT via AT+MCONFIG; refuses if already connected
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT 配置
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 已连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_configure(modem_handle_t me,
                                          const modem_mqtt_config_t *config);
/**
 * @brief 建立 Air780EP MQTT TCP 通道
 * @details Connect MQTT TCP channel via AT+MIPSTART
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未配置或 TCP 已连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_tcp_connect(modem_handle_t me);
/**
 * @brief 连接 Air780EP MQTT 会话
 * @details Connect MQTT session via AT+MCONNECT
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未配置、TCP 未连接或会话已连接
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_connect(modem_handle_t me);
/**
 * @brief 断开 Air780EP MQTT 会话
 * @details Disconnect MQTT session via AT+MDISCONNECT
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 会话未连接
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_disconnect(modem_handle_t me);
/**
 * @brief 断开 Air780EP MQTT TCP 通道
 * @details Disconnect MQTT TCP channel via AT+MIPCLOSE
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: TCP 未连接或会话仍连接
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_tcp_disconnect(modem_handle_t me);
/**
 * @brief 订阅 Air780EP MQTT 主题
 * @details Subscribe MQTT topic via AT+MSUB
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_subscribe(modem_handle_t me,
                                           const modem_mqtt_topic_t *topic);
/**
 * @brief 取消订阅 Air780EP MQTT 主题
 * @details Unsubscribe MQTT topic via AT+MUNSUB
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_unsubscribe(modem_handle_t me,
                                            const modem_mqtt_topic_t *topic);
/**
 * @brief 发布 Air780EP MQTT 消息
 * @details Publish MQTT message via AT+MPUBEX with binary payload
 * @param[in] me 调制解调器句柄
 * @param[in] publish MQTT 发布参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NOT_SUPPORTED: QoS 不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_publish(modem_handle_t me,
                                        const modem_mqtt_publish_t *publish);
/**
 * @brief 查询 Air780EP MQTT 状态
 * @details Query MQTT status via AT+MQTTSTATU; state must be 0-2
 * @param[in] me 调制解调器句柄
 * @param[out] status MQTT 状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_mqtt_get_status(modem_handle_t me,
                                           modem_mqtt_status_t *status);
/**
 * @brief 写入并配置 Air780EP SSL context
 * @details Provision Air780EP SSL context
 * @param[in] me 调制解调器句柄
 * @param[in] config SSL context 配置
 * @param[in] credentials SSL 证书材料
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_ssl_provision(modem_handle_t me,
                                         const modem_ssl_context_config_t *config,
                                         const modem_ssl_credentials_t *credentials);
/**
 * @brief 查询 Air780EP SSL context 状态
 * @details Query Air780EP SSL context status
 * @param[in] me 调制解调器句柄
 * @param[in] context_id SSL context ID
 * @param[out] status SSL context 状态
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_ssl_get_context_status(modem_handle_t me,
                                                 uint8_t context_id,
                                                 modem_ssl_context_status_t *status);
/**
 * @brief 标记 SSL context 缓存状态
 * @details Mark cached SSL context state
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] context_id SSL context ID
 * @param[in] auth_mode SSL 认证模式
 * @param[in] provisioned 是否已配置
 */
static void ssl_mark_context(modem_air780ep_t *self, uint8_t context_id,
                             modem_ssl_auth_mode_t auth_mode, bool provisioned);
/**
 * @brief 查询 SSL context 缓存标记
 * @details Query cached SSL context mark
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] context_id SSL context ID
 * @return true: 已标记； false: 未标记
 */
static bool ssl_context_marked(modem_air780ep_t *self, uint8_t context_id);
/**
 * @brief 生成 SSL 文件对象名
 * @details Generate SSL file object names
 * @param[in] context_id SSL context ID
 * @param[out] ca_name CA 文件名
 * @param[in] ca_name_size CA 文件名缓冲区大小
 * @param[out] client_cert_name 客户端证书文件名
 * @param[in] client_cert_name_size 客户端证书文件名缓冲区大小
 * @param[out] client_key_name 客户端私钥文件名
 * @param[in] client_key_name_size 客户端私钥文件名缓冲区大小
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_ssl_object_names(uint8_t context_id,
                                           char *ca_name, size_t ca_name_size,
                                           char *client_cert_name,
                                           size_t client_cert_name_size,
                                           char *client_key_name,
                                           size_t client_key_name_size);
/**
 * @brief 删除文件并忽略不存在错误
 * @details Delete file and ignore missing-file error
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] name 文件名
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_delete_file_ignore_missing(modem_air780ep_t *self,
                                                     const char *name);
/**
 * @brief 写入 Air780EP 文件系统文件
 * @details Write file into Air780EP filesystem
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] name 文件名
 * @param[in] payload 文件内容
 * @param[in] len 文件内容长度
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_write_file(modem_air780ep_t *self, const char *name,
                                     const uint8_t *payload, size_t len);
/**
 * @brief 绑定 SSL 文件到 context
 * @details Bind SSL file to context
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] tag SSL 配置标签
 * @param[in] context_id SSL context ID
 * @param[in] name 文件名
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_bind_ssl_file(modem_air780ep_t *self,
                                        const char *tag, uint8_t context_id,
                                        const char *name);
/**
 * @brief 查询 Air780EP 文件是否存在
 * @details Query whether Air780EP file exists
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] name 文件名
 * @param[out] exists 是否存在
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_file_exists(modem_air780ep_t *self, const char *name,
                                      bool *exists);
/**
 * @brief 查询 SSL 认证模式
 * @details Query SSL authentication mode
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] context_id SSL context ID
 * @param[out] auth_mode SSL 认证模式
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_query_ssl_auth_mode(modem_air780ep_t *self,
                                              uint8_t context_id,
                                              modem_ssl_auth_mode_t *auth_mode);
/**
 * @brief 失效 SSL context 及其 TLS MQTT 依赖状态
 * @details Invalidate SSL context and dependent TLS MQTT state
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] context_id SSL context ID
 */
static void air780ep_invalidate_ssl_context(modem_air780ep_t *self,
                                            uint8_t context_id);
/**
 * @brief 清除依赖指定 SSL context 的 TLS MQTT 配置
 * @details Clear TLS MQTT config depending on the given SSL context; caller holds lock
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] context_id SSL context ID
 */
static void air780ep_clear_tls_mqtt_config_if_context_locked(modem_air780ep_t *self,
                                                            uint8_t context_id);
/**
 * @brief 清除 SSL 内存状态
 * @details Clear SSL in-memory state
 * @param[in] self Air780EP 调制解调器实例
 */
static void air780ep_clear_ssl_state(modem_air780ep_t *self);
/**
 * @brief 复位 MQTT 模式为直连 ASCII
 * @details Reset MQTT mode via AT+MQTTMSGSET=0 then AT+MQTTMODE=0
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t reset_mqtt_modes(modem_air780ep_t *self);
/**
 * @brief 映射 MQTT 状态值
 * @details Map integer MQTT status (0/1/2) to enum; others map to OFFLINE
 * @param[in] state AT 状态值
 * @return MQTT 状态枚举
 */
static modem_mqtt_status_t map_mqtt_status(int state);
/**
 * @brief Air780EP Ping 探测
 * @details Send AT+CIPPING, parse each reply line and compute summary
 * @param[in] me 调制解调器句柄
 * @param[in] request Ping 请求参数
 * @param[out] replies Ping 响应数组
 * @param[in] max_replies replies 容量，须不小于 request->count
 * @param[out] summary Ping 汇总统计
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_ping(modem_handle_t me,
                               const modem_ping_request_t *request,
                               modem_ping_reply_t *replies,
                               size_t max_replies,
                               modem_ping_summary_t *summary);
/**
 * @brief 执行 Air780EP HTTP 请求
 * @details Execute Air780EP HTTP request
 * @param[in] me 调制解调器句柄
 * @param[in] request HTTP 请求参数
 * @param[out] response HTTP 响应结果
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_http_request(modem_handle_t me,
                                       const modem_http_request_t *request,
                                       modem_http_response_t *response);
/**
 * @brief 解析单行 +CIPPING 响应
 * @details Parse a single +CIPPING reply line into the reply struct
 * @param[in] line +CIPPING 响应行
 * @param[in] request Ping 请求参数
 * @param[out] reply Ping 响应
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_cipping_line(const char *line,
                                    const modem_ping_request_t *request,
                                    modem_ping_reply_t *reply);
/**
 * @brief 解析 +CIPPING 字段中的无符号整数
 * @details Parse unsigned integer at cursor and advance cursor past it
 * @param[in,out] cursor 解析游标
 * @param[in] max_value 允许的最大值
 * @param[out] out_value 解析结果
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_cipping_uint(const char **cursor,
                                    uint32_t max_value,
                                    uint32_t *out_value);
/**
 * @brief 计算 Ping 汇总统计
 * @details Compute sent/received/lost and min/max/avg over received replies
 * @param[in] request Ping 请求参数
 * @param[in] replies Ping 响应数组
 * @param[in] reply_count 响应数量
 * @param[out] summary Ping 汇总统计
 */
static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary);
/**
 * @brief 计算 Ping 命令超时
 * @details Compute AT+CIPPING command timeout in milliseconds
 * @param[in] request Ping 请求参数，可为 NULL
 * @return 超时时间，request 为 NULL 时返回默认值
 */
static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request);

/**
 * @brief 转换为 Air780EP 实例
 * @details Convert to Air780EP instance
 * @param[in] me 调制解调器句柄
 * @return Air780EP 调制解调器实例
 */
static modem_air780ep_t *to_air780ep(modem_handle_t me);

/**
 * @brief 初始化命令上下文
 * @details Initialize command context
 * @param[out] ctx 命令上下文
 */
static void init_cmd_ctx(air780ep_cmd_ctx_t *ctx);

/**
 * @brief 发送 AT 命令
 * @details Send AT command
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] cmd AT 命令
 * @param[out] ctx 命令上下文
 * @param[in] timeout_ms 超时时间，0 使用默认值
 * @return
 *         - ESP_OK: 命令流程完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎错误
 */
static esp_err_t send_cmd(modem_air780ep_t *self, const char *cmd,
                          air780ep_cmd_ctx_t *ctx, uint32_t timeout_ms);

/**
 * @brief 使用选项发送 AT 命令
 * @details Send AT command with options
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] cmd AT 命令
 * @param[out] ctx 命令上下文
 * @param[in] options AT 命令选项
 * @return
 *         - ESP_OK: 命令流程完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎错误
 */
static esp_err_t send_cmd_with_options(modem_air780ep_t *self, const char *cmd,
                                       air780ep_cmd_ctx_t *ctx,
                                       const at_cmd_options_t *options);

/**
 * @brief 确认 AT 响应成功
 * @details Ensure AT response is OK
 * @param[in] response AT 响应
 * @param[in] cmd AT 命令
 * @return
 *         - ESP_OK: 响应成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: 响应失败
 */
static esp_err_t ensure_at_ok(const at_response_t *response, const char *cmd);

/**
 * @brief 查找指定前缀的响应行
 * @details Find response line with prefix
 * @param[in] response AT 响应
 * @param[in] prefix 响应行前缀
 * @return 匹配的响应行或 NULL
 */
static const char *find_line_with_prefix(const at_response_t *response,
                                         const char *prefix);

/**
 * @brief 获取首个数据行
 * @details Get first data line
 * @param[in] response AT 响应
 * @return 首个数据行或 NULL
 */
static const char *first_data_line(const at_response_t *response);

/**
 * @brief 复制字符串字段
 * @details Copy string field
 * @param[out] dst 目标缓冲区
 * @param[in] dst_size 目标缓冲区大小
 * @param[in] src 源字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 字符串被截断
 */
static esp_err_t copy_str_field(char *dst, size_t dst_size, const char *src);

/**
 * @brief 复制字符串字段并剥离外层引号
 * @details Copy string field and strip surrounding quotes
 * @param[out] dst 目标缓冲区
 * @param[in] dst_size 目标缓冲区大小
 * @param[in] src 源字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 字符串被截断
 */
static esp_err_t copy_str_field_strip_quotes(char *dst, size_t dst_size,
                                             const char *src);

/**
 * @brief 检查 PDP 上下文 ID 是否有效
 * @details Check whether PDP context ID is valid
 * @param[in] cid PDP 上下文 ID
 * @return true: 有效； false: 无效
 */
static bool cid_valid(uint8_t cid);

/**
 * @brief 获取指定 PDP 上下文缓存
 * @details Get cached PDP context by CID
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] cid PDP 上下文 ID
 * @return PDP 上下文缓存或 NULL
 */
static modem_pdp_context_t *pdp_by_cid(modem_air780ep_t *self, uint8_t cid);

/**
 * @brief 非阻塞设置调制解调器状态
 * @details Set modem state without blocking
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] state 调制解调器状态
 */
static void set_state_nonblocking(modem_air780ep_t *self, modem_state_t state);

/**
 * @brief 从 URC 行解析 PDP 上下文 ID
 * @details Parse PDP context ID from URC line
 * @param[in] line URC 完整行
 * @param[out] cid PDP 上下文 ID
 * @return true: 成功； false: 失败
 */
static bool parse_cid_from_line(const char *line, uint8_t *cid);

/**
 * @brief 清除所有 PDP 缓存
 * @details Clear all PDP cache entries
 * @param[in] self Air780EP 调制解调器实例
 * @param[out] affected 清除前处于激活状态的上下文
 * @param[in] affected_len affected 数组容量
 * @return 清除前处于激活状态的上下文数量
 */
static size_t clear_all_pdp_cache(modem_air780ep_t *self,
                                  modem_pdp_context_t *affected,
                                  size_t affected_len);

/**
 * @brief 投递 PDP 去激活事件
 * @details Post PDP deactivation events
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] affected 已去激活的上下文数组
 * @param[in] affected_count 上下文数量
 */
static void post_pdp_deactivated_events(modem_air780ep_t *self,
                                        const modem_pdp_context_t *affected,
                                        size_t affected_count);

/**
 * @brief 检查 AT 参数是否安全
 * @details Check whether AT argument is safe
 * @param[in] value AT 参数字符串
 * @return true: 安全； false: 不安全
 */
static bool at_arg_safe(const char *value);

/**
 * @brief 跳过响应前缀并返回值起始位置
 * @details Skip response prefix and return value start
 * @param[in] line 响应行
 * @param[in] prefix 响应前缀
 * @return 值起始位置或 NULL
 */
static const char *skip_prefix_value(const char *line, const char *prefix);

/**
 * @brief 解析前缀后的整数
 * @details Parse integer after prefix
 * @param[in] line 响应行
 * @param[in] prefix 响应前缀
 * @param[out] out 解析结果
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_int_after_prefix(const char *line, const char *prefix, int *out);

/**
 * @brief 解析前缀后的两个整数
 * @details Parse two integers after prefix
 * @param[in] line 响应行
 * @param[in] prefix 响应前缀
 * @param[out] first 第一个整数
 * @param[out] second 第二个整数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_two_ints_after_prefix(const char *line, const char *prefix,
                                             int *first, int *second);

/**
 * @brief 映射网络注册状态
 * @details Map network registration status
 * @param[in] stat AT 注册状态值
 * @return 调制解调器注册状态
 */
static modem_reg_status_t map_reg_status(int stat);

/**
 * @brief 解析网络注册响应行
 * @details Parse network registration response line
 * @param[in] line 响应行
 * @param[in] prefix 响应前缀
 * @param[out] status 网络注册状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_registration_line(const char *line, const char *prefix,
                                          modem_reg_status_t *status);

/**
 * @brief 解析网络注册 URC 行
 * @details Parse network registration URC line
 * @param[in] line URC 行
 * @param[in] prefix URC 前缀
 * @param[out] status 网络注册状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t parse_registration_urc_line(const char *line, const char *prefix,
                                             modem_reg_status_t *status);

/**
 * @brief 消费注册响应附加字段
 * @details Consume registration response extra fields
 * @param[in] cursor 附加字段起始位置
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 */
static esp_err_t consume_registration_extra_fields(const char *cursor);

/**
 * @brief 解析 SIM 状态响应行
 * @details Parse SIM status response line
 * @param[in] line 响应行
 * @return SIM 状态
 */
static modem_sim_status_t parse_sim_status_line(const char *line);

/**
 * @brief 缓存 SIM 状态
 * @details Cache SIM status
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] status SIM 状态
 */
static void cache_sim_status(modem_air780ep_t *self, modem_sim_status_t status);

/**
 * @brief 从 CME 错误码映射明确 SIM 状态
 * @details Map definite CME errors to SIM status
 * @param[in] error_code CME 错误码
 * @param[out] status SIM 状态
 * @return true: 已映射为明确 SIM 状态； false: 非明确 SIM 状态
 */
static bool sim_status_from_cme_error(int error_code, modem_sim_status_t *status);

/**
 * @brief 查询分组域附着状态
 * @details Query packet domain attach status
 * @param[in] self Air780EP 调制解调器实例
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t query_cgatt(modem_air780ep_t *self, bool *attached);

/**
 * @brief 查询 PDP 激活状态
 * @details Query PDP activation status
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] cid PDP 上下文 ID
 * @param[out] active 是否已激活
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - ESP_ERR_NOT_FOUND: 未找到指定上下文
 *         - 其他: AT 命令错误
 */
static esp_err_t query_cgact(modem_air780ep_t *self, uint8_t cid, bool *active);

/**
 * @brief 查询 PDP 地址
 * @details Query PDP address
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] cid PDP 上下文 ID
 * @param[out] ip_addr IP 地址缓冲区
 * @param[in] ip_addr_size IP 地址缓冲区大小
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - ESP_ERR_NOT_FOUND: 未找到指定上下文
 *         - 其他: AT 命令错误
 */
static esp_err_t query_cgpaddr(modem_air780ep_t *self, uint8_t cid,
                               char *ip_addr, size_t ip_addr_size);

/**
 * @brief 检查字符串是否像 IP 地址
 * @details Check whether string looks like an IP address
 * @param[in] line 响应行
 * @return true: 像 IP 地址； false: 不像 IP 地址
 */
static bool looks_like_ip_addr(const char *line);

/**
 * @brief 转换毫秒超时为 FreeRTOS ticks
 * @details Convert millisecond timeout to FreeRTOS ticks
 * @param[in] timeout_ms 超时时间
 * @return FreeRTOS ticks
 */
static TickType_t timeout_ticks(uint32_t timeout_ms);

/**
 * @brief 设置 initialized 标志
 * @details Set initialized flag
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] initialized 初始化状态
 */
static void set_initialized(modem_air780ep_t *self, bool initialized);

/**
 * @brief 设置 MQTT 数据使能标志
 * @details Set MQTT data-enabled flag (lock-protected)
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] enabled 使能状态
 */
static void set_mqtt_data_enabled(modem_air780ep_t *self, bool enabled);

/**
 * @brief 查询 MQTT 数据使能标志
 * @details Get MQTT data-enabled flag (lock-protected)
 * @param[in] self Air780EP 调制解调器实例
 * @return true: 已使能； false: 未使能
 */
static bool mqtt_data_is_enabled(modem_air780ep_t *self);

/**
 * @brief 获取当前毫秒时间
 * @details Get current time in milliseconds
 * @return 当前毫秒时间
 */
static uint32_t now_ms(void);

/**
 * @brief 判断是否已达到超时时间
 * @details Check whether timeout has elapsed
 * @param[in] start_ms 起始毫秒时间
 * @param[in] timeout_ms 超时时间
 * @return true: 已超时； false: 未超时
 */
static bool elapsed_at_least(uint32_t start_ms, uint32_t timeout_ms);

/**
 * @brief 延迟初始化重试
 * @details Delay before initialization retry
 */
static void delay_init_retry(void);

/**
 * @brief 等待 AT 命令通道就绪
 * @details Wait until AT command channel is ready
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_TIMEOUT: 超时
 */
static esp_err_t wait_at_ready(modem_air780ep_t *self);

/**
 * @brief 执行基础初始化命令
 * @details Run basic initialization commands
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t run_basic_init_cmds(modem_air780ep_t *self);

/**
 * @brief 完成调制解调器 ready 流程
 * @details Finish modem ready flow
 * @param[in] me 调制解调器句柄
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 状态设置错误
 */
static esp_err_t finish_modem_ready(modem_handle_t me, modem_air780ep_t *self);

/**
 * @brief 硬件复位模块(通过 EN 引脚)
 * @details Hardware reset module via EN pin
 * @details 拉低 EN 引脚，等待 reset_pulse_ms，拉高 EN 引脚
 * @details Pull EN low, wait reset_pulse_ms, pull EN high
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: GPIO 错误
 */
static esp_err_t hardware_reset(modem_air780ep_t *self);

/**
 * @brief 硬件关机
 * @details 拉低 EN 引脚并保持，使模块断电
 * @note 持 AT 命令路径独占；en_pin==GPIO_NUM_NC 时降级为无操作返回 ESP_OK。
 * @param[in] self Air780EP 实例
 * @return ESP_OK 成功，其它为 GPIO/AT 错误
 */
static esp_err_t hardware_power_off(modem_air780ep_t *self);

/**
 * @brief 注册 Air780EP URC 处理器
 * @details Register Air780EP URC handlers
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注册错误
 */
static esp_err_t register_urcs(modem_air780ep_t *self);

/**
 * @brief 注销 Air780EP URC 处理器
 * @details Unregister Air780EP URC handlers
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注销错误
 */
static esp_err_t unregister_urcs(modem_air780ep_t *self);

/**
 * @brief 注销 Air780EP URC 处理器
 * @details Unregister Air780EP URC handlers
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注销错误
 */
static esp_err_t air780ep_unregister_urcs(modem_air780ep_t *self);

/**
 * @brief 处理 CPIN URC
 * @details Handle CPIN URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief 处理注册状态 URC
 * @details Handle registration status URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief 处理 CGEV URC
 * @details Handle CGEV URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void cgev_urc_handler(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief 处理 PDP 去激活 URC
 * @details Handle PDP deactivation URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void pdp_deact_urc_handler(const char *prefix, const char *line,
                                  void *user_ctx);
/**
 * @brief 处理 TCP 可读 URC
 * @details Handle TCP readable URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void tcp_readable_urc_handler(const char *prefix, const char *line,
                                     void *user_ctx);
/**
 * @brief 处理 TCP 关闭 URC
 * @details Handle TCP closed URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void tcp_closed_urc_handler(const char *prefix, const char *line,
                                   void *user_ctx);
/**
 * @brief 处理 TCP 错误 URC
 * @details Handle TCP error URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void tcp_error_urc_handler(const char *prefix, const char *line,
                                  void *user_ctx);
/**
 * @brief 处理 +MSUB URC
 * @details Handle +MSUB URC: parse direct-mode MQTT message and post data event
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void handle_msub_urc(const char *prefix, const char *line, void *user_ctx);
/**
 * @brief 投递 MQTT 数据事件
 * @details Post MODEM_EVENT_PROTOCOL_DATA with MQTT topic/payload;
 *          ownership of topic/payload transfers to the event queue on success
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] topic 主题缓冲区
 * @param[in] topic_len 主题长度
 * @param[in] payload 负载缓冲区
 * @param[in] payload_len 负载长度
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 事件投递错误
 */
static esp_err_t post_mqtt_data_event(modem_air780ep_t *self, char *topic,
                                       size_t topic_len, uint8_t *payload,
                                       size_t payload_len);
/**
 * @brief 转义 AT 字符串
 * @details Escape \", \\, \\r, \\n for AT command string arguments;
 *          returns a malloc'd string
 * @param[in] value 待转义字符串
 * @return 转义后的字符串或 NULL
 */
static char *escape_at_string(const char *value);
/**
 * @brief 解析 +MSUB 直连模式 URC
 * @details Parse +MSUB:<topic>,<len>,<message> into heap-allocated buffers
 * @param[in] line URC 完整行
 * @param[out] topic 主题缓冲区
 * @param[out] topic_len 主题长度
 * @param[out] payload 负载缓冲区
 * @param[out] payload_len 负载长度
 * @return true: 成功； false: 失败
 */
static bool parse_msub_direct(const char *line, char **topic, size_t *topic_len,
                              uint8_t **payload, size_t *payload_len);

/**********************
 *  STATIC VARIABLES
 **********************/

static const modem_ops_t s_air780ep_ops = {
    .destroy = air780ep_destroy,
    .start = air780ep_start,
    .stop = air780ep_stop,
    .reset = air780ep_reset,
    .get_info = air780ep_get_info,
    .get_sim_status = air780ep_get_sim_status,
    .get_signal = air780ep_get_signal,
    .get_registration = air780ep_get_registration,
    .get_packet_attach_status = air780ep_get_packet_attach_status,
    .set_apn = air780ep_set_apn,
    .activate_pdp = air780ep_activate_pdp,
    .deactivate_pdp = air780ep_deactivate_pdp,
    .get_pdp_context = air780ep_get_pdp_context,
    .ssl_provision = air780ep_ssl_provision,
    .ssl_get_context_status = air780ep_ssl_get_context_status,
    .socket_open = air780ep_socket_open,
    .socket_send = air780ep_socket_send,
    .socket_recv = air780ep_socket_recv,
    .socket_close = air780ep_socket_close,
    .mqtt_configure = air780ep_mqtt_configure,
    .mqtt_tcp_connect = air780ep_mqtt_tcp_connect,
    .mqtt_connect = air780ep_mqtt_connect,
    .mqtt_disconnect = air780ep_mqtt_disconnect,
    .mqtt_tcp_disconnect = air780ep_mqtt_tcp_disconnect,
    .mqtt_subscribe = air780ep_mqtt_subscribe,
    .mqtt_unsubscribe = air780ep_mqtt_unsubscribe,
    .mqtt_publish = air780ep_mqtt_publish,
    .mqtt_get_status = air780ep_mqtt_get_status,
    .ping = air780ep_ping,
    .http_request = air780ep_http_request,
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

modem_handle_t modem_air780ep_create(at_engine_handle_t at,
                               const modem_air780ep_config_t *config)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    if (!at || !config) {
        ESP_LOGE(TAG, "NULL argument");
        return NULL;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：分配子类实例并归一化配置
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    modem_air780ep_t *self = calloc(1, sizeof(modem_air780ep_t));
    if (!self) {
        ESP_LOGE(TAG, "calloc air780ep modem failed");
        return NULL;
    }

    /* 保存配置快照，为 0 的字段填入 Air780EP 默认值 */
    self->config = *config;
    if (self->config.base.timing.default_cmd_timeout_ms == 0) {
        self->config.base.timing.default_cmd_timeout_ms = AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
    }
    if (self->config.base.timing.ready_timeout_ms == 0) {
        self->config.base.timing.ready_timeout_ms = AIR780EP_DEFAULT_READY_TIMEOUT_MS;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：初始化子类私有状态
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* SIM / 注册 / 信号缓存的初始值，对应"尚未查询"语义 */
    self->last_sim_status = MODEM_SIM_UNKNOWN;
    self->last_reg_status = MODEM_REG_UNKNOWN;
    self->last_signal.rssi = 99;       /* CSQ 中 99 表示未知 */
    self->last_signal.ber = 99;
    self->last_signal.rssi_dbm = 0;
    self->last_signal.rssi_dbm_valid = false;
    self->mqtt_transport = MODEM_MQTT_TRANSPORT_PLAIN_TCP;
    self->mqtt_ssl_context_id = 0;

    /* PDP 上下文槽位：cid 从 1 开始编号，默认类型 "IP" */
    for (int i = 0; i < AIR780EP_MAX_PDP_CONTEXTS; i++) {
        self->pdp[i].cid = i + 1;
        strcpy(self->pdp[i].pdp_type, "IP");
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 4：初始化基类（modem_handle_t）
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* modem_base_init 会设置 ops 虚函数表、保存 at 句柄、创建
     * lock / event_queue / event_task 等基类公共资源。
     * 失败时 self 由本函数释放，调用方无需清理。 */
    esp_err_t ret = modem_base_init(&self->base, "air780ep", at, &s_air780ep_ops,
                                    self->config.base.event.event_queue_size,
                                    self->config.base.event.event_task_stack,
                                    self->config.base.event.event_task_priority);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem base init failed: %s", esp_err_to_name(ret));
        free(self);
        return NULL;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 6：返回基类指针
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 调用方只持有 modem_handle_t ，通过 modem_* 包装 API 间接使用；
     * Air780EP 子类细节完全隐藏 */
    return &self->base;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static modem_air780ep_t *to_air780ep(modem_handle_t me)
{
    return MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
}

static void init_cmd_ctx(air780ep_cmd_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->response.lines = ctx->lines;
    ctx->response.max_lines = AIR780EP_MAX_RESPONSE_LINES;
}

static esp_err_t send_cmd(modem_air780ep_t *self, const char *cmd,
                          air780ep_cmd_ctx_t *ctx, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    uint32_t wait_ms = timeout_ms ? timeout_ms : self->config.base.timing.default_cmd_timeout_ms;
    if (wait_ms == 0) {
        wait_ms = AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
    }

    const at_cmd_options_t options = {
        .timeout_ms = wait_ms,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };

    return send_cmd_with_options(self, cmd, ctx, &options);
}

static esp_err_t send_cmd_with_options(modem_air780ep_t *self, const char *cmd,
                                       air780ep_cmd_ctx_t *ctx,
                                       const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(self && self->base.at && cmd && ctx && options,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    init_cmd_ctx(ctx);
    return at_engine_send_cmd_with_options(self->base.at, cmd, &ctx->response, options);
}

static esp_err_t ensure_at_ok(const at_response_t *response, const char *cmd)
{
    ESP_RETURN_ON_FALSE(response, ESP_ERR_INVALID_ARG, TAG, "response is NULL");

    const char *cmd_name = cmd ? cmd : "<unknown>";
    const char *line = first_data_line(response);

    switch (response->status) {
    case AT_RESP_OK:
        return ESP_OK;
    case AT_RESP_ERROR:
        ESP_LOGE(TAG, "%s returned ERROR%s%s", cmd_name,
                 line ? ", line: " : "", line ? line : "");
        return ESP_FAIL;
    case AT_RESP_CME_ERROR:
        ESP_LOGE(TAG, "%s returned +CME ERROR: %d%s%s", cmd_name,
                 response->error_code, line ? ", line: " : "", line ? line : "");
        return ESP_FAIL;
    case AT_RESP_CMS_ERROR:
        ESP_LOGE(TAG, "%s returned +CMS ERROR: %d%s%s", cmd_name,
                 response->error_code, line ? ", line: " : "", line ? line : "");
        return ESP_FAIL;
    case AT_RESP_TIMEOUT:
    case AT_RESP_ABORTED:
    default:
        ESP_LOGE(TAG, "%s failed with response status %d%s%s", cmd_name,
                 response->status, line ? ", line: " : "", line ? line : "");
        return ESP_FAIL;
    }
}

static const char *find_line_with_prefix(const at_response_t *response,
                                         const char *prefix)
{
    if (!response || !response->lines || !prefix) {
        return NULL;
    }

    int count = response->line_count;
    if (count > response->max_lines) {
        count = response->max_lines;
    }

    size_t prefix_len = strlen(prefix);
    for (int i = 0; i < count; i++) {
        const char *line = response->lines[i];
        if (line && strncmp(line, prefix, prefix_len) == 0) {
            return line;
        }
    }

    return NULL;
}

static const char *first_data_line(const at_response_t *response)
{
    return find_line_with_prefix(response, "");
}

static bool response_contains(const at_response_t *response, const char *needle)
{
    if (!response || !response->lines || !needle) {
        return false;
    }

    int count = response->line_count;
    if (count > response->max_lines) {
        count = response->max_lines;
    }

    for (int i = 0; i < count; i++) {
        const char *line = response->lines[i];
        if (line && strstr(line, needle)) {
            return true;
        }
    }

    return false;
}

static const char *find_ciprxget_hex_line(const at_response_t *response,
                                          size_t *out_remaining_len)
{
    if (!response || !response->lines || !out_remaining_len) {
        return NULL;
    }

    int count = response->line_count;
    if (count > response->max_lines) {
        count = response->max_lines;
    }

    for (int i = 0; i < count; i++) {
        const char *line = response->lines[i];
        const char *cursor = skip_prefix_value(line, "+CIPRXGET:");
        if (!cursor) {
            continue;
        }

        errno = 0;
        char *end = NULL;
        unsigned long mode = strtoul(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || mode != 3UL) {
            continue;
        }
        cursor = end;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != ',') {
            continue;
        }
        cursor++;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        errno = 0;
        unsigned long read_len = strtoul(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || read_len > SIZE_MAX) {
            continue;
        }
        cursor = end;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != ',') {
            continue;
        }
        cursor++;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        errno = 0;
        unsigned long remaining_len = strtoul(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || remaining_len > SIZE_MAX) {
            continue;
        }
        cursor = end;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        const char *hex_line = NULL;
        if (*cursor == ',') {
            cursor++;
            while (isspace((unsigned char)*cursor)) {
                cursor++;
            }
            hex_line = cursor;
        } else if (*cursor == '\0' && i + 1 < count) {
            hex_line = response->lines[i + 1];
        } else {
            continue;
        }

        if (!hex_line) {
            continue;
        }
        size_t hex_len = strlen(hex_line);
        if ((hex_len % 2U) != 0 || hex_len / 2U != (size_t)read_len) {
            continue;
        }
        bool valid_hex = true;
        for (size_t j = 0; j < hex_len; j++) {
            if (hex_nibble(hex_line[j]) < 0) {
                valid_hex = false;
                break;
            }
        }
        if (!valid_hex) {
            continue;
        }

        *out_remaining_len = (size_t)remaining_len;
        return hex_line;
    }

    return NULL;
}

static esp_err_t decode_hex_payload(const char *hex, uint8_t **out_payload,
                                    size_t *out_len)
{
    ESP_RETURN_ON_FALSE(hex && out_payload && out_len, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");

    *out_payload = NULL;
    *out_len = 0;

    size_t hex_len = strlen(hex);
    if ((hex_len % 2U) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t payload_len = hex_len / 2U;
    uint8_t *payload = malloc(payload_len ? payload_len : 1U);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < payload_len; i++) {
        int hi = hex_nibble(hex[i * 2U]);
        int lo = hex_nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            free(payload);
            return ESP_ERR_INVALID_RESPONSE;
        }
        payload[i] = (uint8_t)((hi << 4) | lo);
    }

    *out_payload = payload;
    *out_len = payload_len;
    return ESP_OK;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t copy_str_field(char *dst, size_t dst_size, const char *src)
{
    ESP_RETURN_ON_FALSE(dst && src, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    size_t copied = strlcpy(dst, src, dst_size);
    if (copied >= dst_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t copy_str_field_strip_quotes(char *dst, size_t dst_size,
                                             const char *src)
{
    ESP_RETURN_ON_FALSE(dst && src, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *start = src;
    while (isspace((unsigned char)*start)) {
        start++;
    }

    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    if (end > start + 1 && *start == '"' && *(end - 1) == '"') {
        start++;
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len >= dst_size) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(dst, start, len);
    dst[len] = '\0';
    return ESP_OK;
}

static bool cid_valid(uint8_t cid)
{
    return cid >= 1 && cid <= AIR780EP_MAX_PDP_CONTEXTS;
}

static modem_pdp_context_t *pdp_by_cid(modem_air780ep_t *self, uint8_t cid)
{
    if (!self || !cid_valid(cid)) {
        return NULL;
    }

    return &self->pdp[cid - 1];
}

static void set_state_nonblocking(modem_air780ep_t *self, modem_state_t state)
{
    if (!self || !self->base.lock) {
        return;
    }

    if (xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "skip state update %d, lock busy", state);
        return;
    }

    self->base.state = state;
    xSemaphoreGive(self->base.lock);
}

static bool parse_cid_from_line(const char *line, uint8_t *cid)
{
    if (!line || !cid) {
        return false;
    }

    const char *value = skip_prefix_value(line, AIR780EP_URC_CGEV);
    if (!value) {
        return false;
    }

    const char *event = strstr(value, "PDN ACT");
    size_t event_len = sizeof("PDN ACT") - 1;
    if (!event) {
        event = strstr(value, "PDN DEACT");
        event_len = sizeof("PDN DEACT") - 1;
    }
    if (!event) {
        return false;
    }

    const char *cursor = event + event_len;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    if (!isdigit((unsigned char)*cursor)) {
        return false;
    }

    bool overflow = false;
    unsigned int parsed_value = 0;
    while (isdigit((unsigned char)*cursor)) {
        unsigned int digit = (unsigned int)(*cursor - '0');
        if (parsed_value > UINT8_MAX / 10U ||
            (parsed_value == UINT8_MAX / 10U && digit > UINT8_MAX % 10U)) {
            overflow = true;
        }
        if (!overflow) {
            parsed_value = (parsed_value * 10U) + digit;
        }
        cursor++;
    }

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0' && *cursor != ',') {
        return false;
    }

    if (overflow || parsed_value < 1U || parsed_value > AIR780EP_MAX_PDP_CONTEXTS) {
        return false;
    }

    *cid = (uint8_t)parsed_value;
    return true;
}

static size_t clear_all_pdp_cache(modem_air780ep_t *self,
                                  modem_pdp_context_t *affected,
                                  size_t affected_len)
{
    if (!self || !affected) {
        return 0;
    }

    size_t affected_count = 0;
    for (size_t i = 0; i < AIR780EP_MAX_PDP_CONTEXTS; i++) {
        if (self->pdp[i].active && affected_count < affected_len) {
            affected[affected_count] = self->pdp[i];
            affected[affected_count].active = false;
            affected[affected_count].ip_addr[0] = '\0';
            affected_count++;
        }
        self->pdp[i].active = false;
        self->pdp[i].ip_addr[0] = '\0';
    }

    return affected_count;
}

static void post_pdp_deactivated_events(modem_air780ep_t *self,
                                        const modem_pdp_context_t *affected,
                                        size_t affected_count)
{
    if (!self || !affected) {
        return;
    }

    for (size_t i = 0; i < affected_count; i++) {
        const modem_event_t event = {
            .id = MODEM_EVENT_PDP_DEACTIVATED,
            .data.pdp = affected[i],
        };
        esp_err_t ret = modem_post_event(&self->base, &event);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "post PDP deactivated event failed: %s", esp_err_to_name(ret));
        }
    }
}

static bool at_arg_safe(const char *value)
{
    if (!value) {
        return false;
    }

    while (*value) {
        if (*value == '"' || *value == '\r' || *value == '\n') {
            return false;
        }
        value++;
    }

    return true;
}

static const char *skip_prefix_value(const char *line, const char *prefix)
{
    if (!line || !prefix) {
        return NULL;
    }

    size_t prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) {
        return NULL;
    }

    const char *value = line + prefix_len;
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (*value == ':') {
        value++;
    }
    while (isspace((unsigned char)*value)) {
        value++;
    }

    return value;
}

static esp_err_t parse_int_after_prefix(const char *line, const char *prefix, int *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    const char *value = skip_prefix_value(line, prefix);
    if (!value) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = (int)parsed;
    return ESP_OK;
}

static esp_err_t parse_two_ints_after_prefix(const char *line, const char *prefix,
                                             int *first, int *second)
{
    ESP_RETURN_ON_FALSE(first && second, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *value = skip_prefix_value(line, prefix);
    if (!value) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    long parsed_first = strtol(value, &end, 10);
    if (end == value || errno == ERANGE ||
        parsed_first < INT_MIN || parsed_first > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    end = NULL;
    long parsed_second = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE ||
        parsed_second < INT_MIN || parsed_second > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *first = (int)parsed_first;
    *second = (int)parsed_second;
    return ESP_OK;
}

static modem_reg_status_t map_reg_status(int stat)
{
    switch (stat) {
    case 0:
        return MODEM_REG_NOT_REGISTERED;
    case 1:
        return MODEM_REG_REGISTERED_HOME;
    case 2:
        return MODEM_REG_SEARCHING;
    case 3:
        return MODEM_REG_DENIED;
    case 5:
        return MODEM_REG_REGISTERED_ROAMING;
    default:
        return MODEM_REG_UNKNOWN;
    }
}

static esp_err_t parse_registration_line(const char *line, const char *prefix,
                                         modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    const char *value = skip_prefix_value(line, prefix);
    if (!value) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    long parsed_first = strtol(value, &end, 10);
    if (end == value || errno == ERANGE ||
        parsed_first < INT_MIN || parsed_first > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = end;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cursor++;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    errno = 0;
    end = NULL;
    long parsed_second = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE ||
        parsed_second < INT_MIN || parsed_second > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = consume_registration_extra_fields(end);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid registration extra fields");

    *status = map_reg_status((int)parsed_second);
    (void)parsed_first;
    return ESP_OK;
}

static esp_err_t parse_registration_urc_line(const char *line, const char *prefix,
                                             modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    const char *value = skip_prefix_value(line, prefix);
    if (!value) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    long parsed_stat = strtol(value, &end, 10);
    if (end == value || errno == ERANGE ||
        parsed_stat < INT_MIN || parsed_stat > INT_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = consume_registration_extra_fields(end);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid registration URC extra fields");

    *status = map_reg_status((int)parsed_stat);
    return ESP_OK;
}

static esp_err_t consume_registration_extra_fields(const char *cursor)
{
    ESP_RETURN_ON_FALSE(cursor, ESP_ERR_INVALID_ARG, TAG, "cursor is NULL");

    while (true) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            return ESP_OK;
        }
        if (*cursor != ',') {
            return ESP_ERR_INVALID_RESPONSE;
        }

        cursor++;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (*cursor == '"') {
            cursor++;
            while (*cursor && *cursor != '"') {
                if (*cursor == '\r' || *cursor == '\n') {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                cursor++;
            }
            if (*cursor != '"') {
                return ESP_ERR_INVALID_RESPONSE;
            }
            cursor++;
        } else {
            const char *token_start = cursor;
            while (*cursor && *cursor != ',') {
                if (*cursor == '\r' || *cursor == '\n' || *cursor == '"') {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                cursor++;
            }

            const char *token_end = cursor;
            while (token_end > token_start &&
                   isspace((unsigned char)*(token_end - 1))) {
                token_end--;
            }
            if (token_end == token_start) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }

        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor != '\0' && *cursor != ',') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
}

static modem_sim_status_t parse_sim_status_line(const char *line)
{
    if (!line) {
        return MODEM_SIM_ERROR;
    }

    const char *value = skip_prefix_value(line, "+CPIN:");
    if (!value) {
        value = line;
    }

    while (isspace((unsigned char)*value)) {
        value++;
    }

    bool quoted = false;
    if (*value == '"') {
        quoted = true;
        value++;
    }

    const char *end = value;
    if (quoted) {
        while (*end && *end != '"') {
            end++;
        }
        if (*end != '"') {
            return MODEM_SIM_ERROR;
        }

        const char *tail = end + 1;
        while (isspace((unsigned char)*tail)) {
            tail++;
        }
        if (*tail != '\0') {
            return MODEM_SIM_ERROR;
        }
    } else {
        while (*end) {
            end++;
        }
    }

    while (end > value && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    size_t len = (size_t)(end - value);
    if (len == sizeof("READY") - 1 && strncmp(value, "READY", len) == 0) {
        return MODEM_SIM_READY;
    }
    if (len == sizeof("SIM PIN") - 1 && strncmp(value, "SIM PIN", len) == 0) {
        return MODEM_SIM_PIN_REQUIRED;
    }
    if (len == sizeof("SIM PUK") - 1 && strncmp(value, "SIM PUK", len) == 0) {
        return MODEM_SIM_PUK_REQUIRED;
    }
    if ((len == sizeof("NOT INSERTED") - 1 &&
         strncmp(value, "NOT INSERTED", len) == 0) ||
        (len == sizeof("SIM NOT INSERTED") - 1 &&
         strncmp(value, "SIM NOT INSERTED", len) == 0) ||
        (len == sizeof("REMOVED") - 1 && strncmp(value, "REMOVED", len) == 0) ||
        (len == sizeof("SIM REMOVED") - 1 && strncmp(value, "SIM REMOVED", len) == 0)) {
        return MODEM_SIM_NOT_INSERTED;
    }

    return MODEM_SIM_ERROR;
}

static void cache_sim_status(modem_air780ep_t *self, modem_sim_status_t status)
{
    if (!self) {
        return;
    }

    if (!self->base.lock) {
        self->last_sim_status = status;
        return;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->last_sim_status = status;
    xSemaphoreGive(self->base.lock);
}

static bool sim_status_from_cme_error(int error_code, modem_sim_status_t *status)
{
    if (!status) {
        return false;
    }

    switch (error_code) {
    case AIR780EP_CME_SIM_NOT_INSERTED:
        *status = MODEM_SIM_NOT_INSERTED;
        return true;
    case AIR780EP_CME_SIM_PIN_REQUIRED:
        *status = MODEM_SIM_PIN_REQUIRED;
        return true;
    case AIR780EP_CME_SIM_PUK_REQUIRED:
        *status = MODEM_SIM_PUK_REQUIRED;
        return true;
    case AIR780EP_CME_SIM_FAILURE:
    case AIR780EP_CME_SIM_WRONG:
        *status = MODEM_SIM_ERROR;
        return true;
    default:
        return false;
    }
}

static esp_err_t query_cgatt(modem_air780ep_t *self, bool *attached)
{
    ESP_RETURN_ON_FALSE(self && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+CGATT?", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CGATT? failed");

    ret = ensure_at_ok(&ctx.response, "AT+CGATT?");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CGATT? failed");

    const char *line = find_line_with_prefix(&ctx.response, "+CGATT:");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG, "+CGATT line missing");

    int state = 0;
    ret = parse_int_after_prefix(line, "+CGATT:", &state);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +CGATT failed");
    ESP_RETURN_ON_FALSE(state == 0 || state == 1, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid +CGATT state %d", state);

    *attached = (state == 1);
    return ESP_OK;
}

static esp_err_t query_cgact(modem_air780ep_t *self, uint8_t cid, bool *active)
{
    ESP_RETURN_ON_FALSE(self && active, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+CGACT?", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CGACT? failed");

    ret = ensure_at_ok(&ctx.response, "AT+CGACT?");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CGACT? failed");

    int count = ctx.response.line_count;
    if (count > ctx.response.max_lines) {
        count = ctx.response.max_lines;
    }

    for (int i = 0; i < count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, "+CGACT:", sizeof("+CGACT:") - 1) != 0) {
            continue;
        }

        int parsed_cid = 0;
        int state = 0;
        ret = parse_two_ints_after_prefix(line, "+CGACT:", &parsed_cid, &state);
        ESP_RETURN_ON_ERROR(ret, TAG, "parse +CGACT failed");

        if (parsed_cid != cid) {
            continue;
        }
        ESP_RETURN_ON_FALSE(state == 0 || state == 1, ESP_ERR_INVALID_RESPONSE,
                            TAG, "invalid +CGACT state %d", state);

        *active = (state == 1);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t query_cgpaddr(modem_air780ep_t *self, uint8_t cid,
                               char *ip_addr, size_t ip_addr_size)
{
    ESP_RETURN_ON_FALSE(self && ip_addr && ip_addr_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);

    ip_addr[0] = '\0';

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+CGPADDR=%u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+CGPADDR command truncated");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmd);

    ret = ensure_at_ok(&ctx.response, cmd);
    ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmd);

    int count = ctx.response.line_count;
    if (count > ctx.response.max_lines) {
        count = ctx.response.max_lines;
    }

    for (int i = 0; i < count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, "+CGPADDR:", sizeof("+CGPADDR:") - 1) != 0) {
            continue;
        }

        const char *value = skip_prefix_value(line, "+CGPADDR:");
        ESP_RETURN_ON_FALSE(value, ESP_ERR_INVALID_RESPONSE, TAG,
                            "invalid +CGPADDR line");

        errno = 0;
        char *end = NULL;
        long parsed_cid = strtol(value, &end, 10);
        ESP_RETURN_ON_FALSE(end != value && errno != ERANGE &&
                            parsed_cid >= 0 && parsed_cid <= UINT8_MAX,
                            ESP_ERR_INVALID_RESPONSE, TAG, "invalid +CGPADDR cid");

        if ((uint8_t)parsed_cid != cid) {
            continue;
        }

        const char *cursor = end;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE,
                            TAG, "invalid +CGPADDR format");
        cursor++;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }

        if (*cursor == '\0') {
            return ESP_OK;
        }

        const char *addr_start = cursor;
        const char *addr_end = cursor;
        if (*cursor == '"') {
            addr_start = cursor + 1;
            addr_end = strchr(addr_start, '"');
            ESP_RETURN_ON_FALSE(addr_end, ESP_ERR_INVALID_RESPONSE,
                                TAG, "unterminated +CGPADDR address");
        } else {
            while (*addr_end && *addr_end != ',') {
                addr_end++;
            }
            while (addr_end > addr_start && isspace((unsigned char)*(addr_end - 1))) {
                addr_end--;
            }
        }

        size_t addr_len = (size_t)(addr_end - addr_start);
        if (addr_len == 0) {
            ip_addr[0] = '\0';
            return ESP_OK;
        }
        char addr[MODEM_IP_ADDR_MAX_LEN];
        ESP_RETURN_ON_FALSE(addr_len < sizeof(addr) && addr_len < ip_addr_size,
                            ESP_ERR_INVALID_RESPONSE,
                            TAG, "+CGPADDR address truncated");

        memcpy(addr, addr_start, addr_len);
        addr[addr_len] = '\0';
        ESP_RETURN_ON_FALSE(looks_like_ip_addr(addr), ESP_ERR_INVALID_RESPONSE,
                            TAG, "invalid +CGPADDR address");

        strlcpy(ip_addr, addr, ip_addr_size);
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static bool looks_like_ip_addr(const char *line)
{
    if (!line || line[0] == '\0') {
        return false;
    }

    const char *cursor = line;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }

        int value = 0;
        int digits = 0;
        while (isdigit((unsigned char)*cursor)) {
            value = (value * 10) + (*cursor - '0');
            digits++;
            if (digits > 3 || value > 255) {
                return false;
            }
            cursor++;
        }

        if (part < 3) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        } else if (*cursor != '\0') {
            return false;
        }
    }

    return true;
}

static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        return 1;
    }

    return ticks;
}

static void set_initialized(modem_air780ep_t *self, bool initialized)
{
    if (!self) {
        return;
    }

    if (!self->base.lock) {
        self->initialized = initialized;
        return;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->initialized = initialized;
    xSemaphoreGive(self->base.lock);
}

static void set_mqtt_data_enabled(modem_air780ep_t *self, bool enabled)
{
    if (!self) {
        return;
    }

    if (!self->base.lock) {
        self->mqtt_data_enabled = enabled;
        return;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->mqtt_data_enabled = enabled;
    xSemaphoreGive(self->base.lock);
}

static bool mqtt_data_is_enabled(modem_air780ep_t *self)
{
    if (!self) {
        return false;
    }

    if (!self->base.lock) {
        return self->mqtt_data_enabled;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    bool enabled = self->mqtt_data_enabled;
    xSemaphoreGive(self->base.lock);

    return enabled;
}

static char *clone_mqtt_string(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t len = strlen(value) + 1U;
    char *copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len);
    return copy;
}

static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src)
{
    ESP_RETURN_ON_FALSE(dst && src && src->client_id && src->host && src->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT config");

    modem_mqtt_config_t copy = {
        .client_id = clone_mqtt_string(src->client_id),
        .username = src->username ? clone_mqtt_string(src->username) : NULL,
        .password = src->password ? clone_mqtt_string(src->password) : NULL,
        .host = clone_mqtt_string(src->host),
        .port = src->port,
        .transport = src->transport,
        .ssl_context_id = src->ssl_context_id,
        .clean_session = src->clean_session,
        .keepalive_s = src->keepalive_s,
    };
    if (!copy.client_id || !copy.host ||
        (src->username && !copy.username) ||
        (src->password && !copy.password)) {
        free_mqtt_config(&copy);
        return ESP_ERR_NO_MEM;
    }

    free_mqtt_config(dst);
    *dst = copy;
    return ESP_OK;
}

static void free_mqtt_config(modem_mqtt_config_t *config)
{
    if (!config) {
        return;
    }
    free((void *)config->client_id);
    free((void *)config->username);
    free((void *)config->password);
    free((void *)config->host);
    memset(config, 0, sizeof(*config));
}

static void clear_mqtt_state(modem_air780ep_t *self)
{
    air780ep_clear_ssl_state(self);
}

static void ssl_mark_context(modem_air780ep_t *self, uint8_t context_id,
                             modem_ssl_auth_mode_t auth_mode, bool provisioned)
{
    uint32_t index = context_id;
    if (!self || index >= AIR780EP_SSL_CONTEXT_BITMAP_BITS) {
        return;
    }

    uint32_t mask = 1U << (index % 32U);
    uint32_t *word = &self->ssl_provisioned_bitmap[index / 32U];
    if (provisioned) {
        *word |= mask;
        self->ssl_auth_modes[index] = auth_mode;
    } else {
        *word &= ~mask;
        self->ssl_auth_modes[index] = MODEM_SSL_AUTH_NONE;
    }
}

static bool ssl_context_marked(modem_air780ep_t *self, uint8_t context_id)
{
    uint32_t index = context_id;
    if (!self || index >= AIR780EP_SSL_CONTEXT_BITMAP_BITS) {
        return false;
    }

    uint32_t mask = 1U << (index % 32U);
    return (self->ssl_provisioned_bitmap[index / 32U] & mask) != 0;
}

static void air780ep_clear_tls_mqtt_config_if_context_locked(modem_air780ep_t *self,
                                                            uint8_t context_id)
{
    if (!self || context_id != AIR780EP_SSL_MQTT_CONTEXT_ID) {
        return;
    }

    bool mqtt_uses_context = self->mqtt_transport == MODEM_MQTT_TRANSPORT_TLS &&
                             self->mqtt_ssl_context_id == context_id;
    if (self->mqtt_config.transport == MODEM_MQTT_TRANSPORT_TLS &&
        self->mqtt_config.ssl_context_id == context_id) {
        mqtt_uses_context = true;
    }
    if (!mqtt_uses_context) {
        return;
    }

    self->mqtt_configured = false;
    self->mqtt_tcp_connected = false;
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    free_mqtt_config(&self->mqtt_config);
    self->mqtt_transport = MODEM_MQTT_TRANSPORT_PLAIN_TCP;
    self->mqtt_ssl_context_id = 0;
}

static void air780ep_invalidate_ssl_context(modem_air780ep_t *self,
                                            uint8_t context_id)
{
    if (!self) {
        return;
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    ssl_mark_context(self, context_id, MODEM_SSL_AUTH_NONE, false);
    air780ep_clear_tls_mqtt_config_if_context_locked(self, context_id);
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
}

static esp_err_t air780ep_ssl_object_names(uint8_t context_id,
                                           char *ca_name, size_t ca_name_size,
                                           char *client_cert_name,
                                           size_t client_cert_name_size,
                                           char *client_key_name,
                                           size_t client_key_name_size)
{
    ESP_RETURN_ON_FALSE(ca_name && client_cert_name && client_key_name,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    int written = snprintf(ca_name, ca_name_size, "lwlte_ca_%u.crt",
                           (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < ca_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "CA object name truncated");

    written = snprintf(client_cert_name, client_cert_name_size,
                       "lwlte_client_%u.crt", (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < client_cert_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "client cert object name truncated");

    written = snprintf(client_key_name, client_key_name_size,
                       "lwlte_client_%u.key", (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < client_key_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "client key object name truncated");

    return ESP_OK;
}

static esp_err_t air780ep_delete_file_ignore_missing(modem_air780ep_t *self,
                                                     const char *name)
{
    ESP_RETURN_ON_FALSE(self && name, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char *escaped_name = escape_at_string(name);
    ESP_RETURN_ON_FALSE(escaped_name, ESP_ERR_NO_MEM, TAG, "escape file name failed");

    char cmd[96];
    int written = snprintf(cmd, sizeof(cmd), "AT+FSDEL=\"%s\"", escaped_name);
    free(escaped_name);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+FSDEL command truncated");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+FSDEL failed");

    if (ctx.response.status == AT_RESP_OK ||
        (ctx.response.status == AT_RESP_CME_ERROR &&
         (ctx.response.error_code == 62 || ctx.response.error_code == 100))) {
        return ESP_OK;
    }
    return ensure_at_ok(&ctx.response, "AT+FSDEL");
}

static esp_err_t air780ep_write_file(modem_air780ep_t *self, const char *name,
                                     const uint8_t *payload, size_t len)
{
    ESP_RETURN_ON_FALSE(self && self->base.at && name && payload && len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(len <= AIR780EP_SSL_MAX_PEM_LEN, ESP_ERR_INVALID_SIZE,
                        TAG, "SSL PEM too large");

    esp_err_t ret = air780ep_delete_file_ignore_missing(self, name);
    ESP_RETURN_ON_ERROR(ret, TAG, "delete existing SSL file failed");

    char *escaped_name = escape_at_string(name);
    ESP_RETURN_ON_FALSE(escaped_name, ESP_ERR_NO_MEM, TAG, "escape file name failed");

    char cmd[128];
    int written = snprintf(cmd, sizeof(cmd), "AT+FSCREATE=\"%s\"", escaped_name);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        free(escaped_name);
        return ESP_ERR_INVALID_ARG;
    }

    air780ep_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+FSCREATE");
    }
    if (ret != ESP_OK) {
        free(escaped_name);
        return ret;
    }

    written = snprintf(cmd, sizeof(cmd), "AT+FSWRITE=\"%s\",0,%u,%u",
                       escaped_name, (unsigned int)len,
                       (unsigned int)AIR780EP_SSL_WRITE_TIMEOUT_S);
    free(escaped_name);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        return ESP_ERR_INVALID_ARG;
    }

    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_SSL_CMD_TIMEOUT_MS + AIR780EP_SSL_WRITE_TIMEOUT_S * 1000U,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };
    init_cmd_ctx(&ctx);
    ret = at_engine_send_cmd_with_payload(self->base.at, cmd, payload, len,
                                          ">", &ctx.response, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+FSWRITE");
    }
    return ret;
}

static esp_err_t air780ep_bind_ssl_file(modem_air780ep_t *self,
                                        const char *tag, uint8_t context_id,
                                        const char *name)
{
    ESP_RETURN_ON_FALSE(self && tag && name, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(context_id == AIR780EP_SSL_TCP_CONTEXT_ID ||
                        context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported SSL context");

    char *escaped_name = escape_at_string(name);
    ESP_RETURN_ON_FALSE(escaped_name, ESP_ERR_NO_MEM, TAG, "escape file name failed");

    char cmd[128];
    /* Air780EP command token: AT+SSLCFG="<tag>",<context_id>,"<name>". */
    int written = snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"%s\",%u,\"%s\"",
                           tag, (unsigned)context_id, escaped_name);
    free(escaped_name);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+SSLCFG file command truncated");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+SSLCFG file");
    }
    return ret;
}

static esp_err_t air780ep_file_exists(modem_air780ep_t *self, const char *name,
                                      bool *exists)
{
    ESP_RETURN_ON_FALSE(self && name && exists, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    *exists = false;

    char *escaped_name = escape_at_string(name);
    ESP_RETURN_ON_FALSE(escaped_name, ESP_ERR_NO_MEM, TAG, "escape file name failed");

    char cmd[96];
    int written = snprintf(cmd, sizeof(cmd), "AT+FSFLSIZE=\"%s\"", escaped_name);
    free(escaped_name);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+FSFLSIZE command truncated");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+FSFLSIZE failed");

    if (ctx.response.status == AT_RESP_OK) {
        const char *line = find_line_with_prefix(&ctx.response, "+FSFLSIZE:");
        ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE,
                            TAG, "+FSFLSIZE line missing");

        const char *cursor = skip_prefix_value(line, "+FSFLSIZE:");
        ESP_RETURN_ON_FALSE(cursor, ESP_ERR_INVALID_RESPONSE,
                            TAG, "invalid +FSFLSIZE line");
        if (*cursor == '"') {
            cursor = strchr(cursor + 1, '"');
            ESP_RETURN_ON_FALSE(cursor, ESP_ERR_INVALID_RESPONSE,
                                TAG, "unterminated +FSFLSIZE name");
            cursor++;
            while (isspace((unsigned char)*cursor)) {
                cursor++;
            }
            ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE,
                                TAG, "missing +FSFLSIZE size separator");
            cursor++;
        } else if (!isdigit((unsigned char)*cursor)) {
            cursor = strchr(cursor, ',');
            ESP_RETURN_ON_FALSE(cursor, ESP_ERR_INVALID_RESPONSE,
                                TAG, "missing +FSFLSIZE size field");
            cursor++;
        }
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        ESP_RETURN_ON_FALSE(isdigit((unsigned char)*cursor),
                            ESP_ERR_INVALID_RESPONSE, TAG,
                            "invalid +FSFLSIZE size start");

        errno = 0;
        char *end = NULL;
        unsigned long parsed_size = strtoul(cursor, &end, 10);
        ESP_RETURN_ON_FALSE(end != cursor && errno != ERANGE &&
                            parsed_size <= (unsigned long)SIZE_MAX,
                            ESP_ERR_INVALID_RESPONSE, TAG, "invalid +FSFLSIZE size");
        cursor = end;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        ESP_RETURN_ON_FALSE(*cursor == '\0', ESP_ERR_INVALID_RESPONSE,
                            TAG, "unexpected +FSFLSIZE suffix");

        *exists = parsed_size > 0;
        return ESP_OK;
    }
    if (ctx.response.status == AT_RESP_CME_ERROR &&
        (ctx.response.error_code == 62 || ctx.response.error_code == 100)) {
        return ESP_OK;
    }
    return ensure_at_ok(&ctx.response, "AT+FSFLSIZE");
}

static esp_err_t air780ep_query_ssl_auth_mode(modem_air780ep_t *self,
                                              uint8_t context_id,
                                              modem_ssl_auth_mode_t *auth_mode)
{
    ESP_RETURN_ON_FALSE(self && auth_mode, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(context_id == AIR780EP_SSL_TCP_CONTEXT_ID ||
                        context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported SSL context");

    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"seclevel\",%u", (unsigned)context_id);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx,
                             AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+SSLCFG seclevel");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "query SSL seclevel failed");

    const char *line = find_line_with_prefix(&ctx.response, "+SSLCFG:");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG, "+SSLCFG line missing");

    const char *cursor = skip_prefix_value(line, "+SSLCFG:");
    ESP_RETURN_ON_FALSE(cursor && strncmp(cursor, "\"seclevel\"",
                                         sizeof("\"seclevel\"") - 1) == 0,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid +SSLCFG seclevel line");
    cursor += sizeof("\"seclevel\"") - 1;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE,
                        TAG, "missing +SSLCFG context separator");
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    char *end = NULL;
    long parsed_context = strtol(cursor, &end, 10);
    ESP_RETURN_ON_FALSE(end != cursor && errno != ERANGE &&
                        parsed_context == (long)context_id,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid +SSLCFG context");
    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE,
                        TAG, "missing +SSLCFG mode separator");
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    end = NULL;
    long parsed_mode = strtol(cursor, &end, 10);
    ESP_RETURN_ON_FALSE(end != cursor && errno != ERANGE &&
                        parsed_mode >= (long)MODEM_SSL_AUTH_NONE &&
                        parsed_mode <= (long)MODEM_SSL_AUTH_MUTUAL,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid +SSLCFG auth mode");
    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == '\0', ESP_ERR_INVALID_RESPONSE,
                        TAG, "unexpected +SSLCFG suffix");

    *auth_mode = (modem_ssl_auth_mode_t)parsed_mode;
    return ESP_OK;
}

static void air780ep_clear_ssl_state(modem_air780ep_t *self)
{
    if (!self) {
        return;
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_configured = false;
    self->mqtt_tcp_connected = false;
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    free_mqtt_config(&self->mqtt_config);
    memset(self->ssl_provisioned_bitmap, 0, sizeof(self->ssl_provisioned_bitmap));
    for (size_t i = 0; i < sizeof(self->ssl_auth_modes) / sizeof(self->ssl_auth_modes[0]); i++) {
        self->ssl_auth_modes[i] = MODEM_SSL_AUTH_NONE;
    }
    self->mqtt_transport = MODEM_MQTT_TRANSPORT_PLAIN_TCP;
    self->mqtt_ssl_context_id = 0;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
}

static esp_err_t air780ep_ssl_provision(modem_handle_t me,
                                         const modem_ssl_context_config_t *config,
                                         const modem_ssl_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(me && config && credentials, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(config->context_id == AIR780EP_SSL_TCP_CONTEXT_ID ||
                        config->context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported SSL context");
    ESP_RETURN_ON_FALSE(config->auth_mode >= MODEM_SSL_AUTH_NONE &&
                        config->auth_mode <= MODEM_SSL_AUTH_MUTUAL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid SSL auth mode");

    modem_air780ep_t *self = to_air780ep(me);
    char ca_name[32];
    char client_cert_name[32];
    char client_key_name[32];
    esp_err_t ret = air780ep_ssl_object_names(config->context_id,
                                              ca_name, sizeof(ca_name),
                                              client_cert_name,
                                              sizeof(client_cert_name),
                                              client_key_name,
                                              sizeof(client_key_name));
    ESP_RETURN_ON_ERROR(ret, TAG, "generate SSL object names failed");

    air780ep_invalidate_ssl_context(self, config->context_id);

    switch (config->auth_mode) {
    case MODEM_SSL_AUTH_NONE:
        break;
    case MODEM_SSL_AUTH_SERVER:
        ESP_RETURN_ON_FALSE(credentials->ca_cert_pem && credentials->ca_cert_len > 0,
                            ESP_ERR_INVALID_ARG, TAG, "CA certificate missing");
        ret = air780ep_write_file(self, ca_name, credentials->ca_cert_pem,
                                  credentials->ca_cert_len);
        ESP_RETURN_ON_ERROR(ret, TAG, "write CA certificate failed");
        ret = air780ep_bind_ssl_file(self, "cacert", config->context_id, ca_name);
        ESP_RETURN_ON_ERROR(ret, TAG, "bind CA certificate failed");
        break;
    case MODEM_SSL_AUTH_MUTUAL:
        ESP_RETURN_ON_FALSE(credentials->ca_cert_pem && credentials->ca_cert_len > 0 &&
                            credentials->client_cert_pem && credentials->client_cert_len > 0 &&
                            credentials->client_key_pem && credentials->client_key_len > 0,
                            ESP_ERR_INVALID_ARG, TAG, "mutual SSL credentials missing");
        ret = air780ep_write_file(self, ca_name, credentials->ca_cert_pem,
                                  credentials->ca_cert_len);
        ESP_RETURN_ON_ERROR(ret, TAG, "write CA certificate failed");
        ret = air780ep_write_file(self, client_cert_name, credentials->client_cert_pem,
                                  credentials->client_cert_len);
        ESP_RETURN_ON_ERROR(ret, TAG, "write client certificate failed");
        ret = air780ep_write_file(self, client_key_name, credentials->client_key_pem,
                                  credentials->client_key_len);
        ESP_RETURN_ON_ERROR(ret, TAG, "write client key failed");
        ret = air780ep_bind_ssl_file(self, "cacert", config->context_id, ca_name);
        ESP_RETURN_ON_ERROR(ret, TAG, "bind CA certificate failed");
        ret = air780ep_bind_ssl_file(self, "clientcert", config->context_id,
                                     client_cert_name);
        ESP_RETURN_ON_ERROR(ret, TAG, "bind client certificate failed");
        ret = air780ep_bind_ssl_file(self, "clientkey", config->context_id,
                                     client_key_name);
        ESP_RETURN_ON_ERROR(ret, TAG, "bind client key failed");
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    char cmd[128];
    /* Air780EP command token: AT+SSLCFG="seclevel",<context_id>,<auth>. */
    int written = snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"seclevel\",%u,%u",
                           (unsigned)config->context_id,
                           (unsigned int)config->auth_mode);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+SSLCFG seclevel command truncated");
    air780ep_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+SSLCFG seclevel");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "set SSL seclevel failed");

    if (config->auth_mode != MODEM_SSL_AUTH_NONE) {
        snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"verifymode\",%u,0",
                 (unsigned)config->context_id);
        ret = send_cmd(self, cmd, &ctx,
                       AIR780EP_SSL_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+SSLCFG verifymode");
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL verify mode failed");
    }

    written = snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"ignorelocaltime\",%u,%u",
                       (unsigned)config->context_id,
                       config->ignore_cert_time ? 1U : 0U);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+SSLCFG ignorelocaltime command truncated");
    ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+SSLCFG ignorelocaltime");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "set SSL ignorelocaltime failed");

    if (config->tls_version != 0) {
        written = snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"sslversion\",%u,%u",
                           (unsigned)config->context_id,
                           (unsigned int)config->tls_version);
        ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                            ESP_ERR_INVALID_ARG, TAG, "AT+SSLCFG sslversion command truncated");
        ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+SSLCFG sslversion");
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL version failed");
    }

    if (config->hostname && config->hostname[0] != '\0') {
        char *hostname = escape_at_string(config->hostname);
        ESP_RETURN_ON_FALSE(hostname, ESP_ERR_NO_MEM, TAG, "escape SSL hostname failed");
        int needed = snprintf(NULL, 0, "AT+SSLCFG=\"hostname\",%u,\"%s\"",
                              (unsigned)config->context_id, hostname);
        if (needed < 0) {
            free(hostname);
            return ESP_ERR_INVALID_ARG;
        }
        char *hostname_cmd = malloc((size_t)needed + 1U);
        if (!hostname_cmd) {
            free(hostname);
            return ESP_ERR_NO_MEM;
        }
        snprintf(hostname_cmd, (size_t)needed + 1U,
                 "AT+SSLCFG=\"hostname\",%u,\"%s\"",
                 (unsigned)config->context_id, hostname);
        free(hostname);
        ret = send_cmd(self, hostname_cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+SSLCFG hostname");
        }
        free(hostname_cmd);
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL hostname failed");
    }

    if (config->negotiate_timeout_s != 0) {
        written = snprintf(cmd, sizeof(cmd), "AT+SSLCFG=\"negotiatetimeout\",%u,%u",
                           (unsigned)config->context_id,
                           (unsigned int)config->negotiate_timeout_s);
        ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                            ESP_ERR_INVALID_ARG,
                            TAG, "AT+SSLCFG negotiatetimeout command truncated");
        ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+SSLCFG negotiatetimeout");
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL negotiate timeout failed");
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    ssl_mark_context(self, config->context_id, config->auth_mode, true);
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    return ESP_OK;
}

static esp_err_t air780ep_ssl_get_context_status(modem_handle_t me,
                                                 uint8_t context_id,
                                                 modem_ssl_context_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(context_id == AIR780EP_SSL_TCP_CONTEXT_ID ||
                        context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported SSL context");

    modem_air780ep_t *self = to_air780ep(me);
    memset(status, 0, sizeof(*status));

    modem_ssl_auth_mode_t auth_mode = MODEM_SSL_AUTH_NONE;
    esp_err_t ret = air780ep_query_ssl_auth_mode(self, context_id, &auth_mode);
    ESP_RETURN_ON_ERROR(ret, TAG, "query SSL auth mode failed");

    char ca_name[32];
    char client_cert_name[32];
    char client_key_name[32];
    ret = air780ep_ssl_object_names(context_id, ca_name, sizeof(ca_name),
                                    client_cert_name, sizeof(client_cert_name),
                                    client_key_name, sizeof(client_key_name));
    ESP_RETURN_ON_ERROR(ret, TAG, "generate SSL object names failed");

    bool ca_exists = false;
    bool cert_exists = false;
    bool key_exists = false;
    ret = air780ep_file_exists(self, ca_name, &ca_exists);
    ESP_RETURN_ON_ERROR(ret, TAG, "query CA certificate failed");
    ret = air780ep_file_exists(self, client_cert_name, &cert_exists);
    ESP_RETURN_ON_ERROR(ret, TAG, "query client certificate failed");
    ret = air780ep_file_exists(self, client_key_name, &key_exists);
    ESP_RETURN_ON_ERROR(ret, TAG, "query client key failed");

    status->auth_mode = auth_mode;
    status->ca_cert_present = ca_exists;
    status->client_cert_present = cert_exists;
    status->client_key_present = key_exists;
    status->check_valid = false;
    status->provisioned = status->auth_mode == MODEM_SSL_AUTH_NONE ||
                          (status->auth_mode == MODEM_SSL_AUTH_SERVER && ca_exists) ||
                          (status->auth_mode == MODEM_SSL_AUTH_MUTUAL && ca_exists &&
                           cert_exists && key_exists);

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    ssl_mark_context(self, context_id, status->auth_mode, status->provisioned);
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    if (!status->provisioned) {
        air780ep_invalidate_ssl_context(self, context_id);
    }
    return ESP_OK;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool elapsed_at_least(uint32_t start_ms, uint32_t timeout_ms)
{
    return (uint32_t)(now_ms() - start_ms) >= timeout_ms;
}

static void delay_init_retry(void)
{
    vTaskDelay(timeout_ticks(AIR780EP_INIT_RETRY_DELAY_MS));
}

static esp_err_t wait_at_ready(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const uint32_t timeout_ms = self->config.base.timing.ready_timeout_ms;
    const uint32_t start_ms = now_ms();
    unsigned int attempt = 1;

    while (!elapsed_at_least(start_ms, timeout_ms)) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, "AT", &ctx,
                                 AIR780EP_AT_READY_PROBE_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT");
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "AT ready probe failed (attempt %u): %s",
                 attempt, esp_err_to_name(ret));
        attempt++;

        if (!elapsed_at_least(start_ms, timeout_ms)) {
            /* delay_init_retry wraps vTaskDelay(timeout_ticks(AIR780EP_INIT_RETRY_DELAY_MS)). */
            delay_init_retry();
        }
    }

    ESP_LOGE(TAG, "AT ready probe timeout after %u ms",
             (unsigned int)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_basic_init_cmds(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
        "AT+CGREG=2",
        "AT+CREG=2",
        "AT*I"
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 1; attempt <= AIR780EP_INIT_CMD_MAX_ATTEMPTS; attempt++) {
            air780ep_cmd_ctx_t ctx;
            ret = send_cmd(self, cmds[i], &ctx, 0);
            if (ret == ESP_OK) {
                ret = ensure_at_ok(&ctx.response, cmds[i]);
            }
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "%s failed (attempt %d/%d): %s", cmds[i], attempt,
                     AIR780EP_INIT_CMD_MAX_ATTEMPTS, esp_err_to_name(ret));
            if (attempt < AIR780EP_INIT_CMD_MAX_ATTEMPTS) {
                /* delay_init_retry wraps vTaskDelay(timeout_ticks(AIR780EP_INIT_RETRY_DELAY_MS)). */
                delay_init_retry();
            }
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed after %d attempts", cmds[i],
                            AIR780EP_INIT_CMD_MAX_ATTEMPTS);
    }

    return ESP_OK;
}

static esp_err_t finish_modem_ready(modem_handle_t me, modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(me && self, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_RETURN_ON_ERROR(ret, TAG, "set ready state failed");

    set_initialized(self, true);

    const modem_event_t event = {
        .id = MODEM_EVENT_READY,
    };
    ret = modem_post_event(me, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post ready event failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

static esp_err_t hardware_reset(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    ret = at_engine_flush_rx_exclusive(self->base.at);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "flush RX input before reset failed");

    if (self->config.base.hardware.en_pin == GPIO_NUM_NC) {
        at_engine_end_exclusive(self->base.at);
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << (uint32_t)self->config.base.hardware.en_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure EN GPIO failed");

    ret = gpio_set_level(self->config.base.hardware.en_pin, 0);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set EN GPIO low failed");

    if (self->config.base.timing.reset_pulse_ms > 0) {
        vTaskDelay(timeout_ticks(self->config.base.timing.reset_pulse_ms));
    }

    ret = at_engine_flush_rx_exclusive(self->base.at);
    ESP_GOTO_ON_ERROR(ret, err_restore_en, TAG, "flush RX input after EN low failed");

    ret = gpio_set_level(self->config.base.hardware.en_pin, 1);
    ESP_GOTO_ON_ERROR(ret, err_restore_en, TAG, "set EN GPIO high failed");

    at_engine_end_exclusive(self->base.at);
    return ESP_OK;

err_restore_en:
    {
        esp_err_t restore_ret = gpio_set_level(self->config.base.hardware.en_pin, 1);
        if (restore_ret != ESP_OK) {
            ESP_LOGW(TAG, "restore EN GPIO high failed: %s", esp_err_to_name(restore_ret));
        }
    }
err:
    at_engine_end_exclusive(self->base.at);
    return ret;
}

static esp_err_t hardware_power_off(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    if (self->config.base.hardware.en_pin == GPIO_NUM_NC) {
        at_engine_end_exclusive(self->base.at);
        ESP_LOGW(TAG, "no EN pin; modem stays powered (logical stop only)");
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << (uint32_t)self->config.base.hardware.en_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure EN GPIO failed");

    ret = gpio_set_level(self->config.base.hardware.en_pin, 0);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set EN GPIO low failed");

    (void)at_engine_flush_rx_exclusive(self->base.at);

err:
    at_engine_end_exclusive(self->base.at);
    return ret;
}

static esp_err_t register_urcs(modem_air780ep_t *self)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验 + 幂等保护
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    /* 避免重复注册：urc_registered 保证 start / reconnect 等多次调用路径幂等 */
    if (self->urc_registered) {
        return ESP_OK;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：构造 URC 注册表
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 每个 URC 由三元组组成：前缀（用于 RX task 行匹配）、handler 节点（生命周期
     * 由 self 持有）、callback（URC 派发时实际调用的函数）。
     * handler->user_ctx 统一指向 self，callback 内部通过 user_ctx 取回 modem 实例。 */
    const struct {
        const char *prefix;
        at_urc_handler_t *handler;
        at_urc_callback_t callback;
    } urcs[] = {
        { AIR780EP_URC_CPIN, &self->cpin_handler, cpin_urc_handler },
        { AIR780EP_URC_CREG, &self->creg_handler, reg_urc_handler },
        { AIR780EP_URC_CEREG, &self->cereg_handler, reg_urc_handler },
        { AIR780EP_URC_CGREG, &self->cgreg_handler, reg_urc_handler },
        { AIR780EP_URC_CGEV, &self->cgev_handler, cgev_urc_handler },
        { AIR780EP_URC_PDP_DEACT, &self->pdp_deact_handler, pdp_deact_urc_handler },
        { AIR780EP_URC_PDP_COLON_DEACT, &self->pdp_colon_deact_handler,
          pdp_deact_urc_handler },
        { AIR780EP_URC_MSUB, &self->msub_handler, handle_msub_urc },
        { AIR780EP_CIPRXGET_READY_PREFIX, &self->tcp_readable_handler,
          tcp_readable_urc_handler },
        { AIR780EP_CIPRXGET_READY_COMPACT_PREFIX, &self->tcp_readable_compact_handler,
          tcp_readable_urc_handler },
        { "CLOSED", &self->tcp_closed_handler, tcp_closed_urc_handler },
        { AIR780EP_TCP_ERROR_PREFIX, &self->tcp_error_handler,
          tcp_error_urc_handler },
    };

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：批量初始化 handler 节点
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 预先填好所有 handler 字段，之后注册时只传指针；at_engine_register_urc
     * 内部只做链表插入不修改字段（除了 next），所以先初始化更干净 */
    size_t urc_count = sizeof(urcs) / sizeof(urcs[0]);
    for (size_t i = 0; i < urc_count; i++) {
        *urcs[i].handler = (at_urc_handler_t) {
            .prefix = urcs[i].prefix,
            .callback = urcs[i].callback,
            .user_ctx = self,
            .next = NULL,
        };
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 4：逐个注册到 AT Engine，失败时回滚已注册项
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    size_t registered_count = 0;
    for (size_t i = 0; i < urc_count; i++) {
        esp_err_t ret = at_engine_register_urc(self->base.at, urcs[i].prefix,
                                               urcs[i].handler);
        if (ret != ESP_OK) {
            /*── 回滚：逆序注销已注册的 handler，避免引擎残留只注册了一半的 URC ──*/
            esp_err_t rollback_ret = ESP_OK;
            for (size_t j = 0; j < registered_count; j++) {
                esp_err_t err = at_engine_unregister_urc(self->base.at, urcs[j].prefix);
                if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "rollback unregister %s failed: %s", urcs[j].prefix,
                             esp_err_to_name(err));
                    if (rollback_ret == ESP_OK) {
                        rollback_ret = err;
                    }
                }
            }
            if (rollback_ret == ESP_OK) {
                /* 回滚成功：清空所有 handler 节点，下次 start 可重试 */
                for (size_t j = 0; j < urc_count; j++) {
                    memset(urcs[j].handler, 0, sizeof(*urcs[j].handler));
                }
                self->urc_registered = false;
            } else {
                /* 回滚失败：部分 handler 仍留在引擎链表中，标记已注册防止重入 */
                self->urc_registered = true;
            }
            return ret;
        }
        registered_count++;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 5：全部注册成功，标记已注册
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    self->urc_registered = true;
    return ESP_OK;
}

static esp_err_t unregister_urcs(modem_air780ep_t *self)
{
    esp_err_t ret = air780ep_unregister_urcs(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister URCs failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t air780ep_destroy(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    esp_err_t ret = ESP_OK;

    set_mqtt_data_enabled(self, false);
    clear_mqtt_state(self);

    if (self->urc_registered) {
        ret = air780ep_unregister_urcs(self);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    set_initialized(self, false);
    return ESP_OK;
}

static esp_err_t air780ep_start(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool urc_disabled_for_init = false;
    bool urc_register_attempted = false;
    esp_err_t ret = ESP_OK;

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：重置内部运行状态
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 在锁保护下清零 MQTT 连接标志，防止并发读取到过期状态；
     * 同时清除 initialized 标志，表示模块尚未完成启动 */
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    air780ep_clear_ssl_state(self);
    set_initialized(self, false);

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：迁移状态到 INITIALIZING
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ret = modem_set_state(me, MODEM_STATE_INITIALIZING);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set initializing state failed");

    if (self->urc_registered) {
        ret = unregister_urcs(self);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "disable URCs before init failed");
        urc_disabled_for_init = true;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：硬件复位模块
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 通过 EN 引脚执行硬件复位（拉低 → 拉高），重启 Air780EP */
    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 4：等待 AT 命令通道就绪
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ret = wait_at_ready(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait AT ready failed");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 5：发送基础 AT 初始化命令
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 关闭回显、配置错误报告格式、设置波特率锁定等，
     * 确保模块进入可控的命令交互模式 */
    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 6：注册运行期 URC 处理器
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    urc_register_attempted = true;
    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 7：标记 READY 并通知上层
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* set_state(READY) → set_initialized(true) → post MODEM_EVENT_READY，
     * Core FSM 的 handle_modem_event 收到后进入 READY 状态 */
    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");

    return ESP_OK;

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 错误清理：回滚所有副作用
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
err:
    if (self->urc_registered && (urc_disabled_for_init || urc_register_attempted)) {
        (void)unregister_urcs(self);
    }
    set_initialized(self, false);
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
}

static esp_err_t air780ep_reset(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool urc_disabled_for_init = false;
    bool urc_register_attempted = false;
    esp_err_t ret = ESP_OK;

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    air780ep_clear_ssl_state(self);
    set_initialized(self, false);

    ret = modem_set_state(me, MODEM_STATE_INITIALIZING);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set initializing state failed");

    if (self->urc_registered) {
        ret = unregister_urcs(self);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "disable URCs before init failed");
        urc_disabled_for_init = true;
    }

    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    ret = wait_at_ready(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait AT ready failed");

    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    urc_register_attempted = true;
    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");

    return ESP_OK;

err:
    if (self->urc_registered && (urc_disabled_for_init || urc_register_attempted)) {
        (void)unregister_urcs(self);
    }
    set_initialized(self, false);
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
}

static esp_err_t air780ep_stop(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);

    /* 1. 复位内部运行状态（逆 start 步 1/8）*/
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    air780ep_clear_ssl_state(self);
    set_initialized(self, false);

    esp_err_t ret = ESP_OK;

    /* 2. 注销 URC（逆 start 步 7）*/
    if (self->urc_registered) {
        esp_err_t urc_ret = unregister_urcs(self);
        if (urc_ret != ESP_OK) {
            ESP_LOGW(TAG, "unregister URCs during stop failed: %s", esp_err_to_name(urc_ret));
            if (ret == ESP_OK) {
                ret = urc_ret;
            }
        }
    }

    /* 3. 硬件断电（逆 start 步 4 的上电）*/
    esp_err_t power_ret = hardware_power_off(self);
    if (power_ret != ESP_OK) {
        ESP_LOGW(TAG, "hardware power off failed: %s", esp_err_to_name(power_ret));
        if (ret == ESP_OK) {
            ret = power_ret;
        }
    }

    /* 4. 逻辑停止已完成；即使硬件操作失败也落 OFF，后续 start 会重新上电复位。 */
    (void)modem_set_state(me, MODEM_STATE_OFF);

    return ret;
}

static esp_err_t air780ep_get_info(modem_handle_t me, modem_info_t *info)
{
    ESP_RETURN_ON_FALSE(me && info, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    modem_info_t result = {0};
    const struct {
        const char *cmd;
        const char *prefix;
        char *dst;
        size_t dst_size;
        bool strip_quotes;
    } queries[] = {
        { "AT+CGSN", NULL, result.imei, sizeof(result.imei), false },
        { "AT+CIMI", NULL, result.imsi, sizeof(result.imsi), false },
        { "AT+ICCID", "+ICCID:", result.iccid, sizeof(result.iccid), false },
        { "AT+CGMM", "+CGMM:", result.model, sizeof(result.model), true },
        { "AT+CGMR", "+CGMR:", result.fw_revision, sizeof(result.fw_revision), true },
    };

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, queries[i].cmd, &ctx, 0);
        ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", queries[i].cmd);

        ret = ensure_at_ok(&ctx.response, queries[i].cmd);
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", queries[i].cmd);

        const char *value = NULL;
        if (queries[i].prefix) {
            const char *line = find_line_with_prefix(&ctx.response, queries[i].prefix);
            if (line) {
                value = skip_prefix_value(line, queries[i].prefix);
            }
        }
        if (!value) {
            value = first_data_line(&ctx.response);
        }
        ESP_RETURN_ON_FALSE(value, ESP_ERR_INVALID_RESPONSE, TAG,
                            "%s missing data", queries[i].cmd);

        if (queries[i].strip_quotes) {
            ret = copy_str_field_strip_quotes(queries[i].dst, queries[i].dst_size,
                                              value);
        } else {
            ret = copy_str_field(queries[i].dst, queries[i].dst_size, value);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "copy %s response failed", queries[i].cmd);
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->cached_info = result;
    xSemaphoreGive(self->base.lock);

    *info = result;
    return ESP_OK;
}

static esp_err_t air780ep_get_sim_status(modem_handle_t me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    const uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool sim_busy_seen = false;

    while (true) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, "AT+CPIN?", &ctx, 0);
        ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CPIN? failed");

        if (ctx.response.status == AT_RESP_OK) {
            const char *line = find_line_with_prefix(&ctx.response, "+CPIN:");
            ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                                "+CPIN line missing");

            modem_sim_status_t parsed = parse_sim_status_line(line);
            cache_sim_status(self, parsed);
            *status = parsed;
            return ESP_OK;
        }

        if (ctx.response.status == AT_RESP_CME_ERROR) {
            modem_sim_status_t parsed = MODEM_SIM_UNKNOWN;
            if (sim_status_from_cme_error(ctx.response.error_code, &parsed)) {
                cache_sim_status(self, parsed);
                *status = parsed;
                return ESP_OK;
            }

            if (ctx.response.error_code == AIR780EP_CME_SIM_BUSY) {
                sim_busy_seen = true;
                uint32_t elapsed_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) -
                    start_ms;
                if (elapsed_ms >= AIR780EP_SIM_READY_TIMEOUT_MS) {
                    break;
                }

                uint32_t remaining_ms = AIR780EP_SIM_READY_TIMEOUT_MS - elapsed_ms;
                uint32_t wait_ms = AIR780EP_SIM_READY_POLL_INTERVAL_MS;
                if (remaining_ms < wait_ms) {
                    wait_ms = remaining_ms;
                }
                if (wait_ms == 0) {
                    break;
                }

                ESP_LOGW(TAG, "AT+CPIN? returned SIM busy, retry in %u ms",
                         (unsigned int)wait_ms);
                vTaskDelay(timeout_ticks(wait_ms));
                continue;
            }
        }

        ret = ensure_at_ok(&ctx.response, "AT+CPIN?");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CPIN? failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (sim_busy_seen) {
        cache_sim_status(self, MODEM_SIM_UNKNOWN);
        *status = MODEM_SIM_UNKNOWN;
        ESP_LOGE(TAG, "AT+CPIN? SIM busy timeout after %u ms",
                 (unsigned int)AIR780EP_SIM_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_FAIL;
}

static esp_err_t air780ep_get_signal(modem_handle_t me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;

    esp_err_t ret = send_cmd(self, "AT+CSQ", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CSQ failed");

    ret = ensure_at_ok(&ctx.response, "AT+CSQ");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CSQ failed");

    const char *line = find_line_with_prefix(&ctx.response, "+CSQ:");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG, "+CSQ line missing");

    modem_signal_t result = {0};
    ret = parse_two_ints_after_prefix(line, "+CSQ:", &result.rssi, &result.ber);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +CSQ failed");

    if (!((result.ber >= 0 && result.ber <= 7) || result.ber == 99)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (result.rssi >= 0 && result.rssi <= 31) {
        result.rssi_dbm = -113 + (2 * result.rssi);
        result.rssi_dbm_valid = true;
    } else if (result.rssi == 99) {
        result.rssi_dbm = 0;
        result.rssi_dbm_valid = false;
    } else {
        return ESP_ERR_INVALID_RESPONSE;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->last_signal = result;
    xSemaphoreGive(self->base.lock);

    *signal = result;
    return ESP_OK;
}

static esp_err_t air780ep_get_registration(modem_handle_t me, modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    const struct {
        const char *cmd;
        const char *prefix;
    } queries[] = {
        { "AT+CEREG?", "+CEREG:" },
        { "AT+CGREG?", "+CGREG:" },
        { "AT+CREG?", "+CREG:" },
    };
    esp_err_t last_err = ESP_FAIL;
    bool had_error = false;

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, queries[i].cmd, &ctx, 0);
        if (ret != ESP_OK) {
            last_err = ret;
            had_error = true;
            continue;
        }

        ret = ensure_at_ok(&ctx.response, queries[i].cmd);
        if (ret != ESP_OK) {
            last_err = ret;
            had_error = true;
            continue;
        }

        const char *line = find_line_with_prefix(&ctx.response, queries[i].prefix);
        if (!line) {
            last_err = ESP_ERR_INVALID_RESPONSE;
            had_error = true;
            continue;
        }

        modem_reg_status_t parsed = MODEM_REG_UNKNOWN;
        ret = parse_registration_line(line, queries[i].prefix, &parsed);
        if (ret != ESP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (parsed == MODEM_REG_UNKNOWN) {
            continue;
        }

        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        self->last_reg_status = parsed;
        xSemaphoreGive(self->base.lock);

        *status = parsed;

        modem_state_t state = MODEM_STATE_READY;
        switch (parsed) {
        case MODEM_REG_REGISTERED_HOME:
        case MODEM_REG_REGISTERED_ROAMING:
            state = MODEM_STATE_REGISTERED;
            break;
        case MODEM_REG_SEARCHING:
            state = MODEM_STATE_REGISTERING;
            break;
        case MODEM_REG_NOT_REGISTERED:
        case MODEM_REG_DENIED:
        default:
            state = MODEM_STATE_READY;
            break;
        }

        return modem_set_state(me, state);
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->last_reg_status = MODEM_REG_UNKNOWN;
    xSemaphoreGive(self->base.lock);

    *status = MODEM_REG_UNKNOWN;
    if (!had_error) {
        return ESP_OK;
    }

    return last_err;
}

static esp_err_t air780ep_get_packet_attach_status(modem_handle_t me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    return query_cgatt(self, attached);
}

static esp_err_t air780ep_set_apn(modem_handle_t me, uint8_t cid, const char *apn)
{
    ESP_RETURN_ON_FALSE(me && apn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(strlen(apn) < MODEM_APN_MAX_LEN && at_arg_safe(apn),
                        ESP_ERR_INVALID_ARG, TAG, "invalid APN");

    char cmd[96];
    int written = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%u,\"IP\",\"%s\"",
                           (unsigned int)cid, apn);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+CGDCONT command truncated");

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;

    esp_err_t ret = send_cmd(self, cmd, &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmd);

    ret = ensure_at_ok(&ctx.response, cmd);
    ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmd);

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    modem_pdp_context_t *pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(pdp->apn, apn, sizeof(pdp->apn));
    strlcpy(pdp->pdp_type, "IP", sizeof(pdp->pdp_type));
    xSemaphoreGive(self->base.lock);

    return ESP_OK;
}

static esp_err_t air780ep_activate_pdp(modem_handle_t me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Air780EP TCPIP activation supports cid 1 only");

    modem_air780ep_t *self = to_air780ep(me);

    char apn[MODEM_APN_MAX_LEN] = {0};
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    modem_pdp_context_t *pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(apn, pdp->apn, sizeof(apn));
    xSemaphoreGive(self->base.lock);

    esp_err_t ret;
    air780ep_cmd_ctx_t ctx;

    ret = air780ep_socket_prepare(self);
    ESP_RETURN_ON_ERROR(ret, TAG, "prepare Air780EP TCPIP settings failed");

    char cstt_cmd_buf[96];
    const char *cstt_cmd = "AT+CSTT";
    if (apn[0] != '\0') {
        int written = snprintf(cstt_cmd_buf, sizeof(cstt_cmd_buf),
                               "AT+CSTT=\"%s\"", apn);
        ESP_RETURN_ON_FALSE(written >= 0 &&
                            (size_t)written < sizeof(cstt_cmd_buf),
                            ESP_ERR_INVALID_ARG, TAG, "AT+CSTT command truncated");
        cstt_cmd = cstt_cmd_buf;
    }

    ret = send_cmd(self, cstt_cmd, &ctx, AIR780EP_CSTT_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cstt_cmd);

    ret = ensure_at_ok(&ctx.response, cstt_cmd);
    ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cstt_cmd);

    ret = send_cmd(self, "AT+CIICR", &ctx, AIR780EP_CIICR_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CIICR failed");

    ret = ensure_at_ok(&ctx.response, "AT+CIICR");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIICR failed");

    const at_cmd_success_match_t cifsr_match = {
        .type = AT_CMD_SUCCESS_MATCH_ANY_LINE,
        .value = NULL,
    };
    const at_cmd_options_t cifsr_options = {
        .timeout_ms = self->config.base.timing.default_cmd_timeout_ms,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL,
        .success_matches = &cifsr_match,
        .success_match_count = 1,
    };

    ret = send_cmd_with_options(self, "AT+CIFSR", &ctx, &cifsr_options);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CIFSR failed");

    ret = ensure_at_ok(&ctx.response, "AT+CIFSR");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIFSR failed");

    const char *ip_line = first_data_line(&ctx.response);
    ESP_RETURN_ON_FALSE(looks_like_ip_addr(ip_line), ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid AT+CIFSR response");

    char ip_addr[MODEM_IP_ADDR_MAX_LEN];
    ret = copy_str_field(ip_addr, sizeof(ip_addr), ip_line);
    ESP_RETURN_ON_ERROR(ret, TAG, "copy PDP IP address failed");

    modem_pdp_context_t event_pdp = {0};
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(pdp->ip_addr, ip_addr, sizeof(pdp->ip_addr));
    pdp->active = true;
    strlcpy(pdp->pdp_type, "IP", sizeof(pdp->pdp_type));
    event_pdp = *pdp;
    xSemaphoreGive(self->base.lock);

    ret = modem_set_state(me, MODEM_STATE_PDP_ACTIVE);
    ESP_RETURN_ON_ERROR(ret, TAG, "set PDP active state failed");

    const modem_event_t event = {
        .id = MODEM_EVENT_PDP_ACTIVATED,
        .data.pdp = event_pdp,
    };
    ret = modem_post_event(me, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post PDP activated event failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

static esp_err_t air780ep_deactivate_pdp(modem_handle_t me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == 1, ESP_ERR_NOT_SUPPORTED, TAG,
                        "Air780EP TCPIP deactivation supports cid 1 only");

    modem_air780ep_t *self = to_air780ep(me);

    const at_cmd_success_match_t cipshut_match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "SHUT OK",
    };
    const at_cmd_options_t cipshut_options = {
        .timeout_ms = AIR780EP_CIPSHUT_TIMEOUT_MS,
        .flags = 0,
        .success_matches = &cipshut_match,
        .success_match_count = 1,
    };

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, "AT+CIPSHUT", &ctx,
                                          &cipshut_options);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CIPSHUT failed");

    ret = ensure_at_ok(&ctx.response, "AT+CIPSHUT");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPSHUT failed");

    modem_pdp_context_t affected[AIR780EP_MAX_PDP_CONTEXTS];
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    size_t affected_count = clear_all_pdp_cache(self, affected,
                                                AIR780EP_MAX_PDP_CONTEXTS);
    xSemaphoreGive(self->base.lock);

    ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_RETURN_ON_ERROR(ret, TAG, "set ready state failed");

    post_pdp_deactivated_events(self, affected, affected_count);

    return ESP_OK;
}

static esp_err_t air780ep_get_pdp_context(modem_handle_t me, uint8_t cid,
                                            modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);

    modem_air780ep_t *self = to_air780ep(me);

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    modem_pdp_context_t *cached = pdp_by_cid(self, cid);
    if (!cached) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    modem_pdp_context_t snapshot = *cached;
    xSemaphoreGive(self->base.lock);

    bool active = snapshot.active;
    esp_err_t ret = query_cgact(self, cid, &active);
    if (ret == ESP_OK) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        cached = pdp_by_cid(self, cid);
        if (!cached) {
            xSemaphoreGive(self->base.lock);
            return ESP_ERR_INVALID_ARG;
        }
        cached->active = active;
        if (!active) {
            cached->ip_addr[0] = '\0';
        }
        snapshot = *cached;
        xSemaphoreGive(self->base.lock);

        if (!active) {
            *pdp = snapshot;
            return ESP_OK;
        }
    } else if (ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    if (!snapshot.active) {
        *pdp = snapshot;
        return ESP_OK;
    }

    char ip_addr[MODEM_IP_ADDR_MAX_LEN] = {0};
    ret = query_cgpaddr(self, cid, ip_addr, sizeof(ip_addr));
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    cached = pdp_by_cid(self, cid);
    if (!cached) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (ret == ESP_OK) {
        cached->active = snapshot.active;
        strlcpy(cached->ip_addr, ip_addr, sizeof(cached->ip_addr));
    } else {
        cached->active = false;
        cached->ip_addr[0] = '\0';
    }

    *pdp = *cached;
    xSemaphoreGive(self->base.lock);
    return ESP_OK;
}

static esp_err_t air780ep_socket_prepare(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const char *cmds[] = {
        "AT+CIPMUX=0",
        "AT+CIPMODE=0",
        "AT+CIPQSEND=1",
        "AT+CIPRXF=1",
        "AT+CIPRXGET=5",
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, cmds[i], &ctx, 3000);
        ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmds[i]);

        ret = ensure_at_ok(&ctx.response, cmds[i]);
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmds[i]);
    }

    return ESP_OK;
}

static esp_err_t air780ep_socket_open(modem_handle_t me,
                                      const modem_socket_open_t *open)
{
    ESP_RETURN_ON_FALSE(me && open && open->host && open->host[0] &&
                        open->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket open args");
    ESP_RETURN_ON_FALSE(open->conn_id == AIR780EP_TCP_CONN_ID &&
                        open->proto == MODEM_SOCKET_PROTO_TCP,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");

    modem_air780ep_t *self = to_air780ep(me);

    /* SSL 通道开关：TLS 写 ctx0 hostname + CIPSSL=1；明文显式 CIPSSL=0 清残留。 */
    if (open->transport == MODEM_SOCKET_TRANSPORT_TLS) {
        ESP_RETURN_ON_FALSE(open->ssl_context_id == AIR780EP_SSL_TCP_CONTEXT_ID,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Air780EP TCP TLS requires ssl_context_id 0");
        ESP_RETURN_ON_FALSE(ssl_context_marked(self, AIR780EP_SSL_TCP_CONTEXT_ID),
                            ESP_ERR_INVALID_STATE, TAG,
                            "Air780EP TCP TLS context 0 not provisioned");

        char *hn_host = escape_at_string(open->host);
        ESP_RETURN_ON_FALSE(hn_host, ESP_ERR_NO_MEM, TAG, "escape ssl hostname failed");

        char hostname_cmd[160];
        /* AT command shape: AT+SSLCFG="hostname",0,"%s". */
        int hn = snprintf(hostname_cmd, sizeof(hostname_cmd),
                          "AT+SSLCFG=\"hostname\",0,\"%s\"", hn_host);
        free(hn_host);
        ESP_RETURN_ON_FALSE(hn > 0 && (size_t)hn < sizeof(hostname_cmd),
                            ESP_ERR_INVALID_ARG, TAG, "SSLCFG hostname truncated");

        air780ep_cmd_ctx_t hctx;
        esp_err_t ret = send_cmd(self, hostname_cmd, &hctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL hostname failed");
        ret = ensure_at_ok(&hctx.response, "AT+SSLCFG hostname");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+SSLCFG hostname not OK");

        air780ep_cmd_ctx_t sctx;
        ret = send_cmd(self, "AT+CIPSSL=1", &sctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set CIPSSL=1 failed");
        ret = ensure_at_ok(&sctx.response, "AT+CIPSSL=1");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPSSL=1 not OK");
    } else {
        air780ep_cmd_ctx_t sctx;
        esp_err_t ret = send_cmd(self, "AT+CIPSSL=0", &sctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set CIPSSL=0 failed");
        ret = ensure_at_ok(&sctx.response, "AT+CIPSSL=0");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPSSL=0 not OK");
    }

    char *host = escape_at_string(open->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape socket host failed");

    /* AT command shape: AT+CIPSTART="TCP","%s",%u. */
    int needed = snprintf(NULL, 0, "AT+CIPSTART=\"TCP\",\"%s\",%u",
                          host, (unsigned int)open->port);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    size_t cmd_size = (size_t)needed + 1U;
    char *cmd = malloc(cmd_size);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    int written = snprintf(cmd, cmd_size, "AT+CIPSTART=\"TCP\",\"%s\",%u",
                           host, (unsigned int)open->port);
    if (written < 0 || (size_t)written >= cmd_size) {
        free(cmd);
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    const at_cmd_success_match_t matches[] = {
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "CONNECT OK" },
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "ALREADY CONNECT" },
    };
    const at_cmd_options_t options = {
        .timeout_ms = open->timeout_ms,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = matches,
        .success_match_count = sizeof(matches) / sizeof(matches[0]),
    };

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK &&
        (response_contains(&ctx.response, "CONNECT OK") ||
         response_contains(&ctx.response, "ALREADY CONNECT"))) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    free(cmd);
    free(host);
    return ret;
}

static esp_err_t air780ep_socket_send(modem_handle_t me,
                                      const modem_socket_send_t *send)
{
    ESP_RETURN_ON_FALSE(me && send && send->data && send->len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket send args");
    ESP_RETURN_ON_FALSE(send->conn_id == AIR780EP_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");
    ESP_RETURN_ON_FALSE(send->len <= UINT_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "socket payload too large");
    if (send->modem_error_code) {
        *send->modem_error_code = 0;
    }

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u",
                           (unsigned int)send->len);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+CIPSEND command truncated");

    const at_cmd_success_match_t matches[] = {
        { .type = AT_CMD_SUCCESS_MATCH_PREFIX, .value = "DATAACCEPT" },
        { .type = AT_CMD_SUCCESS_MATCH_PREFIX, .value = "DATA ACCEPT" },
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "SEND OK" },
    };
    const at_cmd_options_t options = {
        .timeout_ms = send->timeout_ms,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = matches,
        .success_match_count = sizeof(matches) / sizeof(matches[0]),
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    esp_err_t ret = at_engine_send_cmd_with_payload(self->base.at, cmd,
                                                    send->data, send->len,
                                                    AIR780EP_TCP_PAYLOAD_PROMPT,
                                                    &ctx.response, &options);
    if (ret == ESP_OK &&
        (response_contains(&ctx.response, "DATAACCEPT") ||
          response_contains(&ctx.response, "DATA ACCEPT") ||
          response_contains(&ctx.response, "SEND OK"))) {
        return ESP_OK;
    }
    if (send->modem_error_code) {
        *send->modem_error_code = ctx.response.error_code;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t air780ep_socket_recv(modem_handle_t me,
                                      const modem_socket_recv_t *recv,
                                      modem_socket_recv_result_t *result)
{
    ESP_RETURN_ON_FALSE(me && recv && result && recv->max_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket recv args");
    ESP_RETURN_ON_FALSE(recv->conn_id == AIR780EP_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");

    size_t read_len = recv->max_len;
    if (read_len > AIR780EP_TCP_MAX_HEX_READ_BYTES) {
        read_len = AIR780EP_TCP_MAX_HEX_READ_BYTES;
    }

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=3,%u",
                           (unsigned int)read_len);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+CIPRXGET command truncated");

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CIPRXGET failed");

    ret = ensure_at_ok(&ctx.response, "AT+CIPRXGET");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPRXGET failed");

    size_t remaining_len = 0;
    const char *hex = find_ciprxget_hex_line(&ctx.response, &remaining_len);
    ESP_RETURN_ON_FALSE(hex, ESP_ERR_INVALID_RESPONSE, TAG,
                        "invalid AT+CIPRXGET response");

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    ret = decode_hex_payload(hex, &payload, &payload_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "decode AT+CIPRXGET payload failed");

    result->conn_id = AIR780EP_TCP_CONN_ID;
    result->payload = payload;
    result->payload_len = payload_len;
    result->remaining_len = remaining_len;
    result->modem_error_code = 0;
    return ESP_OK;
}

static esp_err_t air780ep_socket_close(modem_handle_t me,
                                       const modem_socket_close_t *close)
{
    ESP_RETURN_ON_FALSE(me && close, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket close args");
    ESP_RETURN_ON_FALSE(close->conn_id == AIR780EP_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");
    if (close->modem_error_code) {
        *close->modem_error_code = 0;
    }

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "CLOSE OK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = close->timeout_ms,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, "AT+CIPCLOSE", &ctx, &options);
    if (ret == ESP_OK && response_contains(&ctx.response, "CLOSE OK")) {
        return ESP_OK;
    }
    if (close->modem_error_code) {
        *close->modem_error_code = ctx.response.error_code;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t reset_mqtt_modes(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTMSGSET=0", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTMSGSET=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTTMSGSET to direct mode failed");

    ret = send_cmd(self, "AT+MQTTMODE=0", &ctx,
                   AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTMODE=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTTMODE to ASCII failed");

    return ESP_OK;
}

static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 0:  return MODEM_MQTT_STATUS_OFFLINE;
    case 1:  return MODEM_MQTT_STATUS_AUTHENTICATED;
    case 2:  return MODEM_MQTT_STATUS_TCP_CONNECTED;
    default: return MODEM_MQTT_STATUS_OFFLINE;
    }
}

static esp_err_t air780ep_mqtt_get_status(modem_handle_t me,
                                           modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTSTATU", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTSTATU");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MQTTSTATU failed");

    const char *line = find_line_with_prefix(&ctx.response, "+MQTTSTATU");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                        "+MQTTSTATU line missing");

    int state = 0;
    ret = parse_int_after_prefix(line, "+MQTTSTATU", &state);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +MQTTSTATU failed");
    ESP_RETURN_ON_FALSE(state >= 0 && state <= 2, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid MQTT status %d", state);

    *status = map_mqtt_status(state);
    return ESP_OK;
}

static esp_err_t air780ep_mqtt_configure(modem_handle_t me,
                                          const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id && config->host &&
                        config->port > 0 &&
                        (config->transport == MODEM_MQTT_TRANSPORT_PLAIN_TCP ||
                         config->transport == MODEM_MQTT_TRANSPORT_TLS),
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT config");
    ESP_RETURN_ON_FALSE(config->transport != MODEM_MQTT_TRANSPORT_TLS ||
                        config->ssl_context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported MQTT SSL context");

    modem_air780ep_t *self = to_air780ep(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool connected = self->mqtt_tcp_connected || self->mqtt_session_connected;
    bool ssl_provisioned = ssl_context_marked(self, AIR780EP_SSL_MQTT_CONTEXT_ID);
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(!connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT is connected");
    ESP_RETURN_ON_FALSE(config->transport != MODEM_MQTT_TRANSPORT_TLS || ssl_provisioned,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT SSL context not provisioned");

    esp_err_t ret = reset_mqtt_modes(self);
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTT modes failed");

    modem_mqtt_config_t new_config = {0};
    ret = copy_mqtt_config(&new_config, config);
    ESP_RETURN_ON_ERROR(ret, TAG, "copy MQTT config failed");

    char *client_id = escape_at_string(new_config.client_id);
    char *username = escape_at_string(new_config.username ? new_config.username : "");
    char *password = escape_at_string(new_config.password ? new_config.password : "");
    if (!client_id || !username || !password) {
        free(client_id);
        free(username);
        free(password);
        free_mqtt_config(&new_config);
        return ESP_ERR_NO_MEM;
    }

    int needed = snprintf(NULL, 0, "AT+MCONFIG=\"%s\",\"%s\",\"%s\"",
                          client_id, username, password);
    if (needed < 0) {
        free(client_id);
        free(username);
        free(password);
        free_mqtt_config(&new_config);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(client_id);
        free(username);
        free(password);
        free_mqtt_config(&new_config);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MCONFIG=\"%s\",\"%s\",\"%s\"",
             client_id, username, password);

    air780ep_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MCONFIG");
    }
    if (ret == ESP_OK) {
        if (self->base.lock) {
            xSemaphoreTake(self->base.lock, portMAX_DELAY);
        }
        free_mqtt_config(&self->mqtt_config);
        self->mqtt_config = new_config;
        memset(&new_config, 0, sizeof(new_config));
        self->mqtt_configured = true;
        self->mqtt_transport = self->mqtt_config.transport;
        self->mqtt_ssl_context_id = self->mqtt_config.ssl_context_id;
        if (self->base.lock) {
            xSemaphoreGive(self->base.lock);
        }
    }

    free(cmd);
    free(client_id);
    free(username);
    free(password);
    if (ret != ESP_OK) {
        free_mqtt_config(&new_config);
    }
    return ret;
}

static esp_err_t air780ep_mqtt_tcp_connect(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    char *host_copy = NULL;
    uint16_t port = 0;
    modem_mqtt_transport_t transport = MODEM_MQTT_TRANSPORT_PLAIN_TCP;
    uint8_t ssl_context_id = 0;
    bool tls_context_marked = false;
    bool configured = false;
    bool tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    configured = self->mqtt_configured;
    tcp_connected = self->mqtt_tcp_connected;
    if (configured && !tcp_connected) {
        port = self->mqtt_config.port;
        transport = self->mqtt_transport;
        ssl_context_id = self->mqtt_ssl_context_id;
        if (transport == MODEM_MQTT_TRANSPORT_TLS &&
            ssl_context_id == AIR780EP_SSL_MQTT_CONTEXT_ID) {
            tls_context_marked = ssl_context_marked(self, AIR780EP_SSL_MQTT_CONTEXT_ID);
        }
        if (transport == MODEM_MQTT_TRANSPORT_PLAIN_TCP || tls_context_marked) {
            host_copy = clone_mqtt_string(self->mqtt_config.host);
        }
    }
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(configured, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT not configured");
    ESP_RETURN_ON_FALSE(!tcp_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT TCP already connected");
    ESP_RETURN_ON_FALSE(transport != MODEM_MQTT_TRANSPORT_TLS ||
                        ssl_context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                        ESP_ERR_INVALID_STATE, TAG, "invalid MQTT SSL context");
    ESP_RETURN_ON_FALSE(transport != MODEM_MQTT_TRANSPORT_TLS || tls_context_marked,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT SSL context not provisioned");
    ESP_RETURN_ON_FALSE(host_copy, ESP_ERR_NO_MEM, TAG, "copy MQTT host failed");

    char *host = escape_at_string(host_copy);
    free(host_copy);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape host failed");

    /* Air780EP TLS MQTT command token: AT+SSLMIPSTART=. */
    const char *start_cmd = transport == MODEM_MQTT_TRANSPORT_TLS ?
                            "AT+SSLMIPSTART" : "AT+MIPSTART";
    int needed = snprintf(NULL, 0, "%s=\"%s\",%u",
                          start_cmd, host, (unsigned int)port);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "%s=\"%s\",%u",
             start_cmd, host, (unsigned int)port);

    const at_cmd_success_match_t matches[] = {
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "CONNECT OK" },
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "ALREADY CONNECT" },
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CONNECT_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = matches,
        .success_match_count = sizeof(matches) / sizeof(matches[0]),
    };

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, start_cmd);
    }
    if (ret == ESP_OK) {
        if (self->base.lock) {
            xSemaphoreTake(self->base.lock, portMAX_DELAY);
        }
        self->mqtt_tcp_connected = true;
        if (self->base.lock) {
            xSemaphoreGive(self->base.lock);
        }
    }

    free(cmd);
    free(host);
    return ret;
}

static esp_err_t air780ep_mqtt_connect(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool configured = false;
    bool tcp_connected = false;
    bool session_connected = false;
    bool clean_session = false;
    uint16_t keepalive_s = 0;
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    configured = self->mqtt_configured;
    tcp_connected = self->mqtt_tcp_connected;
    session_connected = self->mqtt_session_connected;
    clean_session = self->mqtt_config.clean_session;
    keepalive_s = self->mqtt_config.keepalive_s;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(configured, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT not configured");
    ESP_RETURN_ON_FALSE(tcp_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT TCP not connected");
    ESP_RETURN_ON_FALSE(!session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session already connected");

    char cmd[48];
    int written = snprintf(cmd, sizeof(cmd), "AT+MCONNECT=%u,%u",
                           clean_session ? 1U : 0U,
                           (unsigned int)keepalive_s);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MCONNECT command truncated");

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "CONNACK OK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CONNECT_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MCONNECT");
    }
    if (ret == ESP_OK) {
        if (self->base.lock) {
            xSemaphoreTake(self->base.lock, portMAX_DELAY);
        }
        self->mqtt_session_connected = true;
        self->mqtt_data_enabled = true;
        if (self->base.lock) {
            xSemaphoreGive(self->base.lock);
        }
    }
    return ret;
}

static esp_err_t air780ep_mqtt_disconnect(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool session_connected = false;
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    session_connected = self->mqtt_session_connected;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(session_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT session not connected");

    set_mqtt_data_enabled(self, false);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MDISCONNECT", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MDISCONNECT");
    }
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_session_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    return ret;
}

static esp_err_t air780ep_mqtt_tcp_disconnect(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool tcp_connected = false;
    bool session_connected = false;
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    tcp_connected = self->mqtt_tcp_connected;
    session_connected = self->mqtt_session_connected;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(tcp_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT TCP not connected");
    ESP_RETURN_ON_FALSE(!session_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT session still connected");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MIPCLOSE", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MIPCLOSE");
    }
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    return ret;
}

static esp_err_t air780ep_mqtt_subscribe(modem_handle_t me,
                                          const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic && topic->qos <= 1,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char *escaped_topic = escape_at_string(topic->topic);
    ESP_RETURN_ON_FALSE(escaped_topic, ESP_ERR_NO_MEM, TAG, "escape topic failed");

    int needed = snprintf(NULL, 0, "AT+MSUB=\"%s\",%u",
                          escaped_topic, (unsigned int)topic->qos);
    if (needed < 0) {
        free(escaped_topic);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(escaped_topic);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MSUB=\"%s\",%u",
             escaped_topic, (unsigned int)topic->qos);

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "SUBACK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CMD_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MSUB");
    }

    free(cmd);
    free(escaped_topic);
    return ret;
}

static esp_err_t air780ep_mqtt_unsubscribe(modem_handle_t me,
                                            const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char *escaped_topic = escape_at_string(topic->topic);
    ESP_RETURN_ON_FALSE(escaped_topic, ESP_ERR_NO_MEM, TAG, "escape topic failed");

    int needed = snprintf(NULL, 0, "AT+MUNSUB=\"%s\"", escaped_topic);
    if (needed < 0) {
        free(escaped_topic);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(escaped_topic);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MUNSUB=\"%s\"", escaped_topic);

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "UNSUBACK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CMD_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MUNSUB");
    }

    free(cmd);
    free(escaped_topic);
    return ret;
}

static esp_err_t air780ep_mqtt_publish(modem_handle_t me,
                                        const modem_mqtt_publish_t *publish)
{
    ESP_RETURN_ON_FALSE(me && publish && publish->topic && publish->payload &&
                        publish->payload_len > 0, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(publish->qos <= 1, ESP_ERR_NOT_SUPPORTED,
                        TAG, "MQTT QoS %u not supported", (unsigned int)publish->qos);

    char *escaped_topic = escape_at_string(publish->topic);
    ESP_RETURN_ON_FALSE(escaped_topic, ESP_ERR_NO_MEM, TAG, "escape topic failed");

    int needed = snprintf(NULL, 0, "AT+MPUBEX=\"%s\",%u,%u,%u",
                          escaped_topic, (unsigned int)publish->qos,
                          publish->retain ? 1U : 0U,
                          (unsigned int)publish->payload_len);
    if (needed < 0) {
        free(escaped_topic);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(escaped_topic);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MPUBEX=\"%s\",%u,%u,%u",
             escaped_topic, (unsigned int)publish->qos, publish->retain ? 1U : 0U,
             (unsigned int)publish->payload_len);

    const at_cmd_success_match_t puback_match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "PUBACK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CMD_TIMEOUT_MS,
        .flags = publish->qos == 1 ?
                 AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK : 0,
        .success_matches = publish->qos == 1 ? &puback_match : NULL,
        .success_match_count = publish->qos == 1 ? 1 : 0,
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    esp_err_t ret = at_engine_send_cmd_with_payload(self->base.at, cmd,
                                                    publish->payload,
                                                    publish->payload_len,
                                                    AIR780EP_MQTT_PAYLOAD_PROMPT,
                                                    &ctx.response, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MPUBEX");
    }

    free(cmd);
    free(escaped_topic);
    return ret;
}

static esp_err_t air780ep_ping(modem_handle_t me,
                               const modem_ping_request_t *request,
                               modem_ping_reply_t *replies,
                               size_t max_replies,
                               modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && request && request->host && request->host[0] &&
                        replies && max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(request->count >= 1 &&
                        request->count <= AIR780EP_CIPPING_MAX_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping count");

    char *host = escape_at_string(request->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape ping host failed");

    /* Command form: AT+CIPPING="%s",%u,%u,%u,%u */
    int needed = snprintf(NULL, 0, "AT+CIPPING=\"%s\",%u,%u,%u,%u",
                          host, (unsigned int)request->count,
                          (unsigned int)request->data_len,
                          (unsigned int)request->timeout_100ms,
                          (unsigned int)request->ttl);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+CIPPING=\"%s\",%u,%u,%u,%u",
             host, (unsigned int)request->count,
             (unsigned int)request->data_len,
             (unsigned int)request->timeout_100ms,
             (unsigned int)request->ttl);

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ping_cmd_timeout_ms(request));
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+CIPPING");
    }
    if (ret != ESP_OK) {
        free(cmd);
        free(host);
        return ret;
    }

    size_t parsed_count = 0;
    int line_count = ctx.response.line_count;
    if (line_count > ctx.response.max_lines) {
        line_count = ctx.response.max_lines;
    }
    for (int i = 0; i < line_count && parsed_count < request->count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, AIR780EP_CIPPING_PREFIX,
                            sizeof(AIR780EP_CIPPING_PREFIX) - 1U) != 0) {
            continue;
        }
        ret = parse_cipping_line(line, request, &replies[parsed_count]);
        if (ret != ESP_OK) {
            free(cmd);
            free(host);
            return ret;
        }
        parsed_count++;
    }

    if (parsed_count != request->count) {
        free(cmd);
        free(host);
        return ESP_ERR_INVALID_RESPONSE;
    }

    calculate_ping_summary(request, replies, parsed_count, summary);
    free(cmd);
    free(host);
    return ESP_OK;
}

static esp_err_t parse_cipping_line(const char *line,
                                    const modem_ping_request_t *request,
                                    modem_ping_reply_t *reply)
{
    ESP_RETURN_ON_FALSE(request && reply, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    if (!line) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t prefix_len = sizeof(AIR780EP_CIPPING_PREFIX) - 1U;
    if (strncmp(line, AIR780EP_CIPPING_PREFIX, prefix_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = line + prefix_len;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == ':') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t seq = 0;
    esp_err_t ret = parse_cipping_uint(&cursor, UINT8_MAX, &seq);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +CIPPING seq");
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    const char *ip_start = cursor;
    const char *ip_end = NULL;
    if (*cursor == '"') {
        ip_start = cursor + 1;
        ip_end = strchr(ip_start, '"');
        if (!ip_end) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        cursor = ip_end + 1;
    } else {
        ip_end = cursor;
        while (*ip_end && *ip_end != ',') {
            ip_end++;
        }
        cursor = ip_end;
        while (ip_end > ip_start && isspace((unsigned char)*(ip_end - 1))) {
            ip_end--;
        }
    }
    if (ip_end == ip_start || (size_t)(ip_end - ip_start) >= sizeof(reply->ip)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t reply_time = 0;
    ret = parse_cipping_uint(&cursor, UINT32_MAX, &reply_time);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +CIPPING reply time");
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t ttl = 0;
    ret = parse_cipping_uint(&cursor, UINT8_MAX, &ttl);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +CIPPING ttl");
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    modem_ping_reply_t parsed = {
        .seq = (uint8_t)seq,
        .time_ms = reply_time,
        .ttl = (uint8_t)ttl,
    };
    memcpy(parsed.ip, ip_start, (size_t)(ip_end - ip_start));
    parsed.ip[ip_end - ip_start] = '\0';

    bool lost = reply_time == (uint32_t)request->timeout_100ms * 100U &&
                parsed.ttl == 255;
    parsed.success = !lost;
    *reply = parsed;
    return ESP_OK;
}

static esp_err_t parse_cipping_uint(const char **cursor,
                                    uint32_t max_value,
                                    uint32_t *out_value)
{
    ESP_RETURN_ON_FALSE(cursor && *cursor && out_value,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *value = *cursor;
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (!isdigit((unsigned char)*value)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || errno == ERANGE || parsed > max_value) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *cursor = end;
    *out_value = (uint32_t)parsed;
    return ESP_OK;
}

static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary)
{
    if (!request || !replies || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    summary->sent = request->count;

    uint64_t total_time_ms = 0;
    for (size_t i = 0; i < reply_count; i++) {
        if (!replies[i].success) {
            continue;
        }

        if (summary->received == 0 || replies[i].time_ms < summary->min_time_ms) {
            summary->min_time_ms = replies[i].time_ms;
        }
        if (replies[i].time_ms > summary->max_time_ms) {
            summary->max_time_ms = replies[i].time_ms;
        }
        total_time_ms += replies[i].time_ms;
        summary->received++;
    }

    summary->lost = summary->sent - summary->received;
    if (summary->received > 0) {
        summary->avg_time_ms = (uint32_t)(total_time_ms / summary->received);
    }
}

static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request)
{
    if (!request) {
        return AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
    }
    if (request->total_timeout_ms != 0) {
        return request->total_timeout_ms;
    }

    return (uint32_t)request->count * (uint32_t)request->timeout_100ms * 100U +
           AIR780EP_CIPPING_CMD_OVERHEAD_MS;
}

static esp_err_t air780ep_http_request(modem_handle_t me,
                                       const modem_http_request_t *request,
                                       modem_http_response_t *response)
{
    modem_air780ep_t *self = to_air780ep(me);
    esp_err_t ret = ESP_OK;
    bool http_initialized = false;

    if (request->modem_error_code) {
        *request->modem_error_code = 0;
    }

    /* 1. AT+HTTPINIT */
    air780ep_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    ret = send_cmd(self, "AT+HTTPINIT", &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPINIT send failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ensure_at_ok(&ctx.response, "AT+HTTPINIT");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPINIT failed");
        return ret;
    }
    http_initialized = true;

    /* cleanup macro: AT+HTTPTERM on any failure path */
#define HTTP_CLEANUP()                                          \
    do {                                                        \
        if (http_initialized) {                                \
            air780ep_cmd_ctx_t term_ctx;                        \
            init_cmd_ctx(&term_ctx);                            \
            (void)send_cmd(self, "AT+HTTPTERM", &term_ctx,      \
                           AIR780EP_HTTP_CMD_TIMEOUT_MS);       \
            http_initialized = false;                           \
        }                                                       \
    } while (0)

    /* 2. AT+HTTPSSL */
    init_cmd_ctx(&ctx);
    const char *ssl_cmd = (request->transport == MODEM_HTTP_TRANSPORT_HTTPS) ?
                          "AT+HTTPSSL=1" : "AT+HTTPSSL=0";
    ret = send_cmd(self, ssl_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPSSL");
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPSSL failed: %s", esp_err_to_name(ret));
        HTTP_CLEANUP();
        return ret;
    }

    /* 3. AT+HTTPPARA="CID",1 */
    init_cmd_ctx(&ctx);
    ret = send_cmd(self, "AT+HTTPPARA=\"CID\",1", &ctx,
                   AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA CID");
    }
    if (ret != ESP_OK) {
        HTTP_CLEANUP();
        return ret;
    }

    /* 4. AT+HTTPPARA="URL",<url> */
    init_cmd_ctx(&ctx);
    int needed = snprintf(NULL, 0, "AT+HTTPPARA=\"URL\",\"%s\"", request->url);
    if (needed < 0) {
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_ARG;
    }
    char *url_cmd = malloc((size_t)needed + 1U);
    if (!url_cmd) {
        HTTP_CLEANUP();
        return ESP_ERR_NO_MEM;
    }
    snprintf(url_cmd, (size_t)needed + 1U, "AT+HTTPPARA=\"URL\",\"%s\"",
             request->url);
    ret = send_cmd(self, url_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    free(url_cmd);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA URL");
    }
    if (ret != ESP_OK) {
        HTTP_CLEANUP();
        return ret;
    }

    /* 5. AT+HTTPPARA="CONTENT",<content_type> (optional) */
    if (request->content_type && request->content_type[0]) {
        init_cmd_ctx(&ctx);
        needed = snprintf(NULL, 0, "AT+HTTPPARA=\"CONTENT\",\"%s\"",
                          request->content_type);
        if (needed < 0) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        char *ct_cmd = malloc((size_t)needed + 1U);
        if (!ct_cmd) {
            HTTP_CLEANUP();
            return ESP_ERR_NO_MEM;
        }
        snprintf(ct_cmd, (size_t)needed + 1U,
                 "AT+HTTPPARA=\"CONTENT\",\"%s\"", request->content_type);
        ret = send_cmd(self, ct_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
        free(ct_cmd);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA CONTENT");
        }
        if (ret != ESP_OK) {
            HTTP_CLEANUP();
            return ret;
        }
    }

    /* 6. POST: AT+HTTPDATA=<len>,<time> + body via prompt */
    if (request->method == MODEM_HTTP_METHOD_POST && request->body &&
        request->body_len > 0) {
        if (request->body_len > AIR780EP_HTTPDATA_BODY_MAX) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_SIZE;
        }
        char data_cmd[48];
        int written = snprintf(data_cmd, sizeof(data_cmd),
                               "AT+HTTPDATA=%u,%u",
                               (unsigned int)request->body_len,
                               (unsigned int)AIR780EP_HTTPDATA_PROMPT_MS);
        if (written < 0 || (size_t)written >= sizeof(data_cmd)) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        const at_cmd_options_t data_options = {
            .timeout_ms = AIR780EP_HTTPDATA_PROMPT_MS,
            .flags = 0,
        };
        init_cmd_ctx(&ctx);
        ret = at_engine_send_cmd_with_payload(self->base.at, data_cmd,
                                              request->body, request->body_len,
                                              "DOWNLOAD", &ctx.response,
                                              &data_options);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPDATA");
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AT+HTTPDATA failed: %s", esp_err_to_name(ret));
            HTTP_CLEANUP();
            return ret;
        }
    }

    /* 7. AT+HTTPACTION=<method> — wait for +HTTPACTION URC as success line */
    const char *action_cmd = (request->method == MODEM_HTTP_METHOD_POST) ?
                             "AT+HTTPACTION=1" : "AT+HTTPACTION=0";
    const at_cmd_success_match_t action_match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = "+HTTPACTION:",
    };
    uint32_t action_timeout = request->timeout_ms > 0 ?
                              request->timeout_ms : AIR780EP_HTTP_ACTION_TIMEOUT_MS;
    const at_cmd_options_t action_options = {
        .timeout_ms = action_timeout,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &action_match,
        .success_match_count = 1,
    };
    init_cmd_ctx(&ctx);
    ret = send_cmd_with_options(self, action_cmd, &ctx, &action_options);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPACTION failed: %s", esp_err_to_name(ret));
        if (request->modem_error_code) {
            *request->modem_error_code = ctx.response.error_code;
        }
        HTTP_CLEANUP();
        return (ret == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT
                                        : ESP_ERR_INVALID_RESPONSE;
    }

    /* Parse +HTTPACTION: <method>,<status>,<len> */
    const char *action_line = find_line_with_prefix(&ctx.response, "+HTTPACTION:");
    if (!action_line) {
        ESP_LOGW(TAG, "missing +HTTPACTION line");
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }
    int method_val = 0, status_val = 0, data_len = 0;
    int parsed = sscanf(action_line, "+HTTPACTION: %d,%d,%d",
                        &method_val, &status_val, &data_len);
    if (parsed < 2) {
        ESP_LOGW(TAG, "parse +HTTPACTION failed: %s", action_line);
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Module-side errors: 600..606 */
    if (status_val >= 600 && status_val <= 606) {
        ESP_LOGW(TAG, "HTTPACTION module error %d", status_val);
        if (request->modem_error_code) {
            *request->modem_error_code = status_val;
        }
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }

    response->status_code = status_val;

    /* 8. AT+HTTPREAD (if body expected) */
    if (data_len > 0) {
        size_t read_len = (size_t)data_len;
        if (read_len > AIR780EP_HTTPREAD_BODY_MAX) {
            read_len = AIR780EP_HTTPREAD_BODY_MAX;
        }
        char read_cmd[40];
        int r_written = snprintf(read_cmd, sizeof(read_cmd),
                                 "AT+HTTPREAD=0,%u", (unsigned int)read_len);
        if (r_written < 0 || (size_t)r_written >= sizeof(read_cmd)) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        init_cmd_ctx(&ctx);
        ret = send_cmd(self, read_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPREAD");
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AT+HTTPREAD failed: %s", esp_err_to_name(ret));
            HTTP_CLEANUP();
            return ret;
        }
        /* Find +HTTPREAD:<len> then concatenate data lines */
        const char *read_hdr = find_line_with_prefix(&ctx.response, "+HTTPREAD:");
        if (!read_hdr) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_RESPONSE;
        }
        int hdr_len = 0;
        if (sscanf(read_hdr, "+HTTPREAD: %d", &hdr_len) != 1 || hdr_len <= 0) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t *body_buf = malloc((size_t)hdr_len);
        if (!body_buf) {
            HTTP_CLEANUP();
            return ESP_ERR_NO_MEM;
        }
        size_t copied = 0;
        for (int i = 0; i < ctx.response.line_count && (int)copied < hdr_len; i++) {
            const char *line = ctx.response.lines[i];
            if (!line) {
                continue;
            }
            if (strncmp(line, "+HTTPREAD:", 10) == 0) {
                continue;
            }
            size_t line_len = strlen(line);
            size_t remain = (size_t)hdr_len - copied;
            /* Restore \r\n between data lines split by AT engine line parser */
            if (copied > 0 && remain >= 2) {
                body_buf[copied++] = '\r';
                body_buf[copied++] = '\n';
            }
            size_t to_copy = line_len < remain ? line_len : remain;
            memcpy(body_buf + copied, line, to_copy);
            copied += to_copy;
        }
        response->body = body_buf;
        response->body_len = copied;
    }

#undef HTTP_CLEANUP

    /* 9. AT+HTTPTERM (finally cleanup) */
    init_cmd_ctx(&ctx);
    (void)send_cmd(self, "AT+HTTPTERM", &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);

    return ESP_OK;
}

static esp_err_t post_mqtt_data_event(modem_air780ep_t *self, char *topic,
                                       size_t topic_len, uint8_t *payload,
                                       size_t payload_len)
{
    ESP_RETURN_ON_FALSE(self && topic && payload, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");

    const modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_DATA,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_MQTT,
            .topic = topic,
            .topic_len = topic_len,
            .payload = payload,
            .payload_len = payload_len,
        },
    };

    return modem_post_event(&self->base, &event);
}

static char *escape_at_string(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t escaped_len = 0;
    for (const char *cursor = value; *cursor; cursor++) {
        switch (*cursor) {
        case '"':
        case '\\':
        case '\r':
        case '\n':
            escaped_len += 3;
            break;
        default:
            escaped_len++;
            break;
        }
    }

    char *escaped = malloc(escaped_len + 1U);
    if (!escaped) {
        return NULL;
    }

    char *out = escaped;
    for (const char *cursor = value; *cursor; cursor++) {
        switch (*cursor) {
        case '"':
            memcpy(out, "\\22", 3);
            out += 3;
            break;
        case '\\':
            memcpy(out, "\\5C", 3);
            out += 3;
            break;
        case '\r':
            memcpy(out, "\\0D", 3);
            out += 3;
            break;
        case '\n':
            memcpy(out, "\\0A", 3);
            out += 3;
            break;
        default:
            *out++ = *cursor;
            break;
        }
    }
    *out = '\0';
    return escaped;
}

static bool parse_msub_direct(const char *line, char **topic, size_t *topic_len,
                              uint8_t **payload, size_t *payload_len)
{
    if (!line || !topic || !topic_len || !payload || !payload_len) {
        return false;
    }

    *topic = NULL;
    *topic_len = 0;
    *payload = NULL;
    *payload_len = 0;

    const char *cursor = skip_prefix_value(line, AIR780EP_URC_MSUB);
    if (!cursor) {
        return false;
    }

    const char *topic_start = cursor;
    const char *topic_end = NULL;
    if (*cursor == '"') {
        topic_start = ++cursor;
        topic_end = strchr(topic_start, '"');
        if (!topic_end) {
            return false;
        }
        cursor = topic_end + 1;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor != ',') {
            return false;
        }
    } else {
        topic_end = strchr(cursor, ',');
        if (!topic_end) {
            return false;
        }
        while (topic_end > topic_start && isspace((unsigned char)*(topic_end - 1))) {
            topic_end--;
        }
    }

    if (topic_end == topic_start) {
        return false;
    }

    cursor = strchr(cursor, ',');
    if (!cursor) {
        return false;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed_len = strtoul(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || parsed_len > SIZE_MAX) {
        return false;
    }
    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return false;
    }
    cursor++;

    size_t parsed_payload_len = (size_t)parsed_len;
    if (strlen(cursor) < parsed_payload_len) {
        return false;
    }

    size_t parsed_topic_len = (size_t)(topic_end - topic_start);
    char *topic_buf = malloc(parsed_topic_len + 1U);
    if (!topic_buf) {
        return false;
    }
    uint8_t *payload_buf = malloc(parsed_payload_len > 0 ? parsed_payload_len : 1U);
    if (!payload_buf) {
        free(topic_buf);
        return false;
    }

    memcpy(topic_buf, topic_start, parsed_topic_len);
    topic_buf[parsed_topic_len] = '\0';
    if (parsed_payload_len > 0) {
        memcpy(payload_buf, cursor, parsed_payload_len);
    }

    *topic = topic_buf;
    *topic_len = parsed_topic_len;
    *payload = payload_buf;
    *payload_len = parsed_payload_len;
    return true;
}

static esp_err_t air780ep_unregister_urcs(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const struct {
        const char *prefix;
        at_urc_handler_t *handler;
    } urcs[] = {
        { AIR780EP_URC_CPIN, &self->cpin_handler },
        { AIR780EP_URC_CREG, &self->creg_handler },
        { AIR780EP_URC_CEREG, &self->cereg_handler },
        { AIR780EP_URC_CGREG, &self->cgreg_handler },
        { AIR780EP_URC_CGEV, &self->cgev_handler },
        { AIR780EP_URC_PDP_DEACT, &self->pdp_deact_handler },
        { AIR780EP_URC_PDP_COLON_DEACT, &self->pdp_colon_deact_handler },
        { AIR780EP_URC_MSUB, &self->msub_handler },
        { AIR780EP_CIPRXGET_READY_PREFIX, &self->tcp_readable_handler },
        { AIR780EP_CIPRXGET_READY_COMPACT_PREFIX,
          &self->tcp_readable_compact_handler },
        { "CLOSED", &self->tcp_closed_handler },
        { AIR780EP_TCP_ERROR_PREFIX, &self->tcp_error_handler },
    };

    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < sizeof(urcs) / sizeof(urcs[0]); i++) {
        esp_err_t err = at_engine_unregister_urc(self->base.at, urcs[i].prefix);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && ret == ESP_OK) {
            ret = err;
        }
    }

    if (ret != ESP_OK) {
        return ret;
    }

    for (size_t i = 0; i < sizeof(urcs) / sizeof(urcs[0]); i++) {
        memset(urcs[i].handler, 0, sizeof(*urcs[i].handler));
    }
    self->urc_registered = false;

    return ret;
}

static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    modem_sim_status_t status = parse_sim_status_line(line);

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        self->last_sim_status = status;
        xSemaphoreGive(self->base.lock);
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_SIM_CHANGED,
        .data.sim_status = status,
    };
    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post SIM changed event failed: %s", esp_err_to_name(ret));
    }
}

static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    modem_reg_status_t status = MODEM_REG_UNKNOWN;
    esp_err_t ret = parse_registration_urc_line(line, prefix, &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse registration URC failed: %s", line ? line : "<NULL>");
        status = MODEM_REG_UNKNOWN;
    }

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop registration URC, lock busy");
        return;
    }
    self->last_reg_status = status;
    xSemaphoreGive(self->base.lock);

    switch (status) {
    case MODEM_REG_REGISTERED_HOME:
    case MODEM_REG_REGISTERED_ROAMING:
        set_state_nonblocking(self, MODEM_STATE_REGISTERED);
        break;
    case MODEM_REG_SEARCHING:
        set_state_nonblocking(self, MODEM_STATE_REGISTERING);
        break;
    case MODEM_REG_NOT_REGISTERED:
    case MODEM_REG_DENIED:
        set_state_nonblocking(self, MODEM_STATE_READY);
        break;
    case MODEM_REG_UNKNOWN:
    default:
        break;
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_REG_CHANGED,
        .data.reg_status = status,
    };
    ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post registration changed event failed: %s", esp_err_to_name(ret));
    }
}

static void cgev_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    modem_event_id_t event_id;
    bool active;
    if (strstr(line, "PDN ACT")) {
        event_id = MODEM_EVENT_PDP_ACTIVATED;
        active = true;
    } else if (strstr(line, "PDN DEACT")) {
        event_id = MODEM_EVENT_PDP_DEACTIVATED;
        active = false;
    } else {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    uint8_t cid = 0;
    if (!parse_cid_from_line(line, &cid)) {
        ESP_LOGW(TAG, "drop malformed +CGEV URC: %s", line);
        return;
    }

    if (!self->base.lock) {
        ESP_LOGW(TAG, "drop +CGEV URC, lock busy");
        return;
    }
    xSemaphoreTake(self->base.lock, portMAX_DELAY);

    modem_pdp_context_t *pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return;
    }
    pdp->active = active;
    if (!active) {
        pdp->ip_addr[0] = '\0';
        self->mqtt_data_enabled = false;
        self->mqtt_session_connected = false;
        self->mqtt_tcp_connected = false;
    }

    const modem_event_t event = {
        .id = event_id,
        .data.pdp = *pdp,
    };
    xSemaphoreGive(self->base.lock);

    if (!active) {
        set_state_nonblocking(self, MODEM_STATE_READY);
    }

    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post PDP event failed: %s", esp_err_to_name(ret));
    }
}

static void pdp_deact_urc_handler(const char *prefix, const char *line,
                                  void *user_ctx)
{
    (void)prefix;
    (void)line;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;

    modem_pdp_context_t affected[AIR780EP_MAX_PDP_CONTEXTS];
    if (!self->base.lock) {
        ESP_LOGW(TAG, "drop PDP deactivation URC, lock busy");
        return;
    }
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    size_t affected_count = clear_all_pdp_cache(self, affected,
                                                AIR780EP_MAX_PDP_CONTEXTS);
    xSemaphoreGive(self->base.lock);

    set_state_nonblocking(self, MODEM_STATE_READY);
    post_pdp_deactivated_events(self, affected, affected_count);
}

static void air780ep_post_tcp_readable(modem_air780ep_t *self)
{
    if (!self) {
        return;
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_DATA,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = AIR780EP_TCP_CONN_ID,
        },
    };
    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post TCP readable event failed: %s", esp_err_to_name(ret));
    }
}

static void air780ep_post_tcp_closed(modem_air780ep_t *self, int reason,
                                     int modem_error_code)
{
    if (!self) {
        return;
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_CLOSED,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = AIR780EP_TCP_CONN_ID,
            .reason = reason,
            .modem_error_code = modem_error_code,
        },
    };
    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post TCP closed event failed: %s", esp_err_to_name(ret));
    }
}

static void tcp_readable_urc_handler(const char *prefix, const char *line,
                                     void *user_ctx)
{
    (void)prefix;
    (void)line;

    if (!user_ctx) {
        return;
    }

    air780ep_post_tcp_readable((modem_air780ep_t *)user_ctx);
}

static void tcp_closed_urc_handler(const char *prefix, const char *line,
                                   void *user_ctx)
{
    (void)prefix;
    (void)line;

    if (!user_ctx) {
        return;
    }

    air780ep_post_tcp_closed((modem_air780ep_t *)user_ctx, 0, 0);
}

static void tcp_error_urc_handler(const char *prefix, const char *line,
                                  void *user_ctx)
{
    (void)prefix;

    if (!user_ctx) {
        return;
    }

    int error_code = 0;
    if (line) {
        const char *value = skip_prefix_value(line, AIR780EP_TCP_ERROR_PREFIX);
        if (value) {
            errno = 0;
            char *end = NULL;
            long parsed = strtol(value, &end, 10);
            if (end != value && errno != ERANGE && parsed >= INT_MIN && parsed <= INT_MAX) {
                error_code = (int)parsed;
            }
        }
    }

    air780ep_post_tcp_closed((modem_air780ep_t *)user_ctx, ESP_FAIL, error_code);
}

static void handle_msub_urc(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    char *topic = NULL;
    size_t topic_len = 0;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (!parse_msub_direct(line, &topic, &topic_len, &payload, &payload_len)) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    if (!mqtt_data_is_enabled(self)) {
        free(topic);
        free(payload);
        return;
    }

    esp_err_t ret = post_mqtt_data_event(self, topic, topic_len, payload, payload_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post MQTT data event failed: %s", esp_err_to_name(ret));
        free(topic);
        free(payload);
    }
}
