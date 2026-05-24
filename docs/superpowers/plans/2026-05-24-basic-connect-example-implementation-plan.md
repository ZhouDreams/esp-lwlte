# Basic Connect Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Do not commit unless the user explicitly authorizes it; use status checkpoints instead.

**Goal:** Build `examples/basic_connect`, a standalone ESP-IDF example that powers Air780EP EN, starts Core, connects LTE, and logs basic lifecycle/network events.

**Architecture:** The example is an application-layer ESP-IDF project. It performs board-specific hardware enable, constructs AT Engine and Air780EP Modem, then uses only Core public APIs for runtime start/connect/status flow.

**Tech Stack:** ESP-IDF C application, FreeRTOS delays, ESP logging, GPIO/UART drivers, `at_engine.h`, `modem_air780ep.h`, `modem.h`, and `lwlte_core.h`.

---

## File Structure

- Create `examples/basic_connect/CMakeLists.txt`: standalone ESP-IDF project entry that references the repository component in `../../src`.
- Create `examples/basic_connect/main/CMakeLists.txt`: registers the example `main.c` and declares component requirements.
- Create `examples/basic_connect/main/main.c`: complete basic-connect demonstration, including EN GPIO, object lifecycle, Core event callback, wait helpers, and state logging.
- Create `examples/basic_connect/README.md`: usage guide, wiring table, expected logs, troubleshooting.
- Create `examples/basic_connect/sdkconfig.defaults`: minimal defaults for logging/main stack.

Root `main/main.c` must not be changed.

---

## Task 1: Create Example Project Scaffold

**Files:**
- Create: `examples/basic_connect/CMakeLists.txt`
- Create: `examples/basic_connect/main/CMakeLists.txt`
- Create: `examples/basic_connect/sdkconfig.defaults`

- [ ] **Step 1: Write the top-level example CMake file**

Create `examples/basic_connect/CMakeLists.txt` with exactly:

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basic_connect)
```

- [ ] **Step 2: Write the example main component CMake file**

Create `examples/basic_connect/main/CMakeLists.txt` with exactly:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart esp_event
)
```

- [ ] **Step 3: Write minimal sdkconfig defaults**

Create `examples/basic_connect/sdkconfig.defaults` with exactly:

```text
CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

- [ ] **Step 4: Run scaffold red build**

Run from the repository root:

```bash
idf.py -C examples/basic_connect set-target esp32c3
idf.py -C examples/basic_connect build
```

Expected result: build fails because `examples/basic_connect/main/main.c` does not exist yet. This confirms the standalone example project is being discovered.

- [ ] **Step 5: Check workspace status**

Run:

```bash
git status --short --untracked-files=all
```

Expected result: new untracked files under `examples/basic_connect/`; no commit is made.

---

## Task 2: Implement Basic Connect Main Program

**Files:**
- Create: `examples/basic_connect/main/main.c`

- [ ] **Step 1: Write the complete example source**

Create `examples/basic_connect/main/main.c` with exactly:

```c
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
 * @return
 *         - 非 NULL: 创建并初始化成功
 *         - NULL: 创建或初始化失败
 */
static modem_t *create_modem(at_engine_t *at);

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

    runtime.modem = create_modem(runtime.at);
    if (!runtime.modem) {
        ESP_LOGE(TAG, "create modem failed");
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

static modem_t *create_modem(at_engine_t *at)
{
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
        return NULL;
    }

    esp_err_t ret = modem_init(modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem init failed: %s", esp_err_to_name(ret));
        (void)modem_destroy(modem);
        return NULL;
    }

    return modem;
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
        flags->error = true;
        flags->error_code = data ? data->error_code : ESP_FAIL;
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
        }
        runtime->core = NULL;
    }
    if (runtime->modem) {
        esp_err_t ret = modem_destroy(runtime->modem);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem failed: %s", esp_err_to_name(ret));
        }
        runtime->modem = NULL;
    }
    if (runtime->at) {
        esp_err_t ret = at_engine_destroy(runtime->at);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy AT engine failed: %s", esp_err_to_name(ret));
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
```

- [ ] **Step 2: Build the example**

Run from the repository root:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build succeeds. If it fails because the component name in `REQUIRES` is not `src`, inspect the generated ESP-IDF component name and update only `examples/basic_connect/main/CMakeLists.txt` accordingly.

- [ ] **Step 3: Check workspace status**

Run:

```bash
git status --short --untracked-files=all
```

Expected result: `examples/basic_connect/main/main.c` appears as an untracked file along with scaffold files; no commit is made.

---

## Task 3: Add Example README

**Files:**
- Create: `examples/basic_connect/README.md`

- [ ] **Step 1: Write the README**

Create `examples/basic_connect/README.md` with exactly:

```markdown
# Basic Connect Example

This example demonstrates the minimum useful esp-lwlte flow:

1. Enable the Air780EP module.
2. Create AT Engine over UART.
3. Create and initialize the Air780EP modem adapter.
4. Create Core Service.
5. Start Core and connect LTE.
6. Print Core/network events and periodic state.

## Hardware Wiring

Default wiring targets the ESP32-C3 Pro DevKit setup used during development.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Held high by the example |
| GND | GND | Common ground required |

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. This example holds GPIO2 high before creating the modem.

Do not wire GPIO2 to `pwrkey_pin` in this example. The current Air780EP adapter treats `pwrkey_pin` as a pulse signal and drives it low after the pulse.

## Defaults

| Setting | Value |
|---------|-------|
| UART | `UART_NUM_1` |
| Baud rate | `115200` |
| APN | empty string, using Air780EP/operator default path |
| Primary CID | `1` |

## Build

From the repository root:

```bash
idf.py -C examples/basic_connect set-target esp32c3
idf.py -C examples/basic_connect build
```

## Flash And Monitor

Replace `/dev/cu.usbserial-XXXX` with the board's serial port:

```bash
idf.py -C examples/basic_connect -p /dev/cu.usbserial-XXXX flash monitor
```

In a non-interactive agent environment, use the repository serial monitor helper after flashing:

```bash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-XXXX
```

## Expected Logs

Successful startup should show milestones similar to:

```text
esp-lwlte basic connect example
Air780EP EN GPIO2 held high
Core event: STARTED
Core event: READY
Core ready reached
Core event: NET_CONNECTING net=ACTIVATING err=0
Core event: NET_ONLINE net=ONLINE err=0
LTE network is online
periodic: core=ONLINE net=ONLINE
```

If the network does not become online, the example logs `NET_ERROR` or a timeout and prints current Core/network state.

## Troubleshooting

- No AT response: check TX/RX cross-wiring, common ground, EN held high, and baud rate `115200`.
- SIM not ready: check SIM insertion, SIM PIN state, and module antenna/power.
- Registration timeout/error: check antenna, signal, LTE coverage, and SIM network availability.
- PDP/APN errors: this example uses an empty APN to request the module/operator default. If your SIM requires an explicit APN, change `EXAMPLE_LTE_APN` in `main/main.c`.
- Serial monitor unavailable: check whether another monitor process is holding the port.
```

- [ ] **Step 2: Verify README references match source defaults**

Run:

```bash
rg 'GPIO0|GPIO1|GPIO2|115200|EXAMPLE_LTE_APN|UART_NUM_1' examples/basic_connect
```

Expected result: README and `main.c` consistently mention GPIO0, GPIO1, GPIO2, 115200, empty APN/source APN constant, and UART1.

- [ ] **Step 3: Check workspace status**

Run:

```bash
git status --short --untracked-files=all
```

Expected result: README appears under `examples/basic_connect/`; no commit is made.

---

## Task 4: Build And Boundary Verification

**Files:**
- Verify only; do not modify files unless a verification command fails.

- [ ] **Step 1: Build the example as an independent ESP-IDF project**

Run from the repository root:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build succeeds with zero compile errors.

- [ ] **Step 2: Build the root project**

Use the ESP-IDF MCP build tool if available:

```text
esp-idf-eim_build_project
```

Expected result: root project build succeeds.

- [ ] **Step 3: Run Core boundary checks**

Run:

```bash
if rg '#include "(at_engine|modem_air780ep|modem_priv)\.h"' src/core src/include/lwlte_core.h; then exit 1; else test $? -eq 1; fi
if rg 'at_engine_' src/core src/include/lwlte_core.h; then exit 1; else test $? -eq 1; fi
```

Expected result: both commands exit 0 with no output.

- [ ] **Step 4: Run whitespace check**

Run:

```bash
git diff --check
```

Expected result: no output.

- [ ] **Step 5: Final status checkpoint**

Run:

```bash
git status --short --untracked-files=all
```

Expected result: example files are untracked or modified as intended, along with existing Core worktree changes. Do not commit unless the user explicitly authorizes it.

---

## Task 5: Optional Hardware Smoke Test

**Files:**
- No file changes unless serial logs reveal a compile-time configuration mistake.

- [ ] **Step 1: Find the serial port**

Run:

```bash
ls /dev/tty.usb* /dev/cu.usb* 2>/dev/null
```

Expected result: one or more USB serial ports are listed.

- [ ] **Step 2: Flash the example**

Run, replacing the port with the actual board port:

```bash
idf.py -C examples/basic_connect -p /dev/cu.usbserial-XXXX flash
```

Expected result: flashing succeeds.

- [ ] **Step 3: Capture serial logs**

Run from the repository root:

```bash
python3 docs/agents/serial_monitor.py --timeout 45 --port /dev/cu.usbserial-XXXX
```

Expected result: logs show startup, EN high, and either `NET_ONLINE` or a clear `NET_ERROR`/timeout state. Only claim hardware success if `NET_ONLINE` appears.

- [ ] **Step 4: Report hardware result**

If `NET_ONLINE` appears, report hardware smoke test success with the observed log milestones. If it does not, report the exact failing stage and preserve the logs for diagnosis.

---

## Self-Review

Spec coverage:

- Standalone `examples/basic_connect` structure: Task 1.
- Hardware defaults and EN semantics: Task 2 source constants and README in Task 3.
- AT Engine, Modem, Core object creation: Task 2.
- Core event logging and state loop: Task 2.
- Error handling and cleanup: Task 2.
- README content: Task 3.
- Build, boundary, and optional hardware validation: Task 4 and Task 5.
- Out-of-scope items are not included: no MQTT/HTTP, no Kconfig, no OLED, no Air780EP adapter changes, no root `main/main.c` changes.

Placeholder scan:

- No unresolved placeholders or deferred implementation sections are present.
- The only placeholder-like value is `/dev/cu.usbserial-XXXX`, explicitly documented as a user-specific serial port replacement.

Type consistency:

- `example_runtime_t`, `example_flags_t`, helper names, Core event IDs, and ESP-IDF types match the public headers used by the example.
- Hardware defaults match the approved wiring: UART1, GPIO0 TX, GPIO1 RX, GPIO2 EN, baud 115200, empty APN.
