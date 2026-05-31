/**
 * @file lwlte_air780ep.c
 * @brief Air780EP LTE 用户门面工厂实现
 * @details Air780EP LTE user facade factory implementation
 * @author JovisDreams
 * @date 2026-05-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "lwlte_air780ep.h"
#include "lwlte_priv.h"

#include "at_engine.h"
#include "core.h"
#include "modem_air780ep.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "lwlte_air780ep"
#define LWLTE_AIR780EP_DEFAULT_READY_MS      30000U
#define LWLTE_AIR780EP_PRIMARY_CID           1U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 检查 Air780EP 门面配置
 * @details Validate Air780EP facade configuration
 * @param[in] config Air780EP LTE 初始化配置
 * @return
 *         - ESP_OK: 配置有效
 *         - ESP_ERR_INVALID_ARG: 配置无效
 */
static esp_err_t validate_config(const lwlte_air780ep_config_t *config);

/**
 * @brief 检查必填 GPIO 是否有效
 * @details Check whether required GPIO is valid
 * @param[in] pin GPIO 编号
 * @return
 *         - true: 有效
 *         - false: 无效
 */
static bool gpio_required_valid(gpio_num_t pin);

/**
 * @brief 检查可选 GPIO 是否有效
 * @details Check whether optional GPIO is valid
 * @param[in] pin GPIO 编号
 * @return
 *         - true: GPIO_NUM_NC 或有效 GPIO
 *         - false: 无效
 */
static bool gpio_optional_valid(gpio_num_t pin);

/**
 * @brief 检查整数是否非负
 * @details Check whether integer is non-negative
 * @param[in] value 整数值
 * @return
 *         - true: 非负
 *         - false: 负数
 */
static bool non_negative_int(int value);

/**
 * @brief 获取初始化 ready 总超时
 * @details Get total init ready timeout
 * @param[in] config Air780EP LTE 初始化配置
 * @return 初始化 ready 总超时； Total init ready timeout
 */
static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config);

/**
 * @brief 计算初始化剩余超时
 * @details Calculate remaining init timeout
 * @param[in] start_tick 初始化开始 tick； Init start tick
 * @param[in] total_timeout_ms 初始化总超时； Total init timeout
 * @param[out] out_timeout_ms 剩余超时输出； Remaining timeout output
 * @return
 *         - ESP_OK: 成功； Success
 *         - ESP_ERR_INVALID_ARG: 参数无效； Invalid argument
 *         - ESP_ERR_TIMEOUT: 初始化已超时； Init timed out
 */
static esp_err_t remaining_timeout_ms(TickType_t start_tick,
                                      uint32_t total_timeout_ms,
                                      uint32_t *out_timeout_ms);

/**
 * @brief 初始化失败后清理门面
 * @details Clean facade after initialization failure
 * @param[in] me LTE 用户门面句柄，可能为 NULL
 * @param[in] original_err 原始错误码
 * @return 原始错误码；若无原始错误则返回清理错误码或 ESP_FAIL
 */
static esp_err_t cleanup_after_failure(lwlte_t *me, esp_err_t original_err);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");
    *out_lte = NULL;

    esp_err_t ret = validate_config(config);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid config");
    const uint32_t total_ready_timeout_ms = ready_timeout_ms(config);
    const TickType_t init_start_tick = xTaskGetTickCount();

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
        .rx_line_buf_size = config->at_rx_line_buf_size,
        .cmd_default_timeout_ms = config->at_cmd_default_timeout_ms,
        .max_response_lines = config->at_max_response_lines,
    };
    me->at = at_engine_create(&at_config);
    if (!me->at) {
        ESP_LOGE(TAG, "create AT engine failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    uint32_t stage_timeout_ms = 0;
    ret = remaining_timeout_ms(init_start_tick, total_ready_timeout_ms,
                               &stage_timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init timeout before modem create");
        return cleanup_after_failure(me, ret);
    }

    const modem_air780ep_config_t modem_config = {
        .en_pin = config->en_pin,
        .reset_pulse_ms = config->modem_reset_pulse_ms,
        .ready_timeout_ms = stage_timeout_ms,
        .default_cmd_timeout_ms = config->modem_default_cmd_timeout_ms,
        .event_queue_size = config->modem_event_queue_size,
        .event_task_stack = config->modem_event_task_stack,
        .event_task_priority = config->modem_event_task_priority,
    };
    me->modem = modem_air780ep_create(me->at, &modem_config);
    if (!me->modem) {
        ESP_LOGE(TAG, "create Air780EP modem failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    ret = modem_start(me->modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start modem failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }

    const core_config_t core_config = {
        .apn = config->apn ? config->apn : "",
        .primary_cid = config->primary_cid,
        .net_activate_timeout_ms = config->net_activate_timeout_ms,
        .reconnect_delay_ms = config->reconnect_delay_ms,
        .auto_connect = false,
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

    ret = core_start(me->core);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start core failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }

    ret = remaining_timeout_ms(init_start_tick, total_ready_timeout_ms,
                               &stage_timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init timeout before waiting core ready");
        return cleanup_after_failure(me, ret);
    }

    ret = lwlte_wait_ready(me, stage_timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wait ready failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }

    if (config->auto_connect) {
        ret = lwlte_connect(me);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "auto connect request failed: %s", esp_err_to_name(ret));
            return cleanup_after_failure(me, ret);
        }
    }

    *out_lte = me;
    return ESP_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static esp_err_t validate_config(const lwlte_air780ep_config_t *config)
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
    ESP_RETURN_ON_FALSE(config->primary_cid == LWLTE_AIR780EP_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG,
                        "primary CID must be 1");
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
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT host is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.port > 0,
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT port is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.client_id && config->mqtt_client.client_id[0],
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT client_id is required");
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

static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config)
{
    if (config && config->init_ready_timeout_ms > 0) {
        return config->init_ready_timeout_ms;
    }

    return LWLTE_AIR780EP_DEFAULT_READY_MS;
}

static esp_err_t remaining_timeout_ms(TickType_t start_tick,
                                      uint32_t total_timeout_ms,
                                      uint32_t *out_timeout_ms)
{
    ESP_RETURN_ON_FALSE(out_timeout_ms, ESP_ERR_INVALID_ARG, TAG,
                        "out_timeout_ms is NULL");

    TickType_t total_ticks = pdMS_TO_TICKS(total_timeout_ms);
    if (total_timeout_ms > 0 && total_ticks == 0) {
        total_ticks = 1;
    }
    if ((uint64_t)total_ticks * portTICK_PERIOD_MS < total_timeout_ms) {
        total_ticks++;
    }

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    if (elapsed_ticks >= total_ticks) {
        *out_timeout_ms = 0;
        return ESP_ERR_TIMEOUT;
    }

    TickType_t remaining_ticks = total_ticks - elapsed_ticks;
    *out_timeout_ms = (uint32_t)(remaining_ticks * portTICK_PERIOD_MS);
    if (*out_timeout_ms == 0) {
        *out_timeout_ms = portTICK_PERIOD_MS ? portTICK_PERIOD_MS : 1;
    }

    return ESP_OK;
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
