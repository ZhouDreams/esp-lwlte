/**
 * @file main.c
 * @brief ML307R 串口探测示例
 * @details ML307R UART probe example
 * @author JovisDreams
 * @date 2026-06-03
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "ml307r_probe"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE           115200

#define EXAMPLE_UART_RX_BUFFER_SIZE          1024
#define EXAMPLE_UART_TX_BUFFER_SIZE          0
#define EXAMPLE_STARTUP_DRAIN_MS             3000
#define EXAMPLE_COMMAND_TIMEOUT_MS           2000
#define EXAMPLE_AT_RETRY_COUNT               3
#define EXAMPLE_AT_RETRY_DELAY_MS            1000
#define EXAMPLE_IDLE_LOG_INTERVAL_MS         5000

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 初始化 ML307R 串口
 * @details Initialize ML307R UART
 * @return
 *         - ESP_OK: 成功
 *         - 其他: ESP-IDF 驱动错误
 */
static esp_err_t init_uart(void);

/**
 * @brief 拉高模组使能引脚
 * @details Drive modem enable pin high
 * @return
 *         - ESP_OK: 成功
 *         - 其他: ESP-IDF 驱动错误
 */
static esp_err_t enable_modem(void);

/**
 * @brief 发送 AT 命令并打印响应
 * @details Send AT command and log response
 * @param[in] label 命令标签
 * @param[in] command AT 命令字符串，包含行尾
 * @param[in] timeout_ms 响应等待时间
 * @return 是否收到任何响应字节
 */
static bool send_command_and_log_response(const char *label, const char *command,
                                          uint32_t timeout_ms);

/**
 * @brief 读取并打印串口数据
 * @details Read and log UART data
 * @param[in] label 日志标签
 * @param[in] timeout_ms 总读取时间
 * @return 是否收到任何响应字节
 */
static bool log_uart_rx(const char *label, uint32_t timeout_ms);

/**
 * @brief 打印可读串口数据
 * @details Log printable UART data
 * @param[in] label 日志标签
 * @param[in] data 数据缓冲区
 * @param[in] len 数据长度
 */
static void log_rx_chunk(const char *label, const uint8_t *data, int len);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    ESP_LOGI(TAG, "ML307R UART probe example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN);

    esp_err_t ret = enable_modem();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "enable modem failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = init_uart();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init UART failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "draining startup URC for %d ms", EXAMPLE_STARTUP_DRAIN_MS);
    bool got_startup = log_uart_rx("startup", EXAMPLE_STARTUP_DRAIN_MS);
    if (!got_startup) {
        ESP_LOGW(TAG, "no startup URC received; ML307R autobaud mode may require AT first");
    }

    (void)send_command_and_log_response("ATE0", "ATE0\r\n",
                                        EXAMPLE_COMMAND_TIMEOUT_MS);

    bool got_at = false;
    for (int i = 0; i < EXAMPLE_AT_RETRY_COUNT && !got_at; i++) {
        ESP_LOGI(TAG, "AT probe attempt %d/%d", i + 1, EXAMPLE_AT_RETRY_COUNT);
        got_at = send_command_and_log_response("AT", "AT\r\n",
                                               EXAMPLE_COMMAND_TIMEOUT_MS);
        if (!got_at) {
            vTaskDelay(pdMS_TO_TICKS(EXAMPLE_AT_RETRY_DELAY_MS));
        }
    }

    if (!got_at) {
        ESP_LOGW(TAG, "AT probe received no bytes; check wiring, power, EN pin, and baud rate");
    }

    while (1) {
        ESP_LOGI(TAG, "probe idle; listening for unsolicited UART data");
        (void)log_uart_rx("idle", EXAMPLE_IDLE_LOG_INTERVAL_MS);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t init_uart(void)
{
    const uart_config_t uart_config = {
        .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(EXAMPLE_LTE_UART_NUM,
                                        EXAMPLE_UART_RX_BUFFER_SIZE,
                                        EXAMPLE_UART_TX_BUFFER_SIZE, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_param_config(EXAMPLE_LTE_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_set_pin(EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
                       EXAMPLE_LTE_UART_RX_PIN, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }

    return uart_flush_input(EXAMPLE_LTE_UART_NUM);
}

static esp_err_t enable_modem(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << EXAMPLE_LTE_EN_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    return gpio_set_level(EXAMPLE_LTE_EN_PIN, 1);
}

static bool send_command_and_log_response(const char *label, const char *command,
                                          uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "TX %s: %s", label, command);
    esp_err_t ret = uart_flush_input(EXAMPLE_LTE_UART_NUM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "flush UART before %s failed: %s", label, esp_err_to_name(ret));
    }

    int written = uart_write_bytes(EXAMPLE_LTE_UART_NUM, command, strlen(command));
    if (written < 0 || written != (int)strlen(command)) {
        ESP_LOGW(TAG, "write %s incomplete: written=%d expected=%u", label,
                 written, (unsigned int)strlen(command));
    }

    return log_uart_rx(label, timeout_ms);
}

static bool log_uart_rx(const char *label, uint32_t timeout_ms)
{
    uint8_t data[128];
    bool got_data = false;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        int len = uart_read_bytes(EXAMPLE_LTE_UART_NUM, data, sizeof(data) - 1,
                                  pdMS_TO_TICKS(100));
        if (len > 0) {
            got_data = true;
            log_rx_chunk(label, data, len);
        }
        elapsed_ms += 100;
    }

    if (!got_data) {
        ESP_LOGW(TAG, "RX %s: no data in %lu ms", label, (unsigned long)timeout_ms);
    }

    return got_data;
}

static void log_rx_chunk(const char *label, const uint8_t *data, int len)
{
    char text[129];
    int copy_len = len < (int)(sizeof(text) - 1) ? len : (int)(sizeof(text) - 1);

    for (int i = 0; i < copy_len; i++) {
        char ch = (char)data[i];
        text[i] = (ch >= 32 && ch <= 126) ? ch : '.';
    }
    text[copy_len] = '\0';

    ESP_LOGI(TAG, "RX %s: len=%d text='%s'", label, len, text);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_INFO);
}
