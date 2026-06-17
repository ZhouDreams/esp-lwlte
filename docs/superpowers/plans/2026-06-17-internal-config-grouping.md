# Internal Config Grouping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor persistent internal creation configs into nested groups while preserving public config shape and runtime behavior.

**Architecture:** Keep public `lwlte_*_config_t.base` unchanged. Group only internal persistent creation configs by layer-owned concepts: AT Engine `uart/runtime`, Modem `base.hardware/timing/event`, Core `event/network/fsm`, and MQTT `endpoint/auth/session/fsm/event`. Leave command/request value objects flat.

**Tech Stack:** C99, ESP-IDF/FreeRTOS, `esp_err_t`, Python host contract tests with `pytest`.

---

## File Structure

- Modify `src/at_engine/at_engine.h`: introduce grouped AT Engine config typedefs.
- Modify `src/at_engine/at_engine.c`: update normalization, UART init, RX task creation, response buffers, and destroy rollback to nested config paths.
- Modify `src/modem/modem.h`: define common modem config groups shared by Air780EP and ML307R.
- Modify `src/modem/modem_air780ep.h`: wrap `modem_base_config_t base` in Air780EP config.
- Modify `src/modem/modem_ml307r.h`: wrap `modem_base_config_t base` in ML307R config.
- Modify `src/modem/modem_air780ep.c`: update `self->config` accesses to `base.hardware`, `base.timing`, and `base.event` paths.
- Modify `src/modem/modem_ml307r.c`: update `self->config` accesses to `base.hardware`, `base.timing`, and `base.event` paths.
- Modify `src/core/core.h`: introduce grouped Core config typedefs.
- Modify `src/core/core.c`: update validation, APN copy, defaults, event posting, PDP init, and config storage paths.
- Modify `src/core/core_fsm.c`: update FSM queue/task config paths.
- Modify `src/core/net_mgr.c`: update network config paths used by activation/reconnect logic.
- Modify `src/mqtt_client/mqtt_client.h`: introduce grouped MQTT client config typedefs.
- Modify `src/mqtt_client/mqtt_client.c`: update validation, normalization, string ownership, event loop registration, FSM queue/task config paths, and core command submission paths.
- Modify `src/lwlte/lwlte_air780ep.c`: map public config into grouped AT/Modem/Core internal configs.
- Modify `src/lwlte/lwlte_ml307r.c`: map public config into grouped AT/Modem/Core internal configs while preserving ML307R line-buffer fallback.
- Modify `src/lwlte/lwlte.c`: map public MQTT config into grouped `mqtt_client_config_t`.
- Modify host contract tests under `tests/host/`: update static assertions for grouped internal paths and add explicit flat-command-object guards.
- Modify `docs/agents/classes.md`, `docs/agents/architecture.md`, and `docs/agents/oop-design.md`: update internal config examples and fix stale `core_config_t` event loop documentation.

Do not modify `AGENTS.md` or `AGENTS_ZH.md` unless the document index changes. Do not commit unless the user explicitly asks.

---

### Task 1: AT Engine Config Groups

**Files:**
- Modify: `src/at_engine/at_engine.h`
- Modify: `src/at_engine/at_engine.c`
- Test: host tests that inspect AT Engine config mapping, especially `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Update or add failing host assertions for AT Engine nested config**

Edit `tests/host/test_mqtt_end_to_end_contract.py`. In the facade config mapping test near the existing `config->base.uart.*` assertions, require nested internal initializer paths:

```python
for token in [
    ".uart = {",
    ".uart_num = config->base.uart.num",
    ".tx_pin = config->base.uart.tx_pin",
    ".rx_pin = config->base.uart.rx_pin",
    ".baud_rate = config->base.uart.baud_rate",
    ".rx_buf_size = config->base.at_engine.rx_buf_size",
    ".runtime = {",
    ".rx_task_stack = config->base.at_engine.rx_task_stack",
    ".rx_task_priority = config->base.at_engine.rx_task_priority",
    ".rx_line_buf_size = config->base.at_engine.rx_line_buf_size",
    ".cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms",
    ".max_response_lines = config->base.at_engine.max_response_lines",
]:
    self.assertIn(token, self.lwlte_air780ep_c)
```

Also add a direct header/source contract in the same test file or the nearest existing AT Engine test block:

```python
for token in [
    "} at_engine_uart_config_t;",
    "} at_engine_runtime_config_t;",
    "at_engine_uart_config_t uart;",
    "at_engine_runtime_config_t runtime;",
    "me->config.uart.uart_num",
    "me->config.uart.rx_buf_size",
    "me->config.runtime.rx_task_stack",
    "me->config.runtime.rx_line_buf_size",
    "me->config.runtime.max_response_lines",
]:
    self.assertIn(token, self.at_engine_h + self.at_engine_c)
```

- [ ] **Step 2: Run the targeted host test and verify failure**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py -q
```

Expected: FAIL because `at_engine_config_t` and the facade initializers are still flat.

- [ ] **Step 3: Refactor `at_engine_config_t` typedefs**

In `src/at_engine/at_engine.h`, replace the flat `at_engine_config_t` definition with:

```c
/**
 * @brief AT 引擎 UART 配置
 * @details AT engine UART configuration
 */
typedef struct {
    uart_port_t uart_num;               /**< UART 端口号； UART port number */
    int tx_pin;                         /**< TX GPIO； TX GPIO */
    int rx_pin;                         /**< RX GPIO； RX GPIO */
    int baud_rate;                      /**< 波特率； Baud rate */
    int rx_buf_size;                    /**< UART RX 环形缓冲区大小； UART RX ring buffer size */
} at_engine_uart_config_t;

/**
 * @brief AT 引擎运行参数
 * @details AT engine runtime configuration
 */
typedef struct {
    int rx_task_stack;                  /**< 接收任务栈大小； RX task stack size */
    int rx_task_priority;               /**< 接收任务优先级； RX task priority */
    int rx_line_buf_size;               /**< 单行最大长度； Maximum line length */
    int cmd_default_timeout_ms;         /**< 默认命令超时； Default command timeout */
    int max_response_lines;             /**< 单次响应最大行数； Maximum response lines */
} at_engine_runtime_config_t;

/**
 * @brief AT 引擎配置
 * @details AT engine configuration
 */
typedef struct {
    at_engine_uart_config_t    uart;     /**< UART 硬件； UART hardware */
    at_engine_runtime_config_t runtime;  /**< 运行参数； Runtime parameters */
} at_engine_config_t;
```

- [ ] **Step 4: Update `src/at_engine/at_engine.c` field paths**

Replace every flat `me->config` and `in/out` config field access with nested paths:

```c
me->config.runtime.rx_task_stack
me->config.runtime.rx_task_priority
me->config.uart.uart_num
me->config.uart.baud_rate
me->config.uart.rx_buf_size
me->config.uart.tx_pin
me->config.uart.rx_pin
me->config.runtime.rx_line_buf_size
me->config.runtime.max_response_lines
me->config.runtime.cmd_default_timeout_ms
```

The key code blocks should end up equivalent to:

```c
BaseType_t task_ret = xTaskCreate(rx_task, "at_engine_rx",
                                  me->config.runtime.rx_task_stack, me,
                                  me->config.runtime.rx_task_priority,
                                  &me->rx_task);
```

```c
esp_err_t del_ret = uart_driver_delete(me->config.uart.uart_num);
```

```c
static esp_err_t normalize_config(const at_engine_config_t *in, at_engine_config_t *out)
{
    ESP_RETURN_ON_FALSE(in && out, ESP_ERR_INVALID_ARG, TAG, "NULL config");
    ESP_RETURN_ON_FALSE(in->uart.uart_num >= UART_NUM_0 &&
                        in->uart.uart_num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(in->uart.tx_pin >= 0 && in->uart.rx_pin >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(in->uart.baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid baud rate");

    *out = *in;
    if (out->uart.rx_buf_size <= 0) {
        out->uart.rx_buf_size = AT_ENGINE_DEFAULT_RX_BUF_SIZE;
    }
    if (out->runtime.rx_task_stack <= 0) {
        out->runtime.rx_task_stack = AT_ENGINE_DEFAULT_RX_TASK_STACK;
    }
    if (out->runtime.rx_task_priority <= 0) {
        out->runtime.rx_task_priority = AT_ENGINE_DEFAULT_RX_TASK_PRIORITY;
    }
    if (out->runtime.rx_line_buf_size <= 0) {
        out->runtime.rx_line_buf_size = AT_ENGINE_DEFAULT_LINE_BUF_SIZE;
    }
    if (out->runtime.cmd_default_timeout_ms <= 0) {
        out->runtime.cmd_default_timeout_ms = AT_ENGINE_DEFAULT_TIMEOUT_MS;
    }
    if (out->runtime.max_response_lines <= 0) {
        out->runtime.max_response_lines = AT_ENGINE_DEFAULT_MAX_RESP_LINES;
    } else if (out->runtime.max_response_lines < AT_ENGINE_DEFAULT_MAX_RESP_LINES) {
        out->runtime.max_response_lines = AT_ENGINE_DEFAULT_MAX_RESP_LINES;
    }

    return ESP_OK;
}
```

```c
me->line_buf = calloc(1, me->config.runtime.rx_line_buf_size);
me->line_work_buf = calloc(1, me->config.runtime.rx_line_buf_size);
me->response_pool_lines = me->config.runtime.max_response_lines;
me->response_line_size = me->config.runtime.rx_line_buf_size;
```

```c
const uart_config_t uart_config = {
    .baud_rate = me->config.uart.baud_rate,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_DEFAULT,
};

ESP_RETURN_ON_ERROR(uart_driver_install(me->config.uart.uart_num,
                                        me->config.uart.rx_buf_size,
                                        AT_ENGINE_UART_TX_BUF_SIZE,
                                        AT_ENGINE_UART_EVENT_QUEUE_SIZE,
                                        &me->uart_queue, 0),
                    TAG, "uart_driver_install failed");
ESP_RETURN_ON_ERROR(uart_param_config(me->config.uart.uart_num, &uart_config),
                    TAG, "uart_param_config failed");
ESP_RETURN_ON_ERROR(uart_set_pin(me->config.uart.uart_num,
                                 me->config.uart.tx_pin,
                                 me->config.uart.rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                    TAG, "uart_set_pin failed");
```

- [ ] **Step 5: Update Facade AT Engine initializers**

In `src/lwlte/lwlte_air780ep.c`, change the `at_engine_config_t at_config` initializer to:

```c
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
```

In `src/lwlte/lwlte_ml307r.c`, use the same shape and preserve the line-buffer fallback:

```c
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
        .rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?
                            config->base.at_engine.rx_line_buf_size :
                            LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE,
        .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
        .max_response_lines = config->base.at_engine.max_response_lines,
    },
};
```

- [ ] **Step 6: Run targeted test and fix compile-level misses**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py tests/host/test_ml307r_contract.py -q
```

Expected: AT Engine related assertions pass or expose remaining flat-path static expectations. Update only expectations directly tied to AT Engine grouping.

---

### Task 2: Modem Config Groups

**Files:**
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_air780ep.h`
- Modify: `src/modem/modem_ml307r.h`
- Modify: `src/modem/modem_air780ep.c`
- Modify: `src/modem/modem_ml307r.c`
- Modify: `src/lwlte/lwlte_air780ep.c`
- Modify: `src/lwlte/lwlte_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`, `tests/host/test_lwlte_start_stop_lifecycle.py`, `tests/host/test_air780ep_command_gated_init.py`

- [ ] **Step 1: Update failing tests for modem nested config**

In `tests/host/test_ml307r_contract.py`, change `test_modem_header_declares_config_and_factory` expected tokens to include common base config and nested wrapper paths:

```python
for token in [
    "modem_base_config_t",
    "modem_hardware_config_t hardware;",
    "modem_timing_config_t timing;",
    "modem_event_config_t event;",
    "modem_base_config_t base;",
    "modem_handle_t *modem_ml307r_create(at_engine_handle_t *at,",
]:
    self.assertIn(token, self.ml307r_h + self.ml307r_c)
```

In `tests/host/test_lwlte_start_stop_lifecycle.py`, update power-off expectations:

```python
contains(self, body, "gpio_set_level(self->config.base.hardware.en_pin, 0)", "air780ep hardware_power_off")
absent(self, body, "gpio_set_level(self->config.base.hardware.en_pin, 1)", "air780ep hardware_power_off")
```

and the same for ML307R:

```python
contains(self, body, "gpio_set_level(self->config.base.hardware.en_pin, 0)", "ml307r hardware_power_off")
absent(self, body, "gpio_set_level(self->config.base.hardware.en_pin, 1)", "ml307r hardware_power_off")
```

Add facade initializer assertions for both factories:

```python
for token in [
    ".base = {",
    ".hardware = {",
    ".en_pin = config->base.modem.en_pin",
    ".timing = {",
    ".reset_pulse_ms = config->base.modem.reset_pulse_ms",
    ".ready_timeout_ms = config->base.modem.ready_timeout_ms",
    ".default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms",
    ".event = {",
    ".event_queue_size = config->base.modem.event_queue_size",
    ".event_task_stack = config->base.modem.event_task_stack",
    ".event_task_priority = config->base.modem.event_task_priority",
]:
    self.assertIn(token, self.lwlte_ml307r_c)
```

- [ ] **Step 2: Run targeted tests and verify failure**

Run:

```bash
python3 -m pytest tests/host/test_ml307r_contract.py tests/host/test_lwlte_start_stop_lifecycle.py tests/host/test_air780ep_command_gated_init.py -q
```

Expected: FAIL because modem configs and implementations are still flat.

- [ ] **Step 3: Add common modem config groups**

In `src/modem/modem.h`, after `typedef struct modem_handle modem_handle_t;`, add:

```c
/**
 * @brief 调制解调器硬件配置
 * @details Modem hardware configuration
 */
typedef struct {
    gpio_num_t en_pin;                  /**< EN GPIO，GPIO_NUM_NC 表示不控制； EN GPIO, GPIO_NUM_NC disables control */
} modem_hardware_config_t;

/**
 * @brief 调制解调器时序配置
 * @details Modem timing configuration
 */
typedef struct {
    uint32_t reset_pulse_ms;            /**< 复位脉冲时间； Reset pulse time */
    uint32_t ready_timeout_ms;          /**< 启动 AT OK 等待总超时； Startup AT OK wait total timeout */
    uint32_t default_cmd_timeout_ms;    /**< 默认命令超时； Default command timeout */
} modem_timing_config_t;

/**
 * @brief 调制解调器事件任务配置
 * @details Modem event task configuration
 */
typedef struct {
    int event_queue_size;               /**< 事件队列长度； Event queue size */
    int event_task_stack;               /**< 事件任务栈大小； Event task stack size */
    int event_task_priority;            /**< 事件任务优先级； Event task priority */
} modem_event_config_t;

/**
 * @brief 调制解调器公共基础配置
 * @details Modem common base configuration
 */
typedef struct {
    modem_hardware_config_t hardware;   /**< 硬件控制； Hardware control */
    modem_timing_config_t timing;       /**< 时序参数； Timing parameters */
    modem_event_config_t event;         /**< 事件任务； Event task */
} modem_base_config_t;
```

Add `#include "driver/gpio.h"` to `src/modem/modem.h` because `gpio_num_t` moves into this shared header.

- [ ] **Step 4: Wrap module-specific modem configs**

In `src/modem/modem_air780ep.h`, replace the flat config fields with:

```c
/**
 * @brief Air780EP 调制解调器配置
 * @details Air780EP modem configuration
 */
typedef struct {
    modem_base_config_t base;           /**< 公共基础配置； Common base configuration */
} modem_air780ep_config_t;
```

In `src/modem/modem_ml307r.h`, replace the flat config fields with:

```c
/**
 * @brief ML307R 调制解调器配置
 * @details ML307R modem configuration
 */
typedef struct {
    modem_base_config_t base;           /**< 公共基础配置； Common base configuration */
} modem_ml307r_config_t;
```

Remove direct `#include "driver/gpio.h"` from these module headers only if no other symbol in the file needs it after the change. Keeping it is acceptable but unnecessary once `modem.h` includes GPIO.

- [ ] **Step 5: Update Air780EP modem implementation paths**

In `src/modem/modem_air780ep.c`, update every `self->config` path:

```c
self->config.base.timing.default_cmd_timeout_ms
self->config.base.timing.ready_timeout_ms
self->config.base.timing.reset_pulse_ms
self->config.base.hardware.en_pin
self->config.base.event.event_queue_size
self->config.base.event.event_task_stack
self->config.base.event.event_task_priority
```

The constructor normalization block should become:

```c
if (self->config.base.timing.default_cmd_timeout_ms == 0) {
    self->config.base.timing.default_cmd_timeout_ms = AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
}
if (self->config.base.timing.ready_timeout_ms == 0) {
    self->config.base.timing.ready_timeout_ms = AIR780EP_DEFAULT_READY_TIMEOUT_MS;
}
```

The base init call should use:

```c
ret = modem_base_init(&self->base, "air780ep", at, &air780ep_ops,
                      self->config.base.event.event_queue_size,
                      self->config.base.event.event_task_stack,
                      self->config.base.event.event_task_priority);
```

Hardware power/reset checks should use:

```c
if (self->config.base.hardware.en_pin == GPIO_NUM_NC) {
    return ESP_OK;
}
gpio_set_level(self->config.base.hardware.en_pin, 0);
```

- [ ] **Step 6: Update ML307R modem implementation paths**

In `src/modem/modem_ml307r.c`, apply the same path conversion:

```c
self->config.base.timing.default_cmd_timeout_ms
self->config.base.timing.ready_timeout_ms
self->config.base.timing.reset_pulse_ms
self->config.base.hardware.en_pin
self->config.base.event.event_queue_size
self->config.base.event.event_task_stack
self->config.base.event.event_task_priority
```

Keep module-specific default constants unchanged.

- [ ] **Step 7: Update Facade modem config initializers**

In `src/lwlte/lwlte_air780ep.c`, change `modem_air780ep_config_t modem_config` to:

```c
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
```

In `src/lwlte/lwlte_ml307r.c`, use the same shape with `modem_ml307r_config_t`.

- [ ] **Step 8: Run targeted modem tests**

Run:

```bash
python3 -m pytest tests/host/test_ml307r_contract.py tests/host/test_lwlte_start_stop_lifecycle.py tests/host/test_air780ep_command_gated_init.py -q
```

Expected: PASS for modem config related checks. Any failures should be fixed only if they relate to renamed modem config paths or stale static tokens.

---

### Task 3: Core Config Groups

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`
- Modify: `src/core/net_mgr.c`
- Modify: `src/lwlte/lwlte_air780ep.c`
- Modify: `src/lwlte/lwlte_ml307r.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`, `tests/host/test_ml307r_contract.py`, `tests/host/test_lwlte_start_lifecycle.py`

- [ ] **Step 1: Update failing tests for Core grouped config**

In `tests/host/test_mqtt_end_to_end_contract.py`, update facade factory checks that currently expect `.event_loop = me->event_loop` to expect:

```python
for token in [
    ".event = {",
    ".loop = me->event_loop",
    ".network = {",
    ".apn = config->base.core.apn ? config->base.core.apn : \"\"",
    ".primary_cid = config->base.core.primary_cid",
    ".net_activate_timeout_ms = config->base.core.net_activate_timeout_ms",
    ".reconnect_delay_ms = config->base.core.reconnect_delay_ms",
    ".fsm = {",
    ".queue_size = config->base.core.fsm_queue_size",
    ".task_stack = config->base.core.fsm_task_stack",
    ".task_priority = config->base.core.fsm_task_priority",
]:
    self.assertIn(token, self.lwlte_air780ep_c)
```

In `tests/host/test_ml307r_contract.py`, replace `".event_loop = me->event_loop"` with `".loop = me->event_loop"` and add `".network = {"`, `".fsm = {"`.

Add Core header/source grouped config expectations:

```python
for token in [
    "} core_event_config_t;",
    "} core_network_config_t;",
    "} core_fsm_config_t;",
    "core_event_config_t event;",
    "core_network_config_t network;",
    "core_fsm_config_t fsm;",
    "me->config.event.loop",
    "me->config.network.apn",
    "me->config.network.primary_cid",
    "me->config.fsm.queue_size",
]:
    self.assertIn(token, self.core_h + self.core_c + self.core_fsm_c)
```

- [ ] **Step 2: Run targeted tests and verify failure**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py tests/host/test_ml307r_contract.py tests/host/test_lwlte_start_lifecycle.py -q
```

Expected: FAIL because `core_config_t` is still flat.

- [ ] **Step 3: Refactor `core_config_t` typedefs**

In `src/core/core.h`, replace the flat `core_config_t` with:

```c
/**
 * @brief LTE 核心服务事件配置
 * @details LTE core service event configuration
 */
typedef struct {
    esp_event_loop_handle_t loop;        /**< 共享事件总线（借用）； Shared event bus (borrowed) */
} core_event_config_t;

/**
 * @brief LTE 核心服务网络配置
 * @details LTE core service network configuration
 */
typedef struct {
    const char *apn;                     /**< APN； APN */
    uint8_t primary_cid;                 /**< 主 PDP 上下文 ID； Primary PDP context ID */
    uint32_t net_activate_timeout_ms;    /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;         /**< 重连延迟； Reconnect delay */
} core_network_config_t;

/**
 * @brief LTE 核心服务 FSM 配置
 * @details LTE core service FSM configuration
 */
typedef struct {
    int queue_size;                      /**< FSM 队列长度； FSM queue size */
    int task_stack;                      /**< FSM 任务栈大小； FSM task stack size */
    int task_priority;                   /**< FSM 任务优先级； FSM task priority */
} core_fsm_config_t;

/**
 * @brief LTE 核心服务配置
 * @details LTE core service configuration
 */
typedef struct {
    core_event_config_t event;           /**< 事件总线； Event bus */
    core_network_config_t network;       /**< 网络策略； Network policy */
    core_fsm_config_t fsm;               /**< FSM 资源； FSM resources */
} core_config_t;
```

- [ ] **Step 4: Update Core implementation paths**

In `src/core/core.c`, update validation and normalization:

```c
static bool config_valid(const core_config_t *config, modem_handle_t *modem)
{
    return config && modem && config->network.apn &&
           config->network.primary_cid <= CORE_MAX_PDP_CONTEXTS;
}
```

```c
ESP_RETURN_ON_FALSE(config && normalized && config->network.apn,
                    ESP_ERR_INVALID_ARG, TAG, "NULL argument");

size_t apn_len = strlen(config->network.apn) + 1;
char *apn = malloc(apn_len);
ESP_RETURN_ON_FALSE(apn, ESP_ERR_NO_MEM, TAG, "copy APN failed");

memcpy(apn, config->network.apn, apn_len);
*normalized = *config;
normalized->network.apn = apn;

if (normalized->network.primary_cid == 0) {
    normalized->network.primary_cid = CORE_DEFAULT_PRIMARY_CID;
}
if (normalized->network.net_activate_timeout_ms == 0) {
    normalized->network.net_activate_timeout_ms = CORE_DEFAULT_NET_ACTIVATE_TIMEOUT_MS;
}
if (normalized->network.reconnect_delay_ms == 0) {
    normalized->network.reconnect_delay_ms = CORE_DEFAULT_RECONNECT_DELAY_MS;
}
if (normalized->fsm.queue_size <= 0) {
    normalized->fsm.queue_size = CORE_DEFAULT_FSM_QUEUE_SIZE;
}
if (normalized->fsm.task_stack <= 0) {
    normalized->fsm.task_stack = CORE_DEFAULT_FSM_TASK_STACK;
}
if (normalized->fsm.task_priority <= 0) {
    normalized->fsm.task_priority = CORE_DEFAULT_FSM_TASK_PRIORITY;
}
```

Update these paths everywhere in `src/core/core.c`:

```c
me->config.event.loop
me->config.network.primary_cid
me->config.network.apn
```

For example, `core_post_event()` should use:

```c
if (me->config.event.loop) {
    ret = esp_event_post_to(me->config.event.loop, LWLTE_EVENT,
                            event_id, data, sizeof(*data), 0);
} else {
    ret = esp_event_post(LWLTE_EVENT, event_id, data, sizeof(*data), 0);
}
```

Free APN with:

```c
free((void *)me->config.network.apn);
me->config.network.apn = NULL;
```

- [ ] **Step 5: Update Core FSM and Net Manager paths**

In `src/core/core_fsm.c`, change queue/task config paths:

```c
me->fsm.queue = xQueueCreate(me->config.fsm.queue_size,
                             sizeof(core_fsm_sig_t));

BaseType_t task_ret = xTaskCreate(fsm_task, "lwlte_fsm",
                                  me->config.fsm.task_stack, me,
                                  me->config.fsm.task_priority,
                                  &me->fsm.task);
```

In `src/core/net_mgr.c`, replace network flat paths:

```c
me->config.network.net_activate_timeout_ms
me->config.network.reconnect_delay_ms
me->config.network.primary_cid
me->config.network.apn
```

Keep all network activation behavior unchanged.

- [ ] **Step 6: Update Facade Core config initializers**

In both `src/lwlte/lwlte_air780ep.c` and `src/lwlte/lwlte_ml307r.c`, change `core_config_t core_config` to:

```c
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
```

- [ ] **Step 7: Run targeted Core tests**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py tests/host/test_ml307r_contract.py tests/host/test_lwlte_start_lifecycle.py -q
```

Expected: PASS for Core config related checks.

---

### Task 4: MQTT Client Config Groups

**Files:**
- Modify: `src/mqtt_client/mqtt_client.h`
- Modify: `src/mqtt_client/mqtt_client.c`
- Modify: `src/lwlte/lwlte.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Update failing tests for MQTT grouped config**

In `tests/host/test_mqtt_end_to_end_contract.py`, update `test_core_dispatches_cached_mqtt_commands_and_tcp_disconnect` expected submit paths:

```python
for token in [
    "cmd.data.mqtt_config.client_id = me->config.auth.client_id;",
    "cmd.data.mqtt_config.username = me->config.auth.username;",
    "cmd.data.mqtt_config.password = me->config.auth.password;",
    "cmd.data.mqtt_config.host = me->config.endpoint.host;",
    "cmd.data.mqtt_config.port = me->config.endpoint.port;",
    "cmd.data.mqtt_config.clean_session = me->config.session.clean_session;",
    "cmd.data.mqtt_config.keepalive_s = me->config.session.keepalive_s;",
]:
    self.assertIn(token, mqtt_submit_body)
```

Add config header/source expectations:

```python
for token in [
    "} mqtt_client_endpoint_config_t;",
    "} mqtt_client_auth_config_t;",
    "} mqtt_client_session_config_t;",
    "} mqtt_client_fsm_config_t;",
    "} mqtt_client_event_config_t;",
    "mqtt_client_endpoint_config_t endpoint;",
    "mqtt_client_auth_config_t auth;",
    "mqtt_client_session_config_t session;",
    "mqtt_client_fsm_config_t fsm;",
    "mqtt_client_event_config_t event;",
    "config->endpoint.host",
    "config->endpoint.port",
    "config->auth.client_id",
    "config->endpoint.transport",
    "me->config.event.loop",
    "me->config.fsm.queue_size",
]:
    self.assertIn(token, self.mqtt_h + self.mqtt_c)
```

Add a `lwlte_mqtt_init()` mapping assertion near existing public MQTT config checks:

```python
for token in [
    ".endpoint = {",
    ".transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP",
    ".host = config->host",
    ".port = config->port",
    ".auth = {",
    ".client_id = config->client_id",
    ".username = config->username",
    ".password = config->password",
    ".session = {",
    ".keepalive_s = config->keepalive_s",
    ".clean_session = config->clean_session",
    ".fsm = {",
    ".queue_size = config->fsm_queue_size",
    ".task_stack = config->fsm_task_stack",
    ".task_priority = config->fsm_task_priority",
    ".event = {",
    ".loop = me->event_loop",
]:
    self.assertIn(token, self.lwlte_c)
```

- [ ] **Step 2: Run targeted test and verify failure**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py -q
```

Expected: FAIL because MQTT config is still flat.

- [ ] **Step 3: Refactor `mqtt_client_config_t` typedefs**

In `src/mqtt_client/mqtt_client.h`, replace the flat `mqtt_client_config_t` with:

```c
typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
} mqtt_client_endpoint_config_t;

typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
} mqtt_client_auth_config_t;

typedef struct {
    uint16_t keepalive_s;
    bool clean_session;
} mqtt_client_session_config_t;

typedef struct {
    int queue_size;
    int task_stack;
    int task_priority;
} mqtt_client_fsm_config_t;

typedef struct {
    esp_event_loop_handle_t loop;        /**< 共享事件总线（借用）； Shared event bus (borrowed) */
} mqtt_client_event_config_t;

typedef struct {
    mqtt_client_endpoint_config_t endpoint;
    mqtt_client_auth_config_t auth;
    mqtt_client_session_config_t session;
    mqtt_client_fsm_config_t fsm;
    mqtt_client_event_config_t event;
} mqtt_client_config_t;
```

- [ ] **Step 4: Update MQTT implementation config paths**

In `src/mqtt_client/mqtt_client.c`, update validation:

```c
static bool config_valid(const mqtt_client_config_t *config, core_handle_t *core)
{
    return config && core && config->endpoint.host && config->endpoint.host[0] &&
           config->endpoint.port > 0 && config->auth.client_id &&
           config->auth.client_id[0] &&
           (config->endpoint.transport == MQTT_CLIENT_TRANSPORT_PLAIN_TCP ||
            config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS);
}
```

Update normalization:

```c
*normalized = *config;
normalized->endpoint.host = clone_string(config->endpoint.host);
normalized->auth.client_id = clone_string(config->auth.client_id);
normalized->auth.username = config->auth.username ? clone_string(config->auth.username) : NULL;
normalized->auth.password = config->auth.password ? clone_string(config->auth.password) : NULL;
if (!normalized->endpoint.host || !normalized->auth.client_id ||
    (config->auth.username && !normalized->auth.username) ||
    (config->auth.password && !normalized->auth.password)) {
    free((void *)normalized->endpoint.host);
    free((void *)normalized->auth.client_id);
    free((void *)normalized->auth.username);
    free((void *)normalized->auth.password);
    memset(normalized, 0, sizeof(*normalized));
    return ESP_ERR_NO_MEM;
}
if (normalized->session.keepalive_s == 0) {
    normalized->session.keepalive_s = MQTT_CLIENT_DEFAULT_KEEPALIVE_S;
}
if (normalized->fsm.queue_size <= 0) {
    normalized->fsm.queue_size = MQTT_CLIENT_DEFAULT_FSM_QUEUE_SIZE;
}
if (normalized->fsm.task_stack <= 0) {
    normalized->fsm.task_stack = MQTT_CLIENT_DEFAULT_FSM_TASK_STACK;
}
if (normalized->fsm.task_priority <= 0) {
    normalized->fsm.task_priority = MQTT_CLIENT_DEFAULT_FSM_PRIORITY;
}
```

Update TLS rejection:

```c
if (config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS) {
    return NULL;
}
```

Update queue/task creation:

```c
me->fsm_queue = xQueueCreate(me->config.fsm.queue_size, sizeof(mqtt_fsm_sig_t));
BaseType_t task_ret = xTaskCreate(mqtt_fsm_task, "mqtt_fsm",
                                  me->config.fsm.task_stack, me,
                                  me->config.fsm.task_priority,
                                  &me->fsm_task);
```

Update event registration/unregistration:

```c
if (me->config.event.loop) {
    ret = esp_event_handler_register_with(me->config.event.loop, LWLTE_EVENT,
                                          LWLTE_EVENT_NET_ONLINE,
                                          handle_lwlte_event, me);
}
```

and the corresponding unregister calls.

Update owned string frees:

```c
free((void *)me->config.endpoint.host);
free((void *)me->config.auth.client_id);
free((void *)me->config.auth.username);
free((void *)me->config.auth.password);
```

Update `submit_core_cmd()` MQTT configure assignments:

```c
cmd.data.mqtt_config.client_id = me->config.auth.client_id;
cmd.data.mqtt_config.username = me->config.auth.username;
cmd.data.mqtt_config.password = me->config.auth.password;
cmd.data.mqtt_config.host = me->config.endpoint.host;
cmd.data.mqtt_config.port = me->config.endpoint.port;
cmd.data.mqtt_config.clean_session = me->config.session.clean_session;
cmd.data.mqtt_config.keepalive_s = me->config.session.keepalive_s;
```

- [ ] **Step 5: Update public MQTT facade mapping**

In `src/lwlte/lwlte.c`, change `mqtt_client_config_t mqtt_config` in `lwlte_mqtt_init()` to:

```c
const mqtt_client_config_t mqtt_config = {
    .endpoint = {
        .transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
        .host = config->host,
        .port = config->port,
    },
    .auth = {
        .client_id = config->client_id,
        .username = config->username,
        .password = config->password,
    },
    .session = {
        .keepalive_s = config->keepalive_s,
        .clean_session = config->clean_session,
    },
    .fsm = {
        .queue_size = config->fsm_queue_size,
        .task_stack = config->fsm_task_stack,
        .task_priority = config->fsm_task_priority,
    },
    .event = {
        .loop = me->event_loop,
    },
};
```

- [ ] **Step 6: Run targeted MQTT test**

Run:

```bash
python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py -q
```

Expected: PASS for MQTT config related checks.

---

### Task 5: Docs And Contract Sweep

**Files:**
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/oop-design.md`
- Modify: host tests under `tests/host/` only where static contracts now point to stale internal config paths
- Test: all host tests

- [ ] **Step 1: Update docs contract tests first**

Add or update static assertions so docs mention grouped internal config names and no longer claim Core event loop is absent from config.

In an existing docs-related host test, assert these tokens appear in combined docs:

```python
for token in [
    "at_engine_uart_config_t",
    "at_engine_runtime_config_t",
    "modem_base_config_t",
    "core_event_config_t",
    "core_network_config_t",
    "core_fsm_config_t",
    "mqtt_client_endpoint_config_t",
    "mqtt_client_auth_config_t",
    "mqtt_client_session_config_t",
    "config.event.loop",
]:
    self.assertIn(token, docs)
```

Also assert the stale statement is gone:

```python
self.assertNotIn("Event loop 参数不放入 config", docs)
```

- [ ] **Step 2: Run host tests and verify docs failures**

Run:

```bash
python3 -m pytest tests/host/ -q
```

Expected: FAIL only for stale docs/tests/static string expectations, not for C syntax yet if previous tasks are complete.

- [ ] **Step 3: Update `docs/agents/classes.md`**

Update sections for:

- `at_engine_config_t`: show `at_engine_uart_config_t uart` and `at_engine_runtime_config_t runtime`.
- `modem_air780ep_config_t` and `modem_ml307r_config_t`: show `modem_base_config_t base` with `hardware/timing/event`.
- `core_config_t`: show `event/network/fsm`; remove the stale sentence saying event loop is not in config.
- `mqtt_client_config_t`: show `endpoint/auth/session/fsm/event`.

Use concrete snippets matching the implementation, for example:

```c
typedef struct {
    core_event_config_t   event;
    core_network_config_t network;
    core_fsm_config_t     fsm;
} core_config_t;
```

- [ ] **Step 4: Update `docs/agents/architecture.md`**

Update Facade factory example to use grouped internal initializers:

```c
at_engine_config_t at_cfg = {
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
```

Use similar grouped snippets for `modem_air780ep_config_t` and `core_config_t`.

- [ ] **Step 5: Update `docs/agents/oop-design.md` if it mentions internal config snippets**

If the document shows module factory examples with flat modem config, update them to:

```c
modem_air780ep_config_t modem_cfg = {
    .base = {
        .hardware = {
            .en_pin = GPIO_NUM_NC,
        },
        .timing = {
            .ready_timeout_ms = 30000,
        },
        .event = {
            .event_queue_size = 8,
        },
    },
};
```

Leave public `lwlte_air780ep_config_t.base` examples unchanged unless they show internal flat config paths.

- [ ] **Step 6: Sweep stale static expectations**

Use content search for flat internal paths and update only stale assertions or docs:

```bash
rg "me->config\.(host|port|client_id|event_loop|fsm_queue_size|fsm_task_stack|fsm_task_priority|en_pin|ready_timeout_ms|default_cmd_timeout_ms|rx_task_stack|rx_line_buf_size)|config->(host|port|client_id|transport|event_loop)|\.event_loop = me->event_loop" tests/host docs/agents src
```

Expected remaining matches are allowed only for public structs or command/request value objects such as `lwlte_mqtt_config_t`, `ping_client_request_t`, `modem_mqtt_config_t`, and `core_cmd_t`.

Because shell `rg` is used here to count and inspect remaining matches, do not edit files with shell tools. Use `apply_patch` for any fixes.

- [ ] **Step 7: Run all host tests**

Run:

```bash
python3 -m pytest tests/host/ -q
```

Expected: all host tests pass.

---

### Task 6: Full Verification

**Files:**
- No planned edits unless verification exposes defects.
- Verify all changed source, tests, docs, and spec/plan files.

- [ ] **Step 1: Run whitespace check**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 2: Run host tests**

Run:

```bash
python3 -m pytest tests/host/ -q
```

Expected: all tests pass.

- [ ] **Step 3: Run ESP-IDF build**

Prefer the ESP-IDF MCP build tool. If it fails because of the known Python environment mismatch, use the documented shell fallback:

```bash
source "$HOME/.espressif/v6.0/esp-idf/export.sh" && idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 4: Inspect final diff scope**

Run:

```bash
git status --short
git diff --stat
```

Expected: changes are limited to internal config grouping source files, host contract tests, docs, and the new spec/plan. Existing unrelated `AGENTS.md` / `AGENTS_ZH.md` edits may still appear if they were already present; do not modify or revert them unless explicitly requested.

- [ ] **Step 5: Report results**

Report:

- Which internal configs were grouped.
- Which value objects stayed flat.
- Host test result.
- ESP-IDF build result.
- Any uncommitted unrelated changes observed.

Do not commit unless the user explicitly says to commit.
