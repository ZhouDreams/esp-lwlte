/**
 * @file at_engine.c
 * @brief AT 引擎实现
 * @details AT engine implementation
 * @author JovisDreams
 * @date 2026-05-22
 */

/*********************
 *      INCLUDES
 *********************/
#include "at_engine.h"

#include "sdkconfig.h"

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "at_engine"

#define AT_ENGINE_UART_EVENT_QUEUE_SIZE      10
#define AT_ENGINE_UART_TX_BUF_SIZE           0
#define AT_ENGINE_DEFAULT_RX_BUF_SIZE        2048
#define AT_ENGINE_DEFAULT_RX_TASK_STACK      4096
#define AT_ENGINE_DEFAULT_RX_TASK_PRIORITY   10
#define AT_ENGINE_DEFAULT_LINE_BUF_SIZE      256
#define AT_ENGINE_DEFAULT_TIMEOUT_MS         3000
#define AT_ENGINE_DEFAULT_MAX_RESP_LINES     101
#define AT_ENGINE_RX_WAIT_MS                 100
#define AT_ENGINE_RX_TASK_STOP_WAIT_MS       20
#define AT_ENGINE_RX_TASK_STOP_POLL_LIMIT    50

_Static_assert(AT_ENGINE_DEFAULT_MAX_RESP_LINES >= 101,
               "AT Engine default response storage must hold max CIPPING replies plus final status");

/**********************
 *      TYPEDEFS
 **********************/
/**
 * @brief AT Engine 内部状态
 * @details AT Engine internal state
 */
typedef enum {
    AT_STATE_IDLE = 0,                  /**< 空闲，无活动命令； Idle, no active command */
    AT_STATE_SENDING,                   /**< 正在写入命令； Writing command */
    AT_STATE_WAITING,                   /**< 等待响应或 prompt； Waiting for response or prompt */
    AT_STATE_RECEIVING,                 /**< 正在接收响应行； Receiving response lines */
    AT_STATE_ABORTING,                  /**< 正在中止当前命令； Aborting current command */
} at_state_t;

/**
 * @brief 当前 AT 命令上下文
 * @details Current AT command context
 * @note 该结构只在命令执行期间有效，由 at_engine_handle_t 内嵌存储，不单独分配。
 */
typedef struct {
    const char *cmd;                    /**< AT 命令字符串，借用调用方内存； AT command string, borrowed */
    const uint8_t *payload;             /**< 原始 payload，借用调用方内存； Raw payload, borrowed */
    size_t payload_len;                 /**< payload 字节数； Payload length in bytes */
    const char *payload_prompt;         /**< payload 输入提示符，借用调用方内存； Payload prompt, borrowed */
    uint32_t timeout_ms;                /**< 本次命令总超时； Total command timeout */
    esp_err_t io_error;                 /**< RX task 侧记录的 UART 写入错误； UART write error recorded by RX task */
    at_response_t *response;            /**< 响应对象，借用调用方内存； Response object, borrowed */
    at_cmd_options_t options;           /**< 命令选项副本； Command options copy */
    int echo_consumed;                  /**< 命令 echo 是否已消费； Whether command echo has been consumed */
    int data_line_index;                /**< 下一个响应行写入索引； Next response line write index */
    bool result_received;               /**< 是否已收到最终结果； Whether final result has been received */
    bool payload_sent;                  /**< payload 是否已写入 UART； Whether payload has been written to UART */
} at_cmd_ctx_t;

/**
 * @brief AT Engine 句柄实际定义
 * @details Actual definition of the opaque AT Engine handle
 * @note 该结构只在本文件可见，对外通过 at_engine_handle_t opaque pointer 暴露。
 */
struct at_engine_handle {
    /* ── Configuration ─────────────────────────────────────────── */
    at_engine_config_t config;          /**< 归一化后的配置副本； Normalized configuration copy */

    /* ── UART & RX Task ────────────────────────────────────────── */
    QueueHandle_t uart_queue;           /**< UART driver 事件队列； UART driver event queue */
    TaskHandle_t rx_task;               /**< UART RX task 句柄； UART RX task handle */
    SemaphoreHandle_t rx_task_done_sema; /**< RX task 退出完成信号； RX task exit completion signal */
    bool uart_driver_installed;         /**< UART driver 已安装标志； UART driver installed flag */
    volatile bool rx_task_stop_requested; /**< RX task 停止请求标志； RX task stop request flag */

    /* ── Synchronization ───────────────────────────────────────── */
    SemaphoreHandle_t cmd_mutex;        /**< 命令路径串行化互斥锁； Command path serialization mutex */
    SemaphoreHandle_t cmd_done_sema;    /**< 当前命令完成信号； Current command completion signal */
    SemaphoreHandle_t lock;             /**< 保护状态、上下文、缓冲和 URC 链表的互斥锁； Mutex for state, context, buffers and URC list */

    /* ── State ─────────────────────────────────────────────────── */
    at_state_t state;                   /**< 当前内部状态； Current internal state */
    bool destroying;                    /**< 正在销毁标志； Destroy in progress flag */
    int active_callers;                 /**< 已进入命令路径的调用方数量； Number of active command-path callers */

    /* ── Command Context ───────────────────────────────────────── */
    at_cmd_ctx_t cmd_ctx_storage;       /**< 当前命令上下文内嵌存储； Embedded current command context storage */
    at_cmd_ctx_t *cmd_ctx;              /**< 当前活动命令上下文，空闲时为 NULL； Active command context, NULL when idle */

    /* ── URC ───────────────────────────────────────────────────── */
    at_urc_handler_t *urc_handlers;     /**< URC handler 单向链表头； Head of URC handler singly linked list */
    int urc_handler_count;              /**< 已注册 URC handler 数量； Registered URC handler count */

    /* ── RX Line Parsing ───────────────────────────────────────── */
    char *line_buf;                     /**< RX 行组装缓冲； RX line assembly buffer */
    char *line_work_buf;                /**< 完整行处理工作缓冲； Complete-line work buffer */
    int line_buf_pos;                   /**< RX 行缓冲当前写入位置； Current RX line buffer write position */
    bool line_overflow;                 /**< 当前 RX 行已溢出标志； Current RX line overflow flag */
    uint32_t rx_epoch;                  /**< RX 输入代次，用于丢弃 flush 前旧数据； RX epoch for discarding stale data before flush */

    /* ── Response Pool ─────────────────────────────────────────── */
    char *response_pool;                /**< 响应行文本池； Response line text pool */
    int response_pool_lines;            /**< 响应文本池行数； Response text pool line count */
    int response_line_size;             /**< 单条响应文本容量； Capacity of one response text line */
};

/**********************
 *  STATIC PROTOTYPES
 **********************/
/**
 * @brief 归一化 AT Engine 配置
 * @details Normalize AT Engine configuration
 * @param[in] in 调用方输入配置
 * @param[out] out 应用默认值后的配置副本
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 配置无效
 */
static esp_err_t normalize_config(const at_engine_config_t *in, at_engine_config_t *out);

/**
 * @brief 初始化 UART driver
 * @details Initialize UART driver
 * @param[in] me AT Engine 实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: UART driver 返回的错误码
 */
static esp_err_t init_uart(at_engine_handle_t *me);

/**
 * @brief 初始化 AT Engine 内部资源
 * @details Initialize AT Engine internal resources
 * @param[in] me AT Engine 实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存或同步对象不足
 */
static esp_err_t init_resources(at_engine_handle_t *me);

/**
 * @brief 清理 AT Engine 内部资源
 * @details Clean up AT Engine internal resources
 * @param[in] me AT Engine 实例，可为 NULL
 */
static void cleanup_resources(at_engine_handle_t *me);

/**
 * @brief UART RX 任务入口
 * @details UART RX task entry
 * @note 运行在 AT Engine RX task 上，负责读取 UART 事件并驱动行解析。
 * @param[in] arg AT Engine 实例指针
 */
static void rx_task(void *arg);

/**
 * @brief 标记一次命令路径调用开始
 * @details Mark one command-path caller as active
 * @note 内部会获取 me->lock；成功后必须调用 end_send_call() 配对释放计数。
 * @param[in] me AT Engine 实例
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_STATE: 实例正在销毁
 */
static esp_err_t begin_send_call(at_engine_handle_t *me);

/**
 * @brief 标记一次命令路径调用结束
 * @details Mark one command-path caller as inactive
 * @note 内部会获取 me->lock，并减少 active_callers 计数。
 * @param[in] me AT Engine 实例
 */
static void end_send_call(at_engine_handle_t *me);

/**
 * @brief 执行通用命令发送流程
 * @details Execute common command send flow
 * @note 调用方任务中执行；负责串行化命令、建立命令上下文、等待 RX task 完成通知。
 * @param[in] me AT Engine 实例
 * @param[in] cmd AT 命令字符串
 * @param[in] payload 可选原始 payload，无 payload 时为 NULL
 * @param[in] payload_len payload 字节数
 * @param[in] payload_prompt payload 输入提示符，无 payload 时为 NULL
 * @param[out] response 响应对象
 * @param[in] options 命令选项
 * @return
 *         - ESP_OK: 命令流程完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_TIMEOUT: 等待命令路径或响应超时
 *         - ESP_FAIL: UART 写入失败
 */
static esp_err_t send_cmd_internal(at_engine_handle_t *me, const char *cmd,
                                   const uint8_t *payload, size_t payload_len,
                                   const char *payload_prompt,
                                   at_response_t *response,
                                   const at_cmd_options_t *options);

/**
 * @brief 写入 AT 命令
 * @details Write AT command
 * @note 若 cmd 未带 CR/LF，函数会自动追加 CRLF 后写入 UART。
 * @param[in] me AT Engine 实例
 * @param[in] cmd AT 命令字符串
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 临时缓冲分配失败
 *         - ESP_FAIL: UART 写入失败
 */
static esp_err_t write_cmd(at_engine_handle_t *me, const char *cmd);

/**
 * @brief 写入原始 payload
 * @details Write raw payload
 * @note 不自动追加 CRLF；调用方必须提供完整 payload 字节流。
 * @param[in] me AT Engine 实例
 * @param[in] payload payload 字节流
 * @param[in] payload_len payload 字节数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: UART 写入失败
 */
static esp_err_t write_payload(at_engine_handle_t *me, const uint8_t *payload,
                               size_t payload_len);

/**
 * @brief 将毫秒超时转换为 FreeRTOS tick
 * @details Convert timeout in milliseconds to FreeRTOS ticks
 * @param[in] timeout_ms 超时时间，单位毫秒
 * @return FreeRTOS tick 数，正超时时间至少返回 1 tick
 */
static TickType_t timeout_ticks_from_ms(uint32_t timeout_ms);

/**
 * @brief 计算剩余超时 tick
 * @details Calculate remaining timeout ticks
 * @param[in] start_ticks 起始 tick
 * @param[in] total_ticks 总超时 tick
 * @return 剩余 tick，已超时则返回 0
 */
static TickType_t remaining_timeout_ticks(TickType_t start_ticks,
                                          TickType_t total_ticks);

/**
 * @brief 重置响应对象
 * @details Reset response object
 * @note 会清空调用方提供的 lines 指针数组。
 * @param[in,out] response 响应对象
 */
static void reset_response(at_response_t *response);

/**
 * @brief 清空响应文本池
 * @details Clear response text pool
 * @note 调用方必须持有 me->lock。
 * @param[in] me AT Engine 实例
 */
static void clear_response_pool(at_engine_handle_t *me);

/**
 * @brief 清空命令完成信号
 * @details Clear command completion signal
 * @param[in] me AT Engine 实例
 */
static void clear_done_signal(at_engine_handle_t *me);

/**
 * @brief 清空 UART RX 输入和行缓冲
 * @details Flush UART RX input and line buffers
 * @note 调用方必须持有 me->lock；函数会递增 rx_epoch 使旧 RX 数据失效。
 * @param[in] me AT Engine 实例
 */
static void flush_rx_input_locked(at_engine_handle_t *me);

/**
 * @brief 处理一批 RX 字节
 * @details Process a batch of RX bytes
 * @note 由 RX task 调用；逐字节交给 process_rx_char()。
 * @param[in] me AT Engine 实例
 * @param[in] data RX 字节缓冲
 * @param[in] len RX 字节数
 * @param[in] epoch 本批数据所属 RX epoch
 */
static void process_rx_bytes(at_engine_handle_t *me, const uint8_t *data, int len, uint32_t epoch);

/**
 * @brief 处理单个 RX 字符
 * @details Process one RX character
 * @note 由 RX task 调用；负责行组装、溢出处理和裸 prompt 识别。
 * @param[in] me AT Engine 实例
 * @param[in] c RX 字符
 * @param[in] epoch 当前字符所属 RX epoch
 */
static void process_rx_char(at_engine_handle_t *me, char c, uint32_t epoch);

/**
 * @brief 处理完整响应行
 * @details Handle one complete response line
 * @note 由 RX task 调用；有活动命令时优先按命令响应处理，否则分发 URC。
 * @param[in] me AT Engine 实例
 * @param[in] line 完整响应行，不含 CR/LF
 * @param[in] epoch 当前行所属 RX epoch
 */
static void handle_line(at_engine_handle_t *me, const char *line, uint32_t epoch);

/**
 * @brief 判断响应行是否为命令 echo
 * @details Check whether a response line is command echo
 * @param[in] ctx 当前命令上下文
 * @param[in] line 响应行
 * @return true 表示匹配命令 echo，false 表示不匹配
 */
static bool is_echo_line(const at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 校验命令选项
 * @details Validate command options
 * @param[in] options 命令选项
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 选项无效
 */
static esp_err_t validate_options(const at_cmd_options_t *options);

/**
 * @brief 解析标准错误最终响应
 * @details Parse standard error final response
 * @param[in,out] response 响应对象
 * @param[in] line 响应行
 * @return true 表示 line 是 ERROR/CME/CMS 最终响应，false 表示不是
 */
static bool parse_error_result(at_response_t *response, const char *line);

/**
 * @brief 匹配自定义成功规则
 * @details Match custom success rules
 * @param[in] ctx 当前命令上下文
 * @param[in] line 响应行
 * @return true 表示任一自定义成功规则命中，false 表示未命中
 */
static bool match_custom_success(const at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 匹配单条成功规则
 * @details Match one success rule
 * @param[in] rule 成功匹配规则
 * @param[in] line 响应行
 * @return true 表示匹配成功，false 表示未匹配
 */
static bool match_success_rule(const at_cmd_success_match_t *rule, const char *line);

/**
 * @brief 判断单字符裸 payload prompt
 * @details Check single-character bare payload prompt
 * @note 用于处理模块返回不带 CR/LF 的 ">" 这类输入提示符。
 * @param[in] ctx 当前命令上下文
 * @param[in] c RX 字符
 * @return true 表示当前字符是 payload prompt，false 表示不是
 */
static bool is_bare_payload_prompt(const at_cmd_ctx_t *ctx, char c);

/**
 * @brief 判断整行 payload prompt
 * @details Check line-based payload prompt
 * @param[in] ctx 当前命令上下文
 * @param[in] line 响应行
 * @return true 表示当前行是 payload prompt，false 表示不是
 */
static bool is_payload_prompt(const at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 判断 OK 是否为中间响应
 * @details Check whether OK is an intermediate response
 * @param[in] ctx 当前命令上下文
 * @param[in] line 响应行
 * @return true 表示 OK 应按中间响应处理，false 表示 OK 是最终成功响应
 */
static bool is_intermediate_ok(const at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 解析 CME/CMS 错误码
 * @details Parse CME/CMS error code
 * @param[in] line 错误响应行
 * @return 解析出的错误码，无法解析时返回 0
 */
static int parse_error_code(const char *line);

/**
 * @brief 追加普通响应行
 * @details Append normal response line
 * @note 调用方必须持有 me->lock；超过 response 容量时静默截断。
 * @param[in] me AT Engine 实例
 * @param[in,out] ctx 当前命令上下文
 * @param[in] line 响应行
 */
static void append_response_line_locked(at_engine_handle_t *me, at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 追加最终成功匹配响应行
 * @details Append final success-matched response line
 * @note 调用方必须持有 me->lock；容量已满时覆盖最后一个槽位以保留最终匹配行。
 * @param[in] me AT Engine 实例
 * @param[in,out] ctx 当前命令上下文
 * @param[in] line 响应行
 */
static void append_final_response_line_locked(at_engine_handle_t *me, at_cmd_ctx_t *ctx, const char *line);

/**
 * @brief 完成当前命令
 * @details Finish current command
 * @note 调用方必须持有 me->lock；函数会清除 cmd_ctx、恢复 IDLE 并释放完成信号量。
 * @param[in] me AT Engine 实例
 * @param[in] status 响应状态
 * @param[in] error_code CME/CMS 错误码，无错误时为 0
 */
static void finish_cmd_locked(at_engine_handle_t *me, at_response_status_t status, int error_code);

/**
 * @brief 分发 URC 行
 * @details Dispatch URC line
 * @note 由 RX task 在无活动命令时调用；回调在持有 me->lock 时同步执行。
 * @param[in] me AT Engine 实例
 * @param[in] line URC 行
 * @param[in] epoch 当前行所属 RX epoch
 * @return true 表示找到并调用匹配 handler，false 表示未分发
 */
static bool dispatch_urc(at_engine_handle_t *me, const char *line, uint32_t epoch);

/**
 * @brief 判断字符串前缀
 * @details Check string prefix
 * @param[in] str 待检查字符串
 * @param[in] prefix 前缀字符串
 * @return true 表示 str 以前缀 prefix 开头，false 表示不是
 */
static bool starts_with(const char *str, const char *prefix);
#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO

/**
 * @brief 记录 UART IO 行
 * @details Log UART IO line
 * @note 仅在 CONFIG_LWLTE_AT_ENGINE_LOG_IO 启用时编译；会去除末尾 CR/LF 后输出。
 * @param[in] prefix 日志前缀
 * @param[in] data 数据缓冲
 * @param[in] len 数据长度
 */
static void log_uart_line(const char *prefix, const char *data, size_t len);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

at_engine_handle_t *at_engine_create(const at_engine_config_t *config)
{
    esp_err_t ret = ESP_OK;
    at_engine_config_t normalized = {0};

    ret = normalize_config(config, &normalized);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "invalid config");
        return NULL;
    }

    at_engine_handle_t *me = calloc(1, sizeof(*me));
    if (!me) {
        ESP_LOGE(TAG, "calloc at_engine failed");
        return NULL;
    }

    me->config = normalized;
    me->state = AT_STATE_IDLE;

    ret = init_resources(me);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "init resources failed");

    ret = init_uart(me);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "init UART failed");

    BaseType_t task_ret = xTaskCreate(rx_task, "at_engine_rx",
                                      me->config.rx_task_stack, me,
                                      me->config.rx_task_priority, &me->rx_task);
    ESP_GOTO_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, err, TAG, "create RX task failed");

    return me;

err:
    if (me->uart_driver_installed) {
        esp_err_t del_ret = uart_driver_delete(me->config.uart_num);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "uart_driver_delete during create rollback failed: %s", esp_err_to_name(del_ret));
        }
        me->uart_driver_installed = false;
    }
    cleanup_resources(me);
    free(me);
    return NULL;
}

esp_err_t at_engine_destroy(at_engine_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (!me->destroying) {
        if (me->active_callers > 0 || me->cmd_ctx || me->state != AT_STATE_IDLE) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
        me->destroying = true;
    } else if (me->active_callers > 0) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(me->lock);

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->rx_task_stop_requested = true;
    bool rx_task_exists = (me->rx_task != NULL);
    xSemaphoreGive(me->lock);

    if (rx_task_exists) {
        xSemaphoreTake(me->rx_task_done_sema, portMAX_DELAY);
    }

    esp_err_t ret = uart_driver_delete(me->config.uart_num);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_delete failed: %s", esp_err_to_name(ret));
        return ret;
    }
    me->uart_driver_installed = false;

    cleanup_resources(me);
    free(me);
    return ESP_OK;
}

esp_err_t at_engine_send_cmd(at_engine_handle_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms)
{
    const at_cmd_options_t options = {
        .timeout_ms = timeout_ms,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };

    return at_engine_send_cmd_with_options(me, cmd, response, &options);
}

esp_err_t at_engine_send_cmd_with_options(at_engine_handle_t *me, const char *cmd,
                                          at_response_t *response,
                                          const at_cmd_options_t *options)
{
    return send_cmd_internal(me, cmd, NULL, 0, NULL, response, options);
}

esp_err_t at_engine_send_cmd_with_payload(at_engine_handle_t *me, const char *cmd,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          const char *payload_prompt,
                                          at_response_t *response,
                                          const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(payload && payload_len > 0 && payload_prompt &&
                        payload_prompt[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "invalid payload arguments");

    return send_cmd_internal(me, cmd, payload, payload_len, payload_prompt,
                             response, options);
}

esp_err_t at_engine_begin_exclusive(at_engine_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->cmd_mutex,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = begin_send_call(me);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(me->cmd_mutex, portMAX_DELAY) != pdTRUE) {
        end_send_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->cmd_ctx || me->state != AT_STATE_IDLE) {
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t at_engine_flush_rx_exclusive(at_engine_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->cmd_ctx || me->state != AT_STATE_IDLE) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    flush_rx_input_locked(me);
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

void at_engine_end_exclusive(at_engine_handle_t *me)
{
    if (!me) {
        return;
    }

    if (me->cmd_mutex) {
        xSemaphoreGive(me->cmd_mutex);
    }
    if (me->lock) {
        end_send_call(me);
    }
}

esp_err_t at_engine_flush_rx(at_engine_handle_t *me)
{
    esp_err_t ret = at_engine_begin_exclusive(me);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = at_engine_flush_rx_exclusive(me);
    at_engine_end_exclusive(me);

    return ret;
}

esp_err_t at_engine_register_urc(at_engine_handle_t *me, const char *prefix,
                                 at_urc_handler_t *handler)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* handler 与 prefix 由调用方持有生命周期，callback 不能为空否则
     * RX task 派发 URC 时会崩溃 */
    ESP_RETURN_ON_FALSE(me && prefix && handler && handler->callback,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：加锁并做状态 / 去重校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 与 RX task 的 URC 派发以及 destroy 流程互斥访问 urc_handlers 链表 */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        /* 引擎正在销毁，不允许再向链表中追加新节点 */
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (at_urc_handler_t *it = me->urc_handlers; it; it = it->next) {
        /* 拒绝重复挂入同一节点（链表完整性），以及相同 prefix 重复注册
         * （避免一条 URC 派发到多个 handler 造成歧义） */
        if (it == handler || strcmp(it->prefix, prefix) == 0) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：头插链表并释放锁
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 头插 O(1)，新注册的 handler 优先匹配；prefix 指针由调用方保证有效 */
    handler->prefix = prefix;
    handler->next = me->urc_handlers;
    me->urc_handlers = handler;
    me->urc_handler_count++;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t at_engine_unregister_urc(at_engine_handle_t *me, const char *prefix)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ESP_RETURN_ON_FALSE(me && prefix, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：加锁并校验销毁状态
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 与 RX task 的 URC 派发以及 register/destroy 流程互斥访问链表 */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        /* destroy 流程会统一清空链表，不允许并发摘除避免链表竞争 */
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：遍历链表，按 prefix 摘除匹配节点
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 使用二级指针消除头节点 / 中间节点的特殊判断，统一摘链路径 */
    at_urc_handler_t **link = &me->urc_handlers;
    while (*link) {
        at_urc_handler_t *node = *link;
        if (strcmp(node->prefix, prefix) == 0) {
            /* 摘链后清空 next，方便调用方安全复用或重新注册该节点 */
            *link = node->next;
            node->next = NULL;
            me->urc_handler_count--;
            xSemaphoreGive(me->lock);
            return ESP_OK;
        }
        link = &node->next;
    }
    /* 未找到匹配 prefix；handler 节点所有权仍在调用方手中 */
    xSemaphoreGive(me->lock);
    return ESP_ERR_NOT_FOUND;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static esp_err_t validate_options(const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(options, ESP_ERR_INVALID_ARG, TAG, "options is NULL");
    ESP_RETURN_ON_FALSE(options->success_match_count >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid success_match_count");
    ESP_RETURN_ON_FALSE(options->success_match_count == 0 || options->success_matches,
                        ESP_ERR_INVALID_ARG, TAG, "success_matches is NULL");

    for (int i = 0; i < options->success_match_count; i++) {
        const at_cmd_success_match_t *rule = &options->success_matches[i];
        if (rule->type == AT_CMD_SUCCESS_MATCH_EXACT ||
            rule->type == AT_CMD_SUCCESS_MATCH_PREFIX) {
            ESP_RETURN_ON_FALSE(rule->value && rule->value[0] != '\0',
                                ESP_ERR_INVALID_ARG, TAG, "empty success match value");
        } else {
            ESP_RETURN_ON_FALSE(rule->type == AT_CMD_SUCCESS_MATCH_ANY_LINE,
                                ESP_ERR_INVALID_ARG, TAG, "invalid success match type");
        }
    }

    return ESP_OK;
}

static esp_err_t begin_send_call(at_engine_handle_t *me)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->active_callers++;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

static void end_send_call(at_engine_handle_t *me)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_callers > 0) {
        me->active_callers--;
    }
    xSemaphoreGive(me->lock);
}

static esp_err_t send_cmd_internal(at_engine_handle_t *me, const char *cmd,
                                   const uint8_t *payload, size_t payload_len,
                                   const char *payload_prompt,
                                   at_response_t *response,
                                   const at_cmd_options_t *options)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验与登记
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ESP_RETURN_ON_FALSE(me && cmd && response && options,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(response->lines && response->max_lines > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid response lines");

    esp_err_t ret = validate_options(options);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid command options");

    /* 登记 active_callers，阻止 destroy 期间释放资源 */
    ret = begin_send_call(me);
    if (ret != ESP_OK) {
        return ret;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：计算超时并获取命令串行锁
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    uint32_t wait_ms = options->timeout_ms ? options->timeout_ms :
                       (uint32_t)me->config.cmd_default_timeout_ms;
    if (wait_ms == 0) {
        end_send_call(me);
        return ESP_ERR_INVALID_ARG;
    }
    TickType_t total_timeout_ticks = timeout_ticks_from_ms(wait_ms);
    TickType_t start_ticks = xTaskGetTickCount();

    /* cmd_mutex 保证同一时刻只有一个调用方进入命令流程 */
    if (xSemaphoreTake(me->cmd_mutex, total_timeout_ticks) != pdTRUE) {
        end_send_call(me);
        return ESP_ERR_TIMEOUT;
    }

    /* 拿到锁后计算剩余超时；如果等锁本身已耗尽时间则直接返回 */
    TickType_t remaining_ticks = remaining_timeout_ticks(start_ticks,
                                                        total_timeout_ticks);
    if (remaining_ticks == 0) {
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ESP_ERR_TIMEOUT;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：建立命令上下文
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 预设 response 为超时状态（最坏情况预设），清空残留信号量 */
    reset_response(response);
    clear_done_signal(me);

    at_cmd_ctx_t *ctx = &me->cmd_ctx_storage;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    clear_response_pool(me);
    *ctx = (at_cmd_ctx_t) {
        .cmd = cmd,
        .payload = payload,
        .payload_len = payload_len,
        .payload_prompt = payload_prompt,
        .timeout_ms = wait_ms,
        .io_error = ESP_OK,
        .response = response,
        .options = *options,
        .echo_consumed = 0,
        .data_line_index = 0,
        .result_received = false,
        .payload_sent = false,
    };
    me->cmd_ctx = ctx;
    me->state = AT_STATE_SENDING;
    xSemaphoreGive(me->lock);

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 4：写入 AT 命令到 UART
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ret = write_cmd(me, cmd);
    if (ret != ESP_OK) {
        /* 写入失败：回滚上下文、flush RX、释放所有锁后返回 */
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
        flush_rx_input_locked(me);
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ret;
    }

    /* 写入成功，迁移到 WAITING 状态 */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->state = AT_STATE_WAITING;
    }
    xSemaphoreGive(me->lock);

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 5：阻塞等待 RX task 通过 cmd_done_sema 唤醒
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    remaining_ticks = remaining_timeout_ticks(start_ticks, total_timeout_ticks);
    if (xSemaphoreTake(me->cmd_done_sema, remaining_ticks) != pdTRUE) {
        /* 超时路径：标记超时、清除上下文、递增 rx_epoch 丢弃残留数据 */
        xSemaphoreTake(me->lock, portMAX_DELAY);
        response->status = AT_RESP_TIMEOUT;
        response->error_code = 0;
        if (me->cmd_ctx == ctx) {
            me->cmd_ctx = NULL;
        }
        flush_rx_input_locked(me);
        me->state = AT_STATE_IDLE;
        xSemaphoreGive(me->lock);
        clear_done_signal(me);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ESP_ERR_TIMEOUT;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 6：命令完成，收尾清理
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    esp_err_t io_error = ESP_OK;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    io_error = ctx->io_error;
    if (me->cmd_ctx == ctx) {
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
    }
    xSemaphoreGive(me->lock);

    xSemaphoreGive(me->cmd_mutex);
    end_send_call(me);
    if (io_error != ESP_OK) {
        return io_error;
    }
    return ESP_OK;
}

static TickType_t timeout_ticks_from_ms(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (ticks == 0 && timeout_ms > 0) {
        ticks = 1;
    }
    return ticks;
}

static TickType_t remaining_timeout_ticks(TickType_t start_ticks,
                                          TickType_t total_ticks)
{
    TickType_t elapsed_ticks = xTaskGetTickCount() - start_ticks;
    if (elapsed_ticks >= total_ticks) {
        return 0;
    }
    return total_ticks - elapsed_ticks;
}

static void rx_task(void *arg)
{
    at_engine_handle_t *me = (at_engine_handle_t *)arg;
    uint8_t rx_buf[128];
    uart_event_t event;

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 主循环：UART 事件驱动的 RX 字节派发
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    while (!me->rx_task_stop_requested) {
        /*──── 步骤 1：阻塞等待 UART 事件 ────
         * 带超时以便周期性检查停止标志，否则 destroy 流程无法及时唤醒退出 */
        if (xQueueReceive(me->uart_queue, &event, pdMS_TO_TICKS(AT_ENGINE_RX_WAIT_MS)) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            /*──── 步骤 2：分块读取 UART 数据，并通过 epoch 双重校验后派发 ────*/
            int to_read = event.size;
            while (to_read > 0) {
                int chunk = to_read > (int)sizeof(rx_buf) ? (int)sizeof(rx_buf) : to_read;

                /* 读之前快照 epoch：若期间发生 flush / destroy，epoch 会被递增 */
                xSemaphoreTake(me->lock, portMAX_DELAY);
                uint32_t read_epoch = me->rx_epoch;
                xSemaphoreGive(me->lock);

                int len = uart_read_bytes(me->config.uart_num, rx_buf, chunk,
                                          pdMS_TO_TICKS(AT_ENGINE_RX_WAIT_MS));
                if (len <= 0) {
                    /* 驱动层无数据 / 出错，结束本批，下一轮事件再处理 */
                    break;
                }

                /* 再次取 epoch 与快照对比：不一致说明这批字节属于已作废的轮次，
                 * 直接丢弃，避免污染下一条命令的解析上下文 */
                xSemaphoreTake(me->lock, portMAX_DELAY);
                uint32_t epoch = me->rx_epoch;
                xSemaphoreGive(me->lock);
                if (epoch == read_epoch) {
                    /* 唯一进入按行解析 / URC 派发 / payload 提示符识别的入口 */
                    process_rx_bytes(me, rx_buf, len, epoch);
                }
                to_read -= len;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            /*──── 步骤 3：UART 溢出 ────
             * flush 行缓冲并递增 epoch，丢弃残留半行避免污染下一条命令 */
            ESP_LOGW(TAG, "UART RX overflow, flushing input");
            xSemaphoreTake(me->lock, portMAX_DELAY);
            flush_rx_input_locked(me);
            xSemaphoreGive(me->lock);
        }
        /* 其它 UART 事件类型（如 PARITY_ERR、FRAME_ERR）暂不处理，静默忽略 */
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 退出收尾：通知 destroy 路径并自删任务
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 在锁内清空 rx_task 句柄，destroy 路径据此判断任务已退出 */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->rx_task = NULL;
    xSemaphoreGive(me->lock);
    /* 释放 done 信号量，让 at_engine_destroy() 安全 join 后继续释放队列 / 互斥量 */
    xSemaphoreGive(me->rx_task_done_sema);
    vTaskDelete(NULL);
}

static void process_rx_bytes(at_engine_handle_t *me, const uint8_t *data, int len, uint32_t epoch)
{
    /* 字节数组的 trampoline：逐字节交给 process_rx_char 做行解析；
     * epoch 透传下去，让每个字符都能在临界区内做幂等校验，
     * 防止本批次中途发生 flush / destroy 后仍把残留字节并入行缓冲 */
    for (int i = 0; i < len; i++) {
        process_rx_char(me, (char)data[i], epoch);
    }
}

static void process_rx_char(at_engine_handle_t *me, char c, uint32_t epoch)
{
    /* 局部捕获用于"在临界区外"调用 write_payload 时的快照（避免持锁阻塞写 UART） */
    bool line_ready = false;
    at_cmd_ctx_t *payload_ctx = NULL;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：加锁并做 epoch 幂等校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    xSemaphoreTake(me->lock, portMAX_DELAY);

    /* 与 rx_task 的双重 epoch 校验形成最终防线：若 flush 在 rx_task 校验之后
     * 才发生，本字符仍可能携带过期数据，丢弃避免污染行缓冲 */
    if (epoch != me->rx_epoch) {
        xSemaphoreGive(me->lock);
        return;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：裸 payload prompt 快速路径
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 部分模组的 payload 提示符（如 "> "）不带 \r\n，无法走行解析；
     * 仅在行缓冲为空时识别裸 prompt 字符，避免误伤普通响应行内的 '>' */
    if (me->line_buf_pos == 0 && is_bare_payload_prompt(me->cmd_ctx, c)) {
        /* 快照 payload 引用并预先标记 payload_sent，防止重复触发 */
        payload_ctx = me->cmd_ctx;
        payload = payload_ctx->payload;
        payload_len = payload_ctx->payload_len;
        payload_ctx->payload_sent = true;
        /* 释放锁后再写 payload：write_payload 内部会阻塞 UART，
         * 持锁写会阻塞所有 register/send/destroy 调用方 */
        xSemaphoreGive(me->lock);

        esp_err_t payload_ret = write_payload(me, payload, payload_len);
        if (payload_ret != ESP_OK) {
            /* 写 payload 失败：重新加锁后回滚 ctx 并以 ERROR 结束当前命令 */
            xSemaphoreTake(me->lock, portMAX_DELAY);
            if (epoch == me->rx_epoch && me->cmd_ctx == payload_ctx) {
                payload_ctx->io_error = payload_ret;
                finish_cmd_locked(me, AT_RESP_ERROR, 0);
            }
            xSemaphoreGive(me->lock);
        }
        return;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：按字符驱动的行缓冲状态机
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 协议行尾固定为 "\r\n"，单独的 '\r' 不视为分隔符，直接吞掉 */
    if (c == '\r') {
        xSemaphoreGive(me->lock);
        return;
    }

    /* overflow 模式：之前某一行超过 rx_line_buf_size，丢弃剩余字节直到下一个 '\n' */
    if (me->line_overflow) {
        if (c == '\n') {
            /* 撞到行尾，复位 overflow 状态，从下一行重新开始累 */
            me->line_overflow = false;
            me->line_buf_pos = 0;
            me->line_buf[0] = '\0';
        }
        xSemaphoreGive(me->lock);
        return;
    }

    /* '\n' 触发行交付：拷贝到 work buffer，复位 line_buf，释放锁后再 handle_line，
     * 避免在 handle_line 期间持有 lock 调用 dispatch_urc 等可能耗时的回调 */
    if (c == '\n') {
        if (me->line_buf_pos > 0) {
            me->line_buf[me->line_buf_pos] = '\0';
            memcpy(me->line_work_buf, me->line_buf, (size_t)me->line_buf_pos + 1U);
            line_ready = true;
        }
        me->line_buf_pos = 0;
        me->line_buf[0] = '\0';
        me->line_overflow = false;
        xSemaphoreGive(me->lock);
        if (line_ready) {
#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
            log_uart_line("RX", me->line_work_buf, strlen(me->line_work_buf));
#endif
            handle_line(me, me->line_work_buf, epoch);
        }
        return;
    }

    /* 缓冲将满（预留 1 字节给 '\0'）：丢弃当前行并进入 overflow 模式，
     * 等待下一个 '\n' 才能恢复正常累行 */
    if (me->line_buf_pos + 1 >= me->config.rx_line_buf_size) {
        ESP_LOGW(TAG, "RX line too long, drop current line");
        me->line_buf_pos = 0;
        me->line_buf[0] = '\0';
        me->line_overflow = true;
        xSemaphoreGive(me->lock);
        return;
    }

    /* 普通字符追加到行缓冲并保持 NUL 结尾，方便随时被诊断代码读取 */
    me->line_buf[me->line_buf_pos++] = c;
    me->line_buf[me->line_buf_pos] = '\0';
    xSemaphoreGive(me->lock);
}

static void handle_line(at_engine_handle_t *me, const char *line, uint32_t epoch)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：加锁并做 epoch 幂等校验
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 与 process_rx_char 同样的最终防线：从 char 处理到本函数之间
     * 仍可能发生 flush / destroy，epoch 不一致直接丢弃该行 */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (epoch != me->rx_epoch) {
        xSemaphoreGive(me->lock);
        return;
    }

    /* 快照当前活动命令上下文：非 NULL → 命令响应模式；NULL → URC 模式 */
    at_cmd_ctx_t *ctx = me->cmd_ctx;

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：命令响应模式 — 按优先级分类处理
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    if (ctx) {
        /*──── 2a：吞掉本命令的首次回显 ────
         * 大多数模组开机默认 ATE1，命令首行会被原样回显，
         * echo_consumed 保证每个命令只吞一次，避免把后续同字串的响应误判为回显 */
        if (!ctx->echo_consumed && is_echo_line(ctx, line)) {
            ctx->echo_consumed = 1;
            xSemaphoreGive(me->lock);
            return;
        }

        /*──── 2b：错误响应（ERROR / +CME ERROR / +CMS ERROR）→ 终结命令 ────
         * parse_error_result 内部已把 status 与 error_code 写入 response */
        if (parse_error_result(ctx->response, line)) {
            finish_cmd_locked(me, ctx->response->status, ctx->response->error_code);
            xSemaphoreGive(me->lock);
            return;
        }

        /*──── 2c：独立成行的 payload prompt（如 "> "）→ 释放锁后写 payload ────
         * 与 process_rx_char 的"裸 prompt 快速路径"对应，处理带 \r\n 的 prompt 形式 */
        if (is_payload_prompt(ctx, line)) {
            const uint8_t *payload = ctx->payload;
            size_t payload_len = ctx->payload_len;
            ctx->payload_sent = true;
            /* 释放锁再写 payload，避免持锁阻塞 UART 影响并发调用方 */
            xSemaphoreGive(me->lock);

            esp_err_t payload_ret = write_payload(me, payload, payload_len);
            if (payload_ret != ESP_OK) {
                /* 写失败：重新加锁后回滚 ctx 并以 ERROR 结束当前命令 */
                xSemaphoreTake(me->lock, portMAX_DELAY);
                if (epoch == me->rx_epoch && me->cmd_ctx == ctx) {
                    ctx->io_error = payload_ret;
                    finish_cmd_locked(me, AT_RESP_ERROR, 0);
                }
                xSemaphoreGive(me->lock);
            }
            return;
        }

        /*──── 2d：纯 "OK" ────
         * 默认视为命令终结；除非命令设了 NO_STANDARD_OK_FINAL flag（如某些
         * 数据传输命令在数据之前先返回 OK 作为中间结果） */
        if (strcmp(line, "OK") == 0) {
            if (!is_intermediate_ok(ctx, line)) {
                finish_cmd_locked(me, AT_RESP_OK, 0);
                xSemaphoreGive(me->lock);
                return;
            }
            /* 中间 OK：根据 SKIP_INTERMEDIATE_OK flag 决定丢弃还是保留进响应池 */
            if ((ctx->options.flags & AT_CMD_FLAG_SKIP_INTERMEDIATE_OK) != 0) {
                xSemaphoreGive(me->lock);
                return;
            }
            append_response_line_locked(me, ctx, line);
            xSemaphoreGive(me->lock);
            return;
        }

        /*──── 2e：自定义成功标记（success_match） ────
         * 某些命令不以 "OK" 结束（如 SEND OK / CONNECT），由调用方在 options 注册
         * 匹配模式；命中后把本行作为最终一行落入响应池并以 OK 状态结束 */
        if (match_custom_success(ctx, line)) {
            append_final_response_line_locked(me, ctx, line);
            finish_cmd_locked(me, AT_RESP_OK, 0);
            xSemaphoreGive(me->lock);
            return;
        }

        /*──── 2f：默认 → 累积为中间响应行 ────
         * 命令的数据行（如 +CSQ: 19,99）暂存到响应池，等终结时统一交给调用方 */
        append_response_line_locked(me, ctx, line);
        xSemaphoreGive(me->lock);
        return;
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：URC 模式 — 没有活动命令时按 prefix 派发到注册的 handler
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* 释放锁再 dispatch_urc：dispatch_urc 内部会持锁遍历链表并同步调用 callback，
     * 此处提前释放避免锁的嵌套获取与持锁时间过长 */
    xSemaphoreGive(me->lock);
    (void)dispatch_urc(me, line, epoch);
}

static bool is_echo_line(const at_cmd_ctx_t *ctx, const char *line)
{
    if (!ctx || !ctx->cmd || !line) {
        return false;
    }

    size_t cmd_len = strlen(ctx->cmd);
    while (cmd_len > 0 && (ctx->cmd[cmd_len - 1] == '\r' || ctx->cmd[cmd_len - 1] == '\n')) {
        cmd_len--;
    }

    return strlen(line) == cmd_len && strncmp(line, ctx->cmd, cmd_len) == 0;
}

static bool parse_error_result(at_response_t *response, const char *line)
{
    if (strcmp(line, "ERROR") == 0) {
        response->status = AT_RESP_ERROR;
        response->error_code = 0;
        return true;
    }
    if (starts_with(line, "+CME ERROR:")) {
        response->status = AT_RESP_CME_ERROR;
        response->error_code = parse_error_code(line);
        return true;
    }
    if (starts_with(line, "+CMS ERROR:")) {
        response->status = AT_RESP_CMS_ERROR;
        response->error_code = parse_error_code(line);
        return true;
    }
    return false;
}

static bool is_payload_prompt(const at_cmd_ctx_t *ctx, const char *line)
{
    return ctx && ctx->payload && !ctx->payload_sent && ctx->payload_prompt &&
           strcmp(line, ctx->payload_prompt) == 0;
}

static bool is_bare_payload_prompt(const at_cmd_ctx_t *ctx, char c)
{
    return ctx && ctx->payload && !ctx->payload_sent && ctx->payload_prompt &&
           ctx->payload_prompt[0] == c && ctx->payload_prompt[1] == '\0';
}

static bool is_intermediate_ok(const at_cmd_ctx_t *ctx, const char *line)
{
    if (!ctx || strcmp(line, "OK") != 0) {
        return false;
    }
    return (ctx->options.flags & AT_CMD_FLAG_NO_STANDARD_OK_FINAL) != 0;
}

static bool match_custom_success(const at_cmd_ctx_t *ctx, const char *line)
{
    if (!ctx || !line) {
        return false;
    }

    for (int i = 0; i < ctx->options.success_match_count; i++) {
        if (match_success_rule(&ctx->options.success_matches[i], line)) {
            return true;
        }
    }

    return false;
}

static bool match_success_rule(const at_cmd_success_match_t *rule, const char *line)
{
    if (!rule || !line) {
        return false;
    }

    switch (rule->type) {
    case AT_CMD_SUCCESS_MATCH_EXACT:
        return strcmp(line, rule->value) == 0;
    case AT_CMD_SUCCESS_MATCH_PREFIX:
        return starts_with(line, rule->value);
    case AT_CMD_SUCCESS_MATCH_ANY_LINE:
        return true;
    default:
        return false;
    }
}

static int parse_error_code(const char *line)
{
    const char *colon = strchr(line, ':');
    if (!colon) {
        return 0;
    }
    return atoi(colon + 1);
}

static void append_response_line_locked(at_engine_handle_t *me, at_cmd_ctx_t *ctx, const char *line)
{
    int limit = ctx->response->max_lines;
    if (limit > me->response_pool_lines) {
        limit = me->response_pool_lines;
    }
    if (ctx->data_line_index >= limit) {
        return;
    }

    char *dst = me->response_pool + ((size_t)ctx->data_line_index * (size_t)me->response_line_size);
    strlcpy(dst, line, (size_t)me->response_line_size);
    ctx->response->lines[ctx->data_line_index] = dst;
    ctx->data_line_index++;
    ctx->response->line_count = ctx->data_line_index;
}

static void append_final_response_line_locked(at_engine_handle_t *me, at_cmd_ctx_t *ctx, const char *line)
{
    int limit = ctx->response->max_lines;
    if (limit > me->response_pool_lines) {
        limit = me->response_pool_lines;
    }
    if (limit <= 0) {
        return;
    }
    if (ctx->data_line_index < limit) {
        append_response_line_locked(me, ctx, line);
        return;
    }

    char *dst = me->response_pool + ((size_t)(limit - 1) * (size_t)me->response_line_size);
    strlcpy(dst, line, (size_t)me->response_line_size);
    ctx->response->lines[limit - 1] = dst;
    ctx->response->line_count = limit;
}

static void finish_cmd_locked(at_engine_handle_t *me, at_response_status_t status, int error_code)
{
    if (me->cmd_ctx && me->cmd_ctx->response) {
        me->cmd_ctx->response->status = status;
        me->cmd_ctx->response->error_code = error_code;
    }
    me->cmd_ctx = NULL;
    me->state = AT_STATE_IDLE;
    xSemaphoreGive(me->cmd_done_sema);
}

static bool dispatch_urc(at_engine_handle_t *me, const char *line, uint32_t epoch)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (epoch != me->rx_epoch) {
        xSemaphoreGive(me->lock);
        return false;
    }

    for (at_urc_handler_t *it = me->urc_handlers; it; it = it->next) {
        if (it->prefix && starts_with(line, it->prefix)) {
            it->callback(it->prefix, line, it->user_ctx);
            xSemaphoreGive(me->lock);
            return true;
        }
    }
    xSemaphoreGive(me->lock);
    return false;
}

static bool starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}

static esp_err_t write_cmd(at_engine_handle_t *me, const char *cmd)
{
    size_t len = strlen(cmd);
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "empty command");

    bool has_line_end = (cmd[len - 1] == '\n' || cmd[len - 1] == '\r');
    if (has_line_end) {
#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
        log_uart_line("TX", cmd, len);
#endif
        int written = uart_write_bytes(me->config.uart_num, cmd, len);
        ESP_RETURN_ON_FALSE(written == (int)len, ESP_FAIL, TAG, "uart_write_bytes failed");
        return ESP_OK;
    }

    char *buf = malloc(len + 3);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc command buffer failed");
    memcpy(buf, cmd, len);
    buf[len] = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';

#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
    log_uart_line("TX", buf, len + 2);
#endif
    int written = uart_write_bytes(me->config.uart_num, buf, len + 2);
    free(buf);
    ESP_RETURN_ON_FALSE(written == (int)(len + 2), ESP_FAIL, TAG, "uart_write_bytes failed");
    return ESP_OK;
}

static esp_err_t write_payload(at_engine_handle_t *me, const uint8_t *payload,
                               size_t payload_len)
{
    ESP_RETURN_ON_FALSE(me && payload && payload_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid payload");
#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
    log_uart_line("TX_PAYLOAD", (const char *)payload, payload_len);
#endif
    int written = uart_write_bytes(me->config.uart_num, payload, payload_len);
    ESP_RETURN_ON_FALSE(written == (int)payload_len, ESP_FAIL, TAG,
                        "uart_write_bytes payload failed");
    return ESP_OK;
}

#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
static void log_uart_line(const char *prefix, const char *data, size_t len)
{
    if (!prefix || !data) {
        return;
    }

    while (len > 0 && (data[len - 1] == '\r' || data[len - 1] == '\n')) {
        len--;
    }

    int log_len = len > (size_t)INT_MAX ? INT_MAX : (int)len;
    ESP_LOGI(TAG, "%s:|%.*s", prefix, log_len, data);
}
#endif

static void reset_response(at_response_t *response)
{
    response->status = AT_RESP_TIMEOUT;
    response->error_code = 0;
    response->line_count = 0;
    for (int i = 0; i < response->max_lines; i++) {
        response->lines[i] = NULL;
    }
}

static void clear_response_pool(at_engine_handle_t *me)
{
    if (me->response_pool) {
        memset(me->response_pool, 0,
               (size_t)me->response_pool_lines * (size_t)me->response_line_size);
    }
}

static void clear_done_signal(at_engine_handle_t *me)
{
    while (xSemaphoreTake(me->cmd_done_sema, 0) == pdTRUE) {
    }
}

static void flush_rx_input_locked(at_engine_handle_t *me)
{
    (void)uart_flush_input(me->config.uart_num);
    if (me->uart_queue) {
        xQueueReset(me->uart_queue);
    }
    me->line_buf_pos = 0;
    me->line_overflow = false;
    if (me->line_buf) {
        me->line_buf[0] = '\0';
    }
    me->rx_epoch++;
}

static esp_err_t normalize_config(const at_engine_config_t *in, at_engine_config_t *out)
{
    ESP_RETURN_ON_FALSE(in && out, ESP_ERR_INVALID_ARG, TAG, "NULL config");
    ESP_RETURN_ON_FALSE(in->uart_num >= UART_NUM_0 && in->uart_num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(in->tx_pin >= 0 && in->rx_pin >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(in->baud_rate > 0, ESP_ERR_INVALID_ARG, TAG, "invalid baud rate");

    *out = *in;
    if (out->rx_buf_size <= 0) {
        out->rx_buf_size = AT_ENGINE_DEFAULT_RX_BUF_SIZE;
    }
    if (out->rx_task_stack <= 0) {
        out->rx_task_stack = AT_ENGINE_DEFAULT_RX_TASK_STACK;
    }
    if (out->rx_task_priority <= 0) {
        out->rx_task_priority = AT_ENGINE_DEFAULT_RX_TASK_PRIORITY;
    }
    if (out->rx_line_buf_size <= 0) {
        out->rx_line_buf_size = AT_ENGINE_DEFAULT_LINE_BUF_SIZE;
    }
    if (out->cmd_default_timeout_ms <= 0) {
        out->cmd_default_timeout_ms = AT_ENGINE_DEFAULT_TIMEOUT_MS;
    }
    if (out->max_response_lines <= 0) {
        out->max_response_lines = AT_ENGINE_DEFAULT_MAX_RESP_LINES;
    } else if (out->max_response_lines < AT_ENGINE_DEFAULT_MAX_RESP_LINES) {
        out->max_response_lines = AT_ENGINE_DEFAULT_MAX_RESP_LINES;
    }

    return ESP_OK;
}

static esp_err_t init_resources(at_engine_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->cmd_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(me->cmd_mutex, ESP_ERR_NO_MEM, TAG, "create cmd_mutex failed");

    me->cmd_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->cmd_done_sema, ESP_ERR_NO_MEM, TAG, "create cmd_done_sema failed");

    me->rx_task_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->rx_task_done_sema, ESP_ERR_NO_MEM, TAG,
                        "create rx_task_done_sema failed");

    me->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(me->lock, ESP_ERR_NO_MEM, TAG, "create lock failed");

    me->line_buf = calloc(1, me->config.rx_line_buf_size);
    ESP_RETURN_ON_FALSE(me->line_buf, ESP_ERR_NO_MEM, TAG, "create line_buf failed");

    me->line_work_buf = calloc(1, me->config.rx_line_buf_size);
    ESP_RETURN_ON_FALSE(me->line_work_buf, ESP_ERR_NO_MEM, TAG, "create line_work_buf failed");

    me->response_pool_lines = me->config.max_response_lines;
    me->response_line_size = me->config.rx_line_buf_size;
    me->response_pool = calloc((size_t)me->response_pool_lines, (size_t)me->response_line_size);
    ESP_RETURN_ON_FALSE(me->response_pool, ESP_ERR_NO_MEM, TAG, "create response_pool failed");

    return ESP_OK;
}

static esp_err_t init_uart(at_engine_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    const uart_config_t uart_config = {
        .baud_rate = me->config.baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(me->config.uart_num,
                                            me->config.rx_buf_size,
                                            AT_ENGINE_UART_TX_BUF_SIZE,
                                            AT_ENGINE_UART_EVENT_QUEUE_SIZE,
                                            &me->uart_queue, 0),
                        TAG, "uart_driver_install failed");
    me->uart_driver_installed = true;
    ESP_RETURN_ON_ERROR(uart_param_config(me->config.uart_num, &uart_config),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(me->config.uart_num, me->config.tx_pin, me->config.rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    return ESP_OK;
}

static void cleanup_resources(at_engine_handle_t *me)
{
    if (!me) {
        return;
    }
    if (me->cmd_done_sema) {
        vSemaphoreDelete(me->cmd_done_sema);
        me->cmd_done_sema = NULL;
    }
    if (me->rx_task_done_sema) {
        vSemaphoreDelete(me->rx_task_done_sema);
        me->rx_task_done_sema = NULL;
    }
    if (me->cmd_mutex) {
        vSemaphoreDelete(me->cmd_mutex);
        me->cmd_mutex = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    if (me->line_buf) {
        free(me->line_buf);
        me->line_buf = NULL;
    }
    if (me->line_work_buf) {
        free(me->line_work_buf);
        me->line_work_buf = NULL;
    }
    if (me->response_pool) {
        free(me->response_pool);
        me->response_pool = NULL;
    }
}
