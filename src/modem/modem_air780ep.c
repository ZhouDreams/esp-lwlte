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
#define AIR780EP_MAX_RESPONSE_LINES      8
#define AIR780EP_PARSE_BUF_SIZE          128
#define AIR780EP_DEFAULT_CMD_TIMEOUT_MS  9000
#define AIR780EP_CSTT_TIMEOUT_MS         60000
#define AIR780EP_CIICR_TIMEOUT_MS        90000
#define AIR780EP_CIPSHUT_TIMEOUT_MS      90000
#define AIR780EP_URC_RDY                 "RDY"
#define AIR780EP_URC_CPIN                "+CPIN:"
#define AIR780EP_URC_CREG                "+CREG:"
#define AIR780EP_URC_CEREG               "+CEREG:"
#define AIR780EP_URC_CGREG               "+CGREG:"
#define AIR780EP_URC_CGEV                "+CGEV:"
#define AIR780EP_URC_PDP_DEACT           "+PDP DEACT"
#define AIR780EP_URC_PDP_COLON_DEACT     "+PDP:DEACT"

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
    modem_t base;
    modem_air780ep_config_t config;
    at_urc_handler_t rdy_handler;
    at_urc_handler_t cpin_handler;
    at_urc_handler_t creg_handler;
    at_urc_handler_t cereg_handler;
    at_urc_handler_t cgreg_handler;
    at_urc_handler_t cgev_handler;
    at_urc_handler_t pdp_deact_handler;
    at_urc_handler_t pdp_colon_deact_handler;
    modem_info_t cached_info;
    modem_sim_status_t last_sim_status;
    modem_reg_status_t last_reg_status;
    modem_signal_t last_signal;
    modem_pdp_context_t pdp[AIR780EP_MAX_PDP_CONTEXTS];
    bool urc_registered;
    bool initialized;
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
static esp_err_t air780ep_destroy(modem_t *me);

/**
 * @brief 初始化 Air780EP 调制解调器
 * @details Initialize Air780EP modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: 初始化失败
 */
static esp_err_t air780ep_init(modem_t *me);

/**
 * @brief 复位 Air780EP 调制解调器
 * @details Reset Air780EP modem
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: 复位失败
 */
static esp_err_t air780ep_reset(modem_t *me);

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
static esp_err_t air780ep_get_info(modem_t *me, modem_info_t *info);

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
static esp_err_t air780ep_get_sim_status(modem_t *me, modem_sim_status_t *status);

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
static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal);

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
static esp_err_t air780ep_get_registration(modem_t *me, modem_reg_status_t *status);

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
static esp_err_t air780ep_set_apn(modem_t *me, uint8_t cid, const char *apn);

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
static esp_err_t air780ep_activate_pdp(modem_t *me, uint8_t cid);

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
static esp_err_t air780ep_deactivate_pdp(modem_t *me, uint8_t cid);

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
static esp_err_t air780ep_get_pdp_context(modem_t *me, uint8_t cid,
                                           modem_pdp_context_t *pdp);

/**
 * @brief 转换为 Air780EP 实例
 * @details Convert to Air780EP instance
 * @param[in] me 调制解调器句柄
 * @return Air780EP 调制解调器实例
 */
static modem_air780ep_t *to_air780ep(modem_t *me);

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
 * @brief 毫秒转换为向上取整的 tick
 * @details Convert milliseconds to ticks rounded up
 * @param[in] ms 毫秒数
 * @return FreeRTOS tick 数，0 ms 返回 0，非 0 ms 至少返回 1
 */
static TickType_t ms_to_ticks_round_up(uint32_t ms);

/**
 * @brief 输出 GPIO 脉冲
 * @details Output GPIO pulse
 * @param[in] pin GPIO 编号
 * @param[in] active_ms 高电平保持时间
 * @return
 *         - ESP_OK: 成功
 *         - 其他: GPIO 错误
 */
static esp_err_t pulse_gpio(gpio_num_t pin, uint32_t active_ms);

/**
 * @brief 按需执行开机脉冲
 * @details Perform power-on pulse if needed
 * @param[in] self Air780EP 调制解调器实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: GPIO 错误
 */
static esp_err_t maybe_power_on(modem_air780ep_t *self);

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
 */
static void unregister_urcs(modem_air780ep_t *self);

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
 * @brief 处理 RDY URC
 * @details Handle RDY URC
 * @param[in] prefix URC 前缀
 * @param[in] line URC 完整行
 * @param[in] user_ctx 用户上下文
 */
static void rdy_urc_handler(const char *prefix, const char *line, void *user_ctx);

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

/**********************
 *  STATIC VARIABLES
 **********************/

static const modem_ops_t s_air780ep_ops = {
    .destroy = air780ep_destroy,
    .init = air780ep_init,
    .reset = air780ep_reset,
    .get_info = air780ep_get_info,
    .get_sim_status = air780ep_get_sim_status,
    .get_signal = air780ep_get_signal,
    .get_registration = air780ep_get_registration,
    .set_apn = air780ep_set_apn,
    .activate_pdp = air780ep_activate_pdp,
    .deactivate_pdp = air780ep_deactivate_pdp,
    .get_pdp_context = air780ep_get_pdp_context,
};

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config)
{
    if (!at || !config) {
        ESP_LOGE(TAG, "NULL argument");
        return NULL;
    }

    modem_air780ep_t *self = calloc(1, sizeof(modem_air780ep_t));
    if (!self) {
        ESP_LOGE(TAG, "calloc air780ep modem failed");
        return NULL;
    }

    self->config = *config;
    if (self->config.default_cmd_timeout_ms == 0) {
        self->config.default_cmd_timeout_ms = AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
    }

    self->last_sim_status = MODEM_SIM_UNKNOWN;
    self->last_reg_status = MODEM_REG_UNKNOWN;
    self->last_signal.rssi = 99;
    self->last_signal.ber = 99;
    self->last_signal.rssi_dbm = 0;
    self->last_signal.rssi_dbm_valid = false;

    for (int i = 0; i < AIR780EP_MAX_PDP_CONTEXTS; i++) {
        self->pdp[i].cid = i + 1;
        strcpy(self->pdp[i].pdp_type, "IP");
    }

    esp_err_t ret = modem_base_init(&self->base, "air780ep", at, &s_air780ep_ops,
                                    config->event_queue_size,
                                    config->event_task_stack,
                                    config->event_task_priority);
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

static modem_air780ep_t *to_air780ep(modem_t *me)
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

    uint32_t wait_ms = timeout_ms ? timeout_ms : self->config.default_cmd_timeout_ms;
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

static TickType_t ms_to_ticks_round_up(uint32_t ms)
{
    if (ms == 0) {
        return 0;
    }

    uint64_t ticks = ((uint64_t)ms * configTICK_RATE_HZ + 999ULL) / 1000ULL;
    if (ticks > (uint64_t)portMAX_DELAY) {
        return portMAX_DELAY;
    }

    return (TickType_t)ticks;
}

static esp_err_t pulse_gpio(gpio_num_t pin, uint32_t active_ms)
{
    if (pin == GPIO_NUM_NC) {
        return ESP_OK;
    }

    esp_err_t ret = gpio_reset_pin(pin);
    ESP_RETURN_ON_ERROR(ret, TAG, "reset GPIO %d failed", pin);

    ret = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    ESP_RETURN_ON_ERROR(ret, TAG, "set GPIO %d direction failed", pin);

    ret = gpio_set_level(pin, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "set GPIO %d low failed", pin);
    vTaskDelay(ms_to_ticks_round_up(10));

    ret = gpio_set_level(pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "set GPIO %d high failed", pin);
    if (active_ms > 0) {
        vTaskDelay(ms_to_ticks_round_up(active_ms));
    }

    ret = gpio_set_level(pin, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "set GPIO %d low failed", pin);
    return ESP_OK;
}

static esp_err_t maybe_power_on(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = pulse_gpio(self->config.pwrkey_pin, self->config.power_on_pulse_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "pulse PWRKEY failed");

    if (self->config.boot_wait_ms > 0) {
        vTaskDelay(ms_to_ticks_round_up(self->config.boot_wait_ms));
    }

    return ESP_OK;
}

static esp_err_t register_urcs(modem_air780ep_t *self)
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
        { AIR780EP_URC_RDY, &self->rdy_handler, rdy_urc_handler },
        { AIR780EP_URC_CPIN, &self->cpin_handler, cpin_urc_handler },
        { AIR780EP_URC_CREG, &self->creg_handler, reg_urc_handler },
        { AIR780EP_URC_CEREG, &self->cereg_handler, reg_urc_handler },
        { AIR780EP_URC_CGREG, &self->cgreg_handler, reg_urc_handler },
        { AIR780EP_URC_CGEV, &self->cgev_handler, cgev_urc_handler },
        { AIR780EP_URC_PDP_DEACT, &self->pdp_deact_handler, pdp_deact_urc_handler },
        { AIR780EP_URC_PDP_COLON_DEACT, &self->pdp_colon_deact_handler,
          pdp_deact_urc_handler },
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
                for (size_t j = 0; j < urc_count; j++) {
                    memset(urcs[j].handler, 0, sizeof(*urcs[j].handler));
                }
                self->urc_registered = false;
            } else {
                self->urc_registered = true;
            }
            return ret;
        }
        registered_count++;
    }

    self->urc_registered = true;
    return ESP_OK;
}

static void unregister_urcs(modem_air780ep_t *self)
{
    esp_err_t ret = air780ep_unregister_urcs(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unregister URCs failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t air780ep_destroy(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    esp_err_t ret = ESP_OK;

    if (self->urc_registered) {
        ret = air780ep_unregister_urcs(self);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    self->initialized = false;
    return ESP_OK;
}

static esp_err_t air780ep_init(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    bool urc_registered_before = self->urc_registered;
    esp_err_t ret = ESP_OK;

    ret = modem_set_state(me, MODEM_STATE_INITIALIZING);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set initializing state failed");

    ret = maybe_power_on(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "power on failed");

    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
        "AT+CGREG=2",
        "AT+CREG=2",
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        air780ep_cmd_ctx_t ctx;
        ret = send_cmd(self, cmds[i], &ctx, 0);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "send %s failed", cmds[i]);
        ret = ensure_at_ok(&ctx.response, cmds[i]);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "%s failed", cmds[i]);
    }

    self->initialized = true;

    ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set ready state failed");

    const modem_event_t event = {
        .id = MODEM_EVENT_READY,
    };
    ret = modem_post_event(me, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post ready event failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;

err:
    if (!urc_registered_before && self->urc_registered) {
        unregister_urcs(self);
    }
    self->initialized = false;
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
}

static esp_err_t air780ep_reset(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;

    esp_err_t ret = send_cmd(self, "AT+RESET", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+RESET failed");

    ret = ensure_at_ok(&ctx.response, "AT+RESET");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+RESET failed");

    self->initialized = false;
    return modem_set_state(me, MODEM_STATE_CREATED);
}

static esp_err_t air780ep_get_info(modem_t *me, modem_info_t *info)
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

static esp_err_t air780ep_get_sim_status(modem_t *me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;

    esp_err_t ret = send_cmd(self, "AT+CPIN?", &ctx, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CPIN? failed");

    ret = ensure_at_ok(&ctx.response, "AT+CPIN?");
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+CPIN? failed");

    const char *line = find_line_with_prefix(&ctx.response, "+CPIN:");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG, "+CPIN line missing");

    modem_sim_status_t parsed = parse_sim_status_line(line);

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->last_sim_status = parsed;
    xSemaphoreGive(self->base.lock);

    *status = parsed;
    return ESP_OK;
}

static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal)
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

static esp_err_t air780ep_get_registration(modem_t *me, modem_reg_status_t *status)
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

static esp_err_t air780ep_set_apn(modem_t *me, uint8_t cid, const char *apn)
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

static esp_err_t air780ep_activate_pdp(modem_t *me, uint8_t cid)
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

    modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
    esp_err_t ret = air780ep_get_sim_status(me, &sim_status);
    ESP_RETURN_ON_ERROR(ret, TAG, "get SIM status failed");
    ESP_RETURN_ON_FALSE(sim_status == MODEM_SIM_READY, ESP_ERR_INVALID_STATE,
                        TAG, "SIM not ready");

    modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
    ret = air780ep_get_registration(me, &reg_status);
    ESP_RETURN_ON_ERROR(ret, TAG, "get registration failed");
    ESP_RETURN_ON_FALSE(reg_status == MODEM_REG_REGISTERED_HOME ||
                        reg_status == MODEM_REG_REGISTERED_ROAMING,
                        ESP_ERR_INVALID_STATE, TAG, "network not registered");

    bool attached = false;
    ret = query_cgatt(self, &attached);
    ESP_RETURN_ON_ERROR(ret, TAG, "query CGATT failed");
    ESP_RETURN_ON_FALSE(attached, ESP_ERR_INVALID_STATE, TAG,
                        "packet domain not attached");

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

    air780ep_cmd_ctx_t ctx;
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
        .timeout_ms = self->config.default_cmd_timeout_ms,
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

static esp_err_t air780ep_deactivate_pdp(modem_t *me, uint8_t cid)
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
    size_t affected_count = clear_all_pdp_cache(self, affected,
                                                AIR780EP_MAX_PDP_CONTEXTS);
    xSemaphoreGive(self->base.lock);

    ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_RETURN_ON_ERROR(ret, TAG, "set ready state failed");

    post_pdp_deactivated_events(self, affected, affected_count);

    return ESP_OK;
}

static esp_err_t air780ep_get_pdp_context(modem_t *me, uint8_t cid,
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

static esp_err_t air780ep_unregister_urcs(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.at, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    const struct {
        const char *prefix;
        at_urc_handler_t *handler;
    } urcs[] = {
        { AIR780EP_URC_RDY, &self->rdy_handler },
        { AIR780EP_URC_CPIN, &self->cpin_handler },
        { AIR780EP_URC_CREG, &self->creg_handler },
        { AIR780EP_URC_CEREG, &self->cereg_handler },
        { AIR780EP_URC_CGREG, &self->cgreg_handler },
        { AIR780EP_URC_CGEV, &self->cgev_handler },
        { AIR780EP_URC_PDP_DEACT, &self->pdp_deact_handler },
        { AIR780EP_URC_PDP_COLON_DEACT, &self->pdp_colon_deact_handler },
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

static void rdy_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;
    (void)line;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    const modem_event_t event = {
        .id = MODEM_EVENT_READY,
    };
    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post ready event failed: %s", esp_err_to_name(ret));
    }
}

static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    modem_sim_status_t status = parse_sim_status_line(line);

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop +CPIN URC, lock busy");
        return;
    }
    self->last_sim_status = status;
    xSemaphoreGive(self->base.lock);

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

    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop +CGEV URC, lock busy");
        return;
    }

    modem_pdp_context_t *pdp = pdp_by_cid(self, cid);
    if (!pdp) {
        xSemaphoreGive(self->base.lock);
        return;
    }
    pdp->active = active;
    if (!active) {
        pdp->ip_addr[0] = '\0';
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
    if (!self->base.lock || xSemaphoreTake(self->base.lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop PDP deactivation URC, lock busy");
        return;
    }
    size_t affected_count = clear_all_pdp_cache(self, affected,
                                                AIR780EP_MAX_PDP_CONTEXTS);
    xSemaphoreGive(self->base.lock);

    set_state_nonblocking(self, MODEM_STATE_READY);
    post_pdp_deactivated_events(self, affected, affected_count);
}
