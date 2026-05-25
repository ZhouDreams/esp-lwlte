/**
 * @file at_engine.h
 * @brief AT 引擎层间接口
 * @details AT engine inter-layer interface
 * @author JovisDreams
 * @date 2026-05-22
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**
 * @brief 不把标准 OK 作为最终成功响应
 * @details Do not treat standard OK as a final success response
 * @note 标准 OK 将作为中间响应处理。
 */
#define AT_CMD_FLAG_NO_STANDARD_OK_FINAL        (1U << 0)

/**
 * @brief 跳过中间 OK 响应行
 * @details Skip intermediate OK response line
 * @note 仅当标准 OK 为中间响应时生效，并阻止该 OK 存入 response.lines。
 */
#define AT_CMD_FLAG_SKIP_INTERMEDIATE_OK        (1U << 1)

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief AT 引擎配置
 * @details AT engine configuration
 */
typedef struct {
    uart_port_t uart_num;               /**< UART 端口号； UART port number */
    int tx_pin;                         /**< TX GPIO； TX GPIO */
    int rx_pin;                         /**< RX GPIO； RX GPIO */
    int baud_rate;                      /**< 波特率； Baud rate */
    int rx_buf_size;                    /**< UART RX 环形缓冲区大小； UART RX ring buffer size */
    int rx_task_stack;                  /**< 接收任务栈大小； RX task stack size */
    int rx_task_priority;               /**< 接收任务优先级； RX task priority */
    int rx_line_buf_size;               /**< 单行最大长度； Maximum line length */
    int cmd_default_timeout_ms;         /**< 默认命令超时； Default command timeout */
    int max_response_lines;             /**< 单次响应最大行数； Maximum response lines */
} at_engine_config_t;

/**
 * @brief AT 引擎句柄
 * @details AT engine handle
 */
typedef struct at_engine at_engine_t;

/**
 * @brief AT 响应状态
 * @details AT response status
 */
typedef enum {
    AT_RESP_OK = 0,                     /**< 成功终止响应； Successful final response */
    AT_RESP_ERROR,                      /**< 收到 ERROR； ERROR received */
    AT_RESP_CME_ERROR,                  /**< 收到 +CME ERROR； +CME ERROR received */
    AT_RESP_CMS_ERROR,                  /**< 收到 +CMS ERROR； +CMS ERROR received */
    AT_RESP_TIMEOUT,                    /**< 命令超时； Command timeout */
    AT_RESP_ABORTED,                    /**< 命令被中止； Command aborted */
} at_response_status_t;

/**
 * @brief AT 命令响应
 * @details AT command response
 * @note 调用方负责分配 lines 指针数组，且 max_lines 必须大于 0。
 * @note AT 引擎将 lines[i] 指向引擎拥有的字符串，调用方不得释放或修改。
 * @note lines[i] 指向的字符串在同一引擎实例下一次发送命令前有效。
 */
typedef struct {
    at_response_status_t status;         /**< 响应状态； Response status */
    int error_code;                      /**< CME/CMS 错误码； CME/CMS error code */
    int line_count;                      /**< 数据行数； Data line count */
    int max_lines;                       /**< lines 数组容量； Lines array capacity */
    char **lines;                        /**< 数据行指针数组； Data line pointer array */
} at_response_t;

/**
 * @brief AT 命令成功匹配类型
 * @details AT command success match type
 */
typedef enum {
    AT_CMD_SUCCESS_MATCH_EXACT = 0,      /**< 完整匹配； Exact match */
    AT_CMD_SUCCESS_MATCH_PREFIX,         /**< 前缀匹配； Prefix match */
    AT_CMD_SUCCESS_MATCH_ANY_LINE,       /**< 任意非错误响应行； Any non-error response line */
} at_cmd_success_match_type_t;

/**
 * @brief AT 命令成功匹配规则
 * @details AT command success match rule
 * @note AT_CMD_SUCCESS_MATCH_EXACT 和 AT_CMD_SUCCESS_MATCH_PREFIX 的 value 必须非 NULL 且非空。
 * @note AT_CMD_SUCCESS_MATCH_ANY_LINE 忽略 value。
 */
typedef struct {
    at_cmd_success_match_type_t type;     /**< 匹配类型； Match type */
    const char *value;                    /**< 匹配文本，ANY_LINE 时忽略； Match text, ignored for ANY_LINE */
} at_cmd_success_match_t;

/**
 * @brief AT 命令选项
 * @details AT command options
 * @note at_engine_send_cmd_with_options() 的 options 参数必须非 NULL。
 * @note success_match_count == 0 合法，表示不使用自定义成功匹配规则。
 * @note success_matches 仅在 success_match_count == 0 时可为 NULL。
 * @note success_matches 指向的数组在 at_engine_send_cmd_with_options() 返回前必须保持有效。
 */
typedef struct {
    uint32_t timeout_ms;                                  /**< 超时时间，0 表示使用默认值； Timeout, 0 uses default */
    uint32_t flags;                                       /**< AT_CMD_FLAG_* 标志； AT_CMD_FLAG_* flags */
    const at_cmd_success_match_t *success_matches;        /**< 自定义成功匹配规则数组； Custom success match rules */
    int success_match_count;                              /**< 自定义成功匹配规则数量； Custom success match count */
} at_cmd_options_t;

/**
 * @brief URC 回调函数
 * @details URC callback function
 * @note 回调在 AT 引擎 RX 任务中同步执行，必须短小且非阻塞。
 * @note 回调不得在同一引擎实例上调用会获取引擎锁的 AT Engine API，包括
 *       at_engine_send_cmd()、at_engine_send_cmd_with_options()、
 *       at_engine_register_urc() 和 at_engine_unregister_urc()。
 * @param[in] prefix 匹配到的 URC 前缀
 * @param[in] line 完整 URC 行
 * @param[in] user_ctx 用户上下文
 */
typedef void (*at_urc_callback_t)(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief URC 处理器
 * @details URC handler
 * @note handler 节点由调用方拥有，注册期间必须保持有效且不得重复注册。
 * @note prefix 字符串由调用方管理，注册期间必须保持有效。
 */
typedef struct at_urc_handler {
    const char *prefix;                  /**< URC 前缀； URC prefix */
    at_urc_callback_t callback;          /**< URC 回调； URC callback */
    void *user_ctx;                      /**< 用户上下文； User context */
    struct at_urc_handler *next;         /**< 下一个节点； Next node */
} at_urc_handler_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 AT 引擎
 * @details Create AT engine
 * @param[in] config AT 引擎配置
 * @return
 *         - AT 引擎句柄: 成功
 *         - NULL: 失败
 */
at_engine_t *at_engine_create(const at_engine_config_t *config);

/**
 * @brief 销毁 AT 引擎
 * @details Destroy AT engine
 * @note 调用方必须先停止上层用户，且不得与同一句柄上的其他 AT Engine API 并发调用。
 * @param[in] me AT 引擎句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前有命令执行
 *         - ESP_FAIL: UART 驱动删除失败
 */
esp_err_t at_engine_destroy(at_engine_t *me);

/**
 * @brief 发送 AT 命令
 * @details Send AT command
 * @param[in] me AT 引擎句柄
 * @param[in] cmd AT 命令，不要求包含 CRLF
 * @param[out] response 响应对象
 * @param[in] timeout_ms 超时时间，0 表示使用默认值
 * @note response->lines 由调用方分配，response->max_lines 必须大于 0。
 * @note AT 引擎填充 response->lines[i] 为引擎拥有的字符串指针，调用方不得释放或修改。
 * @note response->lines[i] 指向的字符串在同一引擎实例下一次发送命令前有效。
 * @return
 *         - ESP_OK: 命令流程完成，AT 业务结果见 response->status
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待响应超时
 *         - ESP_FAIL: UART 写入失败
 */
esp_err_t at_engine_send_cmd(at_engine_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms);

/**
 * @brief 使用选项发送 AT 命令
 * @details Send AT command with options
 * @param[in] me AT 引擎句柄
 * @param[in] cmd AT 命令，不要求包含 CRLF
 * @param[out] response 响应对象
 * @param[in] options 单次命令选项
 * @note response->lines 由调用方分配，response->max_lines 必须大于 0。
 * @note AT 引擎填充 response->lines[i] 为引擎拥有的字符串指针，调用方不得释放或修改。
 * @note response->lines[i] 指向的字符串在同一引擎实例下一次发送命令前有效。
 * @note options 必须非 NULL。
 * @note options->success_matches 指向的数组在函数返回前必须保持有效。
 * @return
 *         - ESP_OK: 命令流程完成，AT 业务结果见 response->status
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待响应超时
 *         - ESP_FAIL: UART 写入失败
 */
esp_err_t at_engine_send_cmd_with_options(at_engine_t *me, const char *cmd,
                                          at_response_t *response,
                                          const at_cmd_options_t *options);

/**
 * @brief 注册 URC 处理器
 * @details Register URC handler
 * @param[in] me AT 引擎句柄
 * @param[in] prefix URC 前缀
 * @param[in] handler URC 处理器节点，生命周期由调用方管理
 * @note handler 和 prefix 均由调用方管理，注销前必须保持有效。
 * @note 同一 handler 节点不可同时注册多次。
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 前缀/节点已注册或正在销毁
 */
esp_err_t at_engine_register_urc(at_engine_t *me, const char *prefix,
                                 at_urc_handler_t *handler);

/**
 * @brief 注销 URC 处理器
 * @details Unregister URC handler
 * @param[in] me AT 引擎句柄
 * @param[in] prefix URC 前缀
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 正在销毁
 *         - ESP_ERR_NOT_FOUND: 未找到匹配前缀
 */
esp_err_t at_engine_unregister_urc(at_engine_t *me, const char *prefix);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
