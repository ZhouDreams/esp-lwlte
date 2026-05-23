# AT Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the first module from `docs/agents/classes.md`: a complete ESP-IDF AT Engine layer with UART RX, blocking command send, response parsing, timeout handling, and URC dispatch.

**Architecture:** Add one layer API header and one implementation file. The implementation owns UART driver setup, one RX task, command serialization, a per-instance response text pool, and a caller-owned URC handler list. After implementation, update `docs/agents/classes.md` to reflect intentional design changes from the approved spec.

**Tech Stack:** C99, ESP-IDF 6.0, FreeRTOS task/semaphore/mutex APIs, ESP-IDF UART driver, `esp_err_t`, `esp_check.h`, project Doxygen/style templates.

---

## User Constraint

Do not create git commits unless the user explicitly asks for a commit. This overrides the usual plan pattern that includes frequent commit steps.

## File Structure

- Create: `src/include/at_engine.h`
  - Layer API for Board Init and Modem Adapter.
  - Defines `at_engine_config_t`, opaque `at_engine_t`, `at_response_t`, `at_urc_handler_t`, and public AT Engine functions.
- Create: `src/at_engine/at_engine.c`
  - Defines `struct at_engine`, internal state, UART lifecycle, RX task, command send path, response pool, line parser, and URC dispatch.
- Modify: `src/CMakeLists.txt`
  - Register `at_engine/at_engine.c` in the component source list and require `esp_driver_uart` for public `driver/uart.h` usage.
- Modify near the end: `docs/agents/classes.md`
  - Concisely sync the parts where implementation/spec intentionally differs from or expands the original class design.

## Task 1: Public API Header

**Files:**
- Create: `src/include/at_engine.h`

- [ ] **Step 1: Write the layer API header**

Create `src/include/at_engine.h` with this content:

```c
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
 * @param[in] prefix 匹配到的 URC 前缀
 * @param[in] line 完整 URC 行
 * @param[in] user_ctx 用户上下文
 */
typedef void (*at_urc_callback_t)(const char *prefix, const char *line, void *user_ctx);

/**
 * @brief URC 处理器
 * @details URC handler
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
 * @param[in] me AT 引擎句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前有命令执行
 */
esp_err_t at_engine_destroy(at_engine_t *me);

/**
 * @brief 发送 AT 命令
 * @details Send AT command
 * @param[in] me AT 引擎句柄
 * @param[in] cmd AT 命令，不要求包含 CRLF
 * @param[out] response 响应对象
 * @param[in] timeout_ms 超时时间，0 表示使用默认值
 * @return
 *         - ESP_OK: 命令流程完成，AT 业务结果见 response->status
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待响应超时
 */
esp_err_t at_engine_send_cmd(at_engine_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms);

/**
 * @brief 注册 URC 处理器
 * @details Register URC handler
 * @param[in] me AT 引擎句柄
 * @param[in] prefix URC 前缀
 * @param[in] handler URC 处理器节点，生命周期由调用方管理
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 前缀已注册
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
 *         - ESP_ERR_NOT_FOUND: 未找到匹配前缀
 */
esp_err_t at_engine_unregister_urc(at_engine_t *me, const char *prefix);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 2: Run a build check after the header exists**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build still succeeds or fails only because source registration has not happened yet. Do not edit unrelated files based on this step.

## Task 2: Source Skeleton, Types, and Lifecycle

**Files:**
- Create: `src/at_engine/at_engine.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Register the source file in CMake**

Change `src/CMakeLists.txt` to:

```cmake
idf_component_register(
    SRCS "at_engine/at_engine.c"
    INCLUDE_DIRS include
    REQUIRES esp_driver_uart
)
```

- [ ] **Step 2: Create the source skeleton with internal data model**

Create `src/at_engine/at_engine.c` with the file header, includes, constants, typedefs, static prototypes, and `struct at_engine` shown here:

```c
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
#define AT_ENGINE_DEFAULT_MAX_RESP_LINES     8
#define AT_ENGINE_RX_WAIT_MS                 100
#define AT_ENGINE_RX_TASK_STOP_WAIT_MS       20
#define AT_ENGINE_RX_TASK_STOP_POLL_LIMIT    50

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    AT_STATE_IDLE = 0,
    AT_STATE_SENDING,
    AT_STATE_WAITING,
    AT_STATE_RECEIVING,
    AT_STATE_ABORTING,
} at_state_t;

typedef struct {
    const char *cmd;
    uint32_t timeout_ms;
    at_response_t *response;
    int echo_consumed;
    int data_line_index;
    bool result_received;
} at_cmd_ctx_t;

struct at_engine {
    at_engine_config_t config;
    uart_port_t uart_num;
    QueueHandle_t uart_queue;
    TaskHandle_t rx_task;
    SemaphoreHandle_t cmd_mutex;
    SemaphoreHandle_t cmd_done_sema;
    SemaphoreHandle_t lock;
    at_state_t state;
    at_cmd_ctx_t cmd_ctx_storage;
    at_cmd_ctx_t *cmd_ctx;
    at_urc_handler_t *urc_handlers;
    int urc_handler_count;
    char *line_buf;
    int line_buf_pos;
    char *response_pool;
    int response_pool_lines;
    int response_line_size;
    bool uart_driver_installed;
    volatile bool rx_task_stop_requested;
};

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t normalize_config(const at_engine_config_t *in, at_engine_config_t *out);
static esp_err_t init_uart(at_engine_t *me);
static esp_err_t init_resources(at_engine_t *me);
static void cleanup_resources(at_engine_t *me);
static void rx_task(void *arg);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   STATIC FUNCTIONS
 **********************/

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
    }

    return ESP_OK;
}
```

- [ ] **Step 3: Add lifecycle implementation**

Add these global functions and static helpers to `src/at_engine/at_engine.c`:

```c
at_engine_t *at_engine_create(const at_engine_config_t *config)
{
    esp_err_t ret = ESP_OK;
    at_engine_config_t normalized = {0};

    ret = normalize_config(config, &normalized);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "invalid config");
        return NULL;
    }

    at_engine_t *me = calloc(1, sizeof(at_engine_t));
    if (!me) {
        ESP_LOGE(TAG, "calloc at_engine failed");
        return NULL;
    }

    me->config = normalized;
    me->uart_num = normalized.uart_num;
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
        esp_err_t del_ret = uart_driver_delete(me->uart_num);
        if (del_ret != ESP_OK) {
            ESP_LOGW(TAG, "uart_driver_delete during create rollback failed: %s", esp_err_to_name(del_ret));
        }
        me->uart_driver_installed = false;
    }
    cleanup_resources(me);
    free(me);
    return NULL;
}

esp_err_t at_engine_destroy(at_engine_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool busy = (me->cmd_ctx != NULL || me->state != AT_STATE_IDLE);
    xSemaphoreGive(me->lock);
    ESP_RETURN_ON_FALSE(!busy, ESP_ERR_INVALID_STATE, TAG, "command is running");

    me->rx_task_stop_requested = true;
    if (me->rx_task) {
        for (int i = 0; i < AT_ENGINE_RX_TASK_STOP_POLL_LIMIT && me->rx_task; i++) {
            vTaskDelay(pdMS_TO_TICKS(AT_ENGINE_RX_TASK_STOP_WAIT_MS));
        }
        if (me->rx_task) {
            TaskHandle_t task = me->rx_task;
            me->rx_task = NULL;
            vTaskDelete(task);
        }
    }

    esp_err_t ret = uart_driver_delete(me->uart_num);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_delete failed: %s", esp_err_to_name(ret));
    }
    me->uart_driver_installed = false;

    cleanup_resources(me);
    free(me);
    return ESP_OK;
}

static esp_err_t init_resources(at_engine_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->cmd_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(me->cmd_mutex, ESP_ERR_NO_MEM, TAG, "create cmd_mutex failed");

    me->cmd_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->cmd_done_sema, ESP_ERR_NO_MEM, TAG, "create cmd_done_sema failed");

    me->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(me->lock, ESP_ERR_NO_MEM, TAG, "create lock failed");

    me->line_buf = calloc(1, me->config.rx_line_buf_size);
    ESP_RETURN_ON_FALSE(me->line_buf, ESP_ERR_NO_MEM, TAG, "create line_buf failed");

    me->response_pool_lines = me->config.max_response_lines;
    me->response_line_size = me->config.rx_line_buf_size;
    me->response_pool = calloc((size_t)me->response_pool_lines, (size_t)me->response_line_size);
    ESP_RETURN_ON_FALSE(me->response_pool, ESP_ERR_NO_MEM, TAG, "create response_pool failed");

    return ESP_OK;
}

static esp_err_t init_uart(at_engine_t *me)
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

    ESP_RETURN_ON_ERROR(uart_driver_install(me->uart_num,
                                            me->config.rx_buf_size,
                                            AT_ENGINE_UART_TX_BUF_SIZE,
                                            AT_ENGINE_UART_EVENT_QUEUE_SIZE,
                                            &me->uart_queue, 0),
                        TAG, "uart_driver_install failed");
    me->uart_driver_installed = true;

    ESP_RETURN_ON_ERROR(uart_param_config(me->uart_num, &uart_config),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(me->uart_num, me->config.tx_pin, me->config.rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    return ESP_OK;
}

static void cleanup_resources(at_engine_t *me)
{
    if (!me) {
        return;
    }
    if (me->cmd_done_sema) {
        vSemaphoreDelete(me->cmd_done_sema);
        me->cmd_done_sema = NULL;
    }
    if (me->cmd_mutex) {
        vSemaphoreDelete(me->cmd_mutex);
        me->cmd_mutex = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    free(me->line_buf);
    me->line_buf = NULL;
    free(me->response_pool);
    me->response_pool = NULL;
}
```

- [ ] **Step 4: Run build and fix compile-only issues in touched files**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build fails because public functions from Task 3 and Task 4 are not all defined yet, or passes if stubs were added. Only fix syntax, include, or CMake errors in `src/include/at_engine.h`, `src/at_engine/at_engine.c`, and `src/CMakeLists.txt`.

## Task 3: Blocking Command Send and Response Pool

**Files:**
- Modify: `src/at_engine/at_engine.c`

- [ ] **Step 1: Add command helper prototypes**

In the `STATIC PROTOTYPES` section, add:

```c
static esp_err_t write_cmd(at_engine_t *me, const char *cmd);
static void reset_response(at_response_t *response);
static void clear_response_pool(at_engine_t *me);
static void clear_done_signal(at_engine_t *me);
static void flush_rx_input_locked(at_engine_t *me);
```

- [ ] **Step 2: Implement `at_engine_send_cmd()`**

Add this global function:

```c
esp_err_t at_engine_send_cmd(at_engine_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(me && cmd && response, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(response->lines && response->max_lines > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid response lines");

    uint32_t wait_ms = timeout_ms ? timeout_ms : (uint32_t)me->config.cmd_default_timeout_ms;
    ESP_RETURN_ON_FALSE(wait_ms > 0, ESP_ERR_INVALID_ARG, TAG, "invalid timeout");

    if (xSemaphoreTake(me->cmd_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    reset_response(response);
    clear_done_signal(me);

    at_cmd_ctx_t *ctx = &me->cmd_ctx_storage;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    clear_response_pool(me);
    *ctx = (at_cmd_ctx_t) {
        .cmd = cmd,
        .timeout_ms = wait_ms,
        .response = response,
        .echo_consumed = 0,
        .data_line_index = 0,
        .result_received = false,
    };
    me->cmd_ctx = ctx;
    me->state = AT_STATE_SENDING;
    xSemaphoreGive(me->lock);

    esp_err_t ret = write_cmd(me, cmd);
    if (ret != ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        return ret;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->state = AT_STATE_WAITING;
    xSemaphoreGive(me->lock);

    if (xSemaphoreTake(me->cmd_done_sema, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (me->cmd_ctx == ctx) {
            response->status = AT_RESP_TIMEOUT;
            me->cmd_ctx = NULL;
            flush_rx_input_locked(me);
            me->state = AT_STATE_IDLE;
        }
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
    }
    xSemaphoreGive(me->lock);

    xSemaphoreGive(me->cmd_mutex);
    return ESP_OK;
}
```

- [ ] **Step 3: Implement command helper functions**

Add these static functions:

```c
static esp_err_t write_cmd(at_engine_t *me, const char *cmd)
{
    size_t len = strlen(cmd);
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "empty command");

    bool has_line_end = (cmd[len - 1] == '\n' || cmd[len - 1] == '\r');
    if (has_line_end) {
        int written = uart_write_bytes(me->uart_num, cmd, len);
        ESP_RETURN_ON_FALSE(written == (int)len, ESP_FAIL, TAG, "uart_write_bytes failed");
        return ESP_OK;
    }

    char *buf = malloc(len + 3);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc command buffer failed");
    memcpy(buf, cmd, len);
    buf[len] = '\r';
    buf[len + 1] = '\n';
    buf[len + 2] = '\0';

    int written = uart_write_bytes(me->uart_num, buf, len + 2);
    free(buf);
    ESP_RETURN_ON_FALSE(written == (int)(len + 2), ESP_FAIL, TAG, "uart_write_bytes failed");
    return ESP_OK;
}

static void reset_response(at_response_t *response)
{
    response->status = AT_RESP_TIMEOUT;
    response->error_code = 0;
    response->line_count = 0;
    for (int i = 0; i < response->max_lines; i++) {
        response->lines[i] = NULL;
    }
}

static void clear_response_pool(at_engine_t *me)
{
    if (me->response_pool) {
        memset(me->response_pool, 0,
               (size_t)me->response_pool_lines * (size_t)me->response_line_size);
    }
}

static void clear_done_signal(at_engine_t *me)
{
    while (xSemaphoreTake(me->cmd_done_sema, 0) == pdTRUE) {
    }
}

static void flush_rx_input_locked(at_engine_t *me)
{
    (void)uart_flush_input(me->uart_num);
    if (me->uart_queue) {
        xQueueReset(me->uart_queue);
    }
    me->line_buf_pos = 0;
    if (me->line_buf) {
        me->line_buf[0] = '\0';
    }
}
```

- [ ] **Step 4: Run build and keep failures scoped**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build fails only because RX task, parser, or URC functions are not complete yet, or passes after Task 4 code is already present. Fix syntax/type issues in touched files only.

## Task 4: UART RX Task, Parser, and URC Dispatch

**Files:**
- Modify: `src/at_engine/at_engine.c`

- [ ] **Step 1: Add parser and URC helper prototypes**

In the `STATIC PROTOTYPES` section, add:

```c
static void process_rx_bytes(at_engine_t *me, const uint8_t *data, int len);
static void process_rx_char(at_engine_t *me, char c);
static void handle_line(at_engine_t *me, const char *line);
static bool is_echo_line(const at_cmd_ctx_t *ctx, const char *line);
static bool parse_final_result(at_response_t *response, const char *line);
static int parse_error_code(const char *line);
static void append_response_line_locked(at_engine_t *me, at_cmd_ctx_t *ctx, const char *line);
static void finish_cmd_locked(at_engine_t *me, at_response_status_t status, int error_code);
static bool dispatch_urc(at_engine_t *me, const char *line);
static bool starts_with(const char *str, const char *prefix);
```

- [ ] **Step 2: Implement the RX task**

Add the `rx_task()` function body:

```c
static void rx_task(void *arg)
{
    at_engine_t *me = (at_engine_t *)arg;
    uint8_t rx_buf[128];
    uart_event_t event;

    while (!me->rx_task_stop_requested) {
        if (xQueueReceive(me->uart_queue, &event, pdMS_TO_TICKS(AT_ENGINE_RX_WAIT_MS)) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            int to_read = event.size;
            while (to_read > 0) {
                int chunk = to_read > (int)sizeof(rx_buf) ? (int)sizeof(rx_buf) : to_read;
                int len = uart_read_bytes(me->uart_num, rx_buf, chunk, pdMS_TO_TICKS(AT_ENGINE_RX_WAIT_MS));
                if (len <= 0) {
                    break;
                }
                process_rx_bytes(me, rx_buf, len);
                to_read -= len;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "UART RX overflow, flushing input");
            uart_flush_input(me->uart_num);
            xQueueReset(me->uart_queue);
            me->line_buf_pos = 0;
            if (me->line_buf) {
                me->line_buf[0] = '\0';
            }
        }
    }

    me->rx_task = NULL;
    vTaskDelete(NULL);
}
```

- [ ] **Step 3: Implement line assembly**

Add:

```c
static void process_rx_bytes(at_engine_t *me, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        process_rx_char(me, (char)data[i]);
    }
}

static void process_rx_char(at_engine_t *me, char c)
{
    if (c == '\r') {
        return;
    }

    if (c == '\n') {
        if (me->line_buf_pos > 0) {
            me->line_buf[me->line_buf_pos] = '\0';
            handle_line(me, me->line_buf);
            me->line_buf_pos = 0;
            me->line_buf[0] = '\0';
        }
        return;
    }

    if (me->line_buf_pos + 1 >= me->config.rx_line_buf_size) {
        ESP_LOGW(TAG, "RX line too long, drop current line");
        me->line_buf_pos = 0;
        me->line_buf[0] = '\0';
        return;
    }

    me->line_buf[me->line_buf_pos++] = c;
    me->line_buf[me->line_buf_pos] = '\0';
}
```

- [ ] **Step 4: Implement line handling and command completion**

Add:

```c
static void handle_line(at_engine_t *me, const char *line)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    at_cmd_ctx_t *ctx = me->cmd_ctx;

    if (ctx) {
        if (!ctx->echo_consumed && is_echo_line(ctx, line)) {
            ctx->echo_consumed = 1;
            xSemaphoreGive(me->lock);
            return;
        }

        if (parse_final_result(ctx->response, line)) {
            finish_cmd_locked(me, ctx->response->status, ctx->response->error_code);
            xSemaphoreGive(me->lock);
            return;
        }

        append_response_line_locked(me, ctx, line);
        xSemaphoreGive(me->lock);
        return;
    }

    xSemaphoreGive(me->lock);
    (void)dispatch_urc(me, line);
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

static bool parse_final_result(at_response_t *response, const char *line)
{
    if (strcmp(line, "OK") == 0) {
        response->status = AT_RESP_OK;
        response->error_code = 0;
        return true;
    }
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

static int parse_error_code(const char *line)
{
    const char *colon = strchr(line, ':');
    if (!colon) {
        return 0;
    }
    return atoi(colon + 1);
}

static void append_response_line_locked(at_engine_t *me, at_cmd_ctx_t *ctx, const char *line)
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

static void finish_cmd_locked(at_engine_t *me, at_response_status_t status, int error_code)
{
    if (me->cmd_ctx && me->cmd_ctx->response) {
        me->cmd_ctx->response->status = status;
        me->cmd_ctx->response->error_code = error_code;
    }
    me->state = AT_STATE_IDLE;
    xSemaphoreGive(me->cmd_done_sema);
}
```

- [ ] **Step 5: Implement URC registration and dispatch**

Add the two global URC functions and helper functions:

```c
esp_err_t at_engine_register_urc(at_engine_t *me, const char *prefix,
                                 at_urc_handler_t *handler)
{
    ESP_RETURN_ON_FALSE(me && prefix && handler && handler->callback,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    for (at_urc_handler_t *it = me->urc_handlers; it; it = it->next) {
        if (strcmp(it->prefix, prefix) == 0) {
            xSemaphoreGive(me->lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    handler->prefix = prefix;
    handler->next = me->urc_handlers;
    me->urc_handlers = handler;
    me->urc_handler_count++;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t at_engine_unregister_urc(at_engine_t *me, const char *prefix)
{
    ESP_RETURN_ON_FALSE(me && prefix, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    at_urc_handler_t **link = &me->urc_handlers;
    while (*link) {
        at_urc_handler_t *node = *link;
        if (strcmp(node->prefix, prefix) == 0) {
            *link = node->next;
            node->next = NULL;
            me->urc_handler_count--;
            xSemaphoreGive(me->lock);
            return ESP_OK;
        }
        link = &node->next;
    }
    xSemaphoreGive(me->lock);
    return ESP_ERR_NOT_FOUND;
}

static bool dispatch_urc(at_engine_t *me, const char *line)
{
    at_urc_callback_t callback = NULL;
    void *user_ctx = NULL;
    const char *matched_prefix = NULL;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    for (at_urc_handler_t *it = me->urc_handlers; it; it = it->next) {
        if (it->prefix && starts_with(line, it->prefix)) {
            callback = it->callback;
            user_ctx = it->user_ctx;
            matched_prefix = it->prefix;
            break;
        }
    }
    xSemaphoreGive(me->lock);

    if (!callback) {
        return false;
    }

    callback(matched_prefix, line, user_ctx);
    return true;
}

static bool starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    return strncmp(str, prefix, prefix_len) == 0;
}
```

- [ ] **Step 6: Run build and fix implementation issues**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build passes or reports concrete compile issues in `at_engine.c`. Fix only the compile issues in touched files.

## Task 5: Tighten Resource Cleanup and Edge Cases

**Files:**
- Modify: `src/at_engine/at_engine.c`

- [ ] **Step 1: Verify UART driver cleanup tracking exists**

Confirm `struct at_engine` already includes `bool uart_driver_installed`, `init_uart()` sets it immediately after successful `uart_driver_install()`, `at_engine_create()` rollback deletes the UART driver when the flag is set, and `at_engine_destroy()` clears the flag after delete. These were moved into Task 2 because code review found the create-failure leak before Task 5.

- [ ] **Step 2: Avoid using `me->lock` before it exists in partial cleanup**

Keep `cleanup_resources()` limited to deleting non-NULL handles and freeing buffers. Do not take `me->lock` inside cleanup.

- [ ] **Step 3: Ensure command completion clears active context after the waiter wakes**

Do not set `me->cmd_ctx = NULL` inside `finish_cmd_locked()`. The waiting `at_engine_send_cmd()` clears it after `cmd_done_sema` wakes, which keeps stack `ctx` valid until the waiter exits.

- [ ] **Step 4: Run build again**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build passes.

## Task 6: Sync `classes.md` With Implemented Design

**Files:**
- Modify: `docs/agents/classes.md`

- [ ] **Step 1: Update internal structure description**

In section `1.3 at_engine_t`, update the internal struct example to include the actual additions from the implementation:

```c
struct at_engine {
    at_engine_config_t       config;          // 配置快照
    uart_port_t              uart_num;        // UART 端口号
    QueueHandle_t            uart_queue;      // ESP-IDF UART 事件队列
    TaskHandle_t             rx_task;         // UART 接收任务句柄
    SemaphoreHandle_t        cmd_mutex;       // 命令互斥锁
    SemaphoreHandle_t        cmd_done_sema;   // 命令完成信号量
    SemaphoreHandle_t        lock;            // 内部状态/URC 链表保护锁
    at_state_t               state;           // 当前状态
    at_cmd_ctx_t            *cmd_ctx;         // 当前命令上下文
    at_urc_handler_t        *urc_handlers;    // URC 链表头
    int                      urc_handler_count;
    char                    *line_buf;        // 行组装缓冲区
    int                      line_buf_pos;
    char                    *response_pool;   // 响应文本池
    int                      response_pool_lines;
    int                      response_line_size;
    bool                     uart_driver_installed;
    volatile bool            rx_task_stop_requested;
};
```

- [ ] **Step 2: Update response memory decision**

In section `1.4 at_response_t`, replace the memory decision bullets with:

```markdown
- `lines` 数组由调用方分配，AT Engine 填入指向实例内响应文本池的指针
- 响应文本池按 `min(config.max_response_lines, response->max_lines)` 截断行数
- 命令响应数据在 `send_cmd` 返回后保证有效，直到同一 AT Engine 实例的下次 `send_cmd` 调用
```

- [ ] **Step 3: Update command context fields**

In section `1.7 at_cmd_ctx_t`, remove `timeout_ticks` and keep timeout as caller-wait metadata:

```c
typedef struct {
    const char     *cmd;                 // 命令字符串（调用方传入，不拷贝）
    uint32_t        timeout_ms;          // 超时时间（毫秒）
    at_response_t  *response;            // 指向调用方的 response 对象
    int             echo_consumed;       // 是否已消费命令回显行
    int             data_line_index;     // 当前填充到 response->lines 的索引
    bool            result_received;     // 已收到最终结果（OK/ERROR/CME ERROR 等）
} at_cmd_ctx_t;
```

- [ ] **Step 4: Update thread model timeout text**

In section `1.8 AT Engine 线程模型`, replace the timeout handling paragraph with:

```text
超时处理（在调用方线程中完成）
                ──→ send_cmd 使用 xSemaphoreTake(cmd_done_sema, timeout)
                ──→ 超时 → 设置 AT_RESP_TIMEOUT → 清除 cmd_ctx
                    → 释放 cmd_mutex → 返回 ESP_ERR_TIMEOUT
```

Add a short note after the diagram:

```markdown
**实现差异说明**：最初文档写为 RX task 轮询 `timeout_ticks`，实际实现改为调用方线程在 `xSemaphoreTake()` 上等待超时。这样超时归属更清晰，RX task 不需要周期性扫描命令上下文。
```

- [ ] **Step 5: Document URC scope during commands**

Add this concise note under section `1.5 at_urc_handler_t` or `1.8 AT Engine 线程模型`:

```markdown
第一版实现中，命令等待期间收到的非最终响应行优先归入当前命令响应，不同时分发为 URC；无当前命令时才按 URC 前缀分发。这避免查询响应与同前缀 URC 混淆。
```

## Task 7: Final Verification

**Files:**
- Verify: `src/include/at_engine.h`
- Verify: `src/at_engine/at_engine.c`
- Verify: `src/CMakeLists.txt`
- Verify: `docs/agents/classes.md`

- [ ] **Step 1: Run ESP-IDF build**

Run: ESP-IDF MCP build tool from the workspace root.

Expected: build passes.

- [ ] **Step 2: Inspect git diff without committing**

Run: `git diff -- src/include/at_engine.h src/at_engine/at_engine.c src/CMakeLists.txt docs/agents/classes.md docs/superpowers/specs/2026-05-22-at-engine-design.md docs/superpowers/plans/2026-05-22-at-engine-implementation-plan.md`

Expected: diff only contains AT Engine implementation, the approved spec, this plan, and concise `classes.md` sync changes.

- [ ] **Step 3: Report verification status**

Final response must state:

```text
Implemented AT Engine layer.
Build: pass/fail with the exact command or MCP tool used.
Docs: classes.md synced with implementation/spec differences.
Commit: not created, per user instruction.
```

## Self-Review Notes

- Spec coverage: Tasks cover public API, implementation file, CMake registration, UART lifecycle, RX task, line parsing, response pool, command timeout, URC registration/dispatch, `classes.md` sync, and build verification.
- Placeholder scan: This plan uses concrete file paths, function names, code blocks, and commands. There are no open-ended implementation steps.
- Type consistency: Public types match `docs/superpowers/specs/2026-05-22-at-engine-design.md` and `docs/agents/classes.md` naming: `at_engine_t`, `at_response_t`, `at_urc_handler_t`, `at_cmd_ctx_t`, `at_state_t`.
