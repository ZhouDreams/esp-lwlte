/**
 * @file main.c
 * @brief LTE MQTT 客户端示例
 * @details LTE MQTT client example with ThingsBoard interaction
 * @author JovisDreams
 * @date 2026-05-27
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwlte.h"
#include "lwlte_air780ep.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                      "appmain"

#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1

#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_INIT_READY_TIMEOUT_MS            30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS            120000
#define EXAMPLE_MQTT_CONNECT_TIMEOUT_MS          30000
#define EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS        10000
#define EXAMPLE_POLL_INTERVAL_MS                 100
#define EXAMPLE_TELEMETRY_INTERVAL_MS            5000
#define EXAMPLE_TELEMETRY_BUF_LEN                128
#define EXAMPLE_RPC_TOPIC_BUF_LEN                96
#define EXAMPLE_RPC_ID_BUF_LEN                   16

#define TB_TOPIC_TELEMETRY                       "v1/devices/me/telemetry"
#define TB_TOPIC_RPC_REQUEST_PREFIX              "v1/devices/me/rpc/request/"
#define TB_TOPIC_RPC_RESPONSE_PREFIX             "v1/devices/me/rpc/response/"
#define TB_TOPIC_ATTRIBUTES                      "v1/devices/me/attributes"

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
 * @brief 处理 RPC 请求
 * @details Handle RPC request from ThingsBoard
 * @param[in] lte LTE 用户门面句柄
 * @param[in] msg MQTT 消息
 */
static void handle_rpc_request(lwlte_t *lte, const lwlte_mqtt_msg_t *msg);

/**
 * @brief 发布 telemetry 数据
 * @details Publish telemetry data to ThingsBoard
 * @param[in] lte LTE 用户门面句柄
 */
static void publish_telemetry(lwlte_t *lte);

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

/**
 * @brief 获取 MQTT 状态字符串
 * @details Get MQTT state string
 * @param[in] state MQTT 状态
 * @return 状态字符串
 */
static const char *mqtt_state_name(lwlte_mqtt_state_t state);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool s_net_online;
static volatile bool s_net_error;
static volatile int s_net_error_code;
static volatile bool s_mqtt_connected;
static volatile int s_subscribe_count;
static volatile uint32_t s_counter;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    lwlte_t *lte = NULL;

    const char *mqtt_username = CONFIG_EXAMPLE_MQTT_TOKEN;
    if (mqtt_username[0] == '\0') {
        mqtt_username = NULL;
    }

    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .auto_connect = false,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
        .mqtt_client = {
            .enabled = true,
            .host = CONFIG_EXAMPLE_MQTT_HOST,
            .port = CONFIG_EXAMPLE_MQTT_PORT,
            .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
            .username = mqtt_username,
            .password = NULL,
            .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
            .clean_session = true,
        },
    };

    ESP_LOGI(TAG, "esp-lwlte MQTT client example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);
    ESP_LOGI(TAG, "MQTT host=%s port=%d client_id=%s",
             CONFIG_EXAMPLE_MQTT_HOST, CONFIG_EXAMPLE_MQTT_PORT,
             CONFIG_EXAMPLE_MQTT_CLIENT_ID);

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

    ret = lwlte_connect(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect request failed: %s", esp_err_to_name(ret));
        log_lte_status(lte, "connect request failed");
        cleanup_lte(lte);
        idle_forever();
    }

    uint32_t elapsed_ms = 0;
    while (!s_net_online && !s_net_error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (!s_net_online) {
        ESP_LOGW(TAG, "LTE network did not become online, error=%d",
                 s_net_error_code);
        log_lte_status(lte, "network wait ended");
        cleanup_lte(lte);
        idle_forever();
    }

    ESP_LOGI(TAG, "LTE network is online, starting MQTT client");
    ret = lwlte_mqtt_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start request failed: %s", esp_err_to_name(ret));
        cleanup_lte(lte);
        idle_forever();
    }

    elapsed_ms = 0;
    while (!s_mqtt_connected && elapsed_ms < EXAMPLE_MQTT_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT did not connect within timeout");
        cleanup_lte(lte);
        idle_forever();
    }

    ESP_LOGI(TAG, "MQTT connected, subscribing to ThingsBoard topics");
    s_subscribe_count = 0;

    ret = lwlte_mqtt_subscribe(lte, TB_TOPIC_RPC_REQUEST_PREFIX "+", 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "subscribe RPC topic failed: %s", esp_err_to_name(ret));
        cleanup_lte(lte);
        idle_forever();
    }

    elapsed_ms = 0;
    while (s_subscribe_count < 1 && elapsed_ms < EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (s_subscribe_count < 1) {
        ESP_LOGW(TAG, "RPC subscribe not confirmed within timeout");
        cleanup_lte(lte);
        idle_forever();
    }

    ESP_LOGI(TAG, "RPC topic subscribed, subscribing attributes topic");
    ret = lwlte_mqtt_subscribe(lte, TB_TOPIC_ATTRIBUTES, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "subscribe attributes topic failed: %s", esp_err_to_name(ret));
        cleanup_lte(lte);
        idle_forever();
    }

    elapsed_ms = 0;
    while (s_subscribe_count < 2 && elapsed_ms < EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (s_subscribe_count < 2) {
        ESP_LOGW(TAG, "attributes subscribe not confirmed within timeout");
        cleanup_lte(lte);
        idle_forever();
    }

    ESP_LOGI(TAG, "All ThingsBoard subscriptions confirmed");

    while (1) {
        if (s_mqtt_connected) {
            publish_telemetry(lte);
        } else {
            ESP_LOGW(TAG, "MQTT disconnected, skipping telemetry");
        }
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_TELEMETRY_INTERVAL_MS));
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
        ESP_LOGI(TAG, "LTE event: %s net=%s mqtt=%s err=%d",
                 lte_event_name(event_id), net_state_name(data->net_state),
                 mqtt_state_name(data->mqtt_state), data->error_code);
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
        s_mqtt_connected = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_error_code = data ? data->error_code : ESP_FAIL;
        s_net_online = false;
        s_net_error = true;
        s_mqtt_connected = false;
        break;
    case LWLTE_EVENT_MQTT_CONNECTED:
        s_mqtt_connected = true;
        break;
    case LWLTE_EVENT_MQTT_DISCONNECTED:
        s_mqtt_connected = false;
        break;
    case LWLTE_EVENT_MQTT_SUBSCRIBED:
        s_subscribe_count++;
        ESP_LOGI(TAG, "subscription confirmed (%d/2)", s_subscribe_count);
        break;
    case LWLTE_EVENT_MQTT_DATA:
        if (data) {
            handle_rpc_request(lte, &data->data.mqtt_msg);
        }
        break;
    default:
        break;
    }
}

static void handle_rpc_request(lwlte_t *lte, const lwlte_mqtt_msg_t *msg)
{
    if (!msg || !msg->topic || msg->topic_len == 0) {
        return;
    }

    const size_t prefix_len = strlen(TB_TOPIC_RPC_REQUEST_PREFIX);
    if (msg->topic_len <= prefix_len ||
        strncmp(msg->topic, TB_TOPIC_RPC_REQUEST_PREFIX, prefix_len) != 0) {
        return;
    }

    const char *id_start = msg->topic + prefix_len;
    size_t id_len = msg->topic_len - prefix_len;
    if (id_len == 0 || id_len >= EXAMPLE_RPC_ID_BUF_LEN) {
        ESP_LOGW(TAG, "RPC request id too long or empty");
        return;
    }

    char id_buf[EXAMPLE_RPC_ID_BUF_LEN] = {0};
    memcpy(id_buf, id_start, id_len);

    char resp_topic[EXAMPLE_RPC_TOPIC_BUF_LEN] = {0};
    int written = snprintf(resp_topic, sizeof(resp_topic),
                           TB_TOPIC_RPC_RESPONSE_PREFIX "%s", id_buf);
    if (written <= 0 || (size_t)written >= sizeof(resp_topic)) {
        ESP_LOGW(TAG, "RPC response topic too long");
        return;
    }

    static const char resp_payload[] = "{\"status\":\"ok\"}";
    esp_err_t ret = lwlte_mqtt_publish(lte, resp_topic,
                                       (const uint8_t *)resp_payload,
                                       strlen(resp_payload), 0, false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RPC response sent to %s", resp_topic);
    } else {
        ESP_LOGW(TAG, "RPC response failed: %s", esp_err_to_name(ret));
    }
}

static void publish_telemetry(lwlte_t *lte)
{
    char buf[EXAMPLE_TELEMETRY_BUF_LEN] = {0};
    int written = snprintf(buf, sizeof(buf),
                           "{\"temperature\":25.5,\"counter\":%lu}",
                           (unsigned long)s_counter);
    if (written <= 0 || (size_t)written >= sizeof(buf)) {
        ESP_LOGW(TAG, "telemetry payload too long");
        return;
    }

    esp_err_t ret = lwlte_mqtt_publish(lte, TB_TOPIC_TELEMETRY,
                                       (const uint8_t *)buf,
                                       (size_t)written, 0, false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "telemetry published: %s", buf);
        s_counter++;
    } else {
        ESP_LOGW(TAG, "telemetry publish failed: %s", esp_err_to_name(ret));
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
    case LWLTE_EVENT_MQTT_STARTED:
        return "MQTT_STARTED";
    case LWLTE_EVENT_MQTT_STOPPED:
        return "MQTT_STOPPED";
    case LWLTE_EVENT_MQTT_CONNECTING:
        return "MQTT_CONNECTING";
    case LWLTE_EVENT_MQTT_CONNECTED:
        return "MQTT_CONNECTED";
    case LWLTE_EVENT_MQTT_DISCONNECTED:
        return "MQTT_DISCONNECTED";
    case LWLTE_EVENT_MQTT_SUBSCRIBED:
        return "MQTT_SUBSCRIBED";
    case LWLTE_EVENT_MQTT_UNSUBSCRIBED:
        return "MQTT_UNSUBSCRIBED";
    case LWLTE_EVENT_MQTT_PUBLISHED:
        return "MQTT_PUBLISHED";
    case LWLTE_EVENT_MQTT_DATA:
        return "MQTT_DATA";
    case LWLTE_EVENT_MQTT_ERROR:
        return "MQTT_ERROR";
    default:
        return "UNKNOWN";
    }
}

static const char *mqtt_state_name(lwlte_mqtt_state_t state)
{
    switch (state) {
    case LWLTE_MQTT_STATE_STOPPED:
        return "STOPPED";
    case LWLTE_MQTT_STATE_WAITING_NET:
        return "WAITING_NET";
    case LWLTE_MQTT_STATE_CONNECTING:
        return "CONNECTING";
    case LWLTE_MQTT_STATE_CONNECTED:
        return "CONNECTED";
    case LWLTE_MQTT_STATE_DISCONNECTING:
        return "DISCONNECTING";
    case LWLTE_MQTT_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
