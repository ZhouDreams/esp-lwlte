/**
 * @file ml307r_basic_connect.c
 * @brief ML307R LTE 基础连接示例
 * @details ML307R LTE basic connection example
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"
#include "example_event_names.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "ml307r_basic"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_3
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_10
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_4
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1

#define EXAMPLE_MODEM_RESET_PULSE_MS         500
#define EXAMPLE_READY_TIMEOUT_MS             30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS        120000
#define EXAMPLE_POLL_INTERVAL_MS             100
#define EXAMPLE_IDLE_DELAY_MS                1000

#define EXAMPLE_PING_HOST                    "8.8.8.8"
#define EXAMPLE_PING_COUNT                   4

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief LTE 事件回调（共享事件总线）
 * @details LTE event callback (shared event bus)
 * @param[in] arg handler 注册时传入的上下文
 * @param[in] base 事件 base（LWLTE_EVENT）
 * @param[in] event_id 事件 ID
 * @param[in] event_data 事件数据
 */
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);

/**
 * @brief 执行一次 Ping 测试
 * @details Run one Ping test
 * @param[in] lte LTE 用户门面句柄
 */
static void do_ping(lwlte_handle_t lte);

/**
 * @brief 进入常驻等待
 * @details Enter forever delay loop
 */
static void idle_forever(void);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool s_net_online;
static volatile bool s_net_error;
static volatile int s_last_error;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void example_ml307r_basic_connect_run(void)
{
    s_net_online = false;
    s_net_error = false;
    s_last_error = 0;

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwlte_handle_t lte = NULL;
    const lwlte_ml307r_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
            .event = {
                .loop = NULL,
            },
        },
    };

    ESP_LOGI(TAG, "ML307R basic connect example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);

    /* 创建 ML307R 门面：这里只填写必填字段和启动相关超时。 */
    esp_err_t ret = lwlte_ml307r_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init ML307R failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    /* 注册事件回调：联网结果会异步从回调里返回。 */
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                               lwlte_event_cb, NULL));

    /* 启动异步联网；ESP_OK 只表示请求已经提交。 */
    ret = lwlte_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 简单示例用轮询等待事件回调设置 online 标志。 */
    uint32_t elapsed_ms = 0;
    while (!s_net_online && !s_net_error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (!s_net_online) {
        ESP_LOGW(TAG, "network wait ended: error=%d code=%d",
                 (int)s_net_error, s_last_error);
        idle_forever();
    }

    ESP_LOGI(TAG, "ML307R network is online");
    do_ping(lte);
    idle_forever();
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    const lwlte_event_data_t *data = event_data;
    const lwlte_net_state_t net_state = data ? data->net_state : (lwlte_net_state_t)-1;

    ESP_LOGI(TAG, "LTE event=%d(%s) net=%d(%s) err=%d", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id),
             (int)net_state, example_lwlte_net_state_name(net_state),
             data ? data->error_code : 0);

    switch ((lwlte_event_id_t)event_id) {
    case LWLTE_EVENT_NET_CONNECTING:
        s_net_online = false;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_ONLINE:
        s_net_online = true;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        s_net_online = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_online = false;
        s_last_error = data ? data->error_code : ESP_FAIL;
        s_net_error = true;
        break;
    default:
        break;
    }
}

static void do_ping(lwlte_handle_t lte)
{
    const lwlte_ping_request_t req = {
        .host = EXAMPLE_PING_HOST,
        .count = EXAMPLE_PING_COUNT,
        .data_len = 64,
        .timeout_100ms = 10,
        .ttl = 64,
        .total_timeout_ms = 0,
    };
    lwlte_ping_reply_t replies[EXAMPLE_PING_COUNT] = {0};
    lwlte_ping_summary_t summary = {0};

    ESP_LOGI(TAG, "ping %s count=%u", req.host, (unsigned int)req.count);
    esp_err_t ret = lwlte_ping(lte, &req, replies, req.count, &summary);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ping failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "ping summary: sent=%u recv=%u lost=%u min=%lums max=%lums avg=%lums",
             (unsigned int)summary.sent, (unsigned int)summary.received,
             (unsigned int)summary.lost, (unsigned long)summary.min_time_ms,
             (unsigned long)summary.max_time_ms,
             (unsigned long)summary.avg_time_ms);
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}
