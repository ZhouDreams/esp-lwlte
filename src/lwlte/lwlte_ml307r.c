/**
 * @file lwlte_ml307r.c
 * @brief ML307R LTE 用户门面工厂实现
 * @details ML307R LTE user facade factory implementation
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include "lwlte.h"
#include "lwlte_priv.h"

#include "at_engine.h"
#include "core.h"
#include "modem_ml307r.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                         "lwlte_ml307r"
#define LWLTE_ML307R_PRIMARY_CID     1U
#define LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE 2048

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t validate_config(const lwlte_ml307r_config_t *config);
static bool gpio_required_valid(gpio_num_t pin);
static bool gpio_optional_valid(gpio_num_t pin);
static bool non_negative_int(int value);
static esp_err_t cleanup_after_failure(lwlte_t *me, esp_err_t original_err);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");
    *out_lte = NULL;

    esp_err_t ret = validate_config(config);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid config");

    lwlte_t *me = NULL;
    ret = lwlte_create_empty(&me);
    ESP_RETURN_ON_ERROR(ret, TAG, "create facade failed");

    const at_engine_config_t at_config = {
        .uart_num = config->uart_num,
        .tx_pin = config->uart_tx_pin,
        .rx_pin = config->uart_rx_pin,
        .baud_rate = config->uart_baud_rate,
        .rx_buf_size = config->at_rx_buf_size,
        .rx_task_stack = config->at_rx_task_stack,
        .rx_task_priority = config->at_rx_task_priority,
        .rx_line_buf_size = config->at_rx_line_buf_size ?
                             config->at_rx_line_buf_size :
                             LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE,
        .cmd_default_timeout_ms = config->at_cmd_default_timeout_ms,
        .max_response_lines = config->at_max_response_lines,
    };
    me->at = at_engine_create(&at_config);
    if (!me->at) {
        ESP_LOGE(TAG, "create AT engine failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    const modem_ml307r_config_t modem_config = {
        .en_pin = config->en_pin,
        .reset_pulse_ms = config->modem_reset_pulse_ms,
        .ready_timeout_ms = config->init_ready_timeout_ms,
        .default_cmd_timeout_ms = config->modem_default_cmd_timeout_ms,
        .event_queue_size = config->modem_event_queue_size,
        .event_task_stack = config->modem_event_task_stack,
        .event_task_priority = config->modem_event_task_priority,
    };
    me->modem = modem_ml307r_create(me->at, &modem_config);
    if (!me->modem) {
        ESP_LOGE(TAG, "create ML307R modem failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    const core_config_t core_config = {
        .apn = config->apn ? config->apn : "",
        .primary_cid = config->primary_cid,
        .net_activate_timeout_ms = config->net_activate_timeout_ms,
        .reconnect_delay_ms = config->reconnect_delay_ms,
        .fsm_queue_size = config->core_fsm_queue_size,
        .fsm_task_stack = config->core_fsm_task_stack,
        .fsm_task_priority = config->core_fsm_task_priority,
    };
    me->core = core_create(&core_config, me->modem);
    if (!me->core) {
        ESP_LOGE(TAG, "create core failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    ret = core_register_event_callback(me->core, lwlte_handle_core_event, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register core event bridge failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }

    me->ping = ping_client_create(me->core);
    if (!me->ping) {
        ESP_LOGE(TAG, "create Ping client failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    if (config->mqtt_client.enabled) {
        const mqtt_client_config_t mqtt_config = {
            .transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
            .host = config->mqtt_client.host,
            .port = config->mqtt_client.port,
            .client_id = config->mqtt_client.client_id,
            .username = config->mqtt_client.username,
            .password = config->mqtt_client.password,
            .keepalive_s = config->mqtt_client.keepalive_s,
            .clean_session = config->mqtt_client.clean_session,
            .fsm_queue_size = config->mqtt_client.fsm_queue_size,
            .fsm_task_stack = config->mqtt_client.fsm_task_stack,
            .fsm_task_priority = config->mqtt_client.fsm_task_priority,
        };
        me->mqtt = mqtt_client_create(&mqtt_config, me->core);
        if (!me->mqtt) {
            ESP_LOGE(TAG, "create MQTT client failed");
            return cleanup_after_failure(me, ESP_OK);
        }
        ret = mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register MQTT event bridge failed: %s", esp_err_to_name(ret));
            return cleanup_after_failure(me, ret);
        }
    }

    *out_lte = me;
    return ESP_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t validate_config(const lwlte_ml307r_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->uart_num >= UART_NUM_0 &&
                        config->uart_num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(gpio_required_valid(config->uart_tx_pin) &&
                        gpio_required_valid(config->uart_rx_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(gpio_optional_valid(config->en_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid en_pin GPIO");
    ESP_RETURN_ON_FALSE(config->uart_baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART baud rate");
    ESP_RETURN_ON_FALSE(config->primary_cid == LWLTE_ML307R_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG, "primary CID must be 1");
    ESP_RETURN_ON_FALSE(non_negative_int(config->at_rx_buf_size) &&
                        non_negative_int(config->at_rx_task_stack) &&
                        non_negative_int(config->at_rx_task_priority) &&
                        non_negative_int(config->at_rx_line_buf_size) &&
                        non_negative_int(config->at_cmd_default_timeout_ms) &&
                        non_negative_int(config->at_max_response_lines) &&
                        non_negative_int(config->modem_event_queue_size) &&
                        non_negative_int(config->modem_event_task_stack) &&
                        non_negative_int(config->modem_event_task_priority) &&
                        non_negative_int(config->core_fsm_queue_size) &&
                        non_negative_int(config->core_fsm_task_stack) &&
                        non_negative_int(config->core_fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");
    if (config->mqtt_client.enabled) {
        ESP_RETURN_ON_FALSE(config->mqtt_client.host && config->mqtt_client.host[0],
                            ESP_ERR_INVALID_ARG, TAG, "MQTT host is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.port > 0,
                            ESP_ERR_INVALID_ARG, TAG, "MQTT port is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.client_id &&
                            config->mqtt_client.client_id[0],
                            ESP_ERR_INVALID_ARG, TAG, "MQTT client_id is required");
        ESP_RETURN_ON_FALSE(non_negative_int(config->mqtt_client.fsm_queue_size) &&
                            non_negative_int(config->mqtt_client.fsm_task_stack) &&
                            non_negative_int(config->mqtt_client.fsm_task_priority),
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT task fields must be non-negative");
    }

    return ESP_OK;
}

static bool gpio_required_valid(gpio_num_t pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX;
}

static bool gpio_optional_valid(gpio_num_t pin)
{
    return pin == GPIO_NUM_NC || gpio_required_valid(pin);
}

static bool non_negative_int(int value)
{
    return value >= 0;
}

static esp_err_t cleanup_after_failure(lwlte_t *me, esp_err_t original_err)
{
    esp_err_t ret = original_err;
    if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (!me) {
        return ret;
    }
    esp_err_t cleanup_ret = lwlte_destroy(me);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGW(TAG, "cleanup after init failure failed: %s",
                 esp_err_to_name(cleanup_ret));
        if (original_err == ESP_OK) {
            return cleanup_ret;
        }
    }
    return ret;
}
