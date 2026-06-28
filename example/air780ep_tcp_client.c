/**
 * @file air780ep_tcp_client.c
 * @brief Air780EP LTE TCP 客户端示例
 * @details Air780EP LTE TCP client example
 * @author JovisDreams
 * @date 2026-06-18
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
#define TAG                                      "air780ep_tcp"

#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1

#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_READY_TIMEOUT_MS                 30000
#define EXAMPLE_IDLE_DELAY_MS                    1000
#define EXAMPLE_PAYLOAD_BUF_LEN                  256

#ifndef CONFIG_EXAMPLE_TCP_TLS_ENABLE
#define CONFIG_EXAMPLE_TCP_TLS_ENABLE            0
#endif

#ifndef CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM
#define CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM       ""
#endif

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);
static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data);
static size_t build_payload(uint8_t *out, size_t out_len);
static int hex_nibble(char c);
static char *example_normalize_pem_newlines(const char *pem);
static void idle_forever(void);

/**********************
 *  STATIC VARIABLES
 **********************/
static lwlte_tcp_conn_t s_conn;
static uint8_t s_payload[EXAMPLE_PAYLOAD_BUF_LEN];
static size_t s_payload_len;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void example_air780ep_tcp_client_run(void)
{
    s_conn = NULL;
    s_payload_len = build_payload(s_payload, sizeof(s_payload));

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwlte_handle_t lte = NULL;
    lwlte_air780ep_config_t config = {
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
    config.base.at_engine.rx_line_buf_size = 2048;

    const lwlte_tcp_config_t tcp_config = {
        .send_queue_size = CONFIG_EXAMPLE_TCP_SEND_QUEUE_SIZE,
        .max_tx_len = CONFIG_EXAMPLE_TCP_MAX_TX_LEN,
        .max_rx_event_len = CONFIG_EXAMPLE_TCP_MAX_RX_EVENT_LEN,
        .open_timeout_ms = CONFIG_EXAMPLE_TCP_OPEN_TIMEOUT_MS,
    };

    ESP_LOGI(TAG, "Air780EP TCP client example host=%s port=%d payload_len=%u",
             CONFIG_EXAMPLE_TCP_HOST, CONFIG_EXAMPLE_TCP_PORT,
             (unsigned int)s_payload_len);

    ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                               lwlte_event_cb, lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_TCP_EVENT, ESP_EVENT_ANY_ID,
                                               tcp_event_cb, lte));
    ESP_ERROR_CHECK(lwlte_tcp_init(lte, &tcp_config));
    ESP_ERROR_CHECK(lwlte_start(lte));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_data;
    lwlte_handle_t lte = (lwlte_handle_t)arg;
    if ((lwlte_event_id_t)event_id != LWLTE_EVENT_NET_ONLINE || s_conn) {
        return;
    }

    esp_err_t ret;
    if (CONFIG_EXAMPLE_TCP_TLS_ENABLE) {
        const char *ca_pem = CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM;
        if (ca_pem[0] == '\0') {
            ESP_LOGE(TAG,
                     "TCP TLS enabled but CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM is empty");
            idle_forever();
        }
        char *ca_pem_normalized = example_normalize_pem_newlines(ca_pem);
        if (!ca_pem_normalized) {
            ESP_LOGE(TAG, "TCP TLS CA PEM normalization failed");
            idle_forever();
        }
        const lwlte_ssl_context_config_t ssl_config = {
            .context_id = 0,
            .auth_mode = LWLTE_SSL_AUTH_SERVER,
            .ignore_cert_time = true,
            .hostname = CONFIG_EXAMPLE_TCP_HOST,
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
        ret = lwlte_ssl_get_context_status(lte, 0, &status);
        const bool ssl_status_valid = ret == ESP_OK &&
                                      status.provisioned &&
                                      status.auth_mode == LWLTE_SSL_AUTH_SERVER &&
                                      status.ca_cert_present;
        if (!ssl_status_valid) {
            ESP_LOGE(TAG,
                     "SSL status invalid: ret=%s provisioned=%d auth=%d ca=%d",
                     esp_err_to_name(ret), (int)status.provisioned,
                     (int)status.auth_mode, (int)status.ca_cert_present);
            idle_forever();
        }
    }

    const lwlte_tcp_open_config_t open_cfg = {
        .host = CONFIG_EXAMPLE_TCP_HOST,
        .port = CONFIG_EXAMPLE_TCP_PORT,
        .transport = CONFIG_EXAMPLE_TCP_TLS_ENABLE
                        ? LWLTE_TCP_TRANSPORT_TLS
                        : LWLTE_TCP_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = 0,
        .user_ctx = lte,
    };
    ret = lwlte_tcp_open(lte, &open_cfg, &s_conn);
    ESP_LOGI(TAG, "TCP open submitted: %s", esp_err_to_name(ret));
}

static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;

    lwlte_tcp_event_data_t *data = event_data;
    const lwlte_tcp_conn_state_t conn_state = data ? data->conn_state : (lwlte_tcp_conn_state_t)-1;
    ESP_LOGI(TAG, "TCP event=%d(%s) state=%d(%s) err=%d modem=%d reason=%d",
             (int)event_id,
             example_lwlte_tcp_event_name((lwlte_tcp_event_id_t)event_id),
             (int)conn_state, example_lwlte_tcp_conn_state_name(conn_state),
             data ? data->error_code : 0,
             data ? data->modem_error_code : 0,
             data ? data->reason : 0);

    switch ((lwlte_tcp_event_id_t)event_id) {
    case LWLTE_TCP_EVENT_CONNECTED:
        if (data && data->conn && s_payload_len > 0) {
            esp_err_t ret = lwlte_tcp_send(data->conn, s_payload, s_payload_len);
            ESP_LOGI(TAG, "TCP send submitted: %s", esp_err_to_name(ret));
        }
        break;
    case LWLTE_TCP_EVENT_SENT:
        ESP_LOGI(TAG, "TCP sent len=%u", data ? (unsigned int)data->sent_len : 0U);
        break;
    case LWLTE_TCP_EVENT_DATA:
        if (data) {
            ESP_LOGI(TAG, "TCP RX len=%u", (unsigned int)data->payload_len);
            lwlte_tcp_conn_t conn = data->conn;
            (void)lwlte_tcp_close(conn);
        }
        break;
    case LWLTE_TCP_EVENT_DISCONNECTED:
    case LWLTE_TCP_EVENT_ERROR:
        if (s_conn) {
            (void)lwlte_tcp_conn_destroy(s_conn);
            s_conn = NULL;
        }
        break;
    default:
        break;
    }
    lwlte_tcp_event_data_release(data);
}

static size_t build_payload(uint8_t *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return 0;
    }

    const char *hex = CONFIG_EXAMPLE_TCP_PAYLOAD_HEX;
    if (hex[0] == '\0') {
        size_t len = strnlen(CONFIG_EXAMPLE_TCP_PAYLOAD, out_len);
        memcpy(out, CONFIG_EXAMPLE_TCP_PAYLOAD, len);
        return len;
    }

    size_t written = 0;
    while (hex[0] && hex[1] && written < out_len) {
        int hi = hex_nibble(hex[0]);
        int lo = hex_nibble(hex[1]);
        if (hi < 0 || lo < 0) {
            break;
        }
        out[written++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return written;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
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
