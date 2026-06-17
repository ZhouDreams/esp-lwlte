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
#include "esp_event.h"
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
static esp_err_t cleanup_after_failure(lwlte_handle_t *me, esp_err_t original_err);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_handle_t **out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");
    *out_lte = NULL;

    esp_err_t ret = validate_config(config);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid config");

    lwlte_handle_t *me = NULL;
    ret = lwlte_create_empty(&me);
    ESP_RETURN_ON_ERROR(ret, TAG, "create facade failed");

    const at_engine_config_t at_config = {
        /* uart 组 */
        .uart_num = config->base.uart.num,
        .tx_pin = config->base.uart.tx_pin,
        .rx_pin = config->base.uart.rx_pin,
        .baud_rate = config->base.uart.baud_rate,
        /* at_engine 组 */
        .rx_buf_size = config->base.at_engine.rx_buf_size,
        .rx_task_stack = config->base.at_engine.rx_task_stack,
        .rx_task_priority = config->base.at_engine.rx_task_priority,
        .rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?
                             config->base.at_engine.rx_line_buf_size :
                             LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE,
        .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
        .max_response_lines = config->base.at_engine.max_response_lines,
    };
    me->at = at_engine_create(&at_config);
    if (!me->at) {
        ESP_LOGE(TAG, "create AT engine failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    const modem_ml307r_config_t modem_config = {
        .en_pin = config->base.modem.en_pin,
        .reset_pulse_ms = config->base.modem.reset_pulse_ms,
        .ready_timeout_ms = config->base.modem.ready_timeout_ms,
        .default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms,
        .event_queue_size = config->base.modem.event_queue_size,
        .event_task_stack = config->base.modem.event_task_stack,
        .event_task_priority = config->base.modem.event_task_priority,
    };
    me->modem = modem_ml307r_create(me->at, &modem_config);
    if (!me->modem) {
        ESP_LOGE(TAG, "create ML307R modem failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    me->event_loop = config->base.event.loop;   /* NULL = use default loop */

    const core_config_t core_config = {
        .apn = config->base.core.apn ? config->base.core.apn : "",
        .primary_cid = config->base.core.primary_cid,
        .net_activate_timeout_ms = config->base.core.net_activate_timeout_ms,
        .reconnect_delay_ms = config->base.core.reconnect_delay_ms,
        .fsm_queue_size = config->base.core.fsm_queue_size,
        .fsm_task_stack = config->base.core.fsm_task_stack,
        .fsm_task_priority = config->base.core.fsm_task_priority,
        .event_loop = me->event_loop,
    };
    me->core = core_create(&core_config, me->modem);
    if (!me->core) {
        ESP_LOGE(TAG, "create core failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    /* Register internal handler for lwlte_wait_ready synchronization */
    if (me->event_loop) {
        ret = esp_event_handler_register_with(me->event_loop, LWLTE_EVENT,
                                              LWLTE_EVENT_READY,
                                              facade_ready_handler, me);
    } else {
        ret = esp_event_handler_register(LWLTE_EVENT,
                                         LWLTE_EVENT_READY,
                                         facade_ready_handler, me);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register ready handler failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }
    if (me->event_loop) {
        ret = esp_event_handler_register_with(me->event_loop, LWLTE_EVENT,
                                              LWLTE_EVENT_ERROR,
                                              facade_ready_handler, me);
    } else {
        ret = esp_event_handler_register(LWLTE_EVENT,
                                         LWLTE_EVENT_ERROR,
                                         facade_ready_handler, me);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register error handler failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }

    me->ping = ping_client_create(me->core);
    if (!me->ping) {
        ESP_LOGE(TAG, "create Ping client failed");
        return cleanup_after_failure(me, ESP_OK);
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
    ESP_RETURN_ON_FALSE(config->base.uart.num >= UART_NUM_0 &&
                        config->base.uart.num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(gpio_required_valid(config->base.uart.tx_pin) &&
                        gpio_required_valid(config->base.uart.rx_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(gpio_optional_valid(config->base.modem.en_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid en_pin GPIO");
    ESP_RETURN_ON_FALSE(config->base.uart.baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART baud rate");
    ESP_RETURN_ON_FALSE(config->base.core.primary_cid == LWLTE_ML307R_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG, "primary CID must be 1");
    ESP_RETURN_ON_FALSE(non_negative_int(config->base.at_engine.rx_buf_size) &&
                        non_negative_int(config->base.at_engine.rx_task_stack) &&
                        non_negative_int(config->base.at_engine.rx_task_priority) &&
                        non_negative_int(config->base.at_engine.rx_line_buf_size) &&
                        non_negative_int(config->base.at_engine.cmd_default_timeout_ms) &&
                        non_negative_int(config->base.at_engine.max_response_lines) &&
                        non_negative_int(config->base.modem.event_queue_size) &&
                        non_negative_int(config->base.modem.event_task_stack) &&
                        non_negative_int(config->base.modem.event_task_priority) &&
                        non_negative_int(config->base.core.fsm_queue_size) &&
                        non_negative_int(config->base.core.fsm_task_stack) &&
                        non_negative_int(config->base.core.fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");

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

static esp_err_t cleanup_after_failure(lwlte_handle_t *me, esp_err_t original_err)
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
