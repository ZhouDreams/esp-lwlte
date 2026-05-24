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

#include "at_engine.h"
#include "lwlte_core.h"
#include "modem.h"
#include "modem_air780ep.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "basic_connect"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1

#define EXAMPLE_MODULE_POWER_STABLE_MS       3000
#define EXAMPLE_CORE_READY_TIMEOUT_MS        30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS        120000
#define EXAMPLE_POLL_INTERVAL_MS             100
#define EXAMPLE_STATUS_LOG_INTERVAL_MS       5000

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    at_engine_t *at;
    modem_t *modem;
    lwlte_core_t *core;
} example_runtime_t;

typedef struct {
    volatile bool started;
    volatile bool ready;
    volatile bool connecting;
    volatile bool online;
    volatile bool offline;
    volatile bool stopped;
    volatile bool error;
    volatile int error_code;
} example_flags_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 初始化 Air780EP EN 引脚
 * @details Initialize Air780EP EN pin
 * @return
 *         - ESP_OK: 成功
 *         - 其他: GPIO 错误码
 */
static esp_err_t init_module_enable_pin(void);

/**
 * @brief 创建 AT 引擎
 * @details Create AT engine
 * @return
 *         - 非 NULL: 创建成功
 *         - NULL: 创建失败
 */
static at_engine_t *create_at_engine(void);

/**
 * @brief 创建并初始化 Air780EP modem
 * @details Create and initialize Air780EP modem
 * @param[in] at AT 引擎句柄
 * @param[out] out_modem modem 句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - 其他: modem 错误码
 */
static esp_err_t create_modem(at_engine_t *at, modem_t **out_modem);

/**
 * @brief 创建 Core 服务
 * @details Create Core service
 * @param[in] modem modem 句柄
 * @return
 *         - 非 NULL: 创建成功
 *         - NULL: 创建失败
 */
static lwlte_core_t *create_core(modem_t *modem);

/**
 * @brief Core 事件回调
 * @details Core event callback
 * @param[in] core Core 句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据
 * @param[in] user_ctx 用户上下文
 */
static void core_event_cb(lwlte_core_t *core, lwlte_core_event_id_t event_id,
                          const lwlte_core_event_data_t *data, void *user_ctx);

/**
 * @brief 等待布尔标志为 true
 * @details Wait until boolean flag becomes true
 * @param[in] flag 标志地址
 * @param[in] timeout_ms 超时时间
 * @param[in] description 等待描述
 * @return
 *         - true: 等到标志
 *         - false: 等待超时
 */
static bool wait_flag(const volatile bool *flag, uint32_t timeout_ms,
                      const char *description);

/**
 * @brief 打印 Core 和网络状态
 * @details Log Core and network state
 * @param[in] core Core 句柄
 * @param[in] stage 当前阶段描述
 */
static void log_core_status(lwlte_core_t *core, const char *stage);

/**
 * @brief 清理运行时资源
 * @details Cleanup runtime resources
 * @param[in,out] runtime 运行时资源
 */
static void cleanup_runtime(example_runtime_t *runtime);

/**
 * @brief 进入永久等待
 * @details Enter forever delay loop
 */
static void idle_forever(void);

/**
 * @brief 获取 Core 状态字符串
 * @details Get Core state string
 * @param[in] state Core 状态
 * @return 状态字符串
 */
static const char *core_state_name(lwlte_core_state_t state);

/**
 * @brief 获取网络状态字符串
 * @details Get network state string
 * @param[in] state 网络状态
 * @return 状态字符串
 */
static const char *net_state_name(lwlte_net_state_t state);

/**
 * @brief 获取 Core 事件字符串
 * @details Get Core event string
 * @param[in] event_id Core 事件 ID
 * @return 事件字符串
 */
static const char *core_event_name(lwlte_core_event_id_t event_id);

/**********************
 *  STATIC VARIABLES
 **********************/
static example_flags_t s_flags;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    example_runtime_t runtime = {0};

    ESP_LOGI(TAG, "esp-lwlte basic connect example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);

    esp_err_t ret = init_module_enable_pin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init module EN pin failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    runtime.at = create_at_engine();
    if (!runtime.at) {
        ESP_LOGE(TAG, "create AT engine failed");
        cleanup_runtime(&runtime);
        idle_forever();
    }

    ret = create_modem(runtime.at, &runtime.modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create modem failed: %s", esp_err_to_name(ret));
        cleanup_runtime(&runtime);
        idle_forever();
    }

    runtime.core = create_core(runtime.modem);
    if (!runtime.core) {
        ESP_LOGE(TAG, "create Core failed");
        cleanup_runtime(&runtime);
        idle_forever();
    }

    ret = lwlte_core_register_event_callback(runtime.core, core_event_cb, &s_flags);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register Core callback failed: %s", esp_err_to_name(ret));
        cleanup_runtime(&runtime);
        idle_forever();
    }

    ret = lwlte_core_start(runtime.core);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start Core request failed: %s", esp_err_to_name(ret));
        log_core_status(runtime.core, "start request failed");
        cleanup_runtime(&runtime);
        idle_forever();
    }

    if (!wait_flag(&s_flags.ready, EXAMPLE_CORE_READY_TIMEOUT_MS, "Core ready")) {
        log_core_status(runtime.core, "Core ready timeout");
        cleanup_runtime(&runtime);
        idle_forever();
    }

    ret = lwlte_core_connect(runtime.core);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect request failed: %s", esp_err_to_name(ret));
        log_core_status(runtime.core, "connect request failed");
        cleanup_runtime(&runtime);
        idle_forever();
    }

    uint32_t elapsed_ms = 0;
    while (!s_flags.online && !s_flags.error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (s_flags.online) {
        ESP_LOGI(TAG, "LTE network is online");
    } else {
        ESP_LOGW(TAG, "LTE network did not become online, error=%d", s_flags.error_code);
        log_core_status(runtime.core, "network wait ended");
    }

    while (1) {
        log_core_status(runtime.core, "periodic");
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_LOG_INTERVAL_MS));
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t init_module_enable_pin(void)
{
    esp_err_t ret = gpio_reset_pin(EXAMPLE_LTE_EN_PIN);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_direction(EXAMPLE_LTE_EN_PIN, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(EXAMPLE_LTE_EN_PIN, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Air780EP EN GPIO%d held high", EXAMPLE_LTE_EN_PIN);
    vTaskDelay(pdMS_TO_TICKS(EXAMPLE_MODULE_POWER_STABLE_MS));

    return ESP_OK;
}

static at_engine_t *create_at_engine(void)
{
    const at_engine_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .rx_buf_size = 0,
        .rx_task_stack = 0,
        .rx_task_priority = 0,
        .rx_line_buf_size = 0,
        .cmd_default_timeout_ms = 0,
        .max_response_lines = 0,
    };

    return at_engine_create(&config);
}

static esp_err_t create_modem(at_engine_t *at, modem_t **out_modem)
{
    if (!at || !out_modem) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_modem = NULL;

    const modem_air780ep_config_t config = {
        .pwrkey_pin = GPIO_NUM_NC,
        .reset_pin = GPIO_NUM_NC,
        .status_pin = GPIO_NUM_NC,
        .power_on_pulse_ms = 0,
        .reset_pulse_ms = 0,
        .boot_wait_ms = 0,
        .default_cmd_timeout_ms = 0,
        .event_queue_size = 0,
        .event_task_stack = 0,
        .event_task_priority = 0,
    };

    modem_t *modem = modem_air780ep_create(at, &config);
    if (!modem) {
        return ESP_FAIL;
    }
    *out_modem = modem;

    esp_err_t ret = modem_init(modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem init failed: %s", esp_err_to_name(ret));
        esp_err_t destroy_ret = modem_destroy(modem);
        if (destroy_ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem after init failure failed: %s",
                     esp_err_to_name(destroy_ret));
            return destroy_ret;
        }
        *out_modem = NULL;
        return ret;
    }

    return ESP_OK;
}

static lwlte_core_t *create_core(modem_t *modem)
{
    const lwlte_core_config_t config = {
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .net_activate_timeout_ms = 0,
        .reconnect_delay_ms = 0,
        .auto_connect = false,
        .fsm_queue_size = 0,
        .fsm_task_stack = 0,
        .fsm_task_priority = 0,
    };

    return lwlte_core_create(&config, modem);
}

static void core_event_cb(lwlte_core_t *core, lwlte_core_event_id_t event_id,
                          const lwlte_core_event_data_t *data, void *user_ctx)
{
    (void)core;

    example_flags_t *flags = (example_flags_t *)user_ctx;
    if (!flags) {
        return;
    }

    if (data) {
        ESP_LOGI(TAG, "Core event: %s net=%s err=%d",
                 core_event_name(event_id), net_state_name(data->net_state),
                 data->error_code);
    } else {
        ESP_LOGI(TAG, "Core event: %s", core_event_name(event_id));
    }

    switch (event_id) {
    case LWLTE_CORE_EVENT_STARTED:
        flags->started = true;
        break;
    case LWLTE_CORE_EVENT_READY:
        flags->ready = true;
        break;
    case LWLTE_CORE_EVENT_NET_CONNECTING:
        flags->connecting = true;
        break;
    case LWLTE_CORE_EVENT_NET_ONLINE:
        flags->online = true;
        flags->error = false;
        flags->error_code = 0;
        break;
    case LWLTE_CORE_EVENT_NET_OFFLINE:
        flags->offline = true;
        flags->online = false;
        break;
    case LWLTE_CORE_EVENT_NET_ERROR:
    case LWLTE_CORE_EVENT_ERROR:
        flags->error_code = data ? data->error_code : ESP_FAIL;
        flags->error = true;
        break;
    case LWLTE_CORE_EVENT_STOPPED:
        flags->stopped = true;
        break;
    default:
        break;
    }
}

static bool wait_flag(const volatile bool *flag, uint32_t timeout_ms,
                      const char *description)
{
    uint32_t elapsed_ms = 0;
    while (!*flag && elapsed_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (*flag) {
        ESP_LOGI(TAG, "%s reached", description);
        return true;
    }

    ESP_LOGW(TAG, "%s timeout after %lu ms", description,
             (unsigned long)timeout_ms);
    return false;
}

static void log_core_status(lwlte_core_t *core, const char *stage)
{
    if (!core) {
        ESP_LOGW(TAG, "%s: Core is NULL", stage);
        return;
    }

    lwlte_core_state_t core_state = LWLTE_CORE_STATE_STOPPED;
    lwlte_net_state_t net_state = LWLTE_NET_STATE_OFFLINE;
    esp_err_t core_ret = lwlte_core_get_state(core, &core_state);
    esp_err_t net_ret = lwlte_core_get_net_state(core, &net_state);

    if (core_ret != ESP_OK || net_ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: get state failed core_ret=%s net_ret=%s", stage,
                 esp_err_to_name(core_ret), esp_err_to_name(net_ret));
        return;
    }

    ESP_LOGI(TAG, "%s: core=%s net=%s", stage, core_state_name(core_state),
             net_state_name(net_state));
}

static void cleanup_runtime(example_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    if (runtime->core) {
        esp_err_t ret = lwlte_core_destroy(runtime->core);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy Core failed: %s", esp_err_to_name(ret));
            return;
        }
        runtime->core = NULL;
    }
    if (runtime->modem) {
        esp_err_t ret = modem_destroy(runtime->modem);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem failed: %s", esp_err_to_name(ret));
            return;
        }
        runtime->modem = NULL;
    }
    if (runtime->at) {
        esp_err_t ret = at_engine_destroy(runtime->at);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy AT engine failed: %s", esp_err_to_name(ret));
            return;
        }
        runtime->at = NULL;
    }
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static const char *core_state_name(lwlte_core_state_t state)
{
    switch (state) {
    case LWLTE_CORE_STATE_STOPPED:
        return "STOPPED";
    case LWLTE_CORE_STATE_STARTING:
        return "STARTING";
    case LWLTE_CORE_STATE_READY:
        return "READY";
    case LWLTE_CORE_STATE_NET_ACTIVATING:
        return "NET_ACTIVATING";
    case LWLTE_CORE_STATE_ONLINE:
        return "ONLINE";
    case LWLTE_CORE_STATE_ERROR:
        return "ERROR";
    case LWLTE_CORE_STATE_DESTROYING:
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

static const char *core_event_name(lwlte_core_event_id_t event_id)
{
    switch (event_id) {
    case LWLTE_CORE_EVENT_STARTED:
        return "STARTED";
    case LWLTE_CORE_EVENT_READY:
        return "READY";
    case LWLTE_CORE_EVENT_NET_CONNECTING:
        return "NET_CONNECTING";
    case LWLTE_CORE_EVENT_NET_ONLINE:
        return "NET_ONLINE";
    case LWLTE_CORE_EVENT_NET_OFFLINE:
        return "NET_OFFLINE";
    case LWLTE_CORE_EVENT_NET_ERROR:
        return "NET_ERROR";
    case LWLTE_CORE_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_CORE_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
