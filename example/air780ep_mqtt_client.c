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
#include <stdlib.h>
#include <string.h>

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

#ifndef CONFIG_EXAMPLE_MQTT_TLS_ENABLE
#define CONFIG_EXAMPLE_MQTT_TLS_ENABLE           0
#endif

#ifndef CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM
#define CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM      ""
#endif

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
static void publish_telemetry(lwlte_handle_t lte);

/**
 * @brief 规范化 PEM 字符串中的转义换行
 * @details Normalize escaped newlines in a PEM string
 * @param[in] pem PEM 字符串
 * @return 规范化后的堆内存字符串，调用方释放； Normalized heap string, caller frees
 */
static char *example_normalize_pem_newlines(const char *pem);

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

    lwlte_handle_t lte = NULL;
    const char *mqtt_username = CONFIG_EXAMPLE_MQTT_USERNAME[0] ? CONFIG_EXAMPLE_MQTT_USERNAME
                                                                : CONFIG_EXAMPLE_MQTT_TOKEN;
    if (mqtt_username[0] == '\0') {
        mqtt_username = NULL;
    }
    const char *mqtt_password = CONFIG_EXAMPLE_MQTT_PASSWORD[0] ? CONFIG_EXAMPLE_MQTT_PASSWORD : NULL;

    const lwlte_mqtt_config_t mqtt_config = {
        .host = CONFIG_EXAMPLE_MQTT_HOST,
        .port = CONFIG_EXAMPLE_MQTT_PORT,
        .transport = CONFIG_EXAMPLE_MQTT_TLS_ENABLE ?
                     LWLTE_MQTT_TRANSPORT_TLS : LWLTE_MQTT_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = 88,
        .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
        .username = mqtt_username,
        .password = mqtt_password,
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
    ESP_LOGI(TAG,
             "MQTT transport=%s port=%d ssl_context=%u auth=%s (TLS default port 8883)",
             CONFIG_EXAMPLE_MQTT_TLS_ENABLE ? "TLS" : "plain TCP",
             CONFIG_EXAMPLE_MQTT_PORT, 88U,
             CONFIG_EXAMPLE_MQTT_TLS_ENABLE ? "server" : "none");

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

    if (CONFIG_EXAMPLE_MQTT_TLS_ENABLE) {
        const char *ca_pem = CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM;
        if (ca_pem[0] == '\0') {
            ESP_LOGE(TAG,
                     "MQTT TLS enabled but CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM is empty");
            idle_forever();
        }
        char *ca_pem_normalized = example_normalize_pem_newlines(ca_pem);
        if (!ca_pem_normalized) {
            ESP_LOGE(TAG, "MQTT TLS CA PEM normalization failed");
            idle_forever();
        }
        const lwlte_ssl_context_config_t ssl_config = {
            .context_id = 88,
            .auth_mode = LWLTE_SSL_AUTH_SERVER,
            .tls_version = 3,
            .ignore_cert_time = true,
            .hostname = CONFIG_EXAMPLE_MQTT_HOST,
        };
        const lwlte_ssl_credentials_t credentials = {
            .ca_cert_pem = (const uint8_t *)ca_pem_normalized,
            .ca_cert_len = strlen(ca_pem_normalized),
        };
        ret = lwlte_ssl_provision(lte, &ssl_config, &credentials);
        free(ca_pem_normalized);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SSL provision failed: %s", esp_err_to_name(ret));
            idle_forever();
        }
        lwlte_ssl_context_status_t status = {0};
        ret = lwlte_ssl_get_context_status(lte, 88, &status);
        const bool ssl_status_valid = ret == ESP_OK &&
                                      status.provisioned &&
                                      status.auth_mode == LWLTE_SSL_AUTH_SERVER &&
                                      status.ca_cert_present;
        if (!ssl_status_valid) {
            ESP_LOGE(TAG, "SSL status invalid: ret=%s provisioned=%d auth=%d ca=%d",
                     esp_err_to_name(ret), (int)status.provisioned,
                     (int)status.auth_mode, (int)status.ca_cert_present);
            idle_forever();
        }
    }

    /* 网络 online 后完成可选 TLS provisioning，再启动 MQTT 客户端。 */
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

static void publish_telemetry(lwlte_handle_t lte)
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

static char *example_normalize_pem_newlines(const char *pem)
{
    if (!pem) {
        return NULL;
    }

    char *normalized = malloc(strlen(pem) + 1U);
    if (!normalized) {
        return NULL;
    }

    const char *src = pem;
    char *dst = normalized;
    while (*src != '\0') {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\n';
            src += 2;
        } else if (src[0] == '\\' && src[1] == 'r') {
            *dst++ = '\r';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    return normalized;
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}
