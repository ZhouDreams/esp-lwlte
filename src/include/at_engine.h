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
    AT_RESP_OK = 0,                     /**< 收到 OK； OK received */
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
 * @note lines[i] 指向的字符串在同一引擎实例下一次调用 at_engine_send_cmd() 前有效。
 */
typedef struct {
    at_response_status_t status;         /**< 响应状态； Response status */
    int error_code;                      /**< CME/CMS 错误码； CME/CMS error code */
    int line_count;                      /**< 数据行数； Data line count */
    int max_lines;                       /**< lines 数组容量； Lines array capacity */
    char **lines;                        /**< 数据行指针数组； Data line pointer array */
} at_response_t;

/**
 * @brief URC 回调函数
 * @details URC callback function
 * @note 回调在 AT 引擎 RX 任务中同步执行，必须短小且非阻塞。
 * @note 回调不得在同一引擎实例上调用会获取引擎锁的 AT Engine API，包括
 *       at_engine_send_cmd()、at_engine_register_urc() 和 at_engine_unregister_urc()。
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
 * @note response->lines[i] 指向的字符串在同一引擎实例下一次调用 at_engine_send_cmd() 前有效。
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
