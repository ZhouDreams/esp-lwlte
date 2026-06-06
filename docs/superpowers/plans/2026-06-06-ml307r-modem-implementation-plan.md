# ML307R Modem Subclass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ML307R as a first-class LTE modem implementation with public facade factory, full `modem_ops_t` coverage, MIPCALL networking, Ping, and MQTT.

**Architecture:** Keep the current LWLTE layering unchanged. Add a concrete `modem_ml307r_t` subclass under Modem Adapter and a `lwlte_ml307r_init()` composition root that mirrors Air780EP while using ML307R-specific AT commands. Use static host contract tests to drive the file/API/command/URC boundaries before running ESP-IDF build verification.

**Tech Stack:** ESP-IDF C component, FreeRTOS, AT Engine, Modem/Core/MQTT/Ping internal services, Python `unittest` static host tests, CMake source registration.

---

## File Structure

- Create `src/modem/modem_ml307r.h`: ML307R Modem Adapter factory config and `modem_ml307r_create()` declaration.
- Create `src/modem/modem_ml307r.c`: ML307R `modem_t` subclass, AT command helpers, startup/reset, network/PDP, MQTT, Ping, and URC handlers.
- Create `src/lwlte/lwlte_ml307r.c`: public facade composition root for ML307R.
- Modify `src/include/lwlte.h`: add ML307R public config types and `lwlte_ml307r_init()` declaration.
- Modify `src/CMakeLists.txt`: register ML307R modem and facade sources.
- Create `tests/host/test_ml307r_contract.py`: static regression tests for public API, build registration, startup gate, MIPCALL, MQTT, Ping, and URC contracts.
- Keep `core`, `mqtt_client`, `ping_client`, and `at_engine` code unchanged unless build verification exposes an integration defect.

## Scope Notes

- This plan implements the approved design in `docs/superpowers/specs/2026-06-06-ml307r-modem-design.md`.
- Do not wait for `+MATREADY`; ML307R startup readiness is only repeated `AT` probes returning `OK`.
- Use `primary_cid == 1` only in this first ML307R facade.
- Do not send AT commands from URC callbacks.
- Do not commit unless the user explicitly requests it. Each task ends with diff/status review instead of commit.

### Task 1: Static Contract Test Skeleton

**Files:**
- Create: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Write the failing test file**

Create `tests/host/test_ml307r_contract.py` with this content:

```python
#!/usr/bin/env python3
"""Static regression checks for the ML307R modem implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
LWLTE_H = ROOT / "src/include/lwlte.h"
SRC_CMAKE = ROOT / "src/CMakeLists.txt"
ML307R_H = ROOT / "src/modem/modem_ml307r.h"
ML307R_C = ROOT / "src/modem/modem_ml307r.c"
LWLTE_ML307R_C = ROOT / "src/lwlte/lwlte_ml307r.c"


def read_optional(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    search_from = 0
    while True:
        start = source.find(signature, search_from)
        if start < 0:
            raise AssertionError(f"missing function definition: {signature}")
        after_signature = start + len(signature)
        brace = source.find("{", after_signature)
        semicolon = source.find(";", after_signature)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        search_from = start + len(signature)

    depth = 0
    for idx in range(brace, len(source)):
        char = source[idx]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:idx]
    raise AssertionError(f"function body not closed for {signature}")


class Ml307rContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional(LWLTE_H)
        cls.src_cmake = read_optional(SRC_CMAKE)
        cls.ml307r_h = read_optional(ML307R_H)
        cls.ml307r_c = read_optional(ML307R_C)
        cls.lwlte_ml307r_c = read_optional(LWLTE_ML307R_C)

    def test_public_api_and_build_entries_exist(self):
        self.assertTrue(ML307R_H.exists(), "missing modem_ml307r.h")
        self.assertTrue(ML307R_C.exists(), "missing modem_ml307r.c")
        self.assertTrue(LWLTE_ML307R_C.exists(), "missing lwlte_ml307r.c")
        for token in [
            "lwlte_ml307r_config_mqtt_client_t",
            "lwlte_ml307r_config_t",
            "lwlte_ml307r_config_mqtt_client_t mqtt_client;",
            "esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,",
        ]:
            self.assertIn(token, self.lwlte_h)
        for token in [
            '"modem/modem_ml307r.c"',
            '"lwlte/lwlte_ml307r.c"',
        ]:
            self.assertIn(token, self.src_cmake)

    def test_modem_header_declares_config_and_factory(self):
        for token in [
            "modem_ml307r_config_t",
            "gpio_num_t en_pin;",
            "uint32_t reset_pulse_ms;",
            "uint32_t ready_timeout_ms;",
            "uint32_t default_cmd_timeout_ms;",
            "modem_t *modem_ml307r_create(at_engine_t *at,",
        ]:
            self.assertIn(token, self.ml307r_h)

    def test_startup_uses_at_probe_not_matready(self):
        for token in [
            "#define ML307R_AT_READY_PROBE_TIMEOUT_MS 1000",
            "#define ML307R_INIT_RETRY_DELAY_MS",
            "static esp_err_t wait_at_ready(modem_ml307r_t *self)",
            "static esp_err_t run_basic_init_cmds(modem_ml307r_t *self)",
        ]:
            self.assertIn(token, self.ml307r_c)
        wait_body = function_body(self.ml307r_c, "static esp_err_t wait_at_ready(modem_ml307r_t *self)")
        self.assertRegex(wait_body, r"send_cmd\s*\(\s*self\s*,\s*\"AT\"")
        self.assertIn("ML307R_AT_READY_PROBE_TIMEOUT_MS", wait_body)
        self.assertIn("ESP_ERR_TIMEOUT", wait_body)
        self.assertNotIn("MATREADY", wait_body)
        self.assertNotIn("+MATREADY", self.ml307r_c)

    def test_basic_init_commands_and_start_reset_order(self):
        init_body = function_body(self.ml307r_c, "static esp_err_t run_basic_init_cmds(modem_ml307r_t *self)")
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"', '"AT+CGREG=2"', '"AT+CREG=2"']
        last = -1
        for token in expected_order:
            index = init_body.find(token)
            self.assertGreater(index, last, token)
            last = index
        for signature in [
            "static esp_err_t ml307r_start(modem_t *me)",
            "static esp_err_t ml307r_reset(modem_t *me)",
        ]:
            body = function_body(self.ml307r_c, signature)
            for token in [
                "hardware_reset(self)",
                "wait_at_ready(self)",
                "run_basic_init_cmds(self)",
                "ret = register_urcs(self)",
                "finish_modem_ready(me, self)",
            ]:
                self.assertIn(token, body, signature)
            self.assertLess(body.index("hardware_reset(self)"), body.index("wait_at_ready(self)"))
            self.assertLess(body.index("wait_at_ready(self)"), body.index("run_basic_init_cmds(self)"))
            self.assertLess(body.index("run_basic_init_cmds(self)"), body.index("ret = register_urcs(self)"))

    def test_facade_factory_mirrors_air780ep_lifecycle(self):
        for token in [
            '#include "modem_ml307r.h"',
            "esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,",
            "at_engine_create(&at_config)",
            "modem_ml307r_create(me->at, &modem_config)",
            "core_create(&core_config, me->modem)",
            "core_register_event_callback(me->core, lwlte_handle_core_event, me)",
            "ping_client_create(me->core)",
            "mqtt_client_create(&mqtt_config, me->core)",
            "mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me)",
        ]:
            self.assertIn(token, self.lwlte_ml307r_c)
        self.assertNotIn("core_start", self.lwlte_ml307r_c)
        self.assertNotIn("modem_start", self.lwlte_ml307r_c)

    def test_identity_status_and_registration_mapping_exists(self):
        for token in [
            "AT+CGSN",
            "AT+CIMI",
            "AT+MCCID",
            "AT+CGMM",
            "AT+CGMR",
            "AT+CPIN?",
            "AT+CSQ",
            "AT+CEREG?",
            "AT+CGREG?",
            "AT+CREG?",
            "AT+CGATT?",
            "parse_sim_status_line",
            "map_reg_status",
            "rssi_dbm_valid",
            ".get_info = ml307r_get_info",
            ".get_sim_status = ml307r_get_sim_status",
            ".get_signal = ml307r_get_signal",
            ".get_registration = ml307r_get_registration",
            ".get_packet_attach_status = ml307r_get_packet_attach_status",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_mipcall_network_mapping_exists(self):
        for token in [
            "ML307R_URC_MIPCALL",
            "AT+CGDCONT=%u,\"IPV4V6\",\"%s\"",
            "AT+MIPCALL?",
            "AT+MIPCALL=1,%u",
            "AT+MIPCALL=0,%u",
            "parse_mipcall_line",
            "query_mipcall",
            ".activate_pdp = ml307r_activate_pdp",
            ".deactivate_pdp = ml307r_deactivate_pdp",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_mqtt_command_mapping_exists(self):
        for token in [
            "ML307R_URC_MQTTURC",
            "AT+MQTTCFG=\"version\",0,4",
            "AT+MQTTCFG=\"cid\",0,1",
            "AT+MQTTCFG=\"keepalive\",0,%u",
            "AT+MQTTCFG=\"clean\",0,%u",
            "AT+MQTTCFG=\"cached\",0,0",
            "AT+MQTTCONN=0,\"%s\",%u,\"%s\",\"%s\",\"%s\"",
            "AT+MQTTDISC=0",
            "AT+MQTTSUB=0,\"%s\",%u",
            "AT+MQTTUNSUB=0,\"%s\"",
            "AT+MQTTPUB=0,\"%s\",%u,%u,%u,\"%s\"",
            "parse_mqtt_conn_urc",
            "parse_mqtt_publish_urc",
            "handle_mqtturc",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_ping_mapping_exists(self):
        for token in [
            "ML307R_MPING_PREFIX",
            "AT+MPING=\"%s\",%u,%u,%u,1",
            "parse_mping_reply_line",
            "parse_mping_statistics_line",
            "calculate_ping_summary",
            ".ping = ml307r_ping",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_urc_registration_and_callback_constraints(self):
        register_body = function_body(self.ml307r_c, "static esp_err_t register_urcs(modem_ml307r_t *self)")
        for token in [
            "ML307R_URC_CPIN",
            "ML307R_URC_CREG",
            "ML307R_URC_CEREG",
            "ML307R_URC_CGREG",
            "ML307R_URC_MIPCALL",
            "ML307R_URC_MQTTURC",
            "at_engine_register_urc",
        ]:
            self.assertIn(token, register_body)
        for handler in [
            "static void cpin_urc_handler",
            "static void reg_urc_handler",
            "static void mipcall_urc_handler",
            "static void mqtturc_urc_handler",
        ]:
            body = function_body(self.ml307r_c, handler)
            self.assertNotIn("send_cmd(", body)
            self.assertNotIn("at_engine_send_cmd", body)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: FAIL. First failures should mention missing `modem_ml307r.h`, `modem_ml307r.c`, `lwlte_ml307r.c`, ML307R public API tokens, and CMake entries.

- [ ] **Step 3: Review diff**

Run:

```bash
git diff -- tests/host/test_ml307r_contract.py
```

Expected: only the new test file is shown.

### Task 2: Public API, Modem Header, And Build Registration

**Files:**
- Modify: `src/include/lwlte.h`
- Create: `src/modem/modem_ml307r.h`
- Create: `src/modem/modem_ml307r.c`
- Create: `src/lwlte/lwlte_ml307r.c`
- Modify: `src/CMakeLists.txt`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted test and confirm current failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_public_api_and_build_entries_exist -v
```

Expected: FAIL with missing file/API/build-entry assertions.

- [ ] **Step 2: Add ML307R public config types and init prototype**

In `src/include/lwlte.h`, insert this block after `lwlte_air780ep_config_t` and before `GLOBAL PROTOTYPES`:

```c
/**
 * @brief ML307R MQTT 客户端配置
 * @details ML307R MQTT client configuration
 * @note enabled 为 false 时 MQTT 服务禁用，其余字段被忽略。
 * @note enabled 为 true 时 host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    bool enabled;                         /**< 是否启用 MQTT 服务； Whether to enable MQTT service */
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_ml307r_config_mqtt_client_t;

/**
 * @brief ML307R LTE 初始化配置
 * @details ML307R LTE initialization configuration
 * @note uart_num、uart_tx_pin、uart_rx_pin、uart_baud_rate 和 primary_cid 为必填字段。
 * @note en_pin 可设为 GPIO_NUM_NC，以禁用门面对 EN GPIO 的控制。
 * @note ML307R 启动不等待 +MATREADY；init_ready_timeout_ms 表示硬复位后重复发送 AT 并等待 OK 的总超时。
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note ML307R 门面当前仅支持 primary_cid 为 1。
 */
typedef struct {
    uart_port_t uart_num;                 /**< 必填 UART 端口号； Required UART port number */
    gpio_num_t uart_tx_pin;               /**< 必填 UART TX GPIO，不能为 GPIO_NUM_NC； Required UART TX GPIO, not GPIO_NUM_NC */
    gpio_num_t uart_rx_pin;               /**< 必填 UART RX GPIO，不能为 GPIO_NUM_NC； Required UART RX GPIO, not GPIO_NUM_NC */
    int uart_baud_rate;                   /**< 必填 UART 波特率，必须大于 0； Required UART baud rate, must be > 0 */
    gpio_num_t en_pin;                    /**< 可选模块 EN GPIO，GPIO_NUM_NC 表示不控制； Optional module EN GPIO, GPIO_NUM_NC disables control */
    const char *apn;                      /**< 可选 APN，NULL/空表示门面不配置； Optional APN, NULL/empty means facade does not configure it */
    uint8_t primary_cid;                  /**< 必填主 PDP 上下文 ID，ML307R 门面当前仅支持 1； Required primary PDP context ID, ML307R facade currently supports 1 only */
    uint32_t init_ready_timeout_ms;        /**< ML307R AT OK 等待总超时，0 使用下层默认值； ML307R AT OK wait timeout, 0 uses lower-layer default */
    uint32_t net_activate_timeout_ms;      /**< 网络激活总超时，0 使用 Core 默认值； Network activation timeout, 0 uses Core default */
    uint32_t reconnect_delay_ms;           /**< 重连延迟，0 使用 Core 默认值； Reconnect delay, 0 uses Core default */
    int at_rx_buf_size;                   /**< AT RX 缓冲大小，0 使用默认值； AT RX buffer size, 0 uses default */
    int at_rx_task_stack;                 /**< AT RX 任务栈大小，0 使用默认值； AT RX task stack, 0 uses default */
    int at_rx_task_priority;              /**< AT RX 任务优先级，0 使用默认值； AT RX task priority, 0 uses default */
    int at_rx_line_buf_size;              /**< AT 单行缓冲大小，0 使用默认值； AT line buffer size, 0 uses default */
    int at_cmd_default_timeout_ms;         /**< AT 默认命令超时，0 使用默认值； AT default command timeout, 0 uses default */
    int at_max_response_lines;             /**< AT 最大响应行数，0 使用默认值； AT maximum response lines, 0 uses default */
    uint32_t modem_reset_pulse_ms;         /**< Modem 复位脉冲(EN 拉低保持)时长，0 表示不额外等待； Modem reset pulse (EN low hold) length, 0 skips extra wait */
    uint32_t modem_default_cmd_timeout_ms; /**< Modem 默认命令超时，0 使用默认值； Modem default command timeout, 0 uses default */
    int modem_event_queue_size;            /**< Modem 事件队列长度，0 使用默认值； Modem event queue size, 0 uses default */
    int modem_event_task_stack;            /**< Modem 事件任务栈大小，0 使用默认值； Modem event task stack, 0 uses default */
    int modem_event_task_priority;         /**< Modem 事件任务优先级，0 使用默认值； Modem event task priority, 0 uses default */
    int core_fsm_queue_size;               /**< Core FSM 队列长度，0 使用默认值； Core FSM queue size, 0 uses default */
    int core_fsm_task_stack;               /**< Core FSM 任务栈大小，0 使用默认值； Core FSM task stack, 0 uses default */
    int core_fsm_task_priority;            /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
    lwlte_ml307r_config_mqtt_client_t mqtt_client; /**< MQTT 客户端配置； MQTT client configuration */
} lwlte_ml307r_config_t;
```

Then insert this prototype after `lwlte_air780ep_init()`:

```c
/**
 * @brief 初始化 ML307R LTE 用户门面
 * @details Initialize ML307R LTE user facade
 * @note 该函数只创建 LTE 用户门面及内部对象，不启动模块、不等待 AT ready、不激活 PDP。
 * @note ML307R 启动阶段由 lwlte_start() 触发，modem_start() 使用 AT OK 探测，不等待 +MATREADY。
 * @param[in] config ML307R LTE 初始化配置
 * @param[out] out_lte LTE 用户门面句柄输出指针
 * @return
 *         - ESP_OK: 初始化成功，门面句柄可用
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: GPIO、UART、Modem 或 Core 创建失败
 */
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_t **out_lte);
```

- [ ] **Step 3: Create `modem_ml307r.h`**

Create `src/modem/modem_ml307r.h`:

```c
/**
 * @file modem_ml307r.h
 * @brief ML307R 调制解调器公共接口
 * @details ML307R modem public interface
 * @author JovisDreams
 * @date 2026-06-06
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>

#include "driver/gpio.h"

#include "at_engine.h"
#include "modem.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief ML307R 调制解调器配置
 * @details ML307R modem configuration
 */
typedef struct {
    gpio_num_t en_pin;                  /**< EN GPIO，GPIO_NUM_NC 表示不控制； EN GPIO, GPIO_NUM_NC disables control */
    uint32_t reset_pulse_ms;            /**< 复位脉冲时间(EN 拉低保持时长)； Reset pulse time (EN low hold duration) */
    uint32_t ready_timeout_ms;          /**< 启动 AT OK 等待总超时； Startup AT OK wait total timeout */
    uint32_t default_cmd_timeout_ms;    /**< 默认命令超时； Default command timeout */
    int event_queue_size;               /**< 事件队列长度； Event queue size */
    int event_task_stack;               /**< 事件任务栈大小； Event task stack size */
    int event_task_priority;            /**< 事件任务优先级； Event task priority */
} modem_ml307r_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 ML307R 调制解调器
 * @details Create ML307R modem
 * @param[in] at AT 引擎句柄
 * @param[in] config ML307R 调制解调器配置
 * @return
 *         - 调制解调器句柄: 成功
 *         - NULL: 失败
 */
modem_t *modem_ml307r_create(at_engine_t *at,
                             const modem_ml307r_config_t *config);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 4: Create Task 2 compiling scaffolds**

Create `src/modem/modem_ml307r.c` with a minimal compiling scaffold. Task 4 replaces this scaffold with the startup-capable modem object, and Tasks 5-7 replace the non-startup `ESP_ERR_NOT_SUPPORTED` methods with concrete implementations:

```c
/**
 * @file modem_ml307r.c
 * @brief ML307R 调制解调器实现
 * @details ML307R modem implementation
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include "modem_ml307r.h"
#include "modem_priv.h"

#include <stdlib.h>

#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "modem_ml307r"

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    modem_t base;
    modem_ml307r_config_t config;
} modem_ml307r_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t ml307r_destroy(modem_t *me);
static esp_err_t ml307r_not_supported_no_arg(modem_t *me);
static modem_ml307r_t *to_ml307r(modem_t *me);

/**********************
 *  STATIC VARIABLES
 **********************/
static const modem_ops_t s_ml307r_ops = {
    .destroy = ml307r_destroy,
    .start = ml307r_not_supported_no_arg,
    .reset = ml307r_not_supported_no_arg,
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
modem_t *modem_ml307r_create(at_engine_t *at,
                             const modem_ml307r_config_t *config)
{
    if (!at || !config) {
        ESP_LOGE(TAG, "NULL argument");
        return NULL;
    }

    modem_ml307r_t *self = calloc(1, sizeof(*self));
    if (!self) {
        return NULL;
    }
    self->config = *config;

    esp_err_t ret = modem_base_init(&self->base, "ml307r", at, &s_ml307r_ops,
                                    config->event_queue_size,
                                    config->event_task_stack,
                                    config->event_task_priority);
    if (ret != ESP_OK) {
        free(self);
        return NULL;
    }

    return &self->base;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static modem_ml307r_t *to_ml307r(modem_t *me)
{
    return MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
}

static esp_err_t ml307r_destroy(modem_t *me)
{
    if (!me) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)to_ml307r(me);
    return ESP_OK;
}

static esp_err_t ml307r_not_supported_no_arg(modem_t *me)
{
    if (!me) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
```

Create `src/lwlte/lwlte_ml307r.c` as a compiling facade scaffold. Task 3 replaces this scaffold with the full Air780EP-style factory:

```c
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

#include "esp_check.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "lwlte_ml307r"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(config && out_lte, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    *out_lte = NULL;
    return ESP_ERR_NOT_SUPPORTED;
}
```

- [ ] **Step 5: Register sources in CMake**

Modify `src/CMakeLists.txt` so the `SRCS` list includes the new files next to matching layers:

```cmake
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "modem/modem_ml307r.c"
```

and:

```cmake
         "lwlte/lwlte.c"
         "lwlte/lwlte_air780ep.c"
         "lwlte/lwlte_ml307r.c"
```

- [ ] **Step 6: Run targeted tests**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_public_api_and_build_entries_exist tests.host.test_ml307r_contract.Ml307rContractTest.test_modem_header_declares_config_and_factory -v
```

Expected: PASS for these two tests. Other ML307R tests still fail because Task 2 intentionally contains only compiling scaffolds.

- [ ] **Step 7: Review diff**

Run:

```bash
git diff -- src/include/lwlte.h src/modem/modem_ml307r.h src/modem/modem_ml307r.c src/lwlte/lwlte_ml307r.c src/CMakeLists.txt tests/host/test_ml307r_contract.py
```

Expected: only public API, new compiling scaffolds, CMake entries, and ML307R contract tests are shown.

### Task 3: Full ML307R Facade Factory

**Files:**
- Modify: `src/lwlte/lwlte_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted test and confirm failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_facade_factory_mirrors_air780ep_lifecycle -v
```

Expected: FAIL because `lwlte_ml307r.c` still contains only the Task 2 scaffold and lacks factory wiring.

- [ ] **Step 2: Replace `src/lwlte/lwlte_ml307r.c` with full factory**

Replace the entire file with:

```c
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
        .rx_line_buf_size = config->at_rx_line_buf_size,
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
```

- [ ] **Step 3: Run targeted test**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_facade_factory_mirrors_air780ep_lifecycle -v
```

Expected: PASS.

- [ ] **Step 4: Run API/build/factory subset**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_public_api_and_build_entries_exist tests.host.test_ml307r_contract.Ml307rContractTest.test_facade_factory_mirrors_air780ep_lifecycle -v
```

Expected: PASS for both tests.

- [ ] **Step 5: Review diff**

Run:

```bash
git diff -- src/lwlte/lwlte_ml307r.c
```

Expected: `lwlte_ml307r.c` now mirrors Air780EP factory and does not call `core_start` or `modem_start`.

### Task 4: ML307R Modem Object And Startup Gate

**Files:**
- Modify: `src/modem/modem_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted startup tests and confirm failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_startup_uses_at_probe_not_matready tests.host.test_ml307r_contract.Ml307rContractTest.test_basic_init_commands_and_start_reset_order -v
```

Expected: FAIL because `modem_ml307r.c` still contains only the Task 2 scaffold.

- [ ] **Step 2: Replace `modem_ml307r.c` with full skeleton through startup**

Replace the file with a full Air780EP-style skeleton that includes all fields and startup helpers. Use this exact top-level structure. In this task, the network/MQTT/Ping ops keep explicit `ESP_ERR_NOT_SUPPORTED` bodies; Tasks 5, 6, and 7 replace those bodies with concrete implementations:

```c
/**
 * @file modem_ml307r.c
 * @brief ML307R 调制解调器实现
 * @details ML307R modem implementation
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include "modem_ml307r.h"
#include "modem_priv.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "modem_ml307r"
#define ML307R_MAX_PDP_CONTEXTS          1
#define ML307R_PRIMARY_CID               1
#define ML307R_MQTT_CONNECT_ID           0
#define ML307R_MAX_RESPONSE_LINES        101
#define ML307R_DEFAULT_CMD_TIMEOUT_MS    9000
#define ML307R_DEFAULT_READY_TIMEOUT_MS  30000
#define ML307R_AT_READY_PROBE_TIMEOUT_MS 1000
#define ML307R_INIT_RETRY_DELAY_MS       500
#define ML307R_INIT_CMD_MAX_ATTEMPTS     3
#define ML307R_MIPCALL_TIMEOUT_MS        90000
#define ML307R_MQTT_CMD_TIMEOUT_MS       9000
#define ML307R_MQTT_CONNECT_TIMEOUT_MS   60000
#define ML307R_MPING_PREFIX              "+MPING:"
#define ML307R_MPING_MAX_COUNT           100
#define ML307R_MPING_CMD_OVERHEAD_MS     5000U
#define ML307R_URC_CPIN                  "+CPIN:"
#define ML307R_URC_CREG                  "+CREG:"
#define ML307R_URC_CEREG                 "+CEREG:"
#define ML307R_URC_CGREG                 "+CGREG:"
#define ML307R_URC_MIPCALL               "+MIPCALL:"
#define ML307R_URC_MQTTURC               "+MQTTURC:"

_Static_assert(ML307R_MAX_RESPONSE_LINES >= ML307R_MPING_MAX_COUNT + 1,
               "ML307R MPING response storage must hold replies plus final status");

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    char *lines[ML307R_MAX_RESPONSE_LINES];
    at_response_t response;
} ml307r_cmd_ctx_t;

typedef struct {
    modem_t base;
    modem_ml307r_config_t config;
    at_urc_handler_t cpin_handler;
    at_urc_handler_t creg_handler;
    at_urc_handler_t cereg_handler;
    at_urc_handler_t cgreg_handler;
    at_urc_handler_t mipcall_handler;
    at_urc_handler_t mqtturc_handler;
    modem_info_t cached_info;
    modem_sim_status_t last_sim_status;
    modem_reg_status_t last_reg_status;
    modem_signal_t last_signal;
    modem_pdp_context_t pdp[ML307R_MAX_PDP_CONTEXTS];
    modem_mqtt_config_t mqtt_config;
    bool urc_registered;
    bool initialized;
    bool mqtt_configured;
    bool mqtt_session_connected;
    bool mqtt_data_enabled;
} modem_ml307r_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t ml307r_destroy(modem_t *me);
static esp_err_t ml307r_start(modem_t *me);
static esp_err_t ml307r_reset(modem_t *me);
static esp_err_t ml307r_get_info(modem_t *me, modem_info_t *info);
static esp_err_t ml307r_get_sim_status(modem_t *me, modem_sim_status_t *status);
static esp_err_t ml307r_get_signal(modem_t *me, modem_signal_t *signal);
static esp_err_t ml307r_get_registration(modem_t *me, modem_reg_status_t *status);
static esp_err_t ml307r_get_packet_attach_status(modem_t *me, bool *attached);
static esp_err_t ml307r_set_apn(modem_t *me, uint8_t cid, const char *apn);
static esp_err_t ml307r_activate_pdp(modem_t *me, uint8_t cid);
static esp_err_t ml307r_deactivate_pdp(modem_t *me, uint8_t cid);
static esp_err_t ml307r_get_pdp_context(modem_t *me, uint8_t cid,
                                         modem_pdp_context_t *pdp);
static esp_err_t ml307r_mqtt_configure(modem_t *me,
                                        const modem_mqtt_config_t *config);
static esp_err_t ml307r_mqtt_tcp_connect(modem_t *me);
static esp_err_t ml307r_mqtt_connect(modem_t *me);
static esp_err_t ml307r_mqtt_disconnect(modem_t *me);
static esp_err_t ml307r_mqtt_tcp_disconnect(modem_t *me);
static esp_err_t ml307r_mqtt_subscribe(modem_t *me,
                                        const modem_mqtt_topic_t *topic);
static esp_err_t ml307r_mqtt_unsubscribe(modem_t *me,
                                          const modem_mqtt_topic_t *topic);
static esp_err_t ml307r_mqtt_publish(modem_t *me,
                                      const modem_mqtt_publish_t *publish);
static esp_err_t ml307r_ping(modem_t *me,
                             const modem_ping_request_t *request,
                             modem_ping_reply_t *replies,
                             size_t max_replies,
                             modem_ping_summary_t *summary);
static modem_ml307r_t *to_ml307r(modem_t *me);
static void init_cmd_ctx(ml307r_cmd_ctx_t *ctx);
static esp_err_t send_cmd(modem_ml307r_t *self, const char *cmd,
                          ml307r_cmd_ctx_t *ctx, uint32_t timeout_ms);
static esp_err_t send_cmd_with_options(modem_ml307r_t *self, const char *cmd,
                                       ml307r_cmd_ctx_t *ctx,
                                       const at_cmd_options_t *options);
static esp_err_t ensure_at_ok(const at_response_t *response, const char *cmd);
static const char *find_line_with_prefix(const at_response_t *response,
                                         const char *prefix);
static const char *first_data_line(const at_response_t *response);
static esp_err_t copy_str_field(char *dst, size_t dst_size, const char *src);
static esp_err_t copy_str_field_strip_quotes(char *dst, size_t dst_size,
                                             const char *src);
static bool cid_valid(uint8_t cid);
static modem_pdp_context_t *pdp_by_cid(modem_ml307r_t *self, uint8_t cid);
static void set_state_nonblocking(modem_ml307r_t *self, modem_state_t state);
static const char *skip_prefix_value(const char *line, const char *prefix);
static esp_err_t parse_int_after_prefix(const char *line, const char *prefix,
                                        int *out);
static esp_err_t parse_two_ints_after_prefix(const char *line, const char *prefix,
                                             int *first, int *second);
static modem_reg_status_t map_reg_status(int stat);
static esp_err_t parse_registration_line(const char *line, const char *prefix,
                                         modem_reg_status_t *status);
static esp_err_t parse_registration_urc_line(const char *line, const char *prefix,
                                             modem_reg_status_t *status);
static esp_err_t consume_registration_extra_fields(const char *cursor);
static modem_sim_status_t parse_sim_status_line(const char *line);
static void cache_sim_status(modem_ml307r_t *self, modem_sim_status_t status);
static bool at_arg_safe(const char *value);
static bool at_text_payload_safe(const uint8_t *payload, size_t payload_len);
static bool looks_like_ip_addr(const char *line);
static TickType_t timeout_ticks(uint32_t timeout_ms);
static void set_initialized(modem_ml307r_t *self, bool initialized);
static void set_mqtt_data_enabled(modem_ml307r_t *self, bool enabled);
static bool mqtt_data_is_enabled(modem_ml307r_t *self);
static char *clone_mqtt_string(const char *value);
static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src);
static void free_mqtt_config(modem_mqtt_config_t *config);
static void clear_mqtt_state(modem_ml307r_t *self);
static char *escape_at_string(const char *value);
static char *copy_payload_text(const uint8_t *payload, size_t payload_len);
static esp_err_t post_mqtt_data_event(modem_ml307r_t *self, char *topic,
                                       size_t topic_len, uint8_t *payload,
                                       size_t payload_len);
static uint32_t now_ms(void);
static bool elapsed_at_least(uint32_t start_ms, uint32_t timeout_ms);
static void delay_init_retry(void);
static esp_err_t wait_at_ready(modem_ml307r_t *self);
static esp_err_t run_basic_init_cmds(modem_ml307r_t *self);
static esp_err_t finish_modem_ready(modem_t *me, modem_ml307r_t *self);
static esp_err_t hardware_reset(modem_ml307r_t *self);
static esp_err_t register_urcs(modem_ml307r_t *self);
static esp_err_t unregister_urcs(modem_ml307r_t *self);
static esp_err_t ml307r_unregister_urcs(modem_ml307r_t *self);
static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void mipcall_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void mqtturc_urc_handler(const char *prefix, const char *line, void *user_ctx);

/* Network, MQTT, and Ping helper prototypes used by Tasks 5-7. */
static esp_err_t query_mipcall(modem_ml307r_t *self, uint8_t cid,
                               modem_pdp_context_t *out_pdp);
static bool parse_mipcall_line(const char *line, modem_pdp_context_t *pdp);
static esp_err_t parse_mqtt_conn_urc(modem_ml307r_t *self, const char *line);
static bool parse_mqtt_publish_urc(const char *line, char **topic,
                                   size_t *topic_len, uint8_t **payload,
                                   size_t *payload_len);
static void handle_mqtturc(modem_ml307r_t *self, const char *line);
static esp_err_t parse_mping_uint(const char **cursor,
                                  uint32_t max_value,
                                  uint32_t *out_value);
static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary);
static esp_err_t parse_mping_reply_line(const char *line,
                                        modem_ping_reply_t *reply);
static esp_err_t parse_mping_statistics_line(const char *line,
                                             modem_ping_summary_t *summary);
static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request);

/**********************
 *  STATIC VARIABLES
 **********************/
static const modem_ops_t s_ml307r_ops = {
    .destroy = ml307r_destroy,
    .start = ml307r_start,
    .reset = ml307r_reset,
    .get_info = ml307r_get_info,
    .get_sim_status = ml307r_get_sim_status,
    .get_signal = ml307r_get_signal,
    .get_registration = ml307r_get_registration,
    .get_packet_attach_status = ml307r_get_packet_attach_status,
    .set_apn = ml307r_set_apn,
    .activate_pdp = ml307r_activate_pdp,
    .deactivate_pdp = ml307r_deactivate_pdp,
    .get_pdp_context = ml307r_get_pdp_context,
    .mqtt_configure = ml307r_mqtt_configure,
    .mqtt_tcp_connect = ml307r_mqtt_tcp_connect,
    .mqtt_connect = ml307r_mqtt_connect,
    .mqtt_disconnect = ml307r_mqtt_disconnect,
    .mqtt_tcp_disconnect = ml307r_mqtt_tcp_disconnect,
    .mqtt_subscribe = ml307r_mqtt_subscribe,
    .mqtt_unsubscribe = ml307r_mqtt_unsubscribe,
    .mqtt_publish = ml307r_mqtt_publish,
    .ping = ml307r_ping,
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
modem_t *modem_ml307r_create(at_engine_t *at,
                             const modem_ml307r_config_t *config)
{
    if (!at || !config) {
        ESP_LOGE(TAG, "NULL argument");
        return NULL;
    }

    modem_ml307r_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "calloc ml307r modem failed");
        return NULL;
    }

    self->config = *config;
    if (self->config.default_cmd_timeout_ms == 0) {
        self->config.default_cmd_timeout_ms = ML307R_DEFAULT_CMD_TIMEOUT_MS;
    }
    if (self->config.ready_timeout_ms == 0) {
        self->config.ready_timeout_ms = ML307R_DEFAULT_READY_TIMEOUT_MS;
    }

    self->last_sim_status = MODEM_SIM_UNKNOWN;
    self->last_reg_status = MODEM_REG_UNKNOWN;
    self->last_signal.rssi = 99;
    self->last_signal.ber = 99;
    self->pdp[0].cid = ML307R_PRIMARY_CID;
    strlcpy(self->pdp[0].pdp_type, "IPV4V6", sizeof(self->pdp[0].pdp_type));

    esp_err_t ret = modem_base_init(&self->base, "ml307r", at, &s_ml307r_ops,
                                    config->event_queue_size,
                                    config->event_task_stack,
                                    config->event_task_priority);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem base init failed: %s", esp_err_to_name(ret));
        free(self);
        return NULL;
    }

    return &self->base;
}
```

Add the shared helper/startup implementations by copying the existing Air780EP helper bodies from `src/modem/modem_air780ep.c` and applying the substitutions in the list below. After copying, run the startup contract tests in Step 3 to prove the copied helpers satisfy the ML307R startup contract:

- `to_ml307r()` returns `MODEM_CONTAINER_OF(me, modem_ml307r_t, base)`.
- `init_cmd_ctx()` sets `ctx->response.lines = ctx->lines` and `ctx->response.max_lines = ML307R_MAX_RESPONSE_LINES`.
- `send_cmd()` wraps `send_cmd_with_options()` with `self->config.default_cmd_timeout_ms`.
- `ensure_at_ok()`, `find_line_with_prefix()`, `first_data_line()`, `copy_str_field()`, `copy_str_field_strip_quotes()`, `skip_prefix_value()`, integer parsers, registration parsers, SIM parser, `timeout_ticks()`, `now_ms()`, and `elapsed_at_least()` should match Air780EP behavior with `TAG` changed to `modem_ml307r`.
- `hardware_reset()` must use `at_engine_begin_exclusive()`, flush RX, drive EN low for `reset_pulse_ms`, flush RX again, drive EN high, and always end exclusive mode.
- `wait_at_ready()` must repeatedly call `send_cmd(self, "AT", &ctx, ML307R_AT_READY_PROBE_TIMEOUT_MS)` until OK or `ESP_ERR_TIMEOUT`. It must not mention `MATREADY`.
- `run_basic_init_cmds()` must execute `ATE0`, `AT+CMEE=1`, `AT+CEREG=2`, `AT+CGREG=2`, `AT+CREG=2` in order with `ML307R_INIT_CMD_MAX_ATTEMPTS` and `ML307R_INIT_RETRY_DELAY_MS`.
- `ml307r_start()` and `ml307r_reset()` must clear MQTT/PDP volatile state, set `MODEM_STATE_INITIALIZING`, unregister existing URCs, call `hardware_reset(self)`, `wait_at_ready(self)`, `run_basic_init_cmds(self)`, `ret = register_urcs(self)`, then `finish_modem_ready(me, self)`. Error cleanup sets initialized false and modem state error.
- `register_urcs()` must register `ML307R_URC_CPIN`, `ML307R_URC_CREG`, `ML307R_URC_CEREG`, `ML307R_URC_CGREG`, `ML307R_URC_MIPCALL`, and `ML307R_URC_MQTTURC`, with rollback on failure.
- `unregister_urcs()` and `ml307r_unregister_urcs()` must unregister the same prefixes and clear handler nodes.
- In this startup task, non-startup ops return `ESP_ERR_NOT_SUPPORTED`; Tasks 5-7 replace those explicit bodies with concrete network, MQTT, and Ping implementations while keeping the same function names and ops assignments.

- [ ] **Step 3: Run targeted startup tests**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_startup_uses_at_probe_not_matready tests.host.test_ml307r_contract.Ml307rContractTest.test_basic_init_commands_and_start_reset_order tests.host.test_ml307r_contract.Ml307rContractTest.test_urc_registration_and_callback_constraints -v
```

Expected: PASS for startup and URC registration tests. Network/MQTT/Ping tests still fail.

- [ ] **Step 4: Run compile check after startup skeleton**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: FAIL only in tests that require MIPCALL, MQTT, and Ping command mappings, not in startup/factory/API tests.

- [ ] **Step 5: Review diff**

Run:

```bash
git diff -- src/modem/modem_ml307r.c tests/host/test_ml307r_contract.py
```

Expected: `modem_ml307r.c` now has full object/startup/URC skeleton, no `+MATREADY` startup gate, and no AT sends from URC handlers.

### Task 5: ML307R Identity, Status, And MIPCALL Network Operations

**Files:**
- Modify: `src/modem/modem_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted tests and confirm current failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_identity_status_and_registration_mapping_exists tests.host.test_ml307r_contract.Ml307rContractTest.test_mipcall_network_mapping_exists -v
```

Expected: FAIL until identity/status and MIPCALL functions are implemented.

- [ ] **Step 2: Add `query_cgatt()` prototype if missing**

In `src/modem/modem_ml307r.c`, add this prototype in `STATIC PROTOTYPES` near the parser/query helpers:

```c
static esp_err_t query_cgatt(modem_ml307r_t *self, bool *attached);
```

Expected: this helper is available to `ml307r_get_packet_attach_status()`.

- [ ] **Step 3: Implement identity and status ops**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` bodies for these exact functions with concrete implementations:

```c
static esp_err_t ml307r_get_info(modem_t *me, modem_info_t *info)
```

Required behavior:

- Validate `me` and `info`.
- Query these commands in order:
- `AT+CGSN` -> `result.imei`, first data line.
- `AT+CIMI` -> `result.imsi`, first data line; if this command fails, leave IMSI empty and continue.
- `AT+MCCID` -> `result.iccid`, first data line; if this command fails, leave ICCID empty and continue.
- `AT+CGMM` -> `result.model`, accept `+CGMM:` or first data line, strip quotes.
- `AT+CGMR` -> `result.fw_revision`, accept `+CGMR:` or first data line, strip quotes.
- A failed `AT+CGSN`, `AT+CGMM`, or `AT+CGMR` is a hard error because the modem identity query is unusable.
- Cache `self->cached_info` under `self->base.lock`, then copy `result` to `*info`.
- Do not post modem events.

```c
static esp_err_t ml307r_get_sim_status(modem_t *me,
                                       modem_sim_status_t *status)
```

Required behavior:

- Validate `me` and `status`.
- Send `AT+CPIN?`.
- Require `AT_RESP_OK` and a `+CPIN:` response line.
- Parse with `parse_sim_status_line()`.
- Cache through `cache_sim_status(self, parsed)`.
- Copy parsed status to `*status`.
- Do not post `MODEM_EVENT_SIM_CHANGED`; only URC handlers post runtime change events.

```c
static esp_err_t ml307r_get_signal(modem_t *me, modem_signal_t *signal)
```

Required behavior:

- Validate `me` and `signal`.
- Send `AT+CSQ`.
- Require `AT_RESP_OK` and a `+CSQ:` line.
- Parse with `parse_two_ints_after_prefix(line, "+CSQ:", &result.rssi, &result.ber)`.
- Validate RSSI `0..31` or `99`; validate BER `0..7` or `99`.
- For RSSI `0..31`, set `result.rssi_dbm = -113 + (2 * result.rssi)` and `result.rssi_dbm_valid = true`.
- For RSSI `99`, set `result.rssi_dbm = 0` and `result.rssi_dbm_valid = false`.
- Cache `self->last_signal` under lock and copy to `*signal`.

```c
static esp_err_t ml307r_get_registration(modem_t *me,
                                         modem_reg_status_t *status)
```

Required behavior:

- Validate `me` and `status`.
- Query in fallback order:
- `AT+CEREG?` with prefix `+CEREG:`.
- `AT+CGREG?` with prefix `+CGREG:`.
- `AT+CREG?` with prefix `+CREG:`.
- For each successful `AT_RESP_OK`, parse with `parse_registration_line()`.
- Skip parsed `MODEM_REG_UNKNOWN` and continue to the next query.
- Cache first non-unknown registration status under lock.
- Set modem state: registered home/roaming -> `MODEM_STATE_REGISTERED`, searching -> `MODEM_STATE_REGISTERING`, not registered/denied -> `MODEM_STATE_READY`.
- If no known status is parsed and no command failed, return `ESP_OK` with `MODEM_REG_UNKNOWN`.
- If all usable queries fail or are malformed, return the last error.

```c
static esp_err_t ml307r_get_packet_attach_status(modem_t *me, bool *attached)
```

Required behavior:

- Validate `me` and `attached`.
- Call `query_cgatt(to_ml307r(me), attached)`.

```c
static esp_err_t query_cgatt(modem_ml307r_t *self, bool *attached)
```

Required behavior:

- Send `AT+CGATT?`.
- Require `AT_RESP_OK` and `+CGATT:` line.
- Parse state with `parse_int_after_prefix()`.
- Accept only state `0` or `1`.
- Set `*attached = (state == 1)`.

- [ ] **Step 4: Implement MIPCALL parser and query helper**

Implement:

```c
static bool parse_mipcall_line(const char *line, modem_pdp_context_t *pdp)
```

Required parsing behavior:

- Return `false` for NULL inputs or non-`+MIPCALL:` lines.
- Accept both `+MIPCALL: <cid>,...` and `+MIPCALL:<cid>,...` spacing.
- Parse CID as unsigned integer; support only CID `1`.
- Parse state as `0` or `1`.
- Initialize `*pdp` to zero, then set `pdp->cid = 1` and `pdp->pdp_type = "IPV4V6"`.
- For state `0`, set `pdp->active = false`, `pdp->ip_addr[0] = '\0'`, and return `true`.
- For state `1`, require a following IP token.
- Accept quoted IP (`"10.1.2.3"`) or unquoted token up to comma/end.
- Require IP length `> 0` and `< MODEM_IP_ADDR_MAX_LEN`.
- Copy the first IP token into `pdp->ip_addr`, set `pdp->active = true`, and return `true`.
- Ignore optional IPv6 token for this first implementation.

Implement:

```c
static esp_err_t query_mipcall(modem_ml307r_t *self, uint8_t cid,
                               modem_pdp_context_t *out_pdp)
```

Required behavior:

- Validate inputs; `cid == 0` returns `ESP_ERR_INVALID_ARG`, `cid != 1` returns `ESP_ERR_NOT_SUPPORTED`.
- Send `AT+MIPCALL?`.
- Require `AT_RESP_OK`.
- Scan every response line whose prefix is `ML307R_URC_MIPCALL`.
- For the first parsed CID 1 line, refresh the cached PDP entry under lock:
- Preserve cached APN if present.
- Preserve cached PDP type unless parser provided a non-empty type; default to `IPV4V6`.
- Copy active and IP from parsed line.
- Copy the cached snapshot to `*out_pdp` when `out_pdp` is non-NULL.
- Return `ESP_OK` if CID 1 is found, otherwise `ESP_ERR_NOT_FOUND`.

- [ ] **Step 5: Implement APN and PDP ops**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` bodies for these functions:

```c
static esp_err_t ml307r_set_apn(modem_t *me, uint8_t cid, const char *apn)
```

Required behavior:

- Validate `me` and `apn`.
- `cid == 0` returns `ESP_ERR_INVALID_ARG`; `cid != 1` returns `ESP_ERR_NOT_SUPPORTED`.
- Require `strlen(apn) < MODEM_APN_MAX_LEN` and `at_arg_safe(apn)`.
- Build command exactly with this format string:

```c
"AT+CGDCONT=%u,\"IPV4V6\",\"%s\""
```

- Send the command with default timeout and require `AT_RESP_OK`.
- Under lock, cache APN and `pdp_type = "IPV4V6"` for CID 1.

```c
static esp_err_t ml307r_activate_pdp(modem_t *me, uint8_t cid)
```

Required behavior:

- Validate `me`; `cid == 0` returns `ESP_ERR_INVALID_ARG`; `cid != 1` returns `ESP_ERR_NOT_SUPPORTED`.
- Call `query_mipcall(self, cid, &snapshot)` before dialing.
- If query returns `ESP_OK` and `snapshot.active && snapshot.ip_addr[0]`, set modem state to `MODEM_STATE_PDP_ACTIVE`, post `MODEM_EVENT_PDP_ACTIVATED` with the snapshot, and return `ESP_OK`.
- If query returns errors other than `ESP_ERR_NOT_FOUND`, return that error.
- Build command exactly with this format string:

```c
"AT+MIPCALL=1,%u"
```

- Send with `ML307R_MIPCALL_TIMEOUT_MS` and require `AT_RESP_OK`.
- Scan command response lines for an activation `+MIPCALL:` line and cache/post if found.
- If no activation line appears in the command response, poll `AT+MIPCALL?` until `ML307R_MIPCALL_TIMEOUT_MS` elapses. Use `delay_init_retry()` between polls. This handles both command-response and idle-URC paths without sending AT commands from the URC handler.
- On success, cache PDP, set `MODEM_STATE_PDP_ACTIVE`, and post `MODEM_EVENT_PDP_ACTIVATED`.
- Return `ESP_ERR_TIMEOUT` if active/IP is not observed within the timeout.

```c
static esp_err_t ml307r_deactivate_pdp(modem_t *me, uint8_t cid)
```

Required behavior:

- Validate `me`; `cid == 0` returns `ESP_ERR_INVALID_ARG`; `cid != 1` returns `ESP_ERR_NOT_SUPPORTED`.
- Snapshot cached PDP before sending the command.
- Build command exactly with this format string:

```c
"AT+MIPCALL=0,%u"
```

- Send with `ML307R_MIPCALL_TIMEOUT_MS` and require `AT_RESP_OK`.
- Under lock, clear cached `active`, `ip_addr`, `mqtt_data_enabled`, and `mqtt_session_connected`.
- Set modem state to `MODEM_STATE_READY`.
- If the previous snapshot was active, post `MODEM_EVENT_PDP_DEACTIVATED` using the previous CID/APN/PDP type with `active=false` and empty IP.

```c
static esp_err_t ml307r_get_pdp_context(modem_t *me, uint8_t cid,
                                        modem_pdp_context_t *pdp)
```

Required behavior:

- Validate `me` and `pdp`; `cid == 0` returns `ESP_ERR_INVALID_ARG`; `cid != 1` returns `ESP_ERR_NOT_SUPPORTED`.
- Call `query_mipcall(self, cid, NULL)`.
- If query returns neither `ESP_OK` nor `ESP_ERR_NOT_FOUND`, return that error.
- Under lock, copy cached CID 1 PDP context to `*pdp`.
- If cached `pdp_type` is empty, set it to `IPV4V6` before returning.

- [ ] **Step 6: Implement system URC event behavior**

Complete these handlers. They must not call `send_cmd()`, `at_engine_send_cmd()`, `at_engine_send_cmd_with_options()`, `at_engine_flush_rx()`, `at_engine_begin_exclusive()`, or `at_engine_register_urc()`.

```c
static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx)
```

Behavior: ignore `prefix`, parse `line` with `parse_sim_status_line()`, cache `last_sim_status`, and post `MODEM_EVENT_SIM_CHANGED`.

```c
static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx)
```

Behavior: parse with `parse_registration_urc_line(line, prefix, &status)`, cache `last_reg_status`, update state using the same mapping as query path, and post `MODEM_EVENT_REG_CHANGED`.

```c
static void mipcall_urc_handler(const char *prefix, const char *line, void *user_ctx)
```

Behavior: ignore `prefix`, parse with `parse_mipcall_line()`, update cached PDP, active -> set `MODEM_STATE_PDP_ACTIVE` and post `MODEM_EVENT_PDP_ACTIVATED`, inactive -> clear PDP IP plus MQTT data/session flags, set `MODEM_STATE_READY`, and post `MODEM_EVENT_PDP_DEACTIVATED`.

- [ ] **Step 7: Run targeted tests**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_identity_status_and_registration_mapping_exists tests.host.test_ml307r_contract.Ml307rContractTest.test_mipcall_network_mapping_exists tests.host.test_ml307r_contract.Ml307rContractTest.test_urc_registration_and_callback_constraints -v
```

Expected: PASS.

Then run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: MQTT and Ping mapping tests may still fail if Tasks 6 and 7 are not implemented yet; API, facade, startup, identity/status, MIPCALL, and URC constraint tests pass.

- [ ] **Step 8: Review diff**

Run:

```bash
git diff -- src/modem/modem_ml307r.c tests/host/test_ml307r_contract.py
```

Expected: ML307R identity/status/MIPCALL code is present, ML307R uses `AT+MCCID` instead of Air780EP `AT+ICCID`, data activation uses `MIPCALL`, CID support is limited to 1, startup still does not mention `+MATREADY`, and URC callbacks contain no AT Engine command calls.

### Task 6: ML307R MQTT Operations And `+MQTTURC` Handling

**Files:**
- Modify: `src/modem/modem_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted test and confirm failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_mqtt_command_mapping_exists -v
```

Expected: FAIL until MQTT command mappings and handlers are implemented.

- [ ] **Step 2: Implement MQTT config ownership helpers**

Implement these helpers in `src/modem/modem_ml307r.c`:

```c
static char *clone_mqtt_string(const char *value)
static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src)
static void free_mqtt_config(modem_mqtt_config_t *config)
static void clear_mqtt_state(modem_ml307r_t *self)
```

Required behavior:

- `clone_mqtt_string()` returns NULL for NULL input; otherwise allocates and copies the NUL-terminated string.
- `copy_mqtt_config()` requires `dst`, `src`, `src->client_id`, `src->host`, and `src->port > 0`.
- `copy_mqtt_config()` deep-copies `client_id`, optional `username`, optional `password`, and `host`.
- `copy_mqtt_config()` normalizes `keepalive_s == 0` to `120` in the copied object.
- `copy_mqtt_config()` frees any old `dst` content only after all new allocations succeed.
- `free_mqtt_config()` frees the four string fields and zeroes the struct.
- `clear_mqtt_state()` clears `mqtt_configured`, `mqtt_session_connected`, and `mqtt_data_enabled` under lock and frees cached MQTT config.

Expected: `ml307r_destroy()` calls `set_mqtt_data_enabled(self, false)` and `clear_mqtt_state(self)` before unregistering URCs and clearing initialized state.

- [ ] **Step 3: Implement safe AT string and payload helpers**

Implement these helpers:

```c
static char *escape_at_string(const char *value)
static bool at_text_payload_safe(const uint8_t *payload, size_t payload_len)
static char *copy_payload_text(const uint8_t *payload, size_t payload_len)
static esp_err_t post_mqtt_data_event(modem_ml307r_t *self, char *topic,
                                       size_t topic_len, uint8_t *payload,
                                       size_t payload_len)
```

Required behavior:

- `escape_at_string()` returns a newly allocated string where `"`, `\`, `\r`, and `\n` are encoded as `\22`, `\5C`, `\0D`, and `\0A`, matching the existing Air780EP helper style.
- `at_text_payload_safe()` returns false for NULL payload with non-zero length, NUL bytes, `"`, `\r`, or `\n`.
- `copy_payload_text()` allocates `payload_len + 1`, copies the payload bytes, and appends NUL. It requires `at_text_payload_safe()` to pass first.
- `post_mqtt_data_event()` posts `MODEM_EVENT_PROTOCOL_DATA` with `MODEM_PROTOCOL_MQTT`; on successful `modem_post_event()`, the modem event task owns heap topic/payload buffers.
- On `modem_post_event()` failure, the caller remains responsible for freeing topic/payload.

- [ ] **Step 4: Implement `ml307r_mqtt_configure()`**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` body with:

```c
static esp_err_t ml307r_mqtt_configure(modem_t *me,
                                       const modem_mqtt_config_t *config)
```

Required behavior:

- Validate `me`, `config`, `config->client_id`, `config->host`, and `config->port > 0`.
- Reject configuration while `self->mqtt_session_connected` is true.
- Deep-copy config into a temporary `modem_mqtt_config_t new_config` using `copy_mqtt_config()`.
- Escape `host`, `client_id`, optional `username` as empty string when NULL, and optional `password` as empty string when NULL.
- Send and require OK for these exact command strings in this order:

```c
"AT+MQTTCFG=\"version\",0,4"
"AT+MQTTCFG=\"cid\",0,1"
"AT+MQTTCFG=\"keepalive\",0,%u"
"AT+MQTTCFG=\"clean\",0,%u"
"AT+MQTTCFG=\"cached\",0,0"
```

- Use `ML307R_MQTT_CMD_TIMEOUT_MS` for each command.
- Only after all commands succeed, swap `new_config` into `self->mqtt_config`, set `mqtt_configured = true`, `mqtt_session_connected = false`, and `mqtt_data_enabled = false` under lock.
- Free all temporary escaped strings and any unused copied config on failure.

- [ ] **Step 5: Implement ML307R MQTT connect/disconnect ops**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` bodies for these functions:

```c
static esp_err_t ml307r_mqtt_tcp_connect(modem_t *me)
static esp_err_t ml307r_mqtt_connect(modem_t *me)
static esp_err_t ml307r_mqtt_disconnect(modem_t *me)
static esp_err_t ml307r_mqtt_tcp_disconnect(modem_t *me)
```

Required behavior:

- `ml307r_mqtt_tcp_connect()` validates `me`, requires `mqtt_configured`, and returns `ESP_OK`; ML307R MQTT transport is part of `AT+MQTTCONN`.
- `ml307r_mqtt_connect()` requires configured and not already connected.
- `ml307r_mqtt_connect()` snapshots and escapes host, client ID, username, and password.
- Build the connect command with this exact format string:

```c
"AT+MQTTCONN=0,\"%s\",%u,\"%s\",\"%s\",\"%s\""
```

- Send the connect command with `ML307R_MQTT_CONNECT_TIMEOUT_MS`.
- Use `send_cmd_with_options()` with `AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` and a prefix success match for `ML307R_URC_MQTTURC`, so command handling can wait for `+MQTTURC: "conn",0,<state>` instead of stopping at the first intermediate `OK`.
- After command returns, scan response lines for `+MQTTURC:` and call `parse_mqtt_conn_urc(self, line)` on the first `"conn"` line.
- `parse_mqtt_conn_urc()` returns `ESP_OK` only for `conn_state == 0`; non-zero states clear MQTT connected/data-enabled flags and return `ESP_FAIL`.
- `ml307r_mqtt_disconnect()` requires a connected session, disables MQTT data first, sends `AT+MQTTDISC=0`, requires OK, and clears `mqtt_session_connected` and `mqtt_data_enabled` under lock regardless of whether a later idle `conn,0,2` URC arrives.
- `ml307r_mqtt_tcp_disconnect()` validates `me`, clears `mqtt_session_connected` and `mqtt_data_enabled` under lock, and returns `ESP_OK`; it sends no AT command.

- [ ] **Step 6: Implement MQTT subscribe/unsubscribe/publish ops**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` bodies for these functions:

```c
static esp_err_t ml307r_mqtt_subscribe(modem_t *me,
                                       const modem_mqtt_topic_t *topic)
static esp_err_t ml307r_mqtt_unsubscribe(modem_t *me,
                                         const modem_mqtt_topic_t *topic)
static esp_err_t ml307r_mqtt_publish(modem_t *me,
                                     const modem_mqtt_publish_t *publish)
```

Required behavior:

- Subscribe/unsubscribe validate `me`, `topic`, `topic->topic`, `topic->topic[0]`, QoS `<= 2`, and connected MQTT session.
- Escape the topic.
- Build subscribe with this exact format string and send with `ML307R_MQTT_CMD_TIMEOUT_MS`:

```c
"AT+MQTTSUB=0,\"%s\",%u"
```

- Build unsubscribe with this exact format string and send with `ML307R_MQTT_CMD_TIMEOUT_MS`:

```c
"AT+MQTTUNSUB=0,\"%s\""
```

- Both require immediate command OK and do not wait for ACK URCs in this first implementation.
- Publish validates `me`, `publish`, `publish->topic`, non-empty topic, `publish->payload`, `publish->payload_len > 0`, QoS `<= 2`, connected MQTT session, and `publish->payload_len <= UINT_MAX`.
- Publish uses `at_text_payload_safe()` and `copy_payload_text()`; unsafe binary/text payload returns `ESP_ERR_INVALID_ARG`.
- Build publish with this exact format string and send with `ML307R_MQTT_CMD_TIMEOUT_MS`:

```c
"AT+MQTTPUB=0,\"%s\",%u,%u,%u,\"%s\""
```

- `retain` maps to `0` or `1`, payload length is the original unescaped text length, and payload text is not backslash-escaped after validation.

- [ ] **Step 7: Implement `+MQTTURC` connection parser**

Implement:

```c
static esp_err_t parse_mqtt_conn_urc(modem_ml307r_t *self, const char *line)
```

Required behavior:

- Validate `self` and `line`.
- Require line prefix `ML307R_URC_MQTTURC`.
- Parse event string; only process `"conn"`.
- Parse connect ID and require `ML307R_MQTT_CONNECT_ID` (`0`).
- Parse `conn_state`.
- `conn_state == 0`: set `mqtt_session_connected = true` and `mqtt_data_enabled = true` under lock, return `ESP_OK`.
- `conn_state == 1`: log reconnecting, clear `mqtt_session_connected = false`, keep `mqtt_data_enabled = false`, return `ESP_FAIL`.
- Any other state: snapshot whether the session was connected, clear `mqtt_session_connected` and `mqtt_data_enabled`, post `MODEM_EVENT_PROTOCOL_CLOSED` if transitioning from connected, return `ESP_FAIL`.

- [ ] **Step 8: Implement direct publish URC parser**

Implement:

```c
static bool parse_mqtt_publish_urc(const char *line, char **topic,
                                   size_t *topic_len, uint8_t **payload,
                                   size_t *payload_len)
```

Required behavior:

- Validate pointers and initialize outputs to NULL/0.
- Accept only `+MQTTURC: "publish",0,<mid>,<topic>,<total_len>,<payload_len>,<payload>`.
- Require connect ID `0`.
- Parse and ignore `<mid>`.
- Parse topic as quoted or unquoted up to comma; require non-empty topic.
- Parse `total_len` and `payload_len`; require `payload_len <= total_len` and payload length fits `size_t`.
- Copy exactly `payload_len` bytes from the remaining line into heap payload; do not depend on NUL termination beyond those bytes.
- Copy topic into heap and NUL-terminate it.
- Return true on success; on any failure, free partial allocations and return false.

- [ ] **Step 9: Implement MQTT URC dispatcher**

Implement:

```c
static void handle_mqtturc(modem_ml307r_t *self, const char *line)
static void mqtturc_urc_handler(const char *prefix, const char *line,
                                void *user_ctx)
```

Required behavior:

- `mqtturc_urc_handler()` ignores `prefix`, validates `user_ctx` and `line`, casts `user_ctx` to `modem_ml307r_t *`, and calls `handle_mqtturc()`.
- `handle_mqtturc()` detects the event string after `+MQTTURC:`.
- For `"conn"`, call `parse_mqtt_conn_urc()` and log non-OK results.
- For `"publish"`, call `parse_mqtt_publish_urc()`. If parsing succeeds and `mqtt_data_is_enabled(self)` is true, call `post_mqtt_data_event()`. If data is disabled or posting fails, free heap topic/payload.
- For `"pubnmi"`, log only and do not call `AT+MQTTREAD` from the URC callback.
- For `"suback"`, `"unsuback"`, `"puback"`, `"pubrec"`, `"pubcomp"`, `"timeout"`, `"drop"`, and `"pingresp"`, log/update local diagnostics only; do not post completion events.
- No MQTT URC handling function may call `send_cmd()` or any `at_engine_*` command/registration API.

- [ ] **Step 10: Run targeted tests**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_mqtt_command_mapping_exists tests.host.test_ml307r_contract.Ml307rContractTest.test_urc_registration_and_callback_constraints -v
```

Expected: PASS.

Then run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: MQTT checks pass; Ping checks may still fail if Task 7 is not implemented yet.

- [ ] **Step 11: Review diff**

Run:

```bash
git diff -- src/modem/modem_ml307r.c tests/host/test_ml307r_contract.py
```

Expected: ML307R MQTT op bodies, helper implementations, and `+MQTTURC` parsing/handling are changed. The diff does not introduce `mqtt_tcp_connected`, does not introduce `+MATREADY`, and does not send AT commands from URC callbacks.

### Task 7: ML307R MPING Operation And Parsers

**Files:**
- Modify: `src/modem/modem_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Run targeted test and confirm failure**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_ping_mapping_exists -v
```

Expected: FAIL until `ml307r_ping()`, `parse_mping_reply_line()`, `parse_mping_statistics_line()`, `calculate_ping_summary()`, and `ping_cmd_timeout_ms()` are implemented.

- [ ] **Step 2: Implement timeout and unsigned parser helpers**

Implement:

```c
static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request)
static esp_err_t parse_mping_uint(const char **cursor,
                                  uint32_t max_value,
                                  uint32_t *out_value)
```

Required behavior:

- `parse_mping_uint()` mirrors Air780EP `parse_cipping_uint()`: skip leading spaces, require digits, use `strtoul`, reject overflow, reject values greater than `max_value`, advance `*cursor`, and write `*out_value`.
- `ping_cmd_timeout_ms()` returns `ML307R_DEFAULT_CMD_TIMEOUT_MS` when request is NULL.
- Convert ML307R per-packet timeout seconds as `timeout_s = (request->timeout_100ms + 9U) / 10U`; if the result is 0, use 1.
- Derive `derived_ms = request->count * timeout_s * 1000U + ML307R_MPING_CMD_OVERHEAD_MS`.
- If `request->total_timeout_ms != 0`, return the larger of `request->total_timeout_ms` and `derived_ms`.
- Otherwise return `derived_ms`.

- [ ] **Step 3: Implement MPING reply parser**

Implement:

```c
static esp_err_t parse_mping_reply_line(const char *line,
                                        modem_ping_reply_t *reply)
```

Required behavior:

- Validate `line` and `reply`.
- Require prefix `ML307R_MPING_PREFIX`.
- Reject statistics lines; those are parsed by `parse_mping_statistics_line()`.
- Parse form:

```text
+MPING: <result>,"<ip>",<packet_len>,<time>,<ttl>
```

- `result == 0` means successful reply.
- Any non-zero result means failed/lost reply; fill `reply->success = false`, `reply->ip[0] = '\0'`, `reply->time_ms = 0`, `reply->ttl = 0`, and return `ESP_OK` after accepting optional remaining fields.
- For successful replies, require quoted non-empty IP that fits `reply->ip`.
- Parse packet length and require `1..1400`; parse but do not store.
- Parse `time` into `reply->time_ms`.
- Parse `ttl` and require `<= UINT8_MAX`.
- Set `reply->success = true`.
- Do not set `reply->seq`; `ml307r_ping()` assigns synthetic sequence numbers from result-line order because ML307R MPING result lines do not include sequence.

- [ ] **Step 4: Implement MPING statistics parser**

Implement:

```c
static esp_err_t parse_mping_statistics_line(const char *line,
                                             modem_ping_summary_t *summary)
```

Required behavior:

- Validate `line` and `summary`.
- Require prefix `ML307R_MPING_PREFIX`.
- Require form:

```text
+MPING: "statistics",<sent>,<lost>,<rtt_min>,<rtt_max>,<rtt_avg>
```

- Parse sent and lost as `uint8_t` values.
- Require `lost <= sent`.
- Parse RTT values as `uint32_t` milliseconds.
- Fill `summary->sent = sent`, `summary->lost = lost`, `summary->received = sent - lost`, and the RTT fields.

- [ ] **Step 5: Implement fallback summary**

Implement:

```c
static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary)
```

Required behavior:

- Return immediately if any pointer is NULL or `summary` is NULL.
- Zero `*summary` first.
- Set `summary->sent = request->count`.
- Count only replies with `success == true` as received.
- Set `summary->lost = summary->sent - summary->received`.
- Compute min/max/avg over successful replies only.
- Leave RTT fields as 0 when no successful replies exist.

- [ ] **Step 6: Implement `ml307r_ping()`**

Replace the Task 4 `ESP_ERR_NOT_SUPPORTED` body with:

```c
static esp_err_t ml307r_ping(modem_t *me,
                             const modem_ping_request_t *request,
                             modem_ping_reply_t *replies,
                             size_t max_replies,
                             modem_ping_summary_t *summary)
```

Required behavior:

- Validate `me`, `request`, `request->host`, non-empty host, `replies`, and `max_replies >= request->count`.
- Require `request->count >= 1 && request->count <= ML307R_MPING_MAX_COUNT`.
- Convert `timeout_s = (request->timeout_100ms + 9U) / 10U`; use 1 when result is 0; require `timeout_s <= 60`.
- Map `packet_len = request->data_len`; if 0, use 16; require `packet_len >= 1 && packet_len <= 1400`.
- Escape host with `escape_at_string()`.
- Build command with this exact format string:

```c
"AT+MPING=\"%s\",%u,%u,%u,1"
```

- Use `send_cmd_with_options()` with a prefix success match for `ML307R_MPING_PREFIX`, `AT_CMD_FLAG_NO_STANDARD_OK_FINAL`, and `AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` so the command can collect `+MPING:` lines after an intermediate `OK`.
- Use `ping_cmd_timeout_ms(request)` as the command timeout.
- Require `ensure_at_ok(&ctx.response, "AT+MPING")` after the command returns.
- Iterate response lines:
- For lines starting with `+MPING: "statistics"`, parse with `parse_mping_statistics_line()` and store one local summary.
- For other lines starting with `+MPING:`, parse with `parse_mping_reply_line()`, set `reply.seq = parsed_count + 1`, and store in `replies[parsed_count]`.
- Require `parsed_count == request->count` before returning `ESP_OK`.
- If statistics is present, require `parsed_summary.sent == request->count`, then copy to `*summary` when `summary` is non-NULL.
- If statistics is absent, call `calculate_ping_summary(request, replies, parsed_count, summary)`.
- Free all allocated command/host buffers on every return path.

- [ ] **Step 7: Run tests**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract.Ml307rContractTest.test_ping_mapping_exists -v
```

Expected: PASS.

Then run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: all ML307R static contract tests pass.

- [ ] **Step 8: Review diff**

Run:

```bash
git diff -- src/modem/modem_ml307r.c tests/host/test_ml307r_contract.py
```

Expected: MPING implementation uses `AT+MPING`, parses ML307R result/statistics lines, does not reuse Air780EP `CIPPING`, does not include request TTL in the command, and still contains no `+MATREADY` startup gate.

### Task 8: Full Host And ESP-IDF Verification

**Files:**
- Verify: entire repository

- [ ] **Step 1: Run ML307R contract suite**

Run:

```bash
python -m unittest tests.host.test_ml307r_contract -v
```

Expected: PASS for all tests.

- [ ] **Step 2: Run all host tests**

Run:

```bash
python -m unittest discover tests/host -v
```

Expected: PASS for all host tests. If failures appear outside ML307R, inspect whether they are caused by this change before modifying unrelated code.

- [ ] **Step 3: Build with ESP-IDF MCP**

Run the ESP-IDF build using the repository-preferred MCP build tool:

```text
esp-idf-eim_build_project
```

Expected: build succeeds. If the MCP build tool is unavailable or fails before invoking ESP-IDF, use the documented fallback:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: build succeeds.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
git status --short
git diff -- src/include/lwlte.h src/CMakeLists.txt src/modem/modem_ml307r.h src/modem/modem_ml307r.c src/lwlte/lwlte_ml307r.c tests/host/test_ml307r_contract.py docs/superpowers/plans/2026-06-06-ml307r-modem-implementation-plan.md
```

Expected: only intended ML307R implementation, public API, build registration, tests, and plan changes are present.

- [ ] **Step 5: Hardware verification note**

If ML307R hardware is connected and the user asks for live verification, follow `docs/agents/build-and-debug.md` and capture serial output with:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 45
```

Expected serial behavior: EN low pulse, EN high, repeated `AT` until `OK`, `ATE0`, `AT+CMEE=1`, `AT+CEREG=2`, `AT+CGREG=2`, `AT+CREG=2`, Core SIM/register/attach checks, `AT+MIPCALL?` or `AT+MIPCALL=1,1`, and `NET_ONLINE` only after an IP is observed.
