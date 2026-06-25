/**
 * @file air780ep_mqtt_client.c
 * @brief Air780EP LTE MQTT 客户端示例
 * @details Air780EP LTE MQTT client example
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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
#define TAG                                      "air780ep_mqtt"

#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1

#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_READY_TIMEOUT_MS                 30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS            120000
#define EXAMPLE_MQTT_CONNECT_TIMEOUT_MS          30000
#define EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS        10000
#define EXAMPLE_POLL_INTERVAL_MS                 100
#define EXAMPLE_IDLE_DELAY_MS                    1000
#define EXAMPLE_TELEMETRY_INTERVAL_MS            5000
#define EXAMPLE_TELEMETRY_BUF_LEN                96

#define TB_TOPIC_TELEMETRY                       "v1/devices/me/telemetry"
#define TB_TOPIC_ATTRIBUTES                      "v1/devices/me/attributes"

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
 * @brief MQTT 事件回调（共享事件总线）
 * @details MQTT event callback (shared event bus)
 * @param[in] arg handler 注册时传入的上下文
 * @param[in] base 事件 base（LWLTE_MQTT_EVENT）
 * @param[in] event_id 事件 ID
 * @param[in] event_data 事件数据
 */
static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data);

/**
 * @brief 发布一条 telemetry 数据
 * @details Publish one telemetry message
 * @param[in] lte LTE 用户门面句柄
 */
static void publish_telemetry(lwlte_handle_t *lte);

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
static volatile bool s_mqtt_connected;
static volatile bool s_mqtt_subscribed;
static volatile int s_last_error;
static volatile uint32_t s_counter;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void example_air780ep_mqtt_client_run(void)
{
    s_net_online = false;
    s_net_error = false;
    s_mqtt_connected = false;
    s_mqtt_subscribed = false;
    s_last_error = 0;
    s_counter = 0;

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwlte_handle_t *lte = NULL;
    const char *mqtt_username = CONFIG_EXAMPLE_MQTT_TOKEN;
    if (mqtt_username[0] == '\0') {
        mqtt_username = NULL;
    }

    const lwlte_mqtt_config_t mqtt_config = {
        .host = CONFIG_EXAMPLE_MQTT_HOST,
        .port = CONFIG_EXAMPLE_MQTT_PORT,
        .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
        .username = mqtt_username,
        .password = NULL,
        .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
        .clean_session = true,
    };

    const lwlte_air780ep_config_t config = {
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

    ESP_LOGI(TAG, "Air780EP MQTT client example");
    ESP_LOGI(TAG, "MQTT host=%s port=%d client_id=%s",
             CONFIG_EXAMPLE_MQTT_HOST, CONFIG_EXAMPLE_MQTT_PORT,
             CONFIG_EXAMPLE_MQTT_CLIENT_ID);

    /* 创建 Air780EP 门面（不含 MQTT；MQTT 由后续 lwlte_mqtt_init 创建）。 */
    esp_err_t ret = lwlte_air780ep_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init Air780EP failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    /* 注册事件回调：网络事件走 LWLTE_EVENT，MQTT 连接与下行数据走 LWLTE_MQTT_EVENT。 */
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                               lwlte_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_MQTT_EVENT, ESP_EVENT_ANY_ID,
                                               mqtt_event_cb, NULL));

    /* 创建 MQTT 客户端对象（不启动连接；连接在网络 online 后由 lwlte_mqtt_start 触发）。 */
    ret = lwlte_mqtt_init(lte, &mqtt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 先启动 LTE 联网，MQTT 会在网络 online 后再启动。 */
    ret = lwlte_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

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

    /* 网络 online 后启动 MQTT 客户端。 */
    ret = lwlte_mqtt_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    elapsed_ms = 0;
    while (!s_mqtt_connected && elapsed_ms < EXAMPLE_MQTT_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT did not connect within timeout");
        idle_forever();
    }

    /* 订阅 ThingsBoard attributes topic，用于验证下行接收。 */
    ret = lwlte_mqtt_subscribe(lte, TB_TOPIC_ATTRIBUTES, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "subscribe attributes failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    elapsed_ms = 0;
    while (!s_mqtt_subscribed && elapsed_ms < EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }
    if (!s_mqtt_subscribed) {
        ESP_LOGW(TAG, "attributes subscribe not confirmed within timeout");
        idle_forever();
    }

    while (1) {
        if (s_mqtt_connected) {
            publish_telemetry(lte);
        } else {
            ESP_LOGW(TAG, "MQTT disconnected, skip telemetry");
        }
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_TELEMETRY_INTERVAL_MS));
    }
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

    ESP_LOGI(TAG, "LTE event=%d(%s)", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id));

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
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_online = false;
        s_last_error = data ? data->error_code : ESP_FAIL;
        s_net_error = true;
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    default:
        break;
    }
}

static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    lwlte_mqtt_event_data_t *data = event_data;

    ESP_LOGI(TAG, "MQTT event=%d(%s)", (int)event_id,
             example_lwlte_mqtt_event_name((lwlte_mqtt_event_id_t)event_id));

    switch ((lwlte_mqtt_event_id_t)event_id) {
    case LWLTE_MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        break;
    case LWLTE_MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    case LWLTE_MQTT_EVENT_ERROR:
        s_last_error = data ? data->error_code : ESP_FAIL;
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    case LWLTE_MQTT_EVENT_SUBSCRIBED:
        s_mqtt_subscribed = true;
        ESP_LOGI(TAG, "MQTT subscribed");
        break;
    case LWLTE_MQTT_EVENT_DATA:
        if (data) {
            const char *topic = data->msg.topic ? data->msg.topic : "";
            const char *payload = data->msg.payload ? (const char *)data->msg.payload : "";
            size_t topic_len = data->msg.topic ? data->msg.topic_len : 0;
            size_t payload_len = data->msg.payload ? data->msg.payload_len : 0;
            ESP_LOGI(TAG, "MQTT RX topic=%.*s payload=%.*s",
                     (int)topic_len, topic, (int)payload_len, payload);
            lwlte_mqtt_event_data_release(data);
        }
        break;
    default:
        break;
    }
}

static void publish_telemetry(lwlte_handle_t *lte)
{
    char payload[EXAMPLE_TELEMETRY_BUF_LEN] = {0};
    int len = snprintf(payload, sizeof(payload),
                       "{\"temperature\":25.5,\"counter\":%lu}",
                       (unsigned long)s_counter);
    if (len <= 0 || (size_t)len >= sizeof(payload)) {
        ESP_LOGW(TAG, "telemetry payload too long");
        return;
    }

    esp_err_t ret = lwlte_mqtt_publish(lte, TB_TOPIC_TELEMETRY,
                                       (const uint8_t *)payload,
                                       (size_t)len, 0, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "telemetry publish failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "telemetry published: %s", payload);
    s_counter++;
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}
