/**
 * @file main.c
 * @brief LTE 基础连接示例
 * @details LTE basic connection example
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "appmain"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1

#define EXAMPLE_MODEM_RESET_PULSE_MS         500
#define EXAMPLE_INIT_READY_TIMEOUT_MS        30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS        120000
#define EXAMPLE_POLL_INTERVAL_MS             100
#define EXAMPLE_STATUS_LOG_INTERVAL_MS       5000

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief LTE 事件回调
 * @details LTE event callback
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据
 * @param[in] user_ctx 用户上下文
 */
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx);

/**
 * @brief 打印 LTE 状态
 * @details Log LTE state
 * @param[in] lte LTE 用户门面句柄
 * @param[in] stage 当前阶段描述
 */
static void log_lte_status(lwlte_t *lte, const char *stage);

/**
 * @brief 清理 LTE 句柄
 * @details Cleanup LTE handle
 * @param[in] lte LTE 用户门面句柄
 */
static void cleanup_lte(lwlte_t *lte);

/**
 * @brief 执行 ping 测试
 * @details Perform ping test
 * @param[in] lte LTE 用户门面句柄
 */
static void do_ping(lwlte_t *lte);

/**
 * @brief 进入永久等待
 * @details Enter forever delay loop
 */
static void idle_forever(void);

/**
 * @brief 获取 LTE 状态字符串
 * @details Get LTE state string
 * @param[in] state LTE 状态
 * @return 状态字符串
 */
static const char *lte_state_name(lwlte_state_t state);

/**
 * @brief 获取 LTE 网络状态字符串
 * @details Get LTE network state string
 * @param[in] state 网络状态
 * @return 状态字符串
 */
static const char *net_state_name(lwlte_net_state_t state);

/**
 * @brief 获取 LTE 事件字符串
 * @details Get LTE event string
 * @param[in] event_id LTE 事件 ID
 * @return 事件字符串
 */
static const char *lte_event_name(lwlte_event_id_t event_id);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool s_net_online;
static volatile bool s_net_error;
static volatile int s_net_error_code;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    lwlte_t *lte = NULL;
    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
    };

    ESP_LOGI(TAG, "esp-lwlte basic connect example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);

    esp_err_t ret = lwlte_air780ep_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init Air780EP LTE failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    ret = lwlte_register_event_callback(lte, lte_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register LTE callback failed: %s", esp_err_to_name(ret));
        cleanup_lte(lte);
        idle_forever();
    }

    ret = lwlte_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
        cleanup_lte(lte);
        idle_forever();
    }

    uint32_t elapsed_ms = 0;
    while (!s_net_online && !s_net_error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (s_net_online) {
        ESP_LOGI(TAG, "LTE network is online");
        do_ping(lte);
    } else {
        ESP_LOGW(TAG, "LTE network did not become online, error=%d",
                 s_net_error_code);
        log_lte_status(lte, "network wait ended");
    }

    while (1) {
        log_lte_status(lte, "periodic");
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_LOG_INTERVAL_MS));
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx)
{
    (void)lte;
    (void)user_ctx;

    if (data) {
        ESP_LOGI(TAG, "LTE event: %s net=%s err=%d",
                 lte_event_name(event_id), net_state_name(data->net_state),
                 data->error_code);
    } else {
        ESP_LOGI(TAG, "LTE event: %s", lte_event_name(event_id));
    }

    switch (event_id) {
    case LWLTE_EVENT_NET_CONNECTING:
        s_net_error_code = 0;
        s_net_error = false;
        s_net_online = false;
        break;
    case LWLTE_EVENT_NET_ONLINE:
        s_net_error_code = 0;
        s_net_error = false;
        s_net_online = true;
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        s_net_online = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_error_code = data ? data->error_code : ESP_FAIL;
        s_net_online = false;
        s_net_error = true;
        break;
    default:
        break;
    }
}

static void log_lte_status(lwlte_t *lte, const char *stage)
{
    if (!lte) {
        ESP_LOGW(TAG, "%s: LTE is NULL", stage);
        return;
    }

    lwlte_state_t lte_state = LWLTE_STATE_STOPPED;
    lwlte_net_state_t net_state = LWLTE_NET_STATE_OFFLINE;
    esp_err_t lte_ret = lwlte_get_state(lte, &lte_state);
    esp_err_t net_ret = lwlte_get_net_state(lte, &net_state);

    if (lte_ret != ESP_OK || net_ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: get state failed lte_ret=%s net_ret=%s", stage,
                 esp_err_to_name(lte_ret), esp_err_to_name(net_ret));
        return;
    }

    ESP_LOGI(TAG, "%s: lte=%s net=%s", stage, lte_state_name(lte_state),
             net_state_name(net_state));
}

static void cleanup_lte(lwlte_t *lte)
{
    if (!lte) {
        return;
    }

    esp_err_t ret = lwlte_destroy(lte);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "destroy LTE failed: %s", esp_err_to_name(ret));
    }
}

static void do_ping(lwlte_t *lte)
{
    lwlte_ping_request_t req = {
        .host = "8.8.8.8",
        .count = 4,
        .data_len = 64,
        .timeout_100ms = 10,
        .ttl = 64,
        .total_timeout_ms = 0,
    };

    lwlte_ping_reply_t replies[4] = {0};
    lwlte_ping_summary_t summary = {0};

    ESP_LOGI(TAG, "ping %s count=%d datalen=%d", req.host, req.count,
             req.data_len);

    esp_err_t ret = lwlte_ping(lte, &req, replies, req.count, &summary);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ping failed: %s", esp_err_to_name(ret));
        return;
    }

    for (int i = 0; i < req.count; i++) {
        ESP_LOGI(TAG, "  [%d] seq=%d ip=%s time=%lums ttl=%d %s",
                 i, replies[i].seq, replies[i].ip,
                 (unsigned long)replies[i].time_ms, replies[i].ttl,
                 replies[i].success ? "ok" : "timeout");
    }
    ESP_LOGI(TAG, "ping summary: sent=%d recv=%d lost=%d min=%lums max=%lums avg=%lums",
             summary.sent, summary.received, summary.lost,
             (unsigned long)summary.min_time_ms,
             (unsigned long)summary.max_time_ms,
             (unsigned long)summary.avg_time_ms);
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static const char *lte_state_name(lwlte_state_t state)
{
    switch (state) {
    case LWLTE_STATE_STOPPED:
        return "STOPPED";
    case LWLTE_STATE_STARTING:
        return "STARTING";
    case LWLTE_STATE_READY:
        return "READY";
    case LWLTE_STATE_NET_ACTIVATING:
        return "NET_ACTIVATING";
    case LWLTE_STATE_ONLINE:
        return "ONLINE";
    case LWLTE_STATE_ERROR:
        return "ERROR";
    case LWLTE_STATE_DESTROYING:
        return "DESTROYING";
    default:
        return "UNKNOWN";
    }
}

static const char *net_state_name(lwlte_net_state_t state)
{
    switch (state) {
    case LWLTE_NET_STATE_OFFLINE:
        return "OFFLINE";
    case LWLTE_NET_STATE_ACTIVATING:
        return "ACTIVATING";
    case LWLTE_NET_STATE_ONLINE:
        return "ONLINE";
    case LWLTE_NET_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static const char *lte_event_name(lwlte_event_id_t event_id)
{
    switch (event_id) {
    case LWLTE_EVENT_STARTED:
        return "STARTED";
    case LWLTE_EVENT_READY:
        return "READY";
    case LWLTE_EVENT_NET_CONNECTING:
        return "NET_CONNECTING";
    case LWLTE_EVENT_NET_ONLINE:
        return "NET_ONLINE";
    case LWLTE_EVENT_NET_OFFLINE:
        return "NET_OFFLINE";
    case LWLTE_EVENT_NET_ERROR:
        return "NET_ERROR";
    case LWLTE_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
