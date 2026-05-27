# MQTT Client End-To-End Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an end-to-end MQTT client path from public `lwlte_mqtt_*` APIs through MQTT Client Service, Core command queue, Modem MQTT ops, and Air780EP MQTT AT commands.

**Architecture:** App code calls only the LWLTE Facade public API. Facade owns MQTT lifecycle and delegates to an internal MQTT Client Service. MQTT depends only on Core; Core serializes typed protocol commands and calls Modem MQTT ops; Air780EP translates those ops to AT commands and URCs.

**Tech Stack:** ESP-IDF C, FreeRTOS tasks/queues/semaphores, `esp_event`, existing C opaque-handle/OOP patterns, host-side Python static regression tests, ESP-IDF build.

---

## File Structure

- Create: `tests/host/test_mqtt_end_to_end_contract.py` — static regression checks that pin public API, layer boundaries, command queue, Modem MQTT ops, Air780EP AT mapping, and `+MSUB:` data path.
- Modify: `src/at_engine/at_engine.h` — add a payload-command API for prompt-based commands such as `AT+MPUBEX`.
- Modify: `src/at_engine/at_engine.c` — implement prompt detection, payload write, and final response wait without exposing UART directly to Modem.
- Modify: `src/modem/modem.h` — add MQTT value objects, protocol event types, and `modem_mqtt_*` wrappers.
- Modify: `src/modem/modem_priv.h` — add MQTT ops to `modem_ops_t`.
- Modify: `src/modem/modem.c` — implement MQTT wrappers and release heap-owned protocol event payloads after callback delivery.
- Modify: `src/modem/modem_air780ep.c` — implement Air780EP MQTT commands, `+MSUB:` URC parsing, and MQTT URC registration.
- Modify: `src/core/core.h` — add `CORE_EVENT_PROTOCOL_*`, protocol data types, `core_cmd_t`, and `core_submit_cmd()`.
- Modify: `src/core/core_priv.h` — add `CORE_SIG_SERVICE_CMD` and Core-owned service command pointer in `core_fsm_sig_t`.
- Modify: `src/core/core.c` — implement command deep copy/free and Core event callback safeguards for protocol data.
- Modify: `src/core/core_fsm.c` — execute service commands serially and publish copied protocol data events.
- Create: `src/mqtt_client/mqtt_client.h` — internal MQTT Client Service layer API.
- Create: `src/mqtt_client/mqtt_client_priv.h` — MQTT FSM private types and helpers.
- Create: `src/mqtt_client/mqtt_client.c` — MQTT Client Service implementation.
- Modify: `src/include/lwlte.h` — add public MQTT state, message type, events, and user APIs.
- Modify: `src/include/lwlte_air780ep.h` — add `lwlte_air780ep_config_mqtt_client_t` and nested `mqtt_client` config field.
- Modify: `src/lwlte/lwlte_priv.h` — include MQTT service handle and MQTT event bridge prototype.
- Modify: `src/lwlte/lwlte.c` — implement public `lwlte_mqtt_*` wrappers and map MQTT events to user callbacks.
- Modify: `src/lwlte/lwlte_air780ep.c` — validate MQTT nested config and create/register/destroy MQTT service around Core.
- Modify: `src/CMakeLists.txt` — compile `mqtt_client/mqtt_client.c` and add `mqtt_client` to private include dirs.
- Modify: `docs/agents/classes.md` and `docs/agents/architecture.md` — keep implementation names and documented names aligned when a task changes an approved symbol.

---

### Task 1: Add MQTT Contract Regression Tests

**Files:**
- Create: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Write the failing static contract test**

Create `tests/host/test_mqtt_end_to_end_contract.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static regression checks for the MQTT end-to-end implementation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]

LWLTE_H = ROOT / "src/include/lwlte.h"
AIR780EP_H = ROOT / "src/include/lwlte_air780ep.h"
LWLTE_PRIV = ROOT / "src/lwlte/lwlte_priv.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"

AT_ENGINE_H = ROOT / "src/at_engine/at_engine.h"
AT_ENGINE_C = ROOT / "src/at_engine/at_engine.c"

MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP_C = ROOT / "src/modem/modem_air780ep.c"

CORE_H = ROOT / "src/core/core.h"
CORE_PRIV = ROOT / "src/core/core_priv.h"
CORE_C = ROOT / "src/core/core.c"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"

MQTT_H = ROOT / "src/mqtt_client/mqtt_client.h"
MQTT_PRIV = ROOT / "src/mqtt_client/mqtt_client_priv.h"
MQTT_C = ROOT / "src/mqtt_client/mqtt_client.c"
SRC_CMAKE = ROOT / "src/CMakeLists.txt"


class MqttEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.air780ep_h = AIR780EP_H.read_text(encoding="utf-8")
        cls.lwlte_priv = LWLTE_PRIV.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")
        cls.lwlte_air780ep_c = LWLTE_AIR780EP_C.read_text(encoding="utf-8")

        cls.at_engine_h = AT_ENGINE_H.read_text(encoding="utf-8")
        cls.at_engine_c = AT_ENGINE_C.read_text(encoding="utf-8")

        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv = MODEM_PRIV.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep_c = AIR780EP_C.read_text(encoding="utf-8")

        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_priv = CORE_PRIV.read_text(encoding="utf-8")
        cls.core_c = CORE_C.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")

        cls.mqtt_h = MQTT_H.read_text(encoding="utf-8")
        cls.mqtt_priv = MQTT_PRIV.read_text(encoding="utf-8")
        cls.mqtt_c = MQTT_C.read_text(encoding="utf-8")
        cls.src_cmake = SRC_CMAKE.read_text(encoding="utf-8")

    def test_public_api_and_air780ep_mqtt_config_exist(self):
        for token in [
            "typedef enum {",
            "LWLTE_MQTT_STATE_STOPPED",
            "LWLTE_MQTT_STATE_CONNECTED",
            "lwlte_mqtt_state_t",
            "lwlte_mqtt_msg_t",
            "LWLTE_EVENT_MQTT_CONNECTED",
            "LWLTE_EVENT_MQTT_DATA",
            "esp_err_t lwlte_mqtt_start(lwlte_t *me);",
            "esp_err_t lwlte_mqtt_stop(lwlte_t *me);",
            "esp_err_t lwlte_mqtt_get_state(lwlte_t *me, lwlte_mqtt_state_t *state);",
            "esp_err_t lwlte_mqtt_subscribe(lwlte_t *me, const char *topic, uint8_t qos);",
            "esp_err_t lwlte_mqtt_unsubscribe(lwlte_t *me, const char *topic);",
            "esp_err_t lwlte_mqtt_publish(lwlte_t *me, const char *topic,",
        ]:
            self.assertIn(token, self.lwlte_h)

        for token in [
            "lwlte_air780ep_config_mqtt_client_t",
            "bool enabled;",
            "const char *host;",
            "uint16_t port;",
            "const char *client_id;",
            "lwlte_air780ep_config_mqtt_client_t mqtt_client;",
        ]:
            self.assertIn(token, self.air780ep_h)

    def test_mqtt_service_layer_exists_and_does_not_cross_boundaries(self):
        self.assertIn('"mqtt_client/mqtt_client.c"', self.src_cmake)
        self.assertIn("PRIV_INCLUDE_DIRS lwlte core mqtt_client modem at_engine", self.src_cmake)
        self.assertIn("typedef struct mqtt_client mqtt_client_t;", self.mqtt_h)
        self.assertIn("mqtt_client_create", self.mqtt_h)
        self.assertIn("core_submit_cmd", self.mqtt_c)
        self.assertIn("esp_event_handler_register_with", self.mqtt_c)
        self.assertIn("MQTT_SIG_CORE_CMD_DONE", self.mqtt_priv)
        self.assertIn("MQTT_SIG_PROTOCOL_DATA", self.mqtt_priv)

        forbidden_includes = [
            '#include "modem.h"',
            '#include "modem_air780ep.h"',
            '#include "at_engine.h"',
            '#include "core_priv.h"',
        ]
        for include in forbidden_includes:
            self.assertNotIn(include, self.mqtt_c)
            self.assertNotIn(include, self.mqtt_h)
            self.assertNotIn(include, self.mqtt_priv)

    def test_core_command_queue_contract_exists(self):
        for token in [
            "CORE_EVENT_PROTOCOL_DATA",
            "CORE_EVENT_PROTOCOL_CLOSED",
            "CORE_PROTOCOL_MQTT",
            "core_protocol_data_t",
            "CORE_CMD_MQTT_CONFIG",
            "CORE_CMD_MQTT_OPEN",
            "CORE_CMD_MQTT_LOGIN",
            "CORE_CMD_MQTT_DISCONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "core_cmd_t",
            "core_submit_cmd(core_t *me, const core_cmd_t *cmd);",
        ]:
            self.assertIn(token, self.core_h)

        self.assertIn("CORE_SIG_SERVICE_CMD", self.core_priv)
        self.assertIn("core_cmd_t *service_cmd;", self.core_priv)
        self.assertIn("static core_cmd_t *clone_core_cmd", self.core_c)
        self.assertIn("static void free_core_cmd", self.core_c)
        self.assertIn("esp_err_t core_submit_cmd", self.core_c)
        self.assertIn("handle_service_cmd", self.core_fsm_c)
        self.assertIn("modem_mqtt_config", self.core_fsm_c)
        self.assertIn("modem_mqtt_publish", self.core_fsm_c)

    def test_modem_mqtt_ops_and_air780ep_commands_exist(self):
        for token in [
            "modem_mqtt_config_t",
            "modem_mqtt_open_t",
            "modem_mqtt_login_t",
            "modem_mqtt_topic_t",
            "modem_mqtt_publish_t",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "MODEM_PROTOCOL_MQTT",
            "modem_mqtt_config(modem_t *me",
            "modem_mqtt_publish(modem_t *me",
        ]:
            self.assertIn(token, self.modem_h)

        for token in [
            "mqtt_config",
            "mqtt_open",
            "mqtt_login",
            "mqtt_disconnect",
            "mqtt_subscribe",
            "mqtt_unsubscribe",
            "mqtt_publish",
        ]:
            self.assertIn(token, self.modem_priv)

        for token in [
            "esp_err_t modem_mqtt_config",
            "esp_err_t modem_mqtt_publish",
            "release_event_payload",
        ]:
            self.assertIn(token, self.modem_c)

        for token in [
            "AIR780EP_URC_MSUB",
            "AT+MCONFIG",
            "AT+MIPSTART",
            "AT+MCONNECT",
            "AT+MDISCONNECT",
            "AT+MSUB",
            "AT+MUNSUB",
            "AT+MPUBEX",
            "air780ep_mqtt_config",
            "air780ep_mqtt_publish",
            "handle_msub_urc",
        ]:
            self.assertIn(token, self.air780ep_c)

    def test_at_engine_payload_prompt_support_exists(self):
        self.assertIn("at_engine_send_cmd_with_payload", self.at_engine_h)
        self.assertIn("const uint8_t *payload", self.at_engine_h)
        self.assertIn("payload_prompt", self.at_engine_c)
        self.assertIn("write_payload", self.at_engine_c)
        self.assertIn("uart_write_bytes", self.at_engine_c)

    def test_protocol_data_path_symbols_exist(self):
        self.assertIn("MODEM_EVENT_PROTOCOL_DATA", self.air780ep_c)
        self.assertIn("clone_protocol_data", self.core_c)
        self.assertIn("CORE_EVENT_PROTOCOL_DATA", self.core_fsm_c)
        self.assertIn("handle_core_event", self.mqtt_c)
        self.assertIn("MQTT_CLIENT_EVENT_DATA", self.mqtt_c)
        self.assertIn("LWLTE_EVENT_MQTT_DATA", self.lwlte_c)
        self.assertIn("lwlte_handle_mqtt_event", self.lwlte_priv)
        self.assertIn("mqtt_client_register_event_callback", self.lwlte_air780ep_c)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the new contract test and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract -v`

Expected: FAIL with an assertion for a missing MQTT contract token. The test harness uses `read_optional()` so planned future files read as empty text instead of aborting `setUpClass()`.

- [ ] **Step 3: Commit the failing contract test**

Run:

```bash
git add tests/host/test_mqtt_end_to_end_contract.py
git commit -m "test: add mqtt end-to-end contract checks"
```

Expected: commit succeeds and includes only `tests/host/test_mqtt_end_to_end_contract.py`.

---

### Task 2: Add AT Engine Prompt Payload Support

**Files:**
- Modify: `src/at_engine/at_engine.h`
- Modify: `src/at_engine/at_engine.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the payload-specific regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_at_engine_payload_prompt_support_exists -v`

Expected: FAIL with missing `at_engine_send_cmd_with_payload`.

- [ ] **Step 2: Add the public AT Engine payload API**

In `src/at_engine/at_engine.h`, after `at_engine_send_cmd_with_options()`, add this prototype and Doxygen block:

```c
/**
 * @brief 发送带 payload prompt 的 AT 命令
 * @details Send AT command that waits for a payload prompt and then writes raw payload
 * @param[in] me AT 引擎句柄
 * @param[in] cmd AT 命令，不要求包含 CRLF
 * @param[in] payload 待写入的原始 payload
 * @param[in] payload_len payload 字节数
 * @param[in] payload_prompt payload 输入提示符，如 ">"
 * @param[out] response 响应对象
 * @param[in] options payload 写入后继续等待最终响应的命令选项
 * @return
 *         - ESP_OK: 命令流程完成，AT 业务结果见 response->status
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待 prompt 或最终响应超时
 *         - ESP_FAIL: UART 写入失败
 */
esp_err_t at_engine_send_cmd_with_payload(at_engine_t *me, const char *cmd,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          const char *payload_prompt,
                                          at_response_t *response,
                                          const at_cmd_options_t *options);
```

Also add `#include <stddef.h>` near the existing includes because the prototype uses `size_t`.

- [ ] **Step 3: Extend the AT command context**

In `src/at_engine/at_engine.c`, extend `at_cmd_ctx_t` with these fields:

```c
const uint8_t *payload;
size_t payload_len;
const char *payload_prompt;
bool payload_sent;
```

Add these static prototypes near `write_cmd()`:

```c
static esp_err_t send_cmd_internal(at_engine_t *me, const char *cmd,
                                   const uint8_t *payload, size_t payload_len,
                                   const char *payload_prompt,
                                   at_response_t *response,
                                   const at_cmd_options_t *options);
static esp_err_t write_payload(at_engine_t *me, const uint8_t *payload,
                               size_t payload_len);
static bool is_payload_prompt(const at_cmd_ctx_t *ctx, const char *line);
```

- [ ] **Step 4: Route normal and payload commands through one internal implementation**

Replace the body of `at_engine_send_cmd_with_options()` with:

```c
return send_cmd_internal(me, cmd, NULL, 0, NULL, response, options);
```

Add this new public function after `at_engine_send_cmd_with_options()`:

```c
esp_err_t at_engine_send_cmd_with_payload(at_engine_t *me, const char *cmd,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          const char *payload_prompt,
                                          at_response_t *response,
                                          const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(payload && payload_len > 0 && payload_prompt &&
                        payload_prompt[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "invalid payload arguments");

    return send_cmd_internal(me, cmd, payload, payload_len, payload_prompt,
                             response, options);
}
```

- [ ] **Step 5: Implement `send_cmd_internal()` by moving the existing send logic**

Move the current implementation body of `at_engine_send_cmd_with_options()` into a new static `send_cmd_internal()` and initialize the new context fields like this:

```c
*ctx = (at_cmd_ctx_t) {
    .cmd = cmd,
    .timeout_ms = wait_ms,
    .response = response,
    .options = *options,
    .echo_consumed = 0,
    .data_line_index = 0,
    .result_received = false,
    .payload = payload,
    .payload_len = payload_len,
    .payload_prompt = payload_prompt,
    .payload_sent = false,
};
```

Keep the same mutex, timeout, response reset, command write, final response wait, timeout cleanup, and `cmd_mutex` release behavior from the current function.

- [ ] **Step 6: Handle the prompt line without finishing the command**

In `handle_line()`, after error parsing and before standard `OK` handling, add:

```c
if (is_payload_prompt(ctx, line)) {
    esp_err_t payload_ret = write_payload(me, ctx->payload, ctx->payload_len);
    if (payload_ret != ESP_OK) {
        finish_cmd_locked(me, AT_RESP_ERROR, 0);
    } else {
        ctx->payload_sent = true;
    }
    xSemaphoreGive(me->lock);
    return;
}
```

Implement the helpers at the end of the static function section:

```c
static bool is_payload_prompt(const at_cmd_ctx_t *ctx, const char *line)
{
    return ctx && ctx->payload && !ctx->payload_sent && ctx->payload_prompt &&
           strcmp(line, ctx->payload_prompt) == 0;
}

static esp_err_t write_payload(at_engine_t *me, const uint8_t *payload,
                               size_t payload_len)
{
    ESP_RETURN_ON_FALSE(me && payload && payload_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid payload");
#ifdef CONFIG_LWLTE_AT_ENGINE_LOG_IO
    log_uart_line("TX_PAYLOAD", (const char *)payload, payload_len);
#endif
    int written = uart_write_bytes(me->uart_num, payload, payload_len);
    ESP_RETURN_ON_FALSE(written == (int)payload_len, ESP_FAIL, TAG,
                        "uart_write_bytes payload failed");
    return ESP_OK;
}
```

- [ ] **Step 7: Run the payload-specific regression and verify it passes**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_at_engine_payload_prompt_support_exists -v`

Expected: PASS.

- [ ] **Step 8: Commit AT Engine payload support**

Run:

```bash
git add src/at_engine/at_engine.h src/at_engine/at_engine.c
git commit -m "feat(at_engine): support prompt payload commands"
```

Expected: commit succeeds and contains only AT Engine changes.

---

### Task 3: Add Modem MQTT Ops And Protocol Event Ownership

**Files:**
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_priv.h`
- Modify: `src/modem/modem.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the Modem contract regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_modem_mqtt_ops_and_air780ep_commands_exist -v`

Expected: FAIL with missing `modem_mqtt_config_t` or `MODEM_EVENT_PROTOCOL_DATA`.

- [ ] **Step 2: Add MQTT and protocol types to `modem.h`**

In `src/modem/modem.h`, add `#include <stddef.h>` and define these types after `modem_pdp_context_t`:

```c
typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
} modem_mqtt_config_t;

typedef struct {
    const char *host;
    uint16_t port;
} modem_mqtt_open_t;

typedef struct {
    bool clean_session;
    uint16_t keepalive_s;
} modem_mqtt_login_t;

typedef struct {
    const char *topic;
    uint8_t qos;
} modem_mqtt_topic_t;

typedef struct {
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} modem_mqtt_publish_t;

typedef enum {
    MODEM_PROTOCOL_MQTT = 0,
} modem_protocol_t;

typedef struct {
    modem_protocol_t protocol;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} modem_protocol_data_t;
```

Extend `modem_event_id_t` by appending after `MODEM_EVENT_ERROR` to keep existing event ID values stable:

```c
MODEM_EVENT_PROTOCOL_DATA,      /**< 协议数据事件； Protocol data event */
MODEM_EVENT_PROTOCOL_CLOSED,    /**< 协议连接关闭； Protocol connection closed */
```

Extend `modem_event_t.data` with:

```c
modem_protocol_data_t protocol_data;     /**< 协议数据； Protocol data */
```

Add wrapper prototypes before the macros section:

```c
esp_err_t modem_mqtt_config(modem_t *me,
                            const modem_mqtt_config_t *config);
esp_err_t modem_mqtt_open(modem_t *me,
                          const modem_mqtt_open_t *open);
esp_err_t modem_mqtt_login(modem_t *me,
                           const modem_mqtt_login_t *login);
esp_err_t modem_mqtt_disconnect(modem_t *me);
esp_err_t modem_mqtt_subscribe(modem_t *me,
                               const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_unsubscribe(modem_t *me,
                                 const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_publish(modem_t *me,
                             const modem_mqtt_publish_t *publish);
```

- [ ] **Step 3: Add MQTT ops to `modem_ops_t`**

In `src/modem/modem_priv.h`, add these members at the end of `modem_ops_t`:

```c
esp_err_t (*mqtt_config)(modem_t *me,
                         const modem_mqtt_config_t *config);
esp_err_t (*mqtt_open)(modem_t *me,
                       const modem_mqtt_open_t *open);
esp_err_t (*mqtt_login)(modem_t *me,
                        const modem_mqtt_login_t *login);
esp_err_t (*mqtt_disconnect)(modem_t *me);
esp_err_t (*mqtt_subscribe)(modem_t *me,
                            const modem_mqtt_topic_t *topic);
esp_err_t (*mqtt_unsubscribe)(modem_t *me,
                              const modem_mqtt_topic_t *topic);
esp_err_t (*mqtt_publish)(modem_t *me,
                          const modem_mqtt_publish_t *publish);
```

- [ ] **Step 4: Add event payload release helper**

In `src/modem/modem.c`, add this static prototype near `call_no_arg()`:

```c
static void release_event_payload(modem_event_t *event);
```

In `event_task()`, after invoking `callback(me, &event, user_ctx);`, call:

```c
release_event_payload(&event);
```

If `modem_post_event()` fails to enqueue a `MODEM_EVENT_PROTOCOL_DATA` event, it must not take ownership. The caller remains responsible for freeing allocations on failure.

`modem_post_event()` must reject events after the event task is stopping or stopped. When the event task exits, and before `modem_base_deinit()` deletes `event_queue`, drain any queued `MODEM_EVENT_PROTOCOL_DATA` entries with `release_event_payload()` so heap-owned protocol payloads are not orphaned.

Add this helper in the static functions section:

```c
static void release_event_payload(modem_event_t *event)
{
    if (!event || event->id != MODEM_EVENT_PROTOCOL_DATA) {
        return;
    }

    free((void *)event->data.protocol_data.topic);
    free((void *)event->data.protocol_data.payload);
    event->data.protocol_data.topic = NULL;
    event->data.protocol_data.payload = NULL;
    event->data.protocol_data.topic_len = 0;
    event->data.protocol_data.payload_len = 0;
}
```

- [ ] **Step 5: Implement MQTT wrapper functions**

In `src/modem/modem.c`, after `modem_get_pdp_context()`, add wrappers following the existing `check_ready(me, false)` pattern:

```c
esp_err_t modem_mqtt_config(modem_t *me,
                            const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_config,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_config not supported");
    return me->ops->mqtt_config(me, config);
}
```

Add equivalent wrappers for `modem_mqtt_open()`, `modem_mqtt_login()`, `modem_mqtt_disconnect()`, `modem_mqtt_subscribe()`, `modem_mqtt_unsubscribe()`, and `modem_mqtt_publish()` using these validation rules:

```c
/* open */
me && open && open->host && open->port > 0

/* login */
me && login

/* disconnect */
me

/* subscribe */
me && topic && topic->topic && topic->qos <= 2

/* unsubscribe */
me && topic && topic->topic

/* publish */
me && publish && publish->topic && publish->payload && publish->payload_len > 0 && publish->qos <= 2
```

- [ ] **Step 6: Run the Modem contract regression**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_modem_mqtt_ops_and_air780ep_commands_exist -v`

Expected: FAIL moves forward to missing Air780EP-specific tokens such as `AT+MCONFIG`. This confirms the Modem layer tokens are present.

- [ ] **Step 7: Commit Modem public ops**

Run:

```bash
git add src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c
git commit -m "feat(modem): add mqtt operation surface"
```

Expected: commit succeeds and contains only generic Modem layer changes.

---

### Task 4: Add Core Command Queue And Protocol Events

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core_priv.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the Core command queue regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_core_command_queue_contract_exists -v`

Expected: FAIL with missing `core_cmd_t` or `core_submit_cmd`.

- [ ] **Step 2: Add Core protocol event and command types to `core.h`**

Add `#include <stddef.h>` to `src/core/core.h`.

Extend `core_event_id_t` by appending after `CORE_EVENT_ERROR` so existing event values stay stable (`CORE_EVENT_STOPPED == 6`, `CORE_EVENT_ERROR == 7`):

```c
CORE_EVENT_PROTOCOL_DATA,            /**< 协议数据； Protocol data */
CORE_EVENT_PROTOCOL_CLOSED,          /**< 协议关闭； Protocol closed */
```

Add these types after `core_event_id_t`:

```c
typedef enum {
    CORE_PROTOCOL_MQTT = 0,
} core_protocol_t;

typedef struct {
    core_protocol_t protocol;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} core_protocol_data_t;

typedef enum {
    CORE_CMD_MQTT_CONFIG = 0,
    CORE_CMD_MQTT_OPEN,
    CORE_CMD_MQTT_LOGIN,
    CORE_CMD_MQTT_DISCONNECT,
    CORE_CMD_MQTT_SUBSCRIBE,
    CORE_CMD_MQTT_UNSUBSCRIBE,
    CORE_CMD_MQTT_PUBLISH,
} core_cmd_type_t;

typedef enum {
    CORE_CMD_RESULT_OK = 0,
    CORE_CMD_RESULT_ERROR,
    CORE_CMD_RESULT_TIMEOUT,
    CORE_CMD_RESULT_INVALID_RESPONSE,
} core_cmd_result_t;

typedef void (*core_cmd_done_callback_t)(core_t *core,
                                         core_cmd_type_t type,
                                         core_cmd_result_t result,
                                         const void *result_data,
                                         void *user_ctx);

typedef struct {
    core_cmd_type_t type;
    core_cmd_done_callback_t done_cb;
    void *user_ctx;
    uint32_t timeout_ms;
    union {
        struct {
            const char *client_id;
            const char *username;
            const char *password;
        } mqtt_config;
        struct {
            const char *host;
            uint16_t port;
        } mqtt_open;
        struct {
            bool clean_session;
            uint16_t keepalive_s;
        } mqtt_login;
        struct {
            const char *topic;
            uint8_t qos;
        } mqtt_subscribe;
        struct {
            const char *topic;
        } mqtt_unsubscribe;
        struct {
            const char *topic;
            const uint8_t *payload;
            size_t payload_len;
            uint8_t qos;
            bool retain;
        } mqtt_publish;
    } data;
} core_cmd_t;
```

Extend `core_event_data_t` with:

```c
core_protocol_data_t protocol_data;   /**< 协议数据； Protocol data */
```

Add prototype after `core_disconnect()`:

```c
esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd);
```

- [ ] **Step 3: Add Core FSM service command signal**

In `src/core/core_priv.h`, add `CORE_SIG_SERVICE_CMD` after `CORE_SIG_NET_DEACTIVATE` and add this field to `core_fsm_sig_t`:

```c
core_cmd_t *service_cmd;
```

Also add these prototypes:

```c
void core_free_cmd(core_cmd_t *cmd);
esp_err_t core_post_protocol_data(core_t *me,
                                  const core_protocol_data_t *protocol_data);
```

- [ ] **Step 4: Implement command clone/free helpers in `core.c`**

Add static prototypes:

```c
static core_cmd_t *clone_core_cmd(const core_cmd_t *cmd);
static void free_core_cmd(core_cmd_t *cmd);
static char *clone_optional_string(const char *value);
static uint8_t *clone_payload(const uint8_t *payload, size_t payload_len);
static bool core_cmd_type_valid(core_cmd_type_t type);
static bool core_cmd_valid(const core_cmd_t *cmd);
static esp_err_t clone_protocol_data(core_event_data_t *event_data,
                                     const core_protocol_data_t *protocol_data);
static void release_core_event_payload(core_event_data_t *event_data);
```

Implement `clone_optional_string()` with `malloc(strlen(value) + 1)` and `memcpy()`. Return NULL for NULL input. Implement `clone_payload()` with `malloc(payload_len)` and `memcpy()`.

Implement `core_cmd_valid()` with exact rules:

```c
switch (cmd->type) {
case CORE_CMD_MQTT_CONFIG:
    return cmd->data.mqtt_config.client_id != NULL;
case CORE_CMD_MQTT_OPEN:
    return cmd->data.mqtt_open.host != NULL && cmd->data.mqtt_open.port > 0;
case CORE_CMD_MQTT_LOGIN:
    return true;
case CORE_CMD_MQTT_DISCONNECT:
    return true;
case CORE_CMD_MQTT_SUBSCRIBE:
    return cmd->data.mqtt_subscribe.topic != NULL && cmd->data.mqtt_subscribe.qos <= 2;
case CORE_CMD_MQTT_UNSUBSCRIBE:
    return cmd->data.mqtt_unsubscribe.topic != NULL;
case CORE_CMD_MQTT_PUBLISH:
    return cmd->data.mqtt_publish.topic != NULL &&
           cmd->data.mqtt_publish.payload != NULL &&
           cmd->data.mqtt_publish.payload_len > 0 &&
           cmd->data.mqtt_publish.qos <= 2;
default:
    return false;
}
```

Implement `clone_core_cmd()` so every pointer field used after enqueue is heap-owned by the clone. If any allocation fails, call `free_core_cmd(clone)` and return NULL.

Implement `core_free_cmd()` as a global wrapper that calls `free_core_cmd()`.

- [ ] **Step 5: Implement `core_submit_cmd()`**

In `src/core/core.c`, after `core_disconnect()`, add:

```c
esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd)
{
    ESP_RETURN_ON_FALSE(me && me->lock && cmd, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(core_cmd_valid(cmd), ESP_ERR_INVALID_ARG, TAG,
                        "invalid core command");
    ESP_RETURN_ON_FALSE(!core_is_destroying(me), ESP_ERR_INVALID_STATE, TAG,
                        "core is destroying");

    core_cmd_t *clone = clone_core_cmd(cmd);
    ESP_RETURN_ON_FALSE(clone, ESP_ERR_NO_MEM, TAG, "clone core command failed");

    core_fsm_sig_t sig = {
        .type = CORE_SIG_SERVICE_CMD,
        .service_cmd = clone,
    };

    esp_err_t ret = core_fsm_send(me, &sig);
    if (ret != ESP_OK) {
        free_core_cmd(clone);
        return ret;
    }

    return ESP_OK;
}
```

- [ ] **Step 6: Deep-copy Modem protocol data before queueing to Core FSM**

In `core_modem_event_cb()`, before constructing `sig`, detect `MODEM_EVENT_PROTOCOL_DATA`. Allocate copies of topic and payload into `sig.modem_event.data.protocol_data`. If allocation fails, log and drop the event. If `core_fsm_send()` fails, release the copied protocol payload before returning.

Use this release pattern in `core.c`:

```c
static void release_modem_protocol_payload(modem_event_t *event)
{
    if (!event || event->id != MODEM_EVENT_PROTOCOL_DATA) {
        return;
    }
    free((void *)event->data.protocol_data.topic);
    free((void *)event->data.protocol_data.payload);
    event->data.protocol_data.topic = NULL;
    event->data.protocol_data.payload = NULL;
}
```

- [ ] **Step 7: Execute service commands in `core_fsm.c`**

Add static prototypes:

```c
static void handle_service_cmd(core_t *me, core_cmd_t *cmd);
static core_cmd_result_t result_from_esp_err(esp_err_t err);
static void finish_service_cmd(core_t *me, core_cmd_t *cmd,
                               core_cmd_result_t result,
                               const void *result_data);
static void release_modem_protocol_payload(modem_event_t *event);
```

In `handle_signal()`, add:

```c
case CORE_SIG_SERVICE_CMD:
    handle_service_cmd(me, sig->service_cmd);
    break;
```

Implement `handle_service_cmd()` with a `switch (cmd->type)` that maps to these calls:

```c
CORE_CMD_MQTT_CONFIG      -> modem_mqtt_config(me->modem, &config)
CORE_CMD_MQTT_OPEN        -> modem_mqtt_open(me->modem, &open)
CORE_CMD_MQTT_LOGIN       -> modem_mqtt_login(me->modem, &login)
CORE_CMD_MQTT_DISCONNECT  -> modem_mqtt_disconnect(me->modem)
CORE_CMD_MQTT_SUBSCRIBE   -> modem_mqtt_subscribe(me->modem, &topic)
CORE_CMD_MQTT_UNSUBSCRIBE -> modem_mqtt_unsubscribe(me->modem, &topic)
CORE_CMD_MQTT_PUBLISH     -> modem_mqtt_publish(me->modem, &publish)
```

Convert `ESP_OK` to `CORE_CMD_RESULT_OK`, `ESP_ERR_TIMEOUT` to `CORE_CMD_RESULT_TIMEOUT`, `ESP_ERR_INVALID_RESPONSE` to `CORE_CMD_RESULT_INVALID_RESPONSE`, and all other errors to `CORE_CMD_RESULT_ERROR`.

Call `finish_service_cmd()` and then `core_free_cmd(cmd)` exactly once.

- [ ] **Step 8: Publish Core protocol events**

In `handle_modem_event()`, add cases for `MODEM_EVENT_PROTOCOL_DATA` and `MODEM_EVENT_PROTOCOL_CLOSED`:

```c
case MODEM_EVENT_PROTOCOL_DATA:
    (void)core_post_protocol_data(me, &event->data.protocol_data);
    break;
case MODEM_EVENT_PROTOCOL_CLOSED:
    post_event_checked(me, CORE_EVENT_PROTOCOL_CLOSED, NULL);
    break;
```

Implement `core_post_protocol_data()` in `core.c` by allocating topic/payload copies into a `core_event_data_t`, posting `CORE_EVENT_PROTOCOL_DATA`, and releasing the copies if `esp_event_post_to()` fails. Add `core_release_event_payload()` so the MQTT service can release successful protocol event payloads after copying during its ESP event callback. Document in a code comment that Core cannot release this payload in `core_event_adapter()` because ESP event data is shared across handlers.

- [ ] **Step 9: Run the Core command queue regression and verify it passes**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_core_command_queue_contract_exists -v`

Expected: PASS.

- [ ] **Step 10: Commit Core command queue support**

Run:

```bash
git add src/core/core.h src/core/core_priv.h src/core/core.c src/core/core_fsm.c
git commit -m "feat(core): add mqtt command queue"
```

Expected: commit succeeds and contains only Core changes.

---

### Task 5: Implement Air780EP MQTT Commands And URC Parsing

**Files:**
- Modify: `src/modem/modem_air780ep.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the Air780EP portion of the Modem regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_modem_mqtt_ops_and_air780ep_commands_exist -v`

Expected: FAIL with missing Air780EP tokens such as `AT+MCONFIG` or `handle_msub_urc`.

- [ ] **Step 2: Add MQTT constants and URC handler field**

In `src/modem/modem_air780ep.c`, add defines near existing URC constants:

```c
#define AIR780EP_URC_MSUB                 "+MSUB:"
#define AIR780EP_MQTT_CMD_TIMEOUT_MS      9000
#define AIR780EP_MQTT_CONNECT_TIMEOUT_MS  60000
#define AIR780EP_MQTT_PAYLOAD_PROMPT      ">"
```

Add this field to `modem_air780ep_t` with the other handler nodes:

```c
at_urc_handler_t msub_handler;
```

- [ ] **Step 3: Add Air780EP MQTT static prototypes**

Add prototypes for:

```c
static esp_err_t air780ep_mqtt_config(modem_t *me,
                                      const modem_mqtt_config_t *config);
static esp_err_t air780ep_mqtt_open(modem_t *me,
                                    const modem_mqtt_open_t *open);
static esp_err_t air780ep_mqtt_login(modem_t *me,
                                     const modem_mqtt_login_t *login);
static esp_err_t air780ep_mqtt_disconnect(modem_t *me);
static esp_err_t air780ep_mqtt_subscribe(modem_t *me,
                                         const modem_mqtt_topic_t *topic);
static esp_err_t air780ep_mqtt_unsubscribe(modem_t *me,
                                           const modem_mqtt_topic_t *topic);
static esp_err_t air780ep_mqtt_publish(modem_t *me,
                                       const modem_mqtt_publish_t *publish);
static void handle_msub_urc(const char *prefix, const char *line,
                            void *user_ctx);
static esp_err_t post_mqtt_data_event(modem_air780ep_t *self,
                                      const char *topic, size_t topic_len,
                                      const uint8_t *payload,
                                      size_t payload_len);
static char *escape_at_string(const char *value);
static bool parse_msub_direct(const char *line, char **out_topic,
                              uint8_t **out_payload, size_t *out_payload_len);
```

- [ ] **Step 4: Add MQTT ops to the Air780EP ops table**

In the `static const modem_ops_t air780ep_ops`, set:

```c
.mqtt_config = air780ep_mqtt_config,
.mqtt_open = air780ep_mqtt_open,
.mqtt_login = air780ep_mqtt_login,
.mqtt_disconnect = air780ep_mqtt_disconnect,
.mqtt_subscribe = air780ep_mqtt_subscribe,
.mqtt_unsubscribe = air780ep_mqtt_unsubscribe,
.mqtt_publish = air780ep_mqtt_publish,
```

- [ ] **Step 5: Register and unregister `+MSUB:` URC**

In the URC registration function, initialize and register:

```c
self->msub_handler.callback = handle_msub_urc;
self->msub_handler.user_ctx = self;
ret = at_engine_register_urc(self->base.at, AIR780EP_URC_MSUB,
                             &self->msub_handler);
```

In the URC unregister path, call:

```c
(void)at_engine_unregister_urc(self->base.at, AIR780EP_URC_MSUB);
```

- [ ] **Step 6: Implement AT string escaping**

Implement `escape_at_string()` so it returns a heap string with these replacements:

```text
"  -> \22
\  -> \5C
\r -> \0D
\n -> \0A
```

For all other bytes, copy the byte unchanged. Return NULL on allocation failure.

- [ ] **Step 7: Implement MQTT config/open/login/disconnect/subscribe/unsubscribe**

Use `snprintf()` into a heap buffer sized from escaped string lengths. Commands must use quoted escaped values:

```c
AT+MCONFIG="<client_id>","<username>","<password>"
AT+MIPSTART="<host>",<port>
AT+MCONNECT=<clean_session>,<keepalive_s>
AT+MDISCONNECT
AT+MSUB="<topic>",<qos>
AT+MUNSUB="<topic>"
```

Use these custom success options:

```c
/* MIPSTART */
static const at_cmd_success_match_t matches[] = {
    { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "CONNECT OK" },
    { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "ALREADY CONNECT" },
};

/* MCONNECT */
static const at_cmd_success_match_t matches[] = {
    { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "CONNACK OK" },
};

/* MSUB */
static const at_cmd_success_match_t matches[] = {
    { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "SUBACK" },
};

/* MUNSUB */
static const at_cmd_success_match_t matches[] = {
    { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "UNSUBACK" },
};
```

For `MIPSTART`, `MCONNECT`, `MSUB`, and `MUNSUB`, set flags:

```c
.flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK
```

Call `ensure_at_ok(&ctx.response, "<command-name>")` after each AT Engine call returns `ESP_OK`.

- [ ] **Step 8: Implement MQTT publish using payload prompt support**

For QoS > 1, return `ESP_ERR_NOT_SUPPORTED`.

For QoS 0, use final `OK` as success. For QoS 1, use custom success `PUBACK` with `AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK`.

Build command:

```c
AT+MPUBEX="<topic>",<qos>,<retain>,<payload_len>
```

Call:

```c
ret = at_engine_send_cmd_with_payload(self->base.at, cmd,
                                      publish->payload,
                                      publish->payload_len,
                                      AIR780EP_MQTT_PAYLOAD_PROMPT,
                                      &ctx.response,
                                      &options);
```

- [ ] **Step 9: Parse `+MSUB:` direct-mode URC**

Implement `parse_msub_direct()` for lines shaped like:

```text
+MSUB:<topic>,<len>,<message>
```

Rules:

- Accept quoted or unquoted topic.
- Parse `<len>` with `strtoul()`.
- Copy exactly `<len>` bytes from `<message>` into a heap payload buffer, plus no required NUL terminator.
- Copy topic into a heap NUL-terminated buffer.
- Return false for cache-mode `+MSUB:<store_addr>` because cache mode is not implemented.

Implement `handle_msub_urc()` so it calls `parse_msub_direct()`, then `post_mqtt_data_event()`. If posting fails, free the allocated topic and payload.

- [ ] **Step 10: Run Air780EP and protocol data static regressions**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_modem_mqtt_ops_and_air780ep_commands_exist -v
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_protocol_data_path_symbols_exist -v
```

Expected: first test PASS; second test may still fail on MQTT/Facade symbols until later tasks.

- [ ] **Step 11: Commit Air780EP MQTT ops**

Run:

```bash
git add src/modem/modem_air780ep.c
git commit -m "feat(air780ep): implement mqtt commands"
```

Expected: commit succeeds and contains only Air780EP changes.

---

### Task 6: Implement MQTT Client Service Module

**Files:**
- Create: `src/mqtt_client/mqtt_client.h`
- Create: `src/mqtt_client/mqtt_client_priv.h`
- Create: `src/mqtt_client/mqtt_client.c`
- Modify: `src/CMakeLists.txt`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the MQTT service boundary regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_mqtt_service_layer_exists_and_does_not_cross_boundaries -v`

Expected: FAIL because `src/mqtt_client/mqtt_client.h` does not exist.

- [ ] **Step 2: Update CMake**

In `src/CMakeLists.txt`, add source:

```cmake
         "mqtt_client/mqtt_client.c"
```

Place it after the Core sources and before `lwlte/lwlte.c`. Change `PRIV_INCLUDE_DIRS` to:

```cmake
    PRIV_INCLUDE_DIRS lwlte core mqtt_client modem at_engine
```

- [ ] **Step 3: Create `mqtt_client.h`**

Create `src/mqtt_client/mqtt_client.h` using the project header template. Include only:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "esp_err.h"
#include "esp_event.h"
```

Define the public layer types from the spec: `mqtt_client_t`, `mqtt_client_transport_t`, `mqtt_client_config_t`, `mqtt_client_state_t`, `MQTT_CLIENT_EVENT`, `mqtt_client_event_id_t`, `mqtt_client_operation_t`, `mqtt_client_publish_t`, `mqtt_client_msg_t`, `mqtt_client_event_data_t`, and `mqtt_client_event_callback_t`.

Declare these functions exactly:

```c
mqtt_client_t *mqtt_client_create(const mqtt_client_config_t *config,
                                  core_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_t *me);
esp_err_t mqtt_client_start(mqtt_client_t *me);
esp_err_t mqtt_client_stop(mqtt_client_t *me);
esp_err_t mqtt_client_register_event_callback(mqtt_client_t *me,
                                              mqtt_client_event_callback_t callback,
                                              void *user_ctx);
esp_event_loop_handle_t mqtt_client_get_event_loop(mqtt_client_t *me);
esp_err_t mqtt_client_get_state(mqtt_client_t *me,
                                mqtt_client_state_t *state);
esp_err_t mqtt_client_subscribe(mqtt_client_t *me,
                                const char *topic,
                                uint8_t qos);
esp_err_t mqtt_client_unsubscribe(mqtt_client_t *me,
                                  const char *topic);
esp_err_t mqtt_client_publish(mqtt_client_t *me,
                              const mqtt_client_publish_t *request);
```

- [ ] **Step 4: Create `mqtt_client_priv.h`**

Create `src/mqtt_client/mqtt_client_priv.h`. Include `mqtt_client.h`, FreeRTOS queue/semaphore/task headers, and no Modem or AT Engine headers.

Define defaults:

```c
#define MQTT_CLIENT_DEFAULT_KEEPALIVE_S       300
#define MQTT_CLIENT_DEFAULT_FSM_QUEUE_SIZE    16
#define MQTT_CLIENT_DEFAULT_FSM_TASK_STACK    4096
#define MQTT_CLIENT_DEFAULT_FSM_PRIORITY      8
#define MQTT_CLIENT_FSM_WAIT_MS               100
#define MQTT_CLIENT_CMD_TIMEOUT_MS            60000
```

Define `mqtt_fsm_sig_type_t`, `mqtt_connect_step_t`, `mqtt_pending_cmd_t`, `mqtt_protocol_data_owned_t`, `mqtt_fsm_sig_t`, and `struct mqtt_client` with the fields described in `docs/agents/classes.md` plus callback tracking fields:

```c
int event_callback_active;
TaskHandle_t event_callback_task;
SemaphoreHandle_t event_callback_done_sema;
bool event_callback_waiting;
```

- [ ] **Step 5: Create `mqtt_client.c` skeleton and lifecycle**

Implement:

- `ESP_EVENT_DEFINE_BASE(MQTT_CLIENT_EVENT);`
- config validation and normalization that copies `host`, `client_id`, `username`, `password`.
- `mqtt_client_create()` with lock, queue, task done semaphore, callback done semaphore, event loop creation, Core event handler registration, and FSM task creation.
- `mqtt_client_destroy()` that unregisters Core event handler, stops FSM task, waits callbacks idle, deletes event loop/resources, frees config strings, drains queued signal payloads, then frees `me`.
- `mqtt_client_register_event_callback()`, `mqtt_client_get_event_loop()`, and `mqtt_client_get_state()`.

Use the same lifecycle style as `core.c`: do not destroy from the FSM task or MQTT event callback task.

- [ ] **Step 6: Implement MQTT API enqueue helpers**

Implement API methods with exact rules:

```c
mqtt_client_start: valid in STOPPED, WAITING_NET, ERROR; enqueue MQTT_SIG_START.
mqtt_client_stop: valid unless DESTROYING; enqueue MQTT_SIG_STOP.
mqtt_client_subscribe: require state CONNECTED, topic non-empty, qos <= 2; deep-copy topic into signal.
mqtt_client_unsubscribe: require state CONNECTED, topic non-empty; deep-copy topic into signal.
mqtt_client_publish: require state CONNECTED, topic non-empty, payload non-NULL, payload_len > 0, qos <= 2; deep-copy topic and payload into signal.
```

If queue send fails, free the copied signal payload and return `ESP_ERR_TIMEOUT`.

- [ ] **Step 7: Implement Core event handler**

Register with:

```c
esp_event_handler_register_with(core_get_event_loop(core), CORE_EVENT,
                                ESP_EVENT_ANY_ID, handle_core_event, me);
```

`handle_core_event()` must convert:

```text
CORE_EVENT_NET_ONLINE       -> MQTT_SIG_NET_ONLINE
CORE_EVENT_NET_OFFLINE      -> MQTT_SIG_NET_OFFLINE
CORE_EVENT_PROTOCOL_CLOSED  -> MQTT_SIG_PROTOCOL_CLOSED
CORE_EVENT_PROTOCOL_DATA    -> MQTT_SIG_PROTOCOL_DATA after deep-copy topic/payload
```

Ignore protocol events where `data->protocol_data.protocol != CORE_PROTOCOL_MQTT`.

- [ ] **Step 8: Implement Core command done callback and command submission**

Implement `mqtt_core_cmd_done_cb()`:

```c
static void mqtt_core_cmd_done_cb(core_t *core, core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data,
                                  void *user_ctx)
{
    (void)core;
    (void)result_data;
    mqtt_client_t *me = (mqtt_client_t *)user_ctx;
    mqtt_fsm_sig_t sig = {
        .type = MQTT_SIG_CORE_CMD_DONE,
        .core_cmd_type = type,
        .core_result = result,
    };
    (void)mqtt_fsm_send(me, &sig);
}
```

Implement `submit_core_cmd()` that fills `done_cb = mqtt_core_cmd_done_cb`, `user_ctx = me`, and `timeout_ms = MQTT_CLIENT_CMD_TIMEOUT_MS`, then calls `core_submit_cmd()`.

- [ ] **Step 9: Implement FSM task and connection flow**

FSM behavior:

```text
MQTT_SIG_START:
  core_get_net_state() == ONLINE -> set CONNECTING, post CONNECTING, submit CONFIG
  otherwise -> set WAITING_NET, post STARTED

MQTT_SIG_NET_ONLINE:
  state WAITING_NET -> set CONNECTING, post CONNECTING, submit CONFIG

MQTT_SIG_CORE_CMD_DONE during connect:
  CONFIG OK -> submit OPEN
  OPEN OK   -> submit LOGIN
  LOGIN OK  -> set CONNECTED, post CONNECTED
  failure   -> set ERROR, post ERROR
```

Runtime operations:

```text
MQTT_SIG_SUBSCRIBE      -> submit CORE_CMD_MQTT_SUBSCRIBE
MQTT_SIG_UNSUBSCRIBE    -> submit CORE_CMD_MQTT_UNSUBSCRIBE
MQTT_SIG_PUBLISH        -> submit CORE_CMD_MQTT_PUBLISH
MQTT_SIG_CORE_CMD_DONE  -> post SUBSCRIBED, UNSUBSCRIBED, or PUBLISHED based on pending_cmd.operation
MQTT_SIG_PROTOCOL_DATA  -> post MQTT_CLIENT_EVENT_DATA
MQTT_SIG_NET_OFFLINE    -> set WAITING_NET and post DISCONNECTED
MQTT_SIG_PROTOCOL_CLOSED -> set WAITING_NET and post DISCONNECTED
MQTT_SIG_STOP           -> submit DISCONNECT when needed, then set STOPPED and post STOPPED
```

Only one `pending_cmd.active` may be true. If another operation signal arrives while pending, post `MQTT_CLIENT_EVENT_ERROR` with `ESP_ERR_INVALID_STATE` and free the signal payload.

- [ ] **Step 10: Implement MQTT event dispatch**

Implement `post_mqtt_event()` so it posts non-DATA events to `me->event_loop` and invokes `me->event_callback` with callback-active tracking. For `MQTT_CLIENT_EVENT_DATA`, dispatch only through the direct callback before freeing signal-owned topic/payload. Event data pointers are valid only during callback.

- [ ] **Step 11: Run MQTT service boundary regression**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_mqtt_service_layer_exists_and_does_not_cross_boundaries -v`

Expected: PASS.

- [ ] **Step 12: Commit MQTT service module**

Run:

```bash
git add src/mqtt_client/mqtt_client.h src/mqtt_client/mqtt_client_priv.h src/mqtt_client/mqtt_client.c src/CMakeLists.txt
git commit -m "feat(mqtt): add client service"
```

Expected: commit succeeds and contains only MQTT service and build registration changes.

---

### Task 7: Add Public Facade MQTT API And Air780EP Wiring

**Files:**
- Modify: `src/include/lwlte.h`
- Modify: `src/include/lwlte_air780ep.h`
- Modify: `src/lwlte/lwlte_priv.h`
- Modify: `src/lwlte/lwlte.c`
- Modify: `src/lwlte/lwlte_air780ep.c`
- Test: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Run the public API regression and verify it fails**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_public_api_and_air780ep_mqtt_config_exist -v`

Expected: FAIL with missing `lwlte_mqtt_state_t` or `lwlte_air780ep_config_mqtt_client_t`.

- [ ] **Step 2: Add public MQTT types and functions to `lwlte.h`**

Add includes:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
```

Add `lwlte_mqtt_state_t` and `lwlte_mqtt_msg_t` from the spec.

Extend `lwlte_event_id_t` with MQTT events after `LWLTE_EVENT_ERROR`.

Extend `lwlte_event_data_t` with:

```c
lwlte_mqtt_state_t mqtt_state;     /**< MQTT 状态； MQTT state */
union {
    lwlte_mqtt_msg_t mqtt_msg;     /**< MQTT 消息； MQTT message */
} data;                            /**< 扩展事件数据； Extended event data */
```

Add public prototypes exactly as listed in the spec.

- [ ] **Step 3: Add nested MQTT config to `lwlte_air780ep.h`**

Before `lwlte_air780ep_config_t`, add:

```c
typedef struct {
    bool enabled;
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keepalive_s;
    bool clean_session;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
} lwlte_air780ep_config_mqtt_client_t;
```

Add field at the end of `lwlte_air780ep_config_t`:

```c
lwlte_air780ep_config_mqtt_client_t mqtt_client;
```

Document that `mqtt_client.enabled == false` disables the MQTT service and that `host`, `port`, and `client_id` are required when enabled.

- [ ] **Step 4: Extend `lwlte_priv.h`**

Include `mqtt_client.h` and add to `struct lwlte`:

```c
mqtt_client_t *mqtt;
```

Add prototype:

```c
void lwlte_handle_mqtt_event(mqtt_client_t *mqtt,
                             mqtt_client_event_id_t event_id,
                             const mqtt_client_event_data_t *data,
                             void *user_ctx);
```

- [ ] **Step 5: Implement Facade MQTT wrappers in `lwlte.c`**

Add helper:

```c
static esp_err_t begin_mqtt_api_call(lwlte_t *me, mqtt_client_t **out_mqtt);
```

It must call `begin_api_call(me, false, NULL)`, then read `me->mqtt` under lock. If `me->mqtt == NULL`, call `end_api_call(me)` and return `ESP_ERR_INVALID_STATE`.

Implement public wrappers:

```c
lwlte_mqtt_start       -> mqtt_client_start(mqtt)
lwlte_mqtt_stop        -> mqtt_client_stop(mqtt)
lwlte_mqtt_get_state   -> mqtt_client_get_state(mqtt, &mqtt_state), then map state
lwlte_mqtt_subscribe   -> mqtt_client_subscribe(mqtt, topic, qos)
lwlte_mqtt_unsubscribe -> mqtt_client_unsubscribe(mqtt, topic)
lwlte_mqtt_publish     -> build mqtt_client_publish_t and call mqtt_client_publish(mqtt, &request)
```

Map internal state to public state with a `map_mqtt_state()` helper.

- [ ] **Step 6: Implement MQTT event bridge in `lwlte.c`**

Implement `lwlte_handle_mqtt_event()` so it maps MQTT events to `LWLTE_EVENT_MQTT_*`, fills `lwlte_event_data_t.mqtt_state`, `error_code`, and `data.mqtt_msg` for data events, then uses the existing user callback tracking pattern.

For `MQTT_CLIENT_EVENT_DATA`, pointers remain callback-scoped and are not copied by Facade.

- [ ] **Step 7: Validate nested MQTT config in `lwlte_air780ep.c`**

In `validate_config()`, add checks:

```c
if (config->mqtt_client.enabled) {
    ESP_RETURN_ON_FALSE(config->mqtt_client.host && config->mqtt_client.host[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "mqtt host required");
    ESP_RETURN_ON_FALSE(config->mqtt_client.port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "mqtt port required");
    ESP_RETURN_ON_FALSE(config->mqtt_client.client_id &&
                        config->mqtt_client.client_id[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "mqtt client_id required");
    ESP_RETURN_ON_FALSE(non_negative_int(config->mqtt_client.fsm_queue_size) &&
                        non_negative_int(config->mqtt_client.fsm_task_stack) &&
                        non_negative_int(config->mqtt_client.fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "mqtt fsm fields must be non-negative");
}
```

- [ ] **Step 8: Create and register MQTT service in Air780EP factory**

In `lwlte_air780ep_init()`, after `core_register_event_callback()` and before `core_start()`, add:

```c
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
        ESP_LOGE(TAG, "create mqtt client failed");
        return cleanup_after_failure(me, ESP_OK);
    }
    ret = mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register mqtt event bridge failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }
}
```

- [ ] **Step 9: Destroy MQTT before Core**

In `destroy_owned_resources()`, before `core_destroy(me->core)`, add:

```c
if (me->mqtt) {
    esp_err_t mqtt_ret = mqtt_client_destroy(me->mqtt);
    if (mqtt_ret != ESP_OK) {
        return mqtt_ret;
    }
    me->mqtt = NULL;
}
```

- [ ] **Step 10: Run Facade and protocol path regressions**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_public_api_and_air780ep_mqtt_config_exist -v
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_protocol_data_path_symbols_exist -v
```

Expected: both PASS.

- [ ] **Step 11: Commit Facade MQTT API and wiring**

Run:

```bash
git add src/include/lwlte.h src/include/lwlte_air780ep.h src/lwlte/lwlte_priv.h src/lwlte/lwlte.c src/lwlte/lwlte_air780ep.c
git commit -m "feat(lwlte): expose mqtt facade api"
```

Expected: commit succeeds and contains only Facade and public header changes.

---

### Task 8: Final Verification And Documentation Alignment

**Files:**
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/architecture.md`
- Test: all host tests and ESP-IDF build

- [ ] **Step 1: Run the full MQTT contract test**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract -v`

Expected: all tests PASS.

- [ ] **Step 2: Run all host regression tests**

Run: `python3 -m unittest discover -s tests/host -p 'test_*.py' -v`

Expected: all tests PASS, including CPIN policy, net_mgr activation flow, and MQTT contract checks.

- [ ] **Step 3: Run ESP-IDF build through MCP**

Run: `esp-idf-eim_build_project`

Expected: build succeeds with exit code 0. If MCP build is unavailable, run: `source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build`.

- [ ] **Step 4: Check layer boundary includes**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_mqtt_service_layer_exists_and_does_not_cross_boundaries -v`

Expected: PASS and no forbidden include appears in MQTT service files.

- [ ] **Step 5: Check docs are aligned with implementation names**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_public_api_and_air780ep_mqtt_config_exist -v`

Expected: PASS. If implementation chose different names than docs, update docs to match implementation and rerun this test.

- [ ] **Step 6: Commit final documentation alignment after checking diff**

Run: `git diff -- docs/agents/classes.md docs/agents/architecture.md`

Expected: no output when documented names already match implementation. If the command shows doc alignment changes, run:

```bash
git add docs/agents/classes.md docs/agents/architecture.md
git commit -m "docs: align mqtt implementation docs"
```

Expected: commit succeeds when doc files changed. When `git diff -- docs/agents/classes.md docs/agents/architecture.md` has no output, do not create a documentation alignment commit.

- [ ] **Step 7: Review final git status and latest commits**

Run:

```bash
git status --short
git log --oneline -8
```

Expected: `git status --short` is empty. Latest commits include the MQTT test, AT Engine payload support, Modem MQTT ops, Core command queue, Air780EP MQTT commands, MQTT service, and Facade API commits.

---

## Self-Review Checklist

- Spec section 1 is covered by Tasks 2 through 7.
- Public API and nested Air780EP config are covered by Task 7.
- MQTT service boundary and Core-only dependency are covered by Task 6 and the forbidden include test.
- Core command queue is covered by Task 4.
- Modem MQTT ops and Air780EP AT command mapping are covered by Tasks 3 and 5.
- `+MSUB:` protocol data path is covered by Tasks 4, 5, 6, and 7.
- Error handling is encoded in validation rules in Tasks 3, 4, 6, and 7.
- Verification is covered by Task 8.
