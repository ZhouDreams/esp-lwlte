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
    SemaphoreHandle_t rx_task_done_sema;
    SemaphoreHandle_t cmd_mutex;
    SemaphoreHandle_t cmd_done_sema;
    SemaphoreHandle_t lock;
    at_state_t state;
    bool destroying;
    int active_callers;
    at_cmd_ctx_t cmd_ctx_storage;
    at_cmd_ctx_t *cmd_ctx;
    at_urc_handler_t *urc_handlers;
    int urc_handler_count;
    char *line_buf;
    char *line_work_buf;
    int line_buf_pos;
    bool line_overflow;
    uint32_t rx_epoch;
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
static esp_err_t begin_send_call(at_engine_t *me);
static void end_send_call(at_engine_t *me);
static esp_err_t write_cmd(at_engine_t *me, const char *cmd);
static void reset_response(at_response_t *response);
static void clear_response_pool(at_engine_t *me);
static void clear_done_signal(at_engine_t *me);
static void flush_rx_input_locked(at_engine_t *me);
static void process_rx_bytes(at_engine_t *me, const uint8_t *data, int len, uint32_t epoch);
static void process_rx_char(at_engine_t *me, char c, uint32_t epoch);
static void handle_line(at_engine_t *me, const char *line, uint32_t epoch);
static bool is_echo_line(const at_cmd_ctx_t *ctx, const char *line);
static bool parse_final_result(at_response_t *response, const char *line);
static int parse_error_code(const char *line);
static void append_response_line_locked(at_engine_t *me, at_cmd_ctx_t *ctx, const char *line);
static void finish_cmd_locked(at_engine_t *me, at_response_status_t status, int error_code);
static bool dispatch_urc(at_engine_t *me, const char *line, uint32_t epoch);
static bool starts_with(const char *str, const char *prefix);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

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

    esp_err_t ret = uart_driver_delete(me->uart_num);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "uart_driver_delete failed: %s", esp_err_to_name(ret));
        return ret;
    }
    me->uart_driver_installed = false;

    cleanup_resources(me);
    free(me);
    return ESP_OK;
}

esp_err_t at_engine_send_cmd(at_engine_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(me && cmd && response, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(response->lines && response->max_lines > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid response lines");

    esp_err_t ret = begin_send_call(me);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t wait_ms = timeout_ms ? timeout_ms : (uint32_t)me->config.cmd_default_timeout_ms;
    if (wait_ms == 0) {
        end_send_call(me);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(me->cmd_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        end_send_call(me);
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

    ret = write_cmd(me, cmd);
    if (ret != ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ret;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->state = AT_STATE_WAITING;
    }
    xSemaphoreGive(me->lock);

    if (xSemaphoreTake(me->cmd_done_sema, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
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

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
    }
    xSemaphoreGive(me->lock);

    xSemaphoreGive(me->cmd_mutex);
    end_send_call(me);
    return ESP_OK;
}

esp_err_t at_engine_register_urc(at_engine_t *me, const char *prefix,
                                 at_urc_handler_t *handler)
{
    ESP_RETURN_ON_FALSE(me && prefix && handler && handler->callback,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (at_urc_handler_t *it = me->urc_handlers; it; it = it->next) {
        if (it == handler || strcmp(it->prefix, prefix) == 0) {
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
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
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

/**********************
 *   STATIC FUNCTIONS
 **********************/

static esp_err_t begin_send_call(at_engine_t *me)
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

static void end_send_call(at_engine_t *me)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_callers > 0) {
        me->active_callers--;
    }
    xSemaphoreGive(me->lock);
}

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
                xSemaphoreTake(me->lock, portMAX_DELAY);
                uint32_t read_epoch = me->rx_epoch;
                xSemaphoreGive(me->lock);
                int len = uart_read_bytes(me->uart_num, rx_buf, chunk,
                                          pdMS_TO_TICKS(AT_ENGINE_RX_WAIT_MS));
                if (len <= 0) {
                    break;
                }
                xSemaphoreTake(me->lock, portMAX_DELAY);
                uint32_t epoch = me->rx_epoch;
                xSemaphoreGive(me->lock);
                if (epoch == read_epoch) {
                    process_rx_bytes(me, rx_buf, len, epoch);
                }
                to_read -= len;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "UART RX overflow, flushing input");
            xSemaphoreTake(me->lock, portMAX_DELAY);
            flush_rx_input_locked(me);
            xSemaphoreGive(me->lock);
        }
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->rx_task = NULL;
    xSemaphoreGive(me->lock);
    xSemaphoreGive(me->rx_task_done_sema);
    vTaskDelete(NULL);
}

static void process_rx_bytes(at_engine_t *me, const uint8_t *data, int len, uint32_t epoch)
{
    for (int i = 0; i < len; i++) {
        process_rx_char(me, (char)data[i], epoch);
    }
}

static void process_rx_char(at_engine_t *me, char c, uint32_t epoch)
{
    bool line_ready = false;

    xSemaphoreTake(me->lock, portMAX_DELAY);

    if (epoch != me->rx_epoch) {
        xSemaphoreGive(me->lock);
        return;
    }

    if (c == '\r') {
        xSemaphoreGive(me->lock);
        return;
    }

    if (me->line_overflow) {
        if (c == '\n') {
            me->line_overflow = false;
            me->line_buf_pos = 0;
            me->line_buf[0] = '\0';
        }
        xSemaphoreGive(me->lock);
        return;
    }

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
            handle_line(me, me->line_work_buf, epoch);
        }
        return;
    }

    if (me->line_buf_pos + 1 >= me->config.rx_line_buf_size) {
        ESP_LOGW(TAG, "RX line too long, drop current line");
        me->line_buf_pos = 0;
        me->line_buf[0] = '\0';
        me->line_overflow = true;
        xSemaphoreGive(me->lock);
        return;
    }

    me->line_buf[me->line_buf_pos++] = c;
    me->line_buf[me->line_buf_pos] = '\0';
    xSemaphoreGive(me->lock);
}

static void handle_line(at_engine_t *me, const char *line, uint32_t epoch)
{
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (epoch != me->rx_epoch) {
        xSemaphoreGive(me->lock);
        return;
    }

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
    me->cmd_ctx = NULL;
    me->state = AT_STATE_IDLE;
    xSemaphoreGive(me->cmd_done_sema);
}

static bool dispatch_urc(at_engine_t *me, const char *line, uint32_t epoch)
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
    }

    return ESP_OK;
}

static esp_err_t init_resources(at_engine_t *me)
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
