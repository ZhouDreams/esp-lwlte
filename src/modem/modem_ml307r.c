/**
 * @file modem_ml307r.c
 * @brief ML307R 调制解调器实现
 * @details ML307R modem implementation
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include "modem_ml307r.h"
#include "modem_priv.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
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
#define TAG "modem_ml307r"
#define ML307R_MAX_PDP_CONTEXTS          1
#define ML307R_PRIMARY_CID               1
#define ML307R_MQTT_CONNECT_ID           0
#define ML307R_TCP_CONN_ID               0
#define ML307R_TCP_PAYLOAD_PROMPT        ">"
#define ML307R_TCP_MAX_HEX_READ_BYTES    730U
#define ML307R_TCP_MIPRD_LINE_OVERHEAD   48U
#define ML307R_MAX_RESPONSE_LINES        101
#define ML307R_DEFAULT_CMD_TIMEOUT_MS    9000
#define ML307R_DEFAULT_READY_TIMEOUT_MS  30000
#define ML307R_AT_READY_PROBE_TIMEOUT_MS 1000
#define ML307R_INIT_RETRY_DELAY_MS       500
#define ML307R_INIT_CMD_MAX_ATTEMPTS     3
#define ML307R_MIPCALL_TIMEOUT_MS        90000
#define ML307R_MQTT_CMD_TIMEOUT_MS       9000
#define ML307R_MQTT_CONNECT_TIMEOUT_MS   60000
#define ML307R_MQTT_MAX_PAYLOAD_LEN      1024U
#define ML307R_MPING_PREFIX              "+MPING:"
#define ML307R_MPING_STATISTICS_PREFIX   "+MPING: \"statistics\""
#define ML307R_MPING_MAX_COUNT           100
#define ML307R_MPING_CMD_OVERHEAD_MS     5000U
#define ML307R_URC_CPIN                  "+CPIN:"
#define ML307R_URC_CEREG                 "+CEREG:"
#define ML307R_URC_MIPCALL               "+MIPCALL:"
#define ML307R_URC_MQTTURC               "+MQTTURC:"
#define ML307R_MIPURC_RTCP_PREFIX        "+MIPURC: \"rtcp\""
#define ML307R_MIPURC_DISCONN_PREFIX     "+MIPURC: \"disconn\""

_Static_assert(ML307R_MAX_RESPONSE_LINES >= ML307R_MPING_MAX_COUNT + 1,
               "ML307R MPING response storage must hold replies plus final status");

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief ML307R 命令上下文
 * @details ML307R command context
 */
typedef struct {
    char *lines[ML307R_MAX_RESPONSE_LINES];
    at_response_t response;
} ml307r_cmd_ctx_t;

/**
 * @brief ML307R 调制解调器实例
 * @details ML307R modem instance
 */
typedef struct {
    modem_handle_t base;
    modem_ml307r_config_t config;
    at_urc_handler_t cpin_handler;
    at_urc_handler_t creg_handler;
    at_urc_handler_t cereg_handler;
    at_urc_handler_t cgreg_handler;
    at_urc_handler_t mipcall_handler;
    at_urc_handler_t mqtturc_handler;
    at_urc_handler_t tcp_readable_handler;
    at_urc_handler_t tcp_disconn_handler;
    modem_info_t cached_info;
    modem_sim_status_t last_sim_status;
    modem_reg_status_t last_reg_status;
    modem_signal_t last_signal;
    modem_pdp_context_t pdp[ML307R_MAX_PDP_CONTEXTS];
    modem_mqtt_config_t mqtt_config;
    bool urc_registered;
    bool initialized;
    bool mqtt_configured;
    bool mqtt_session_connected;
    bool mqtt_data_enabled;
} modem_ml307r_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
/**
 * @brief 销毁 ML307R 子类资源
 * @details Clear MQTT state, unregister URCs and mark the modem uninitialized;
 *          no AT command is sent
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: URC 注销错误
 */
static esp_err_t ml307r_destroy(modem_handle_t *me);
/**
 * @brief 启动 ML307R 调制解调器
 * @details Hardware-reset the module, wait for AT-ready, run the basic init
 *          commands, register URCs and transition to the ready state
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 启动失败（状态切换、复位、初始化或 URC 注册错误）
 */
static esp_err_t ml307r_start(modem_handle_t *me);
/**
 * @brief 停止 ML307R 调制解调器
 * @details Stop ML307R modem and power it off
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 停止失败
 */
static esp_err_t ml307r_stop(modem_handle_t *me);
/**
 * @brief 复位 ML307R 调制解调器
 * @details Hardware-reset the module, wait for AT-ready, run the basic init
 *          commands, register URCs and transition to the ready state
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 复位失败（状态切换、复位、初始化或 URC 注册错误）
 */
static esp_err_t ml307r_reset(modem_handle_t *me);
/**
 * @brief 获取 ML307R 调制解调器信息
 * @details Query IMEI/IMSI/ICCID/model/firmware via AT+CGSN, AT+CIMI, AT+MCCID,
 *          AT+CGMM and AT+CGMR; cache and return the result
 * @param[in] me 调制解调器句柄
 * @param[out] info 调制解调器信息
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 必需字段缺失或无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_info(modem_handle_t *me, modem_info_t *info);
/**
 * @brief 获取 ML307R SIM 卡状态
 * @details Query SIM status via AT+CPIN? and parse the +CPIN line
 * @param[in] me 调制解调器句柄
 * @param[out] status SIM 卡状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_sim_status(modem_handle_t *me, modem_sim_status_t *status);
/**
 * @brief 获取 ML307R 信号质量
 * @details Query signal quality via AT+CSQ; convert RSSI to dBm and validate BER
 * @param[in] me 调制解调器句柄
 * @param[out] signal 信号质量
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_signal(modem_handle_t *me, modem_signal_t *signal);
/**
 * @brief 获取 ML307R 网络注册状态
 * @details Query EPS/LTE registration via AT+CEREG? and update the modem state
 * @param[in] me 调制解调器句柄
 * @param[out] status 网络注册状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_registration(modem_handle_t *me, modem_reg_status_t *status);
/**
 * @brief 获取 ML307R 分组域附着状态
 * @details Query packet-domain attach status via AT+CGATT (delegated to
 *          query_cgatt)
 * @param[in] me 调制解调器句柄
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_packet_attach_status(modem_handle_t *me, bool *attached);
/**
 * @brief 设置 ML307R APN
 * @details Set APN via AT+CGDCONT=<cid>,"IPV4V6","<apn>"; only cid 1 is supported
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID（仅支持 1）
 * @param[in] apn APN 字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NOT_SUPPORTED: cid 不受支持
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_set_apn(modem_handle_t *me, uint8_t cid, const char *apn);
/**
 * @brief 激活 ML307R PDP 上下文
 * @details Activate PDP via AT+MIPCALL=1,<cid>; wait for the +MIPCALL URC or
 *          poll AT+MIPCALL? until active; only cid 1 is supported
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID（仅支持 1）
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NOT_SUPPORTED: cid 不受支持
 *         - ESP_ERR_TIMEOUT: 激活超时
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_activate_pdp(modem_handle_t *me, uint8_t cid);
/**
 * @brief 去激活 ML307R PDP 上下文
 * @details Deactivate PDP via AT+MIPCALL=0,<cid> and post a PDP_DEACTIVATED
 *          event; only cid 1 is supported
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID（仅支持 1）
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NOT_SUPPORTED: cid 不受支持
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_deactivate_pdp(modem_handle_t *me, uint8_t cid);
/**
 * @brief 获取 ML307R PDP 上下文
 * @details Refresh the cached PDP context via AT+MIPCALL? for the given cid;
 *          only cid 1 is supported
 * @param[in] me 调制解调器句柄
 * @param[in] cid PDP 上下文 ID（仅支持 1）
 * @param[out] pdp PDP 上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NOT_SUPPORTED: cid 不受支持
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_get_pdp_context(modem_handle_t *me, uint8_t cid,
                                          modem_pdp_context_t *pdp);
/**
 * @brief 打开 ML307R TCP Socket
 * @details Open ML307R TCP socket
 * @param[in] me 调制解调器句柄
 * @param[in] open Socket 打开参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_socket_open(modem_handle_t *me,
                                    const modem_socket_open_t *open);
/**
 * @brief 发送 ML307R TCP Socket 数据
 * @details Send ML307R TCP socket data
 * @param[in] me 调制解调器句柄
 * @param[in] send Socket 发送参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_socket_send(modem_handle_t *me,
                                    const modem_socket_send_t *send);
/**
 * @brief 接收 ML307R TCP Socket 数据
 * @details Receive ML307R TCP socket data
 * @param[in] me 调制解调器句柄
 * @param[in] recv Socket 接收参数
 * @param[out] result Socket 接收结果
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_socket_recv(modem_handle_t *me,
                                    const modem_socket_recv_t *recv,
                                    modem_socket_recv_result_t *result);
/**
 * @brief 关闭 ML307R TCP Socket
 * @details Close ML307R TCP socket
 * @param[in] me 调制解调器句柄
 * @param[in] close Socket 关闭参数
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_socket_close(modem_handle_t *me,
                                     const modem_socket_close_t *close);
/**
 * @brief 准备 ML307R TCP Socket 模式
 * @details Prepare ML307R TCP socket mode
 * @param[in] self ML307R 调制解调器实例
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_socket_prepare(modem_ml307r_t *self);
/**
 * @brief 投递 TCP 可读事件
 * @details Post TCP readable event
 * @param[in] self ML307R 调制解调器实例
 * @param[in] recv_len 本次可读长度
 * @param[in] total_len 模块缓存总长度
 */
static void ml307r_post_tcp_readable(modem_ml307r_t *self, size_t recv_len,
                                     size_t total_len);
/**
 * @brief 投递 TCP 关闭事件
 * @details Post TCP closed event
 * @param[in] self ML307R 调制解调器实例
 * @param[in] reason 关闭原因
 * @param[in] modem_error_code 模块错误码
 */
static void ml307r_post_tcp_closed(modem_ml307r_t *self, int reason,
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
 * @brief 配置 ML307R MQTT
 * @details Configure MQTT via AT+MQTTCFG for version/cid/encoding/keepalive/
 *          clean/cached; refuses if the MQTT session is already connected
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT 配置
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 已连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_configure(modem_handle_t *me,
                                        const modem_mqtt_config_t *config);
/**
 * @brief 建立 ML307R MQTT TCP 通道
 * @details ML307R couples TCP and session into a single AT+MQTTCONN, so this op
 *          only verifies that MQTT has been configured
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: MQTT 未配置
 */
static esp_err_t ml307r_mqtt_tcp_connect(modem_handle_t *me);
/**
 * @brief 连接 ML307R MQTT 会话
 * @details Connect the MQTT session via AT+MQTTCONN and wait for the
 *          +MQTTURC: "conn" indication
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未配置或会话已连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_connect(modem_handle_t *me);
/**
 * @brief 断开 ML307R MQTT 会话
 * @details Disconnect the MQTT session via AT+MQTTDISC=0
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 会话未连接
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_disconnect(modem_handle_t *me);
/**
 * @brief 断开 ML307R MQTT TCP 通道
 * @details ML307R couples TCP and session, so this op just clears the local
 *          MQTT flags without sending any AT command
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
static esp_err_t ml307r_mqtt_tcp_disconnect(modem_handle_t *me);
/**
 * @brief 订阅 ML307R MQTT 主题
 * @details Subscribe to an MQTT topic via AT+MQTTSUB=0,"<topic>",<qos>
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 会话未连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_subscribe(modem_handle_t *me,
                                        const modem_mqtt_topic_t *topic);
/**
 * @brief 取消订阅 ML307R MQTT 主题
 * @details Unsubscribe from an MQTT topic via AT+MQTTUNSUB=0,"<topic>"
 * @param[in] me 调制解调器句柄
 * @param[in] topic MQTT 主题
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 会话未连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_unsubscribe(modem_handle_t *me,
                                          const modem_mqtt_topic_t *topic);
/**
 * @brief 发布 ML307R MQTT 消息
 * @details Publish an MQTT message via AT+MQTTPUB=0,"<topic>",<qos>,<retain>,0,
 *          <len>,"<hex>" with a hex-encoded binary payload; the dup flag is
 *          hard-wired to 0 because lwlte only sends new messages
 * @param[in] me 调制解调器句柄
 * @param[in] publish MQTT 发布参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 会话未连接
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - 其他: AT 命令错误
 */
static esp_err_t ml307r_mqtt_publish(modem_handle_t *me,
                                      const modem_mqtt_publish_t *publish);
/**
 * @brief 查询 MQTT 连接状态
 * @details Query MQTT connection state via AT+MQTTSTATE
 * @param[in] me 调制解调器句柄
 * @param[out] status MQTT 状态枚举
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: AT 命令失败
 *         - ESP_ERR_INVALID_RESPONSE: 响应解析失败
 */
static esp_err_t ml307r_mqtt_get_status(modem_handle_t *me,
                                         modem_mqtt_status_t *status);
/**
 * @brief 映射 MQTT 状态值
 * @details Map integer MQTT state (1/2/3) to enum; others map to OFFLINE
 * @param[in] state AT 状态值
 * @return MQTT 状态枚举
 */
static modem_mqtt_status_t map_mqtt_status(int state);
/**
 * @brief ML307R Ping 探测
 * @details Send AT+MPING="<host>",<timeout_s>,<count>,<pktlen>,1; parse each
 *          +MPING reply line and the +MPING "statistics" line, then validate
 *          the summary against the parsed replies
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
static esp_err_t ml307r_ping(modem_handle_t *me,
                             const modem_ping_request_t *request,
                             modem_ping_reply_t *replies,
                             size_t max_replies,
                             modem_ping_summary_t *summary);
/**
 * @brief 转换为 ML307R 实例
 * @details Convert to ML307R instance
 * @param[in] me 调制解调器句柄
 * @return ML307R 调制解调器实例
 */
static modem_ml307r_t *to_ml307r(modem_handle_t *me);

/**
 * @brief 初始化命令上下文
 * @details Initialize command context
 * @param[out] ctx 命令上下文
 */
static void init_cmd_ctx(ml307r_cmd_ctx_t *ctx);

/**
 * @brief 发送 AT 命令
 * @details Send AT command
 * @param[in] self ML307R 调制解调器实例
 * @param[in] cmd AT 命令
 * @param[out] ctx 命令上下文
 * @param[in] timeout_ms 超时时间，0 使用默认值
 * @return
 *         - ESP_OK: 命令流程完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎错误
 */
static esp_err_t send_cmd(modem_ml307r_t *self, const char *cmd,
                          ml307r_cmd_ctx_t *ctx, uint32_t timeout_ms);

/**
 * @brief 使用选项发送 AT 命令
 * @details Send AT command with options
 * @param[in] self ML307R 调制解调器实例
 * @param[in] cmd AT 命令
 * @param[out] ctx 命令上下文
 * @param[in] options AT 命令选项
 * @return
 *         - ESP_OK: 命令流程完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎错误
 */
static esp_err_t send_cmd_with_options(modem_ml307r_t *self, const char *cmd,
                                       ml307r_cmd_ctx_t *ctx,
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
 * @brief 检查响应是否包含文本
 * @details Check whether response contains text
 * @param[in] response AT 响应
 * @param[in] needle 查找文本
 * @return true: 包含； false: 不包含
 */
static bool response_contains(const at_response_t *response, const char *needle);

/**
 * @brief Parse +MIPOPEN response
 * @details Parse +MIPOPEN: <conn_id>,<result>
 * @param[in] response AT response
 * @param[out] conn_id Parsed connection ID
 * @param[out] result Parsed open result
 * @return ESP_OK on success, otherwise an error code
 */
static esp_err_t parse_mipopen_response(const at_response_t *response,
                                        uint8_t *conn_id,
                                        int *result);

/**
 * @brief Map ML307R MIPOPEN result to ESP error
 * @details Map module TCP/IP result codes to ESP errors
 * @param[in] result ML307R MIPOPEN result code
 * @return ESP_OK when result is 0, otherwise mapped error
 */
static esp_err_t ml307r_map_mipopen_result(int result);

/**
 * @brief Parse +MIPSEND response
 * @details Parse +MIPSEND: <conn_id>,<sent_len>
 * @param[in] response AT response
 * @param[out] conn_id Parsed connection ID
 * @param[out] sent_len Parsed send length
 * @return ESP_OK on success, otherwise an error code
 */
static esp_err_t parse_mipsend_response(const at_response_t *response,
                                        uint8_t *conn_id,
                                        size_t *sent_len);

/**
 * @brief 计算 AT 行缓冲安全的 MIPRD 读取长度
 * @details Calculate AT line-buffer-safe MIPRD read length
 * @param[in] rx_line_buf_size AT 单行缓冲大小
 * @param[out] out_read_len 安全读取长度
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t ml307r_tcp_read_len_for_line_buf(int rx_line_buf_size,
                                                  size_t *out_read_len);

/**
 * @brief Dispatch TCP URCs collected inside an active command response
 * @details Dispatch TCP URCs collected inside an active command response
 * @param[in] self ML307R modem instance
 * @param[in] response AT response
 */
static void dispatch_tcp_urcs_from_response(modem_ml307r_t *self,
                                            const at_response_t *response);

/**
 * @brief 查找 +MIPRD 十六进制数据行
 * @details Find +MIPRD hex payload line
 * @param[in] response AT 响应
 * @param[out] out_remaining_len 剩余长度输出
 * @return 十六进制负载字符串或 NULL
 */
static const char *find_miprd_hex_line(const at_response_t *response,
                                       size_t *out_remaining_len);

/**
 * @brief 解析十六进制半字节
 * @details Parse hex nibble
 * @param[in] c 十六进制字符
 * @return 0-15 表示成功，-1 表示无效
 */
static int hex_nibble(char c);

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
 * @brief 检查字符串是否仅包含十进制数字
 * @details Check whether string contains only decimal digits
 * @param[in] value 待检查字符串
 * @return true: 仅包含数字； false: NULL、空串或包含非数字字符
 */
static bool decimal_digits_only(const char *value);

/**
 * @brief 校验身份标识值是否合法
 * @details Validate IMEI/IMSI/ICCID against length and digit rules;
 *          returns false when value starts with '+' (likely an error echo)
 * @param[in] cmd 身份查询 AT 命令 (AT+CGSN/AT+CIMI/AT+MCCID)
 * @param[in] value 响应值
 * @return true: 合法； false: 非法
 */
static bool identity_value_valid(const char *cmd, const char *value);
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
 * @param[in] self ML307R 调制解调器实例
 * @param[in] cid PDP 上下文 ID
 * @return PDP 上下文缓存或 NULL
 */
static modem_pdp_context_t *pdp_by_cid(modem_ml307r_t *self, uint8_t cid);

/**
 * @brief 非阻塞设置调制解调器状态
 * @details Set modem state without blocking
 * @param[in] self ML307R 调制解调器实例
 * @param[in] state 调制解调器状态
 */
static void set_state_nonblocking(modem_ml307r_t *self, modem_state_t state);

/**
 * @brief 非阻塞投递调制解调器事件
 * @details Post event to event queue with zero-wait lock and queue send;
 *          drops the event when lock busy, queue full, or task unavailable
 * @param[in] self ML307R 调制解调器实例
 * @param[in] event 待投递事件
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_TIMEOUT: 锁忙或队列已满
 *         - ESP_ERR_INVALID_STATE: 事件任务不可用
 */
static esp_err_t post_event_nonblocking(modem_ml307r_t *self,
                                        const modem_event_t *event);

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
static esp_err_t parse_int_after_prefix(const char *line, const char *prefix,
                                        int *out);

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
 * @param[in] self ML307R 调制解调器实例
 * @param[in] status SIM 状态
 */
static void cache_sim_status(modem_ml307r_t *self, modem_sim_status_t status);

/**
 * @brief 查询分组域附着状态
 * @details Query packet domain attach status
 * @param[in] self ML307R 调制解调器实例
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t query_cgatt(modem_ml307r_t *self, bool *attached);
/**
 * @brief 检查 AT 参数是否安全
 * @details Check whether AT argument is safe
 * @param[in] value AT 参数字符串
 * @return true: 安全； false: 不安全
 */
static bool at_arg_safe(const char *value);

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
 * @param[in] self ML307R 调制解调器实例
 * @param[in] initialized 初始化状态
 */
static void set_initialized(modem_ml307r_t *self, bool initialized);

/**
 * @brief 设置 MQTT 数据使能标志
 * @details Set MQTT data-enabled flag (lock-protected)
 * @param[in] self ML307R 调制解调器实例
 * @param[in] enabled 使能状态
 */
static void set_mqtt_data_enabled(modem_ml307r_t *self, bool enabled);

/**
 * @brief 查询 MQTT 数据使能标志
 * @details Get MQTT data-enabled flag (lock-protected)
 * @param[in] self ML307R 调制解调器实例
 * @return true: 已使能； false: 未使能
 */
static bool mqtt_data_is_enabled(modem_ml307r_t *self);

/**
 * @brief 克隆 MQTT 字符串
 * @details Malloc'd copy of a string; NULL input yields NULL output
 * @param[in] value 源字符串，可为 NULL
 * @return 克隆字符串或 NULL
 */
static char *clone_mqtt_string(const char *value);

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
 * @brief 释放 MQTT 配置
 * @details Free all strings inside config and zero the struct
 * @param[in,out] config MQTT 配置
 */
static void free_mqtt_config(modem_mqtt_config_t *config);

/**
 * @brief 清空 MQTT 状态
 * @details Reset MQTT flags to false and free stored config
 * @param[in] self ML307R 调制解调器实例
 */
static void clear_mqtt_state(modem_ml307r_t *self);
/**
 * @brief 转义 AT 字符串
 * @details Escape \", \\, \\r, \\n for AT command string arguments;
 *          returns a malloc'd string
 * @param[in] value 待转义字符串
 * @return 转义后的字符串或 NULL
 */
static char *escape_at_string(const char *value);

/**
 * @brief 十六进制编码负载
 * @details Hex-encode binary payload as uppercase ASCII string for AT commands;
 *          returns a malloc'd string
 * @param[in] payload 二进制负载，可为 NULL（当 payload_len 为 0）
 * @param[in] payload_len 负载长度
 * @return 十六进制字符串或 NULL
 */
static char *hex_encode_payload(const uint8_t *payload, size_t payload_len);

/**
 * @brief 投递 MQTT 数据事件
 * @details Post MODEM_EVENT_PROTOCOL_DATA with MQTT topic/payload;
 *          ownership of topic/payload transfers to the event queue on success
 * @param[in] self ML307R 调制解调器实例
 * @param[in] topic 主题缓冲区
 * @param[in] topic_len 主题长度
 * @param[in] payload 负载缓冲区
 * @param[in] payload_len 负载长度
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 事件投递错误
 */
static esp_err_t post_mqtt_data_event(modem_ml307r_t *self, char *topic,
                                       size_t topic_len, uint8_t *payload,
                                       size_t payload_len);

/**
 * @brief 匹配 MQTT URC 事件名
 * @details Check +MQTTURC:"<event_name>" prefix and match the quoted event name
 * @param[in] line URC 完整行
 * @param[in] event_name 期望的事件名
 * @param[out] event_end 闭合引号后的位置，可为 NULL
 * @return true: 匹配成功； false: 不匹配或参数无效
 */
static bool mqtt_event_matches(const char *line, const char *event_name,
                               const char **event_end);

/**
 * @brief 解析 MQTT 无符号整数字段
 * @details Parse unsigned integer at cursor, advance cursor past digits,
 *          enforce max_value upper bound
 * @param[in,out] cursor 解析游标
 * @param[in] max_value 允许的最大值
 * @param[out] out_value 解析结果
 * @return true: 成功； false: 失败
 */
static bool parse_mqtt_uint_field(const char **cursor, unsigned long max_value,
                                  unsigned long *out_value);

/**
 * @brief 解析 MQTT 逗号分隔符
 * @details Skip whitespace, consume a comma separator, advance cursor
 * @param[in,out] cursor 解析游标
 * @return true: 成功； false: 失败
 */
static bool parse_mqtt_comma(const char **cursor);
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
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_TIMEOUT: 超时
 */
static esp_err_t wait_at_ready(modem_ml307r_t *self);

/**
 * @brief 执行基础初始化命令
 * @details Run basic initialization commands
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 命令错误
 */
static esp_err_t run_basic_init_cmds(modem_ml307r_t *self);

/**
 * @brief 完成调制解调器 ready 流程
 * @details Finish modem ready flow
 * @param[in] me 调制解调器句柄
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: 状态设置错误
 */
static esp_err_t finish_modem_ready(modem_handle_t *me, modem_ml307r_t *self);

/**
 * @brief 硬件复位模块(通过 EN 引脚)
 * @details Hardware reset module via EN pin
 * @details 拉低 EN 引脚，等待 reset_pulse_ms，拉高 EN 引脚
 * @details Pull EN low, wait reset_pulse_ms, pull EN high
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: GPIO 错误
 */
static esp_err_t hardware_reset(modem_ml307r_t *self);

/**
 * @brief 硬件断电模块(通过 EN 引脚)
 * @details Hardware power off module via EN pin
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: GPIO 或 AT 独占错误
 */
static esp_err_t hardware_power_off(modem_ml307r_t *self);

/**
 * @brief 注册 ML307R URC 处理器
 * @details Register ML307R URC handlers
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注册错误
 */
static esp_err_t register_urcs(modem_ml307r_t *self);

/**
 * @brief 注销 ML307R URC 处理器
 * @details Unregister ML307R URC handlers (wrapper around ml307r_unregister_urcs)
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注销错误
 */
static esp_err_t unregister_urcs(modem_ml307r_t *self);

/**
 * @brief 注销 ML307R URC 处理器
 * @details Unregister ML307R URC handlers
 * @param[in] self ML307R 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: AT 引擎注销错误
 */
static esp_err_t ml307r_unregister_urcs(modem_ml307r_t *self);

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
 * @brief 处理 MIPCALL URC
 * @details Handle MIPCALL URC: parse PDP context, update cache,
 *          post PDP activated/deactivated event
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void mipcall_urc_handler(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief 处理 MQTTURC URC
 * @details Handle MQTTURC URC: forward to MQTT URC dispatcher
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void mqtturc_urc_handler(const char *prefix, const char *line, void *user_ctx);

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
 * @brief 处理 TCP 断连 URC
 * @details Handle TCP disconnection URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void tcp_disconn_urc_handler(const char *prefix, const char *line,
                                    void *user_ctx);

/**
 * @brief 解析 TCP 可读 URC
 * @details Parse +MIPURC: "rtcp",0,<recv_len>,<total_len>
 * @param[in] line URC 完整行
 * @param[out] recv_len 本次可读长度
 * @param[out] total_len 模块缓存总长度
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t parse_tcp_rtcp_urc(const char *line, size_t *recv_len,
                                    size_t *total_len);

/**
 * @brief 解析 TCP 断连 URC
 * @details Parse +MIPURC: "disconn",0,<connect_state>
 * @param[in] line URC 完整行
 * @param[out] connect_state 连接状态
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t parse_tcp_disconn_urc(const char *line, int *connect_state);

/**
 * @brief 查询 ML307R PDP 上下文
 * @details Send AT+MIPCALL? and parse the +MIPCALL line matching the given cid,
 *          then refresh the cached PDP context. Only ML307R_PRIMARY_CID is
 *          supported.
 * @param[in] self ML307R 调制解调器实例
 * @param[in] cid PDP 上下文 ID（仅支持主 CID）
 * @param[out] out_pdp 解析得到的 PDP 上下文，可为 NULL
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: self 为 NULL 或 cid 无效
 *         - ESP_ERR_NOT_SUPPORTED: cid 不是主 CID
 *         - ESP_ERR_NOT_FOUND: 未找到匹配的 +MIPCALL 行
 *         - ESP_ERR_INVALID_RESPONSE: 响应解析失败
 *         - 其他: 底层 AT 命令失败
 */
static esp_err_t query_mipcall(modem_ml307r_t *self, uint8_t cid,
                               modem_pdp_context_t *out_pdp);

/**
 * @brief 从 +MIPCALL 行解析 PDP 上下文 ID
 * @details Parse the leading cid field of a +MIPCALL URC/response line.
 * @param[in] line +MIPCALL 响应行
 * @param[out] cid 解析得到的 PDP 上下文 ID
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: cid 为 NULL
 *         - ESP_ERR_INVALID_RESPONSE: 行格式无效
 */
static esp_err_t parse_mipcall_cid(const char *line, uint8_t *cid);

/**
 * @brief 解析 +MIPCALL 行为 PDP 上下文
 * @details Parse a +MIPCALL line into a PDP context, including cid, active
 *          state, PDP type (defaulted to IPV4V6) and IP address. Active state
 *          0 carries no address field; state 1 requires a valid IP suffix.
 * @param[in] line +MIPCALL 响应行
 * @param[out] pdp 解析结果
 * @return true: 成功； false: 行格式无效或参数为空
 */
static bool parse_mipcall_line(const char *line, modem_pdp_context_t *pdp);

/**
 * @brief 解析 MQTT 连接 URC（可配置阻塞模式）
 * @details Parse +MQTTURC: "conn",<connect_id>,<state> URC, update the cached
 *          MQTT session state and post a MODEM_EVENT_PROTOCOL_CLOSED event when
 *          the session transitions from connected to disconnected. When
 *          nonblocking is true the instance lock is taken with zero timeout.
 * @param[in] self ML307R 调制解调器实例
 * @param[in] line +MQTTURC 行
 * @param[in] nonblocking true 时以非阻塞方式获取实例锁
 * @return
 *         - ESP_OK: 收到连接成功 URC
 *         - ESP_FAIL: 收到断连/重连 URC（事件已投递）
 *         - ESP_ERR_INVALID_ARG: 参数为 NULL
 *         - ESP_ERR_INVALID_RESPONSE: 行格式不匹配
 *         - ESP_ERR_TIMEOUT: 非阻塞模式下实例锁繁忙
 */
static esp_err_t parse_mqtt_conn_urc_ex(modem_ml307r_t *self, const char *line,
                                        bool nonblocking);

/**
 * @brief 解析 MQTT 连接 URC（非阻塞）
 * @details Thin wrapper around parse_mqtt_conn_urc_ex with nonblocking=true,
 *          suitable for use inside URC dispatcher callbacks.
 * @param[in] self ML307R 调制解调器实例
 * @param[in] line +MQTTURC 行
 * @return 同 parse_mqtt_conn_urc_ex
 */
static esp_err_t parse_mqtt_conn_urc(modem_ml307r_t *self, const char *line);

/**
 * @brief 解析 MQTT 推送 URC
 * @details Parse +MQTTURC: "publish",<connect_id>,<mid>,<topic>,<total_len>,
 *          <payload_len>,<payload> URC. Allocates new buffers for topic and
 *          payload via malloc; the caller MUST free() them on success.
 * @param[in] line +MQTTURC 行
 * @param[out] topic 动态分配的 topic 缓冲（需调用方 free）
 * @param[out] topic_len topic 长度
 * @param[out] payload 动态分配的 payload 缓冲（需调用方 free）
 * @param[out] payload_len payload 长度
 * @return true: 成功； false: 行格式无效、参数为空或内存分配失败
 */
static bool parse_mqtt_publish_urc(const char *line, char **topic,
                                   size_t *topic_len, uint8_t **payload,
                                   size_t *payload_len);

/**
 * @brief 处理 MQTT URC
 * @details Dispatch +MQTTURC lines: "conn" updates session state, "publish"
 *          posts a data event when MQTT data is enabled, "pubnmi" is ignored,
 *          and acknowledgement/diagnostic events (suback/unsuback/puback/
 *          pubrec/pubcomp/timeout/drop/pingresp) are logged at debug level.
 * @param[in] self ML307R 调制解调器实例
 * @param[in] line +MQTTURC 行
 */
static void handle_mqtturc(modem_ml307r_t *self, const char *line);

/**
 * @brief 从游标解析无符号整数
 * @details Skip leading whitespace at *cursor, parse a base-10 unsigned integer
 *          capped by max_value, then advance *cursor past the digits.
 * @param[in,out] cursor 当前解析位置，成功时向后移动
 * @param[in] max_value 允许的最大值
 * @param[out] out_value 解析结果
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数为 NULL
 *         - ESP_ERR_INVALID_RESPONSE: 行格式无效或超出范围
 */
static esp_err_t parse_mping_uint(const char **cursor,
                                  uint32_t max_value,
                                  uint32_t *out_value);

/**
 * @brief 汇总 Ping 统计信息
 * @details Iterate over received ping replies (capped by request->count) and
 *          compute sent, received, lost counts and min/max/avg RTT in ms.
 * @param[in] request Ping 请求参数
 * @param[in] replies 单次应答数组
 * @param[in] reply_count replies 数组中的元素数量
 * @param[out] summary 计算得到的统计信息
 */
static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary);

/**
 * @brief 解析单条 +MPING 应答行
 * @details Parse +MPING: <result>,"<ip>",<packet_len>,<time_ms>,<ttl> reply
 *          lines (skipping the "statistics" aggregate line). On result != 0
 *          the reply is marked unsuccessful.
 * @param[in] line +MPING 应答行
 * @param[out] reply 解析得到的应答
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数为 NULL
 *         - ESP_ERR_INVALID_RESPONSE: 行格式无效
 */
static esp_err_t parse_mping_reply_line(const char *line,
                                        modem_ping_reply_t *reply);

/**
 * @brief 解析 +MPING 统计行
 * @details Parse +MPING: "statistics",<sent>,<lost>,<min>,<max>,<avg> aggregate
 *          line, with cross-field sanity checks (lost <= sent, min <= avg <=
 *          max when received > 0).
 * @param[in] line +MPING 统计行
 * @param[out] summary 解析得到的统计信息
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数为 NULL
 *         - ESP_ERR_INVALID_RESPONSE: 行格式无效或字段不一致
 */
static esp_err_t parse_mping_statistics_line(const char *line,
                                             modem_ping_summary_t *summary);

/**
 * @brief 计算 MPING 命令的超时时间
 * @details Derive the AT+MPING command timeout in milliseconds from the request
 *          parameters (count, per-packet timeout, total_timeout_ms). Falls back
 *          to ML307R_DEFAULT_CMD_TIMEOUT_MS when request is NULL, and prefers
 *          total_timeout_ms when it is larger than the derived value.
 * @param[in] request Ping 请求参数，可为 NULL
 * @return 命令超时时间（毫秒）
 */
static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request);

/**********************
 *  STATIC VARIABLES
 **********************/
static const modem_ops_t s_ml307r_ops = {
    .destroy = ml307r_destroy,
    .start = ml307r_start,
    .stop = ml307r_stop,
    .reset = ml307r_reset,
    .get_info = ml307r_get_info,
    .get_sim_status = ml307r_get_sim_status,
    .get_signal = ml307r_get_signal,
    .get_registration = ml307r_get_registration,
    .get_packet_attach_status = ml307r_get_packet_attach_status,
    .set_apn = ml307r_set_apn,
    .activate_pdp = ml307r_activate_pdp,
    .deactivate_pdp = ml307r_deactivate_pdp,
    .get_pdp_context = ml307r_get_pdp_context,
    .socket_open = ml307r_socket_open,
    .socket_send = ml307r_socket_send,
    .socket_recv = ml307r_socket_recv,
    .socket_close = ml307r_socket_close,
    .mqtt_configure = ml307r_mqtt_configure,
    .mqtt_tcp_connect = ml307r_mqtt_tcp_connect,
    .mqtt_connect = ml307r_mqtt_connect,
    .mqtt_disconnect = ml307r_mqtt_disconnect,
    .mqtt_tcp_disconnect = ml307r_mqtt_tcp_disconnect,
    .mqtt_subscribe = ml307r_mqtt_subscribe,
    .mqtt_unsubscribe = ml307r_mqtt_unsubscribe,
    .mqtt_publish = ml307r_mqtt_publish,
    .mqtt_get_status = ml307r_mqtt_get_status,
    .ping = ml307r_ping,
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
modem_handle_t *modem_ml307r_create(at_engine_handle_t *at,
                             const modem_ml307r_config_t *config)
{
    if (!at || !config) {
        ESP_LOGE(TAG, "NULL argument");
        return NULL;
    }

    modem_ml307r_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "calloc ml307r modem failed");
        return NULL;
    }

    self->config = *config;
    if (self->config.base.timing.default_cmd_timeout_ms == 0) {
        self->config.base.timing.default_cmd_timeout_ms = ML307R_DEFAULT_CMD_TIMEOUT_MS;
    }
    if (self->config.base.timing.ready_timeout_ms == 0) {
        self->config.base.timing.ready_timeout_ms = ML307R_DEFAULT_READY_TIMEOUT_MS;
    }

    self->last_sim_status = MODEM_SIM_UNKNOWN;
    self->last_reg_status = MODEM_REG_UNKNOWN;
    self->last_signal.rssi = 99;
    self->last_signal.ber = 99;
    self->pdp[0].cid = ML307R_PRIMARY_CID;
    strlcpy(self->pdp[0].pdp_type, "IPV4V6", sizeof(self->pdp[0].pdp_type));

    esp_err_t ret = modem_base_init(&self->base, "ml307r", at, &s_ml307r_ops,
                                    self->config.base.event.event_queue_size,
                                    self->config.base.event.event_task_stack,
                                    self->config.base.event.event_task_priority);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem base init failed: %s", esp_err_to_name(ret));
        free(self);
        return NULL;
    }

    return &self->base;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static modem_ml307r_t *to_ml307r(modem_handle_t *me)
{
    return MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
}

static void init_cmd_ctx(ml307r_cmd_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->response.lines = ctx->lines;
    ctx->response.max_lines = ML307R_MAX_RESPONSE_LINES;
}

static esp_err_t send_cmd(modem_ml307r_t *self, const char *cmd,
                          ml307r_cmd_ctx_t *ctx, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    uint32_t wait_ms = timeout_ms ? timeout_ms : self->config.base.timing.default_cmd_timeout_ms;
    if (wait_ms == 0) {
        wait_ms = ML307R_DEFAULT_CMD_TIMEOUT_MS;
    }

    const at_cmd_options_t options = {
        .timeout_ms = wait_ms,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };

    return send_cmd_with_options(self, cmd, ctx, &options);
}

static esp_err_t send_cmd_with_options(modem_ml307r_t *self, const char *cmd,
                                       ml307r_cmd_ctx_t *ctx,
                                       const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(self && self->base.at && cmd && ctx && options,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    init_cmd_ctx(ctx);
    esp_err_t ret = at_engine_send_cmd_with_options(self->base.at, cmd,
                                                    &ctx->response, options);
    dispatch_tcp_urcs_from_response(self, &ctx->response);
    return ret;
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

static esp_err_t parse_mipsend_response(const at_response_t *response,
                                        uint8_t *conn_id,
                                        size_t *sent_len)
{
    ESP_RETURN_ON_FALSE(response && conn_id && sent_len,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *line = find_line_with_prefix(response, "+MIPSEND:");
    if (!line) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = skip_prefix_value(line, "+MIPSEND:");
    if (!cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned long parsed_conn_id = 0;
    unsigned long parsed_sent_len = 0;
    unsigned long size_max = (unsigned long)SIZE_MAX;
    if (!parse_mqtt_uint_field(&cursor, UINT8_MAX, &parsed_conn_id) ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, size_max, &parsed_sent_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *conn_id = (uint8_t)parsed_conn_id;
    *sent_len = (size_t)parsed_sent_len;
    return ESP_OK;
}

static esp_err_t parse_mipopen_response(const at_response_t *response,
                                        uint8_t *conn_id,
                                        int *result)
{
    ESP_RETURN_ON_FALSE(response && conn_id && result,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *line = find_line_with_prefix(response, "+MIPOPEN:");
    if (!line) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = skip_prefix_value(line, "+MIPOPEN:");
    if (!cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned long parsed_conn_id = 0;
    unsigned long parsed_result = 0;
    if (!parse_mqtt_uint_field(&cursor, UINT8_MAX, &parsed_conn_id) ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, INT_MAX, &parsed_result)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *conn_id = (uint8_t)parsed_conn_id;
    *result = (int)parsed_result;
    return ESP_OK;
}

static esp_err_t ml307r_map_mipopen_result(int result)
{
    switch (result) {
    case 0:
        return ESP_OK;
    case 558:
        return ESP_ERR_TIMEOUT;
    case 551:
    case 552:
    case 553:
    case 570:
        return ESP_ERR_INVALID_STATE;
    case 580:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

static esp_err_t ml307r_tcp_read_len_for_line_buf(int rx_line_buf_size,
                                                  size_t *out_read_len)
{
    ESP_RETURN_ON_FALSE(out_read_len, ESP_ERR_INVALID_ARG, TAG,
                        "out_read_len is NULL");
    ESP_RETURN_ON_FALSE(rx_line_buf_size > 0, ESP_ERR_INVALID_STATE, TAG,
                        "invalid AT line buffer size");

    if ((size_t)rx_line_buf_size <= ML307R_TCP_MIPRD_LINE_OVERHEAD + 2U) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t hex_chars = (size_t)rx_line_buf_size - ML307R_TCP_MIPRD_LINE_OVERHEAD - 1U;
    size_t line_cap = hex_chars / 2U;
    if (line_cap == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (line_cap > ML307R_TCP_MAX_HEX_READ_BYTES) {
        line_cap = ML307R_TCP_MAX_HEX_READ_BYTES;
    }
    *out_read_len = (size_t)line_cap;
    return ESP_OK;
}

static void dispatch_tcp_urcs_from_response(modem_ml307r_t *self,
                                            const at_response_t *response)
{
    if (!self || !response || !response->lines) {
        return;
    }

    int count = response->line_count;
    if (count > response->max_lines) {
        count = response->max_lines;
    }

    size_t rtcp_prefix_len = strlen(ML307R_MIPURC_RTCP_PREFIX);
    size_t disconn_prefix_len = strlen(ML307R_MIPURC_DISCONN_PREFIX);
    for (int i = 0; i < count; i++) {
        const char *line = response->lines[i];
        if (!line) {
            continue;
        }

        bool line_dispatched = false;
        if (strncmp(line, ML307R_MIPURC_RTCP_PREFIX, rtcp_prefix_len) == 0) {
            tcp_readable_urc_handler(ML307R_MIPURC_RTCP_PREFIX, line, self);
            line_dispatched = true;
        }
        if (!line_dispatched &&
            strncmp(line, ML307R_MIPURC_DISCONN_PREFIX, disconn_prefix_len) == 0) {
            tcp_disconn_urc_handler(ML307R_MIPURC_DISCONN_PREFIX, line, self);
        }
    }
}

static const char *find_miprd_hex_line(const at_response_t *response,
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
        const char *cursor = skip_prefix_value(line, "+MIPRD:");
        if (!cursor) {
            continue;
        }

        unsigned long conn_id = 0;
        unsigned long remaining_len = 0;
        unsigned long data_len = 0;
        unsigned long size_max = (unsigned long)SIZE_MAX;
        if (!parse_mqtt_uint_field(&cursor, UINT_MAX, &conn_id) ||
            conn_id != ML307R_TCP_CONN_ID ||
            !parse_mqtt_comma(&cursor) ||
            !parse_mqtt_uint_field(&cursor, size_max, &remaining_len) ||
            !parse_mqtt_comma(&cursor) ||
            !parse_mqtt_uint_field(&cursor, size_max, &data_len) ||
            !parse_mqtt_comma(&cursor)) {
            continue;
        }

        const char *hex_line = cursor;
        size_t hex_len = strlen(hex_line);
        if ((hex_len % 2U) != 0 || hex_len / 2U != (size_t)data_len) {
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

static bool decimal_digits_only(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }

    while (*value) {
        if (!isdigit((unsigned char)*value)) {
            return false;
        }
        value++;
    }

    return true;
}

static bool identity_value_valid(const char *cmd, const char *value)
{
    if (!cmd || !value || value[0] == '\0' || value[0] == '+') {
        return false;
    }

    size_t len = strlen(value);
    if (strcmp(cmd, "AT+CGSN") == 0) {
        return decimal_digits_only(value) && len >= 14 && len < MODEM_IMEI_MAX_LEN;
    }
    if (strcmp(cmd, "AT+CIMI") == 0) {
        return decimal_digits_only(value) && len >= 14 && len < MODEM_IMSI_MAX_LEN;
    }
    if (strcmp(cmd, "AT+MCCID") == 0) {
        return decimal_digits_only(value) && len >= 10 && len < MODEM_ICCID_MAX_LEN;
    }

    return true;
}

static bool cid_valid(uint8_t cid)
{
    return cid >= 1 && cid <= ML307R_MAX_PDP_CONTEXTS;
}

static modem_pdp_context_t *pdp_by_cid(modem_ml307r_t *self, uint8_t cid)
{
    if (!self || !cid_valid(cid)) {
        return NULL;
    }

    return &self->pdp[cid - 1];
}

static void set_state_nonblocking(modem_ml307r_t *self, modem_state_t state)
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

static esp_err_t post_event_nonblocking(modem_ml307r_t *self,
                                        const modem_event_t *event)
{
    if (!self || !event || !self->base.lock) {
        ESP_LOGW(TAG, "drop modem event, invalid state");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop modem event %d, lock busy", event->id);
        return ESP_ERR_TIMEOUT;
    }

    modem_handle_t *me = &self->base;
    if (me->destroying || me->state == MODEM_STATE_DESTROYING ||
        me->event_task_stop_requested || !me->event_task || !me->event_queue) {
        xSemaphoreGive(me->lock);
        ESP_LOGW(TAG, "drop modem event %d, event task unavailable", event->id);
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t send_ret = xQueueSend(me->event_queue, event, 0);
    xSemaphoreGive(me->lock);

    if (send_ret != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, drop event %d", event->id);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
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

static esp_err_t parse_int_after_prefix(const char *line, const char *prefix,
                                        int *out)
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

    const char *value = skip_prefix_value(line, ML307R_URC_CPIN);
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

static void cache_sim_status(modem_ml307r_t *self, modem_sim_status_t status)
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

static esp_err_t query_cgatt(modem_ml307r_t *self, bool *attached)
{
    ESP_RETURN_ON_FALSE(self && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    ml307r_cmd_ctx_t ctx;
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

static void set_initialized(modem_ml307r_t *self, bool initialized)
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

static void set_mqtt_data_enabled(modem_ml307r_t *self, bool enabled)
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

static bool mqtt_data_is_enabled(modem_ml307r_t *self)
{
    if (!self) {
        return false;
    }

    if (!self->base.lock) {
        return self->mqtt_data_enabled;
    }

    if (xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MQTT data enabled read skipped, lock busy");
        return false;
    }
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
        .clean_session = src->clean_session,
        .keepalive_s = src->keepalive_s ? src->keepalive_s : 120,
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

static void clear_mqtt_state(modem_ml307r_t *self)
{
    if (!self) {
        return;
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_configured = false;
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    free_mqtt_config(&self->mqtt_config);
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
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

static char *hex_encode_payload(const uint8_t *payload, size_t payload_len)
{
    static const char hex[] = "0123456789ABCDEF";

    if ((!payload && payload_len > 0) ||
        payload_len > ML307R_MQTT_MAX_PAYLOAD_LEN ||
        payload_len > (SIZE_MAX - 1U) / 2U) {
        return NULL;
    }

    char *text = malloc((payload_len * 2U) + 1U);
    if (!text) {
        return NULL;
    }

    for (size_t i = 0; i < payload_len; i++) {
        text[i * 2U] = hex[payload[i] >> 4];
        text[(i * 2U) + 1U] = hex[payload[i] & 0x0FU];
    }
    text[payload_len * 2U] = '\0';
    return text;
}

static esp_err_t post_mqtt_data_event(modem_ml307r_t *self, char *topic,
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

    return post_event_nonblocking(self, &event);
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
    vTaskDelay(timeout_ticks(ML307R_INIT_RETRY_DELAY_MS));
}

static esp_err_t wait_at_ready(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const uint32_t timeout_ms = self->config.base.timing.ready_timeout_ms;
    const uint32_t start_ms = now_ms();
    unsigned int attempt = 1;

    while (!elapsed_at_least(start_ms, timeout_ms)) {
        ml307r_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, "AT", &ctx,
                                 ML307R_AT_READY_PROBE_TIMEOUT_MS);
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
            delay_init_retry();
        }
    }

    ESP_LOGE(TAG, "AT ready probe timeout after %u ms",
             (unsigned int)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_basic_init_cmds(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 1; attempt <= ML307R_INIT_CMD_MAX_ATTEMPTS; attempt++) {
            ml307r_cmd_ctx_t ctx;
            ret = send_cmd(self, cmds[i], &ctx, 0);
            if (ret == ESP_OK) {
                ret = ensure_at_ok(&ctx.response, cmds[i]);
            }
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "%s failed (attempt %d/%d): %s", cmds[i], attempt,
                     ML307R_INIT_CMD_MAX_ATTEMPTS, esp_err_to_name(ret));
            if (attempt < ML307R_INIT_CMD_MAX_ATTEMPTS) {
                delay_init_retry();
            }
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed after %d attempts", cmds[i],
                            ML307R_INIT_CMD_MAX_ATTEMPTS);
    }

    return ESP_OK;
}

static esp_err_t finish_modem_ready(modem_handle_t *me, modem_ml307r_t *self)
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

static esp_err_t hardware_reset(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    ret = at_engine_flush_rx_exclusive(self->base.at);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "flush RX input before reset failed");

    if (self->config.base.hardware.en_pin == GPIO_NUM_NC) {
        ret = at_engine_flush_rx_exclusive(self->base.at);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "flush RX input without EN failed");
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

static esp_err_t hardware_power_off(modem_ml307r_t *self)
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

static esp_err_t register_urcs(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (self->urc_registered) {
        return ESP_OK;
    }

    const struct {
        const char *prefix;
        at_urc_handler_t *handler;
        at_urc_callback_t callback;
    } urcs[] = {
        { ML307R_URC_CPIN, &self->cpin_handler, cpin_urc_handler },
        { ML307R_URC_CEREG, &self->cereg_handler, reg_urc_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler, mipcall_urc_handler },
        { ML307R_URC_MQTTURC, &self->mqtturc_handler, mqtturc_urc_handler },
        { ML307R_MIPURC_RTCP_PREFIX, &self->tcp_readable_handler,
          tcp_readable_urc_handler },
        { ML307R_MIPURC_DISCONN_PREFIX, &self->tcp_disconn_handler,
          tcp_disconn_urc_handler },
    };

    size_t urc_count = sizeof(urcs) / sizeof(urcs[0]);
    for (size_t i = 0; i < urc_count; i++) {
        *urcs[i].handler = (at_urc_handler_t) {
            .prefix = urcs[i].prefix,
            .callback = urcs[i].callback,
            .user_ctx = self,
            .next = NULL,
        };
    }

    size_t registered_count = 0;
    for (size_t i = 0; i < urc_count; i++) {
        esp_err_t ret = at_engine_register_urc(self->base.at, urcs[i].prefix,
                                               urcs[i].handler);
        if (ret != ESP_OK) {
            esp_err_t rollback_ret = ESP_OK;
            bool rollback_unregistered[sizeof(urcs) / sizeof(urcs[0])] = { 0 };
            for (size_t j = 0; j < registered_count; j++) {
                esp_err_t err = at_engine_unregister_urc(self->base.at, urcs[j].prefix);
                if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
                    rollback_unregistered[j] = true;
                } else {
                    ESP_LOGW(TAG, "rollback unregister %s failed: %s", urcs[j].prefix,
                             esp_err_to_name(err));
                    if (rollback_ret == ESP_OK) {
                        rollback_ret = err;
                    }
                }
            }
            for (size_t j = 0; j < urc_count; j++) {
                if (j >= registered_count || rollback_unregistered[j]) {
                    memset(urcs[j].handler, 0, sizeof(*urcs[j].handler));
                }
            }
            self->urc_registered = rollback_ret != ESP_OK;
            return ret;
        }
        registered_count++;
    }

    self->urc_registered = true;
    return ESP_OK;
}

static esp_err_t unregister_urcs(modem_ml307r_t *self)
{
    esp_err_t ret = ml307r_unregister_urcs(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister URCs failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t ml307r_unregister_urcs(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const struct {
        const char *prefix;
        at_urc_handler_t *handler;
    } urcs[] = {
        { ML307R_URC_CPIN, &self->cpin_handler },
        { ML307R_URC_CEREG, &self->cereg_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler },
        { ML307R_URC_MQTTURC, &self->mqtturc_handler },
        { ML307R_MIPURC_RTCP_PREFIX, &self->tcp_readable_handler },
        { ML307R_MIPURC_DISCONN_PREFIX, &self->tcp_disconn_handler },
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

static esp_err_t ml307r_destroy(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    esp_err_t ret = ESP_OK;

    set_mqtt_data_enabled(self, false);
    clear_mqtt_state(self);

    if (self->urc_registered) {
        ret = ml307r_unregister_urcs(self);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    set_initialized(self, false);
    return ESP_OK;
}

static esp_err_t ml307r_start(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    bool urc_disabled_for_init = false;
    bool urc_register_attempted = false;
    esp_err_t ret = ESP_OK;

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->pdp[0].active = false;
    self->pdp[0].ip_addr[0] = '\0';
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
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

static esp_err_t ml307r_reset(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    bool urc_disabled_for_init = false;
    bool urc_register_attempted = false;
    esp_err_t ret = ESP_OK;

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->pdp[0].active = false;
    self->pdp[0].ip_addr[0] = '\0';
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
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

static esp_err_t ml307r_stop(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->pdp[0].active = false;
    self->pdp[0].ip_addr[0] = '\0';
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    set_initialized(self, false);

    esp_err_t ret = ESP_OK;
    if (self->urc_registered) {
        esp_err_t urc_ret = unregister_urcs(self);
        if (urc_ret != ESP_OK) {
            ESP_LOGW(TAG, "unregister URCs during stop failed: %s", esp_err_to_name(urc_ret));
            if (ret == ESP_OK) {
                ret = urc_ret;
            }
        }
    }

    esp_err_t power_ret = hardware_power_off(self);
    if (power_ret != ESP_OK) {
        ESP_LOGW(TAG, "hardware power off failed: %s", esp_err_to_name(power_ret));
        if (ret == ESP_OK) {
            ret = power_ret;
        }
    }

    /* Logical OFF even when hardware power-off failed; next start retries hard reset. */
    (void)modem_set_state(me, MODEM_STATE_OFF);

    return ret;
}

static esp_err_t ml307r_get_info(modem_handle_t *me, modem_info_t *info)
{
    ESP_RETURN_ON_FALSE(me && info, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    modem_info_t result = {0};
    const struct {
        const char *cmd;
        const char *prefix;
        char *dst;
        size_t dst_size;
        bool strip_quotes;
        bool required;
    } queries[] = {
        { "AT+CGSN", NULL, result.imei, sizeof(result.imei), false, true },
        { "AT+CIMI", NULL, result.imsi, sizeof(result.imsi), false, false },
        { "AT+MCCID", NULL, result.iccid, sizeof(result.iccid), false, false },
        { "AT+CGMM", "+CGMM:", result.model, sizeof(result.model), true, true },
        { "AT+CGMR", "+CGMR:", result.fw_revision,
          sizeof(result.fw_revision), true, true },
    };

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        ml307r_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, queries[i].cmd, &ctx, 0);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, queries[i].cmd);
        }
        if (ret != ESP_OK) {
            if (queries[i].required) {
                return ret;
            }
            queries[i].dst[0] = '\0';
            continue;
        }

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
        if (!value) {
            if (queries[i].required) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            queries[i].dst[0] = '\0';
            continue;
        }

        if (queries[i].strip_quotes) {
            ret = copy_str_field_strip_quotes(queries[i].dst, queries[i].dst_size,
                                              value);
        } else {
            ret = copy_str_field(queries[i].dst, queries[i].dst_size, value);
        }
        if (ret != ESP_OK) {
            if (queries[i].required) {
                return ret;
            }
            queries[i].dst[0] = '\0';
            continue;
        }
        if (!identity_value_valid(queries[i].cmd, queries[i].dst)) {
            if (queries[i].required) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            queries[i].dst[0] = '\0';
        }
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->cached_info = result;
    xSemaphoreGive(self->base.lock);

    *info = result;
    return ESP_OK;
}

static esp_err_t ml307r_get_sim_status(modem_handle_t *me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+CPIN?", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CPIN? failed");

    ret = ensure_at_ok(&ctx.response, "AT+CPIN?");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CPIN? failed");

    const char *line = find_line_with_prefix(&ctx.response, ML307R_URC_CPIN);
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG, "+CPIN line missing");

    modem_sim_status_t parsed = parse_sim_status_line(line);
    cache_sim_status(self, parsed);
    *status = parsed;
    return ESP_OK;
}

static esp_err_t ml307r_get_signal(modem_handle_t *me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;

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

static esp_err_t ml307r_get_registration(modem_handle_t *me, modem_reg_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    const struct {
        const char *cmd;
        const char *prefix;
    } queries[] = {
        { "AT+CEREG?", ML307R_URC_CEREG },
    };
    esp_err_t last_err = ESP_FAIL;
    bool had_error = false;

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        ml307r_cmd_ctx_t ctx;
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
            last_err = ret;
            had_error = true;
            continue;
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

static esp_err_t ml307r_get_packet_attach_status(modem_handle_t *me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    return query_cgatt(to_ml307r(me), attached);
}

static esp_err_t ml307r_set_apn(modem_handle_t *me, uint8_t cid, const char *apn)
{
    ESP_RETURN_ON_FALSE(me && apn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid != 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == ML307R_PRIMARY_CID, ESP_ERR_NOT_SUPPORTED,
                        TAG, "ML307R MIPCALL supports cid 1 only");
    ESP_RETURN_ON_FALSE(strlen(apn) < MODEM_APN_MAX_LEN && at_arg_safe(apn),
                        ESP_ERR_INVALID_ARG, TAG, "invalid APN");

    char cmd[128];
    /* AT command shape: AT+CGDCONT=%u,"IPV4V6","%s". */
    int written = snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%u,\"IPV4V6\",\"%s\"",
                           (unsigned int)cid, apn);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+CGDCONT command truncated");

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;
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
    pdp->cid = ML307R_PRIMARY_CID;
    strlcpy(pdp->apn, apn, sizeof(pdp->apn));
    strlcpy(pdp->pdp_type, "IPV4V6", sizeof(pdp->pdp_type));
    xSemaphoreGive(self->base.lock);

    return ESP_OK;
}

static esp_err_t ml307r_activate_pdp(modem_handle_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid != 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == ML307R_PRIMARY_CID, ESP_ERR_NOT_SUPPORTED,
                        TAG, "ML307R MIPCALL supports cid 1 only");

    modem_ml307r_t *self = to_ml307r(me);
    modem_pdp_context_t snapshot = {0};
    esp_err_t ret = query_mipcall(self, cid, &snapshot);
    if (ret == ESP_OK && snapshot.active && snapshot.ip_addr[0]) {
        ret = modem_set_state(me, MODEM_STATE_PDP_ACTIVE);
        ESP_RETURN_ON_ERROR(ret, TAG, "set PDP active state failed");

        const modem_event_t event = {
            .id = MODEM_EVENT_PDP_ACTIVATED,
            .data.pdp = snapshot,
        };
        ret = modem_post_event(me, &event);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "post PDP activated event failed: %s", esp_err_to_name(ret));
        }
        return ESP_OK;
    }
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+MIPCALL=1,%u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MIPCALL command truncated");

    ml307r_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, ML307R_MIPCALL_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmd);

    ret = ensure_at_ok(&ctx.response, cmd);
    ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmd);

    modem_pdp_context_t event_pdp = {0};
    bool active_seen = false;
    int count = ctx.response.line_count;
    if (count > ctx.response.max_lines) {
        count = ctx.response.max_lines;
    }
    for (int i = 0; i < count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, ML307R_URC_MIPCALL,
                            sizeof(ML307R_URC_MIPCALL) - 1) != 0) {
            continue;
        }

        uint8_t parsed_cid = 0;
        ret = parse_mipcall_cid(line, &parsed_cid);
        ESP_RETURN_ON_ERROR(ret, TAG, "parse +MIPCALL cid failed");
        if (parsed_cid != cid) {
            continue;
        }

        modem_pdp_context_t parsed = {0};
        if (!parse_mipcall_line(line, &parsed)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (!parsed.active || !parsed.ip_addr[0]) {
            continue;
        }

        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        modem_pdp_context_t *pdp = pdp_by_cid(self, parsed.cid);
        if (!pdp) {
            xSemaphoreGive(self->base.lock);
            return ESP_ERR_INVALID_ARG;
        }
        pdp->cid = parsed.cid;
        pdp->active = true;
        strlcpy(pdp->ip_addr, parsed.ip_addr, sizeof(pdp->ip_addr));
        strlcpy(pdp->pdp_type, parsed.pdp_type[0] ? parsed.pdp_type : "IPV4V6",
                sizeof(pdp->pdp_type));
        event_pdp = *pdp;
        xSemaphoreGive(self->base.lock);

        active_seen = true;
        break;
    }

    uint32_t start_ms = now_ms();
    while (!active_seen && !elapsed_at_least(start_ms, ML307R_MIPCALL_TIMEOUT_MS)) {
        ret = query_mipcall(self, cid, &snapshot);
        if (ret == ESP_OK && snapshot.active && snapshot.ip_addr[0]) {
            event_pdp = snapshot;
            active_seen = true;
            break;
        }
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
            return ret;
        }
        if (!elapsed_at_least(start_ms, ML307R_MIPCALL_TIMEOUT_MS)) {
            delay_init_retry();
        }
    }

    if (!active_seen) {
        return ESP_ERR_TIMEOUT;
    }

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

static esp_err_t ml307r_deactivate_pdp(modem_handle_t *me, uint8_t cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid != 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == ML307R_PRIMARY_CID, ESP_ERR_NOT_SUPPORTED,
                        TAG, "ML307R MIPCALL supports cid 1 only");

    modem_ml307r_t *self = to_ml307r(me);
    modem_pdp_context_t previous = {0};
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    modem_pdp_context_t *pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    previous = *pdp;
    xSemaphoreGive(self->base.lock);

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+MIPCALL=0,%u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MIPCALL command truncated");

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ML307R_MIPCALL_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmd);

    ret = ensure_at_ok(&ctx.response, cmd);
    ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmd);

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    pdp->active = false;
    pdp->ip_addr[0] = '\0';
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    xSemaphoreGive(self->base.lock);

    ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_RETURN_ON_ERROR(ret, TAG, "set ready state failed");

    if (previous.active) {
        previous.active = false;
        previous.ip_addr[0] = '\0';
        const modem_event_t event = {
            .id = MODEM_EVENT_PDP_DEACTIVATED,
            .data.pdp = previous,
        };
        ret = modem_post_event(me, &event);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "post PDP deactivated event failed: %s",
                     esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

static esp_err_t ml307r_get_pdp_context(modem_handle_t *me, uint8_t cid,
                                          modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid != 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == ML307R_PRIMARY_CID, ESP_ERR_NOT_SUPPORTED,
                        TAG, "ML307R MIPCALL supports cid 1 only");

    modem_ml307r_t *self = to_ml307r(me);
    esp_err_t ret = query_mipcall(self, cid, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    modem_pdp_context_t *cached = pdp_by_cid(self, cid);
    if (!cached) {
        xSemaphoreGive(self->base.lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cached->pdp_type[0]) {
        strlcpy(cached->pdp_type, "IPV4V6", sizeof(cached->pdp_type));
    }
    *pdp = *cached;
    xSemaphoreGive(self->base.lock);

    return ESP_OK;
}

static esp_err_t ml307r_socket_prepare(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    char cid_cmd[32];
    int written = snprintf(cid_cmd, sizeof(cid_cmd), "AT+MIPCFG=\"cid\",0,%u",
                           (unsigned int)ML307R_PRIMARY_CID);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cid_cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MIPCFG cid command truncated");

    /* AT command shapes: AT+MIPCFG="cid",0,%u;
     * AT+MIPCFG="encoding",0,0,1; AT+MIPCFG="autofree",0,0. */
    const char *cmds[] = {
        cid_cmd,
        "AT+MIPCFG=\"encoding\",0,0,1",
        "AT+MIPCFG=\"autofree\",0,0",
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ml307r_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, cmds[i], &ctx, 0);
        ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmds[i]);

        ret = ensure_at_ok(&ctx.response, cmds[i]);
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmds[i]);
    }

    return ESP_OK;
}

static esp_err_t ml307r_socket_open(modem_handle_t *me,
                                    const modem_socket_open_t *open)
{
    ESP_RETURN_ON_FALSE(me && open && open->host && open->host[0] &&
                        open->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket open args");
    ESP_RETURN_ON_FALSE(open->conn_id == ML307R_TCP_CONN_ID &&
                        open->proto == MODEM_SOCKET_PROTO_TCP,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");

    modem_ml307r_t *self = to_ml307r(me);
    esp_err_t ret = ml307r_socket_prepare(self);
    ESP_RETURN_ON_ERROR(ret, TAG, "prepare ML307R TCP socket failed");

    char *host = escape_at_string(open->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape socket host failed");

    uint32_t timeout_s = 75U;
    if (open->timeout_ms > 0) {
        timeout_s = (open->timeout_ms / 1000U) +
                    ((open->timeout_ms % 1000U) ? 1U : 0U);
    }

    /* AT command shape: AT+MIPOPEN=0,"TCP","%s",%u,%u,2. */
    int needed = snprintf(NULL, 0, "AT+MIPOPEN=0,\"TCP\",\"%s\",%u,%u,2",
                          host, (unsigned int)open->port,
                          (unsigned int)timeout_s);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    int written = snprintf(cmd, (size_t)needed + 1U,
                           "AT+MIPOPEN=0,\"TCP\",\"%s\",%u,%u,2",
                           host, (unsigned int)open->port,
                           (unsigned int)timeout_s);
    if (written < 0 || (size_t)written > (size_t)needed) {
        free(cmd);
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = "+MIPOPEN:",
    };
    const at_cmd_options_t options = {
        .timeout_ms = open->timeout_ms ? open->timeout_ms : 75000U,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    ml307r_cmd_ctx_t ctx;
    ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        uint8_t open_conn_id = 0;
        int open_result = 0;
        ret = parse_mipopen_response(&ctx.response, &open_conn_id, &open_result);
        if (ret == ESP_OK) {
            if (open->modem_error_code) {
                *open->modem_error_code = open_result;
            }
        }
        if (ret == ESP_OK && open_conn_id != ML307R_TCP_CONN_ID) {
            ret = ESP_ERR_INVALID_RESPONSE;
        } else if (ret == ESP_OK && open_result == 0) {
            ret = ESP_OK;
        } else if (ret == ESP_OK) {
            ret = ml307r_map_mipopen_result(open_result);
        }
    }

    free(cmd);
    free(host);
    return ret;
}

static esp_err_t ml307r_socket_send(modem_handle_t *me,
                                    const modem_socket_send_t *send)
{
    ESP_RETURN_ON_FALSE(me && send && send->data && send->len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket send args");
    ESP_RETURN_ON_FALSE(send->conn_id == ML307R_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");
    ESP_RETURN_ON_FALSE(send->len <= UINT_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "socket payload too large");

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+MIPSEND=0,%u",
                           (unsigned int)send->len);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MIPSEND command truncated");

    const at_cmd_options_t options = {
        .timeout_ms = send->timeout_ms,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    esp_err_t ret = at_engine_send_cmd_with_payload(self->base.at, cmd,
                                                     send->data, send->len,
                                                     ML307R_TCP_PAYLOAD_PROMPT,
                                                     &ctx.response, &options);
    dispatch_tcp_urcs_from_response(self, &ctx.response);
    uint8_t sent_conn_id = 0;
    size_t sent_len = 0;
    if (ret == ESP_OK &&
        parse_mipsend_response(&ctx.response, &sent_conn_id, &sent_len) == ESP_OK &&
        sent_conn_id == ML307R_TCP_CONN_ID && sent_len == send->len) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t ml307r_socket_recv(modem_handle_t *me,
                                    const modem_socket_recv_t *recv,
                                    modem_socket_recv_result_t *result)
{
    ESP_RETURN_ON_FALSE(me && recv && result && recv->max_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket recv args");
    ESP_RETURN_ON_FALSE(recv->conn_id == ML307R_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");
    ESP_RETURN_ON_FALSE(recv->max_len <= UINT_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "socket recv length too large");

    modem_ml307r_t *self = to_ml307r(me);
    size_t line_cap = 0;
    esp_err_t ret = ml307r_tcp_read_len_for_line_buf(self->config.at_rx_line_buf_size, &line_cap);
    ESP_RETURN_ON_ERROR(ret, TAG, "AT line buffer too small for AT+MIPRD");

    size_t read_len = recv->max_len;
    if (read_len > ML307R_TCP_MAX_HEX_READ_BYTES) {
        read_len = ML307R_TCP_MAX_HEX_READ_BYTES;
    }
    if (read_len > line_cap) {
        read_len = line_cap;
    }

    char cmd[32];
    int written = snprintf(cmd, sizeof(cmd), "AT+MIPRD=0,%u",
                           (unsigned int)read_len);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MIPRD command truncated");

    ml307r_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+MIPRD failed");

    ret = ensure_at_ok(&ctx.response, "AT+MIPRD");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MIPRD failed");

    size_t remaining_len = 0;
    const char *hex = find_miprd_hex_line(&ctx.response, &remaining_len);
    if (!hex) {
        if (find_line_with_prefix(&ctx.response, "+MIPRD:")) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        result->conn_id = ML307R_TCP_CONN_ID;
        result->payload = NULL;
        result->payload_len = 0;
        result->remaining_len = 0;
        result->modem_error_code = 0;
        return ESP_OK;
    }

    uint8_t *payload = NULL;
    size_t payload_len = 0;
    ret = decode_hex_payload(hex, &payload, &payload_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "decode AT+MIPRD payload failed");

    result->conn_id = ML307R_TCP_CONN_ID;
    result->payload = payload;
    result->payload_len = payload_len;
    result->remaining_len = remaining_len;
    result->modem_error_code = 0;
    return ESP_OK;
}

static esp_err_t ml307r_socket_close(modem_handle_t *me,
                                     const modem_socket_close_t *close)
{
    ESP_RETURN_ON_FALSE(me && close, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket close args");
    ESP_RETURN_ON_FALSE(close->conn_id == ML307R_TCP_CONN_ID,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported socket");

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = "+MIPCLOSE:",
    };
    const at_cmd_options_t options = {
        .timeout_ms = close->timeout_ms,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, "AT+MIPCLOSE=0", &ctx, &options);
    if (ret == ESP_OK && response_contains(&ctx.response, "+MIPCLOSE: 0")) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t ml307r_mqtt_configure(modem_handle_t *me,
                                        const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id && config->host &&
                        config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT config");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool connected = self->mqtt_session_connected || self->mqtt_data_enabled;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(!connected, ESP_ERR_INVALID_STATE, TAG, "MQTT is connected");

    modem_mqtt_config_t new_config = {0};
    esp_err_t ret = copy_mqtt_config(&new_config, config);
    ESP_RETURN_ON_ERROR(ret, TAG, "copy MQTT config failed");

    /* AT command shapes: AT+MQTTCFG="version",0,4; AT+MQTTCFG="cid",0,1;
     * AT+MQTTCFG="encoding",0,1,0; AT+MQTTCFG="keepalive",0,%u;
     * AT+MQTTCFG="clean",0,%u; AT+MQTTCFG="cached",0,0. */
    const char *fixed_cmds[] = {
        "AT+MQTTCFG=\"version\",0,4",
        "AT+MQTTCFG=\"cid\",0,1",
        "AT+MQTTCFG=\"encoding\",0,1,0",
    };
    for (size_t i = 0; i < sizeof(fixed_cmds) / sizeof(fixed_cmds[0]); i++) {
        ml307r_cmd_ctx_t ctx;
        ret = send_cmd(self, fixed_cmds[i], &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, fixed_cmds[i]);
        }
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }

    char cmd[64];
    int written = snprintf(cmd, sizeof(cmd), "AT+MQTTCFG=\"keepalive\",0,%u",
                           (unsigned int)new_config.keepalive_s);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    ml307r_cmd_ctx_t ctx;
    ret = send_cmd(self, cmd, &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, cmd);
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }

    written = snprintf(cmd, sizeof(cmd), "AT+MQTTCFG=\"clean\",0,%u",
                       new_config.clean_session ? 1U : 0U);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    ret = send_cmd(self, cmd, &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, cmd);
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = send_cmd(self, "AT+MQTTCFG=\"cached\",0,0", &ctx,
                   ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTCFG=\"cached\",0,0");
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    free_mqtt_config(&self->mqtt_config);
    self->mqtt_config = new_config;
    memset(&new_config, 0, sizeof(new_config));
    self->mqtt_configured = true;
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }

cleanup:
    free_mqtt_config(&new_config);
    return ret;
}

static esp_err_t ml307r_mqtt_tcp_connect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool configured = self->mqtt_configured;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }

    ESP_RETURN_ON_FALSE(configured, ESP_ERR_INVALID_STATE, TAG, "MQTT not configured");
    return ESP_OK;
}

static esp_err_t ml307r_mqtt_connect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    bool configured = false;
    bool session_connected = false;
    modem_mqtt_config_t snapshot = {0};
    esp_err_t ret = ESP_OK;

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    configured = self->mqtt_configured;
    session_connected = self->mqtt_session_connected;
    if (configured && !session_connected) {
        ret = copy_mqtt_config(&snapshot, &self->mqtt_config);
    }
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(configured, ESP_ERR_INVALID_STATE, TAG, "MQTT not configured");
    ESP_RETURN_ON_FALSE(!session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session already connected");
    ESP_RETURN_ON_ERROR(ret, TAG, "copy MQTT config failed");

    char *host = escape_at_string(snapshot.host);
    char *client_id = escape_at_string(snapshot.client_id);
    char *username = escape_at_string(snapshot.username ? snapshot.username : "");
    char *password = escape_at_string(snapshot.password ? snapshot.password : "");
    if (!host || !client_id || !username || !password) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* AT command shape: AT+MQTTCONN=0,"%s",%u,"%s","%s","%s". */
    int needed = snprintf(NULL, 0,
                          "AT+MQTTCONN=0,\"%s\",%u,\"%s\",\"%s\",\"%s\"",
                          host, (unsigned int)snapshot.port, client_id,
                          username, password);
    if (needed < 0) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    snprintf(cmd, (size_t)needed + 1U,
             "AT+MQTTCONN=0,\"%s\",%u,\"%s\",\"%s\",\"%s\"",
             host, (unsigned int)snapshot.port, client_id, username, password);

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = ML307R_URC_MQTTURC,
    };
    const at_cmd_options_t options = {
        .timeout_ms = ML307R_MQTT_CONNECT_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    ml307r_cmd_ctx_t ctx;
    ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTCONN");
    }
    if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
        int count = ctx.response.line_count;
        if (count > ctx.response.max_lines) {
            count = ctx.response.max_lines;
        }
        for (int i = 0; i < count; i++) {
            const char *line = ctx.response.lines[i];
            if (line && mqtt_event_matches(line, "conn", NULL)) {
                ret = parse_mqtt_conn_urc_ex(self, line, false);
                break;
            }
        }
    }

    free(cmd);

cleanup:
    free(host);
    free(client_id);
    free(username);
    free(password);
    free_mqtt_config(&snapshot);
    return ret;
}

static esp_err_t ml307r_mqtt_disconnect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool session_connected = self->mqtt_session_connected;
    bool previous_data_enabled = self->mqtt_data_enabled;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session not connected");

    set_mqtt_data_enabled(self, false);
    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTDISC=0", &ctx,
                             ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTDISC=0");
    }
    if (ret != ESP_OK) {
        if (self->base.lock) {
            xSemaphoreTake(self->base.lock, portMAX_DELAY);
        }
        if (self->mqtt_session_connected) {
            self->mqtt_data_enabled = previous_data_enabled;
        }
        if (self->base.lock) {
            xSemaphoreGive(self->base.lock);
        }
        return ret;
    }

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    return ESP_OK;
}

static esp_err_t ml307r_mqtt_tcp_disconnect(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_session_connected = false;
    self->mqtt_data_enabled = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    return ESP_OK;
}

static esp_err_t ml307r_mqtt_subscribe(modem_handle_t *me,
                                        const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic && topic->topic[0] &&
                        topic->qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool session_connected = self->mqtt_session_connected;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session not connected");

    char *escaped_topic = escape_at_string(topic->topic);
    ESP_RETURN_ON_FALSE(escaped_topic, ESP_ERR_NO_MEM, TAG, "escape topic failed");

    /* AT command shape: AT+MQTTSUB=0,"%s",%u. */
    int needed = snprintf(NULL, 0, "AT+MQTTSUB=0,\"%s\",%u",
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
    snprintf(cmd, (size_t)needed + 1U, "AT+MQTTSUB=0,\"%s\",%u",
             escaped_topic, (unsigned int)topic->qos);

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTSUB");
    }

    free(cmd);
    free(escaped_topic);
    return ret;
}

static esp_err_t ml307r_mqtt_unsubscribe(modem_handle_t *me,
                                          const modem_mqtt_topic_t *topic)
{
    ESP_RETURN_ON_FALSE(me && topic && topic->topic && topic->topic[0] &&
                        topic->qos <= 2,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool session_connected = self->mqtt_session_connected;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session not connected");

    char *escaped_topic = escape_at_string(topic->topic);
    ESP_RETURN_ON_FALSE(escaped_topic, ESP_ERR_NO_MEM, TAG, "escape topic failed");

    /* AT command shape: AT+MQTTUNSUB=0,"%s". */
    int needed = snprintf(NULL, 0, "AT+MQTTUNSUB=0,\"%s\"", escaped_topic);
    if (needed < 0) {
        free(escaped_topic);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(escaped_topic);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MQTTUNSUB=0,\"%s\"", escaped_topic);

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTUNSUB");
    }

    free(cmd);
    free(escaped_topic);
    return ret;
}

static esp_err_t ml307r_mqtt_publish(modem_handle_t *me,
                                      const modem_mqtt_publish_t *publish)
{
    ESP_RETURN_ON_FALSE(me && publish && publish->topic && publish->payload &&
                        publish->topic[0] && publish->payload_len > 0 &&
                        publish->qos <= 2 &&
                        publish->payload_len <= ML307R_MQTT_MAX_PAYLOAD_LEN &&
                        publish->payload_len <= UINT_MAX &&
                        publish->payload_len <= SIZE_MAX - 1U,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    bool session_connected = self->mqtt_session_connected;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    ESP_RETURN_ON_FALSE(session_connected, ESP_ERR_INVALID_STATE,
                        TAG, "MQTT session not connected");

    char *hex_payload = hex_encode_payload(publish->payload, publish->payload_len);
    ESP_RETURN_ON_FALSE(hex_payload, ESP_ERR_NO_MEM, TAG, "encode payload failed");

    char *escaped_topic = escape_at_string(publish->topic);
    if (!escaped_topic) {
        free(hex_payload);
        return ESP_ERR_NO_MEM;
    }

    /* AT+MQTTPUB=<connect_id>,"<topic>",<qos>,<retain>,<dup>,<msg_len>,"<message>"
     * lwlte 只发新消息，<dup> 恒为 0；模组自动重传由 MQTTCFG="retrans" 控制。
     * lwlte only publishes new messages; <dup> is always 0. Module auto-retransmit
     * is controlled by MQTTCFG="retrans" and does not use this parameter. */
    const unsigned int dup_flag = 0U;
    int needed = snprintf(NULL, 0, "AT+MQTTPUB=0,\"%s\",%u,%u,%u,%u,\"%s\"",
                           escaped_topic, (unsigned int)publish->qos,
                           publish->retain ? 1U : 0U,
                           dup_flag,
                           (unsigned int)publish->payload_len, hex_payload);
    if (needed < 0) {
        free(escaped_topic);
        free(hex_payload);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(escaped_topic);
        free(hex_payload);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U,
             "AT+MQTTPUB=0,\"%s\",%u,%u,%u,%u,\"%s\"",
             escaped_topic, (unsigned int)publish->qos,
             publish->retain ? 1U : 0U,
             dup_flag,
             (unsigned int)publish->payload_len, hex_payload);

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTPUB");
    }

    free(cmd);
    free(escaped_topic);
    free(hex_payload);
    return ret;
}

static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 2:  return MODEM_MQTT_STATUS_AUTHENTICATED;
    case 1:  return MODEM_MQTT_STATUS_TCP_CONNECTED;
    case 3:  return MODEM_MQTT_STATUS_OFFLINE;
    default: return MODEM_MQTT_STATUS_OFFLINE;
    }
}

static esp_err_t ml307r_mqtt_get_status(modem_handle_t *me,
                                        modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTSTATE=0", &ctx,
                             ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTSTATE=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MQTTSTATE=0 failed");

    const char *line = find_line_with_prefix(&ctx.response, "+MQTTSTATE");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                        "+MQTTSTATE line missing");

    int state = 0;
    ret = parse_int_after_prefix(line, "+MQTTSTATE", &state);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +MQTTSTATE failed");
    ESP_RETURN_ON_FALSE(state >= 1 && state <= 3, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid MQTT state %d", state);

    *status = map_mqtt_status(state);
    return ESP_OK;
}

static esp_err_t ml307r_ping(modem_handle_t *me,
                             const modem_ping_request_t *request,
                             modem_ping_reply_t *replies,
                             size_t max_replies,
                             modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && request && request->host && request->host[0] &&
                        replies && max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(request->count >= 1 &&
                        request->count <= ML307R_MPING_MAX_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping count");

    uint32_t timeout_s = ((uint32_t)request->timeout_100ms + 9U) / 10U;
    if (timeout_s == 0) {
        timeout_s = 1;
    }
    ESP_RETURN_ON_FALSE(timeout_s <= 60U, ESP_ERR_INVALID_ARG,
                        TAG, "invalid ping timeout");

    uint32_t packet_len = request->data_len;
    if (packet_len == 0) {
        packet_len = 16U;
    }
    ESP_RETURN_ON_FALSE(packet_len >= 1U && packet_len <= 1400U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping packet length");

    char *host = escape_at_string(request->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape ping host failed");

    /* AT command shape: AT+MPING="%s",%u,%u,%u,1. */
    int needed = snprintf(NULL, 0, "AT+MPING=\"%s\",%u,%u,%u,1",
                          host, (unsigned int)timeout_s,
                          (unsigned int)request->count,
                          (unsigned int)packet_len);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MPING=\"%s\",%u,%u,%u,1",
             host, (unsigned int)timeout_s, (unsigned int)request->count,
             (unsigned int)packet_len);

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = ML307R_MPING_STATISTICS_PREFIX,
    };
    const at_cmd_options_t options = {
        .timeout_ms = ping_cmd_timeout_ms(request),
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_ml307r_t *self = to_ml307r(me);
    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MPING");
    }
    if (ret != ESP_OK) {
        free(cmd);
        free(host);
        return ret;
    }

    size_t parsed_count = 0;
    size_t success_count = 0;
    bool statistics_seen = false;
    modem_ping_summary_t parsed_summary = {0};
    int line_count = ctx.response.line_count;
    if (line_count > ctx.response.max_lines) {
        line_count = ctx.response.max_lines;
    }
    for (int i = 0; i < line_count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, ML307R_MPING_PREFIX,
                            sizeof(ML307R_MPING_PREFIX) - 1U) != 0) {
            continue;
        }

        const char *value = skip_prefix_value(line, ML307R_MPING_PREFIX);
        if (value && strncmp(value, "\"statistics\"",
                            sizeof("\"statistics\"") - 1U) == 0) {
            ret = parse_mping_statistics_line(line, &parsed_summary);
            if (ret != ESP_OK) {
                free(cmd);
                free(host);
                return ret;
            }
            statistics_seen = true;
            continue;
        }

        if (parsed_count >= request->count) {
            free(cmd);
            free(host);
            return ESP_ERR_INVALID_RESPONSE;
        }

        ret = parse_mping_reply_line(line, &replies[parsed_count]);
        if (ret != ESP_OK) {
            free(cmd);
            free(host);
            return ret;
        }
        replies[parsed_count].seq = (uint8_t)(parsed_count + 1U);
        if (replies[parsed_count].success) {
            success_count++;
        }
        parsed_count++;
    }

    if (parsed_count != request->count) {
        free(cmd);
        free(host);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (statistics_seen) {
        if (parsed_summary.sent != request->count ||
            parsed_summary.received != success_count ||
            parsed_summary.lost != request->count - success_count) {
            free(cmd);
            free(host);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (summary) {
            *summary = parsed_summary;
        }
    } else {
        calculate_ping_summary(request, replies, parsed_count, summary);
    }

    free(cmd);
    free(host);
    return ESP_OK;
}

static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx) {
        return;
    }

    modem_ml307r_t *self = (modem_ml307r_t *)user_ctx;
    modem_sim_status_t status = parse_sim_status_line(line);

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop CPIN URC, lock busy");
    } else {
        self->last_sim_status = status;
        xSemaphoreGive(self->base.lock);
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_SIM_CHANGED,
        .data.sim_status = status,
    };
    esp_err_t ret = post_event_nonblocking(self, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post SIM changed event failed: %s", esp_err_to_name(ret));
    }
}

static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    if (!user_ctx) {
        return;
    }

    modem_ml307r_t *self = (modem_ml307r_t *)user_ctx;
    modem_reg_status_t status = MODEM_REG_UNKNOWN;
    esp_err_t ret = parse_registration_urc_line(line, prefix, &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse registration URC failed: %s", line ? line : "<NULL>");
        status = MODEM_REG_UNKNOWN;
    }

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop registration URC, lock busy");
    } else {
        self->last_reg_status = status;
        xSemaphoreGive(self->base.lock);
    }

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
    ret = post_event_nonblocking(self, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post registration changed event failed: %s", esp_err_to_name(ret));
    }
}

static void mipcall_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    modem_ml307r_t *self = (modem_ml307r_t *)user_ctx;
    modem_pdp_context_t parsed = {0};
    if (!parse_mipcall_line(line, &parsed)) {
        return;
    }

    modem_pdp_context_t event_pdp = {0};
    modem_event_id_t event_id = parsed.active ?
                                MODEM_EVENT_PDP_ACTIVATED :
                                MODEM_EVENT_PDP_DEACTIVATED;

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop MIPCALL URC, lock busy");
        event_pdp = parsed;
    } else {
        modem_pdp_context_t *pdp = pdp_by_cid(self, parsed.cid);
        if (!pdp) {
            xSemaphoreGive(self->base.lock);
            return;
        }
        pdp->cid = parsed.cid;
        pdp->active = parsed.active;
        if (parsed.ip_addr[0]) {
            strlcpy(pdp->ip_addr, parsed.ip_addr, sizeof(pdp->ip_addr));
        }
        if (!parsed.active) {
            pdp->ip_addr[0] = '\0';
            self->mqtt_data_enabled = false;
            self->mqtt_session_connected = false;
        }
        if (parsed.pdp_type[0]) {
            strlcpy(pdp->pdp_type, parsed.pdp_type, sizeof(pdp->pdp_type));
        } else if (!pdp->pdp_type[0]) {
            strlcpy(pdp->pdp_type, "IPV4V6", sizeof(pdp->pdp_type));
        }
        event_pdp = *pdp;
        xSemaphoreGive(self->base.lock);
    }

    set_state_nonblocking(self, parsed.active ? MODEM_STATE_PDP_ACTIVE : MODEM_STATE_READY);

    const modem_event_t event = {
        .id = event_id,
        .data.pdp = event_pdp,
    };
    esp_err_t ret = post_event_nonblocking(self, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post PDP event failed: %s", esp_err_to_name(ret));
    }
}

static void mqtturc_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    handle_mqtturc((modem_ml307r_t *)user_ctx, line);
}

static void ml307r_post_tcp_readable(modem_ml307r_t *self, size_t recv_len,
                                     size_t total_len)
{
    if (!self) {
        return;
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_DATA,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = ML307R_TCP_CONN_ID,
            .payload_len = recv_len,
            .reason = (int)total_len,
        },
    };
    esp_err_t ret = post_event_nonblocking(self, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post TCP readable event failed: %s", esp_err_to_name(ret));
    }
}

static void ml307r_post_tcp_closed(modem_ml307r_t *self, int reason,
                                   int modem_error_code)
{
    if (!self) {
        return;
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_CLOSED,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = ML307R_TCP_CONN_ID,
            .reason = reason,
            .modem_error_code = modem_error_code,
        },
    };
    esp_err_t ret = post_event_nonblocking(self, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post TCP closed event failed: %s", esp_err_to_name(ret));
    }
}

static void tcp_readable_urc_handler(const char *prefix, const char *line,
                                     void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    size_t recv_len = 0;
    size_t total_len = 0;
    esp_err_t ret = parse_tcp_rtcp_urc(line, &recv_len, &total_len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse TCP readable URC failed: %s", line);
        return;
    }

    ml307r_post_tcp_readable((modem_ml307r_t *)user_ctx, recv_len, total_len);
}

static void tcp_disconn_urc_handler(const char *prefix, const char *line,
                                    void *user_ctx)
{
    (void)prefix;

    if (!user_ctx || !line) {
        return;
    }

    int connect_state = 0;
    esp_err_t ret = parse_tcp_disconn_urc(line, &connect_state);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse TCP disconn URC failed: %s", line);
        return;
    }

    ml307r_post_tcp_closed((modem_ml307r_t *)user_ctx, connect_state, 0);
}

static esp_err_t parse_tcp_rtcp_urc(const char *line, size_t *recv_len,
                                    size_t *total_len)
{
    ESP_RETURN_ON_FALSE(recv_len && total_len, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");

    /* URC shape: +MIPURC: "rtcp",0,<recv_len>,<total_len>. */
    const char *cursor = skip_prefix_value(line, ML307R_MIPURC_RTCP_PREFIX);
    if (!cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!parse_mqtt_comma(&cursor)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned long conn_id = 0;
    unsigned long parsed_recv_len = 0;
    unsigned long parsed_total_len = 0;
    unsigned long size_max = (unsigned long)((size_t)-1);
    if (!parse_mqtt_uint_field(&cursor, UINT_MAX, &conn_id) ||
        conn_id != ML307R_TCP_CONN_ID ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, size_max, &parsed_recv_len) ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, size_max, &parsed_total_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *recv_len = (size_t)parsed_recv_len;
    *total_len = (size_t)parsed_total_len;
    return ESP_OK;
}

static esp_err_t parse_tcp_disconn_urc(const char *line, int *connect_state)
{
    ESP_RETURN_ON_FALSE(connect_state, ESP_ERR_INVALID_ARG, TAG,
                        "connect_state is NULL");

    /* URC shape: +MIPURC: "disconn",0,<connect_state>. */
    const char *cursor = skip_prefix_value(line, ML307R_MIPURC_DISCONN_PREFIX);
    if (!cursor) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!parse_mqtt_comma(&cursor)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned long conn_id = 0;
    unsigned long parsed_state = 0;
    if (!parse_mqtt_uint_field(&cursor, UINT_MAX, &conn_id) ||
        conn_id != ML307R_TCP_CONN_ID ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, INT_MAX, &parsed_state)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *connect_state = (int)parsed_state;
    return ESP_OK;
}

static esp_err_t query_mipcall(modem_ml307r_t *self, uint8_t cid,
                               modem_pdp_context_t *out_pdp)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");
    ESP_RETURN_ON_FALSE(cid != 0, ESP_ERR_INVALID_ARG, TAG,
                        "invalid cid %u", (unsigned int)cid);
    ESP_RETURN_ON_FALSE(cid == ML307R_PRIMARY_CID, ESP_ERR_NOT_SUPPORTED,
                        TAG, "ML307R MIPCALL supports cid 1 only");

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MIPCALL?", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+MIPCALL? failed");

    ret = ensure_at_ok(&ctx.response, "AT+MIPCALL?");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MIPCALL? failed");

    int count = ctx.response.line_count;
    if (count > ctx.response.max_lines) {
        count = ctx.response.max_lines;
    }

    for (int i = 0; i < count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, ML307R_URC_MIPCALL,
                            sizeof(ML307R_URC_MIPCALL) - 1) != 0) {
            continue;
        }

        uint8_t parsed_cid = 0;
        ret = parse_mipcall_cid(line, &parsed_cid);
        ESP_RETURN_ON_ERROR(ret, TAG, "parse +MIPCALL cid failed");
        if (parsed_cid != cid) {
            continue;
        }

        modem_pdp_context_t parsed = {0};
        if (!parse_mipcall_line(line, &parsed)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        modem_pdp_context_t *cached = pdp_by_cid(self, cid);
        if (!cached) {
            xSemaphoreGive(self->base.lock);
            return ESP_ERR_INVALID_ARG;
        }

        char apn[MODEM_APN_MAX_LEN] = {0};
        char pdp_type[MODEM_PDP_TYPE_MAX_LEN] = {0};
        strlcpy(apn, cached->apn, sizeof(apn));
        strlcpy(pdp_type, cached->pdp_type, sizeof(pdp_type));

        cached->cid = ML307R_PRIMARY_CID;
        strlcpy(cached->apn, apn, sizeof(cached->apn));
        if (parsed.pdp_type[0]) {
            strlcpy(cached->pdp_type, parsed.pdp_type, sizeof(cached->pdp_type));
        } else if (pdp_type[0]) {
            strlcpy(cached->pdp_type, pdp_type, sizeof(cached->pdp_type));
        } else {
            strlcpy(cached->pdp_type, "IPV4V6", sizeof(cached->pdp_type));
        }
        cached->active = parsed.active;
        strlcpy(cached->ip_addr, parsed.ip_addr, sizeof(cached->ip_addr));
        if (out_pdp) {
            *out_pdp = *cached;
        }
        xSemaphoreGive(self->base.lock);

        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t parse_mipcall_cid(const char *line, uint8_t *cid)
{
    ESP_RETURN_ON_FALSE(cid, ESP_ERR_INVALID_ARG, TAG, "cid is NULL");

    const char *value = skip_prefix_value(line, ML307R_URC_MIPCALL);
    if (!value || !isdigit((unsigned char)*value)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed_cid = strtoul(value, &end, 10);
    if (end == value || errno == ERANGE || parsed_cid > UINT8_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *cid = (uint8_t)parsed_cid;
    return ESP_OK;
}

static bool parse_mipcall_line(const char *line, modem_pdp_context_t *pdp)
{
    if (!line || !pdp) {
        return false;
    }

    const char *value = skip_prefix_value(line, ML307R_URC_MIPCALL);
    if (!value) {
        return false;
    }

    if (!isdigit((unsigned char)*value)) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed_cid = strtoul(value, &end, 10);
    if (end == value || errno == ERANGE || parsed_cid != ML307R_PRIMARY_CID) {
        return false;
    }

    const char *cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return false;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    if (!isdigit((unsigned char)*cursor)) {
        return false;
    }
    errno = 0;
    end = NULL;
    unsigned long state = strtoul(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || (state != 0 && state != 1)) {
        return false;
    }

    memset(pdp, 0, sizeof(*pdp));
    pdp->cid = ML307R_PRIMARY_CID;
    pdp->active = state == 1;
    strlcpy(pdp->pdp_type, "IPV4V6", sizeof(pdp->pdp_type));

    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (state == 0) {
        return *cursor == '\0';
    }

    if (*cursor != ',') {
        return false;
    }
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    const char *addr_start = cursor;
    const char *addr_end = cursor;
    if (*cursor == '"') {
        addr_start = cursor + 1;
        addr_end = strchr(addr_start, '"');
        if (!addr_end) {
            return false;
        }
        const char *tail = addr_end + 1;
        while (isspace((unsigned char)*tail)) {
            tail++;
        }
        if (*tail != '\0' && *tail != ',') {
            return false;
        }
    } else {
        while (*addr_end && *addr_end != ',') {
            addr_end++;
        }
        while (addr_end > addr_start && isspace((unsigned char)*(addr_end - 1))) {
            addr_end--;
        }
    }

    size_t addr_len = (size_t)(addr_end - addr_start);
    if (addr_len == 0 || addr_len >= sizeof(pdp->ip_addr)) {
        return false;
    }

    memcpy(pdp->ip_addr, addr_start, addr_len);
    pdp->ip_addr[addr_len] = '\0';
    return looks_like_ip_addr(pdp->ip_addr);
}

static esp_err_t parse_mqtt_conn_urc(modem_ml307r_t *self, const char *line)
{
    return parse_mqtt_conn_urc_ex(self, line, true);
}

static esp_err_t parse_mqtt_conn_urc_ex(modem_ml307r_t *self, const char *line,
                                        bool nonblocking)
{
    ESP_RETURN_ON_FALSE(self && line, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const char *cursor = NULL;
    if (!mqtt_event_matches(line, "conn", &cursor)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    unsigned long connect_id = 0;
    unsigned long conn_state = 0;
    if (!parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, UINT_MAX, &connect_id) ||
        connect_id != ML307R_MQTT_CONNECT_ID ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, 255U, &conn_state)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool was_connected = false;
    if (self->base.lock) {
        TickType_t wait_ticks = nonblocking ? 0 : portMAX_DELAY;
        if (xSemaphoreTake(self->base.lock, wait_ticks) != pdTRUE) {
            ESP_LOGW(TAG, "drop MQTT conn URC state update, lock busy");
            return ESP_ERR_TIMEOUT;
        }
    }
    was_connected = self->mqtt_session_connected;
    if (conn_state == 0) {
        self->mqtt_session_connected = true;
        self->mqtt_data_enabled = true;
    } else {
        self->mqtt_session_connected = false;
        self->mqtt_data_enabled = false;
    }
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }

    if (conn_state == 0) {
        return ESP_OK;
    }
    if (conn_state == 1) {
        ESP_LOGW(TAG, "MQTT reconnecting");
    } else {
        ESP_LOGW(TAG, "MQTT disconnected, conn_state=%u", (unsigned int)conn_state);
    }

    if (was_connected) {
        const modem_event_t event = {
            .id = MODEM_EVENT_PROTOCOL_CLOSED,
        };
        esp_err_t ret = nonblocking ? post_event_nonblocking(self, &event) :
                        modem_post_event(&self->base, &event);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "post MQTT closed event failed: %s", esp_err_to_name(ret));
        }
    }
    return ESP_FAIL;
}

static bool parse_mqtt_publish_urc(const char *line, char **topic,
                                   size_t *topic_len, uint8_t **payload,
                                   size_t *payload_len)
{
    if (!line || !topic || !topic_len || !payload || !payload_len) {
        return false;
    }

    *topic = NULL;
    *topic_len = 0;
    *payload = NULL;
    *payload_len = 0;

    const char *cursor = NULL;
    if (!mqtt_event_matches(line, "publish", &cursor) ||
        !parse_mqtt_comma(&cursor)) {
        return false;
    }

    unsigned long connect_id = 0;
    unsigned long mid = 0;
    if (!parse_mqtt_uint_field(&cursor, UINT_MAX, &connect_id) ||
        connect_id != ML307R_MQTT_CONNECT_ID ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, UINT_MAX, &mid) ||
        !parse_mqtt_comma(&cursor)) {
        return false;
    }
    (void)mid;

    const char *topic_start = cursor;
    const char *topic_end = NULL;
    if (*cursor == '"') {
        topic_start = cursor + 1;
        topic_end = strchr(topic_start, '"');
        if (!topic_end) {
            return false;
        }
        cursor = topic_end + 1;
    } else {
        topic_end = strchr(cursor, ',');
        if (!topic_end) {
            return false;
        }
        while (topic_end > topic_start &&
               isspace((unsigned char)*(topic_end - 1))) {
            topic_end--;
        }
        cursor = topic_end;
    }
    if (topic_end == topic_start || !parse_mqtt_comma(&cursor)) {
        return false;
    }

    unsigned long parsed_total_len = 0;
    unsigned long parsed_payload_len = 0;
    unsigned long size_max = (unsigned long)((size_t)-1);
    if (!parse_mqtt_uint_field(&cursor, size_max, &parsed_total_len) ||
        !parse_mqtt_comma(&cursor) ||
        !parse_mqtt_uint_field(&cursor, size_max, &parsed_payload_len) ||
        parsed_payload_len != parsed_total_len ||
        !parse_mqtt_comma(&cursor)) {
        return false;
    }

    size_t parsed_topic_len = (size_t)(topic_end - topic_start);
    size_t parsed_payload_size = (size_t)parsed_payload_len;
    for (size_t i = 0; i < parsed_payload_size; i++) {
        if (cursor[i] == '\0') {
            return false;
        }
    }
    if (cursor[parsed_payload_size] != '\0') {
        return false;
    }

    char *topic_buf = malloc(parsed_topic_len + 1U);
    if (!topic_buf) {
        return false;
    }
    uint8_t *payload_buf = malloc(parsed_payload_size > 0 ? parsed_payload_size : 1U);
    if (!payload_buf) {
        free(topic_buf);
        return false;
    }

    memcpy(topic_buf, topic_start, parsed_topic_len);
    topic_buf[parsed_topic_len] = '\0';
    if (parsed_payload_size > 0) {
        memcpy(payload_buf, cursor, parsed_payload_size);
    }

    *topic = topic_buf;
    *topic_len = parsed_topic_len;
    *payload = payload_buf;
    *payload_len = parsed_payload_size;
    return true;
}

static void handle_mqtturc(modem_ml307r_t *self, const char *line)
{
    if (!self || !line) {
        return;
    }

    if (mqtt_event_matches(line, "conn", NULL)) {
        esp_err_t ret = parse_mqtt_conn_urc(self, line);
        if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGW(TAG, "parse MQTT conn URC failed: %s", esp_err_to_name(ret));
        } else if (ret != ESP_OK && ret != ESP_FAIL) {
            ESP_LOGW(TAG, "MQTT conn URC state update failed: %s",
                     esp_err_to_name(ret));
        }
        return;
    }

    if (mqtt_event_matches(line, "pubnmi", NULL)) {
        ESP_LOGW(TAG, "MQTT cached publish notification ignored: %s", line);
        return;
    }

    const char *diagnostic_events[] = {
        "suback",
        "unsuback",
        "puback",
        "pubrec",
        "pubcomp",
        "timeout",
        "drop",
        "pingresp",
    };
    for (size_t i = 0; i < sizeof(diagnostic_events) / sizeof(diagnostic_events[0]); i++) {
        if (mqtt_event_matches(line, diagnostic_events[i], NULL)) {
            ESP_LOGD(TAG, "MQTT URC %s: %s", diagnostic_events[i], line);
            return;
        }
    }

    if (!mqtt_event_matches(line, "publish", NULL)) {
        ESP_LOGD(TAG, "ignore MQTT URC: %s", line);
        return;
    }

    char *topic = NULL;
    size_t topic_len = 0;
    uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (!parse_mqtt_publish_urc(line, &topic, &topic_len, &payload, &payload_len)) {
        return;
    }

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

static bool mqtt_event_matches(const char *line, const char *event_name,
                               const char **event_end)
{
    if (event_end) {
        *event_end = NULL;
    }
    if (!line || !event_name) {
        return false;
    }

    const char *cursor = skip_prefix_value(line, ML307R_URC_MQTTURC);
    if (!cursor || *cursor != '"') {
        return false;
    }
    cursor++;

    const char *start = cursor;
    while (*cursor && *cursor != '"') {
        if (*cursor == '\r' || *cursor == '\n') {
            return false;
        }
        cursor++;
    }
    if (*cursor != '"') {
        return false;
    }

    size_t len = (size_t)(cursor - start);
    if (strlen(event_name) != len || strncmp(start, event_name, len) != 0) {
        return false;
    }

    if (event_end) {
        *event_end = cursor + 1;
    }
    return true;
}

static bool parse_mqtt_uint_field(const char **cursor, unsigned long max_value,
                                  unsigned long *out_value)
{
    if (!cursor || !*cursor || !out_value) {
        return false;
    }

    const char *start = *cursor;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (!isdigit((unsigned char)*start)) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(start, &end, 10);
    if (end == start || errno == ERANGE || parsed > max_value) {
        return false;
    }

    *out_value = parsed;
    *cursor = end;
    return true;
}

static bool parse_mqtt_comma(const char **cursor)
{
    if (!cursor || !*cursor) {
        return false;
    }

    const char *pos = *cursor;
    while (isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ',') {
        return false;
    }
    pos++;
    while (isspace((unsigned char)*pos)) {
        pos++;
    }

    *cursor = pos;
    return true;
}

static esp_err_t parse_mping_uint(const char **cursor,
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
    size_t count = reply_count;
    if (count > request->count) {
        count = request->count;
    }
    for (size_t i = 0; i < count; i++) {
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

static esp_err_t parse_mping_reply_line(const char *line,
                                        modem_ping_reply_t *reply)
{
    ESP_RETURN_ON_FALSE(line && reply, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const size_t prefix_len = sizeof(ML307R_MPING_PREFIX) - 1U;
    if (strncmp(line, ML307R_MPING_PREFIX, prefix_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = line + prefix_len;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (strncmp(cursor, "\"statistics\"", sizeof("\"statistics\"") - 1U) == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t result = 0;
    esp_err_t ret = parse_mping_uint(&cursor, UINT32_MAX, &result);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING result");

    modem_ping_reply_t parsed = {0};
    if (result != 0) {
        parsed.success = false;
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == ',') {
            cursor++;
            while (*cursor) {
                if (*cursor == '\r' || *cursor == '\n') {
                    return ESP_ERR_INVALID_RESPONSE;
                }
                cursor++;
            }
        } else if (*cursor != '\0') {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *reply = parsed;
        return ESP_OK;
    }

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

    if (*cursor != '"') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;
    const char *ip_start = cursor;
    const char *ip_end = strchr(ip_start, '"');
    if (!ip_end || ip_end == ip_start ||
        (size_t)(ip_end - ip_start) >= sizeof(parsed.ip)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor = ip_end + 1;

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t packet_len = 0;
    ret = parse_mping_uint(&cursor, 1400U, &packet_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING packet length");
    if (packet_len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    ret = parse_mping_uint(&cursor, UINT32_MAX, &parsed.time_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING reply time");

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t ttl = 0;
    ret = parse_mping_uint(&cursor, UINT8_MAX, &ttl);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING ttl");
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memcpy(parsed.ip, ip_start, (size_t)(ip_end - ip_start));
    parsed.ip[ip_end - ip_start] = '\0';
    parsed.ttl = (uint8_t)ttl;
    parsed.success = true;
    *reply = parsed;
    return ESP_OK;
}

static esp_err_t parse_mping_statistics_line(const char *line,
                                             modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(line && summary, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const size_t prefix_len = sizeof(ML307R_MPING_PREFIX) - 1U;
    if (strncmp(line, ML307R_MPING_PREFIX, prefix_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *cursor = line + prefix_len;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (strncmp(cursor, "\"statistics\"", sizeof("\"statistics\"") - 1U) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor += sizeof("\"statistics\"") - 1U;

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t sent = 0;
    esp_err_t ret = parse_mping_uint(&cursor, UINT8_MAX, &sent);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING sent count");

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    uint32_t lost = 0;
    ret = parse_mping_uint(&cursor, UINT8_MAX, &lost);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING lost count");
    if (lost > sent) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    modem_ping_summary_t parsed = {
        .sent = (uint8_t)sent,
        .received = (uint8_t)(sent - lost),
        .lost = (uint8_t)lost,
    };
    ret = parse_mping_uint(&cursor, UINT32_MAX, &parsed.min_time_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING min RTT");

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    ret = parse_mping_uint(&cursor, UINT32_MAX, &parsed.max_time_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING max RTT");

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != ',') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cursor++;

    ret = parse_mping_uint(&cursor, UINT32_MAX, &parsed.avg_time_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid +MPING avg RTT");
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (parsed.min_time_ms > parsed.max_time_ms) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (parsed.received > 0 &&
        (parsed.avg_time_ms < parsed.min_time_ms ||
         parsed.avg_time_ms > parsed.max_time_ms)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *summary = parsed;
    return ESP_OK;
}

static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request)
{
    if (!request) {
        return ML307R_DEFAULT_CMD_TIMEOUT_MS;
    }

    uint32_t timeout_s = ((uint32_t)request->timeout_100ms + 9U) / 10U;
    if (timeout_s == 0) {
        timeout_s = 1;
    }

    uint32_t derived_ms = (uint32_t)request->count * timeout_s * 1000U +
                          ML307R_MPING_CMD_OVERHEAD_MS;
    if (request->total_timeout_ms != 0 && request->total_timeout_ms > derived_ms) {
        return request->total_timeout_ms;
    }

    return derived_ms;
}
