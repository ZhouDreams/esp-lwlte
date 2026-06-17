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
#include "lwlte.h"
#include "lwlte_priv.h"

#include "at_engine.h"
#include "core.h"
#include "modem_air780ep.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "lwlte_air780ep"
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
 * @brief 初始化失败后清理门面
 * @details Clean facade after initialization failure
 * @param[in] me LTE 用户门面句柄，可能为 NULL
 * @param[in] original_err 原始错误码
 * @return 原始错误码；若无原始错误则返回清理错误码或 ESP_FAIL
 */
static esp_err_t cleanup_after_failure(lwlte_handle_t *me, esp_err_t original_err);

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
                              lwlte_handle_t **out_lte)
{
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 1：参数校验与初始化准备
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");
    *out_lte = NULL;

    esp_err_t ret = validate_config(config);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid config");

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 2：创建门面壳并创建 AT Engine
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    lwlte_handle_t *me = NULL;
    ret = lwlte_create_empty(&me);
    ESP_RETURN_ON_ERROR(ret, TAG, "create facade failed");

    /* AT Engine 是最底层：直接操作 UART 硬件，运行 RX task 接收字节 */
    const at_engine_config_t at_config = {
        .uart = {
            .uart_num = config->base.uart.num,
            .tx_pin = config->base.uart.tx_pin,
            .rx_pin = config->base.uart.rx_pin,
            .baud_rate = config->base.uart.baud_rate,
            .rx_buf_size = config->base.at_engine.rx_buf_size,
        },
        .runtime = {
            .rx_task_stack = config->base.at_engine.rx_task_stack,
            .rx_task_priority = config->base.at_engine.rx_task_priority,
            .rx_line_buf_size = config->base.at_engine.rx_line_buf_size,
            .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
            .max_response_lines = config->base.at_engine.max_response_lines,
        },
    };
    me->at = at_engine_create(&at_config);
    if (!me->at) {
        ESP_LOGE(TAG, "create AT engine failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 3：创建 Air780EP Modem 适配器
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    const modem_air780ep_config_t modem_config = {
        .base = {
            .hardware = {
                .en_pin = config->base.modem.en_pin,
            },
            .timing = {
                .reset_pulse_ms = config->base.modem.reset_pulse_ms,
                .ready_timeout_ms = config->base.modem.ready_timeout_ms,
                .default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms,
            },
            .event = {
                .event_queue_size = config->base.modem.event_queue_size,
                .event_task_stack = config->base.modem.event_task_stack,
                .event_task_priority = config->base.modem.event_task_priority,
            },
        },
    };
    me->modem = modem_air780ep_create(me->at, &modem_config);
    if (!me->modem) {
        ESP_LOGE(TAG, "create Air780EP modem failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 4：创建 Core Service
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    /* Core 是网络状态机的归属层，负责 PDP 管理、连接/重连策略 */
    me->event_loop = config->base.event.loop;   /* NULL = use default loop */

    const core_config_t core_config = {
        .event = {
            .loop = me->event_loop,
        },
        .network = {
            .apn = config->base.core.apn ? config->base.core.apn : "",
            .primary_cid = config->base.core.primary_cid,
            .net_activate_timeout_ms = config->base.core.net_activate_timeout_ms,
            .reconnect_delay_ms = config->base.core.reconnect_delay_ms,
        },
        .fsm = {
            .queue_size = config->base.core.fsm_queue_size,
            .task_stack = config->base.core.fsm_task_stack,
            .task_priority = config->base.core.fsm_task_priority,
        },
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

    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 5：创建 Ping Client
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
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

static esp_err_t validate_config(const lwlte_air780ep_config_t *config)
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
    ESP_RETURN_ON_FALSE(config->base.core.primary_cid == LWLTE_AIR780EP_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG,
                        "primary CID must be 1");
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
