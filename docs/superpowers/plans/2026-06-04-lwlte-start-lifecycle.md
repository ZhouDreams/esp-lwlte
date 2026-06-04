# lwlte Start Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Separate facade object creation from runtime startup so `lwlte_air780ep_init()` only constructs objects and `lwlte_start()` asynchronously starts the module and reaches PDP/IP online through Core.

**Architecture:** Facade owns dependency construction and exposes the new public `lwlte_start()` API. `lwlte_start()` delegates to `core_start()` only; Core FSM handles `CORE_SIG_START` by calling `modem_start()`, posting ready, and starting the existing network activation flow. `modem_start()` keeps its current module bring-up meaning: reset/wait RDY/basic AT ready, not PDP/IP online.

**Tech Stack:** C, ESP-IDF, FreeRTOS, ESP Event, Python `unittest` host contract tests, existing repository docs under `docs/agents/`.

---

## File Structure

- Create: `tests/host/test_lwlte_start_lifecycle.py` - static contract tests for public API, factory no-start behavior, Core-owned startup sequencing, examples, and docs.
- Modify: `src/include/lwlte.h` - add public `lwlte_start()`, remove public `lwlte_connect()`, remove `auto_connect`, update lifecycle comments.
- Modify: `src/lwlte/lwlte.c` - implement `lwlte_start()` as a thin Core delegation and remove `lwlte_connect()`.
- Modify: `src/lwlte/lwlte_air780ep.c` - make init only construct objects and register bridges; remove `modem_start()`, `core_start()`, `lwlte_wait_ready()`, and `auto_connect` handling.
- Modify: `src/core/core.h` - remove `core_config_t.auto_connect` and update `core_start()` comments to describe the full async startup request.
- Modify: `src/core/core_fsm.c` - make `handle_start()` call `modem_start()`, post ready through `handle_ready()`, then start network activation.
- Modify: `docs/agents/architecture.md` - update lifecycle and startup sequence documentation.
- Modify: `docs/agents/classes.md` - update public API, config, Core start, Modem start, and event-flow documentation.
- Modify: `docs/agents/oop-design.md` - update lifecycle examples that still show `auto_connect` or startup inside init.
- Modify: `examples/basic_connect/main/main.c` - remove `auto_connect`, register callback before `lwlte_start()`, call `lwlte_start()`.
- Modify: `examples/mqtt_client/main/main.c` - remove `auto_connect`, call `lwlte_start()`, keep MQTT start explicit.
- Modify: `main/main.c` - if it uses `auto_connect` or `lwlte_connect()`, update to explicit `lwlte_start()`.

Do not commit during execution unless the user explicitly asks for a commit. Use `git diff` checkpoints instead.

---

### Task 1: Add Failing Lifecycle Contract Test

**Files:**
- Create: `tests/host/test_lwlte_start_lifecycle.py`

- [ ] **Step 1: Write the failing host contract test**

Create `tests/host/test_lwlte_start_lifecycle.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static regression checks for the lwlte init/start lifecycle split."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]

LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"
CORE_H = ROOT / "src/core/core.h"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"
ARCH_DOC = ROOT / "docs/agents/architecture.md"
CLASSES_DOC = ROOT / "docs/agents/classes.md"
OOP_DOC = ROOT / "docs/agents/oop-design.md"
BASIC_EXAMPLE = ROOT / "examples/basic_connect/main/main.c"
MQTT_EXAMPLE = ROOT / "examples/mqtt_client/main/main.c"
MAIN_EXAMPLE = ROOT / "main/main.c"


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


def assert_contains(testcase: unittest.TestCase, haystack: str, needle: str, label: str) -> None:
    if needle not in haystack:
        testcase.fail(f"missing {needle!r} in {label}")


def assert_not_contains(testcase: unittest.TestCase, haystack: str, needle: str, label: str) -> None:
    if needle in haystack:
        testcase.fail(f"unexpected {needle!r} in {label}")


def require_index(testcase: unittest.TestCase, haystack: str, needle: str, label: str, start: int = 0) -> int:
    index = haystack.find(needle, start)
    if index < 0:
        testcase.fail(f"missing anchor {needle!r} in {label}")
    return index


class LwlteStartLifecycleContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")
        cls.lwlte_air780ep_c = LWLTE_AIR780EP_C.read_text(encoding="utf-8")
        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")
        cls.arch_doc = ARCH_DOC.read_text(encoding="utf-8")
        cls.classes_doc = CLASSES_DOC.read_text(encoding="utf-8")
        cls.oop_doc = OOP_DOC.read_text(encoding="utf-8")
        cls.basic_example = read_optional(BASIC_EXAMPLE)
        cls.mqtt_example = read_optional(MQTT_EXAMPLE)
        cls.main_example = read_optional(MAIN_EXAMPLE)

    def test_public_api_has_start_not_connect_or_auto_connect(self):
        assert_contains(self, self.lwlte_h, "esp_err_t lwlte_start(lwlte_t *me);", "lwlte.h")
        assert_not_contains(self, self.lwlte_h, "esp_err_t lwlte_connect(lwlte_t *me);", "lwlte.h")
        config_start = require_index(self, self.lwlte_h, "typedef struct {", "lwlte.h")
        config_start = require_index(self, self.lwlte_h, "uart_port_t uart_num;", "lwlte_air780ep_config_t", config_start)
        config_end = require_index(self, self.lwlte_h, "} lwlte_air780ep_config_t;", "lwlte_air780ep_config_t", config_start)
        config_body = self.lwlte_h[config_start:config_end]
        assert_not_contains(self, config_body, "auto_connect", "lwlte_air780ep_config_t")

    def test_facade_start_delegates_only_to_core_start(self):
        body = function_body(self.lwlte_c, "esp_err_t lwlte_start(lwlte_t *me)")
        assert_contains(self, body, "begin_api_call(me, true, &core)", "lwlte_start")
        assert_contains(self, body, "core_start(core)", "lwlte_start")
        for forbidden in ["modem_start", "lwlte_wait_ready", "core_connect", "modem_"]:
            assert_not_contains(self, body, forbidden, "lwlte_start")

    def test_air780ep_init_constructs_without_runtime_start(self):
        body = function_body(
            self.lwlte_air780ep_c,
            "esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,",
        )
        for required in ["at_engine_create", "modem_air780ep_create", "core_create", "core_register_event_callback"]:
            assert_contains(self, body, required, "lwlte_air780ep_init")
        for forbidden in ["modem_start", "core_start", "lwlte_wait_ready", "lwlte_connect", "auto_connect"]:
            assert_not_contains(self, body, forbidden, "lwlte_air780ep_init")

    def test_core_start_owns_modem_start_and_network_activation(self):
        assert_not_contains(self, self.core_h, "bool auto_connect;", "core.h")
        handle_start = function_body(self.core_fsm_c, "static void handle_start(core_t *me)")
        for required in [
            "core_set_state(me, CORE_STATE_STARTING)",
            "post_event_checked(me, CORE_EVENT_STARTED, NULL)",
            "modem_start(me->modem)",
            "handle_ready(me)",
            "net_mgr_start_activation(me)",
        ]:
            assert_contains(self, handle_start, required, "handle_start")
        self.assertLess(handle_start.index("modem_start(me->modem)"), handle_start.index("net_mgr_start_activation(me)"))

    def test_core_ready_no_longer_auto_connects(self):
        handle_ready = function_body(self.core_fsm_c, "static void handle_ready(core_t *me)")
        assert_contains(self, handle_ready, "core_set_state(me, CORE_STATE_READY)", "handle_ready")
        assert_contains(self, handle_ready, "post_event_checked(me, CORE_EVENT_READY, NULL)", "handle_ready")
        assert_not_contains(self, handle_ready, "auto_connect", "handle_ready")
        assert_not_contains(self, handle_ready, "net_mgr_start_activation", "handle_ready")

    def test_examples_use_explicit_start(self):
        failures = []
        for label, source in [
            ("examples/basic_connect/main/main.c", self.basic_example),
            ("examples/mqtt_client/main/main.c", self.mqtt_example),
        ]:
            if "lwlte_start" not in source:
                failures.append(f"missing 'lwlte_start' in {label}")
            if ".auto_connect" in source:
                failures.append(f"unexpected '.auto_connect' in {label}")
            if "lwlte_connect" in source:
                failures.append(f"unexpected 'lwlte_connect' in {label}")

        if self.main_example:
            if ".auto_connect" in self.main_example:
                failures.append("unexpected '.auto_connect' in main/main.c")
            if "lwlte_connect" in self.main_example:
                failures.append("unexpected 'lwlte_connect' in main/main.c")
        if failures:
            self.fail("; ".join(failures))

    def test_docs_describe_new_lifecycle(self):
        docs = self.arch_doc + self.classes_doc + self.oop_doc
        for token in [
            "lwlte_start",
            "LWLTE_EVENT_NET_ONLINE",
            "modem_start",
            "PDP",
        ]:
            assert_contains(self, docs, token, "docs")
        forbidden_patterns = [
            r"auto_connect\s*=\s*true",
            r"auto_connect\s*=\s*false",
        ]
        for pattern in forbidden_patterns:
            self.assertIsNone(re.search(pattern, docs, re.DOTALL), pattern)
        for old_sequence in [
            "if (modem_start(modem) != ESP_OK)",
            "if (core_start(core) != ESP_OK)",
            "lwlte_wait_ready(lte",
            "if (config->auto_connect",
        ]:
            assert_not_contains(self, docs, old_sequence, "docs")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the new test and verify it fails**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: FAIL. The first failures should mention missing `esp_err_t lwlte_start(lwlte_t *me);`, existing `auto_connect`, existing `lwlte_connect()`, and startup calls still present in `lwlte_air780ep_init()`.

- [ ] **Step 3: Checkpoint without committing**

Run:

```bash
git diff -- tests/host/test_lwlte_start_lifecycle.py
```

Expected: diff shows only the new host contract test.

---

### Task 2: Update Public Facade API Surface

**Files:**
- Modify: `src/include/lwlte.h`
- Modify: `src/lwlte/lwlte.c`

- [ ] **Step 1: Update public header lifecycle declarations and config**

In `src/include/lwlte.h`, remove this field from `lwlte_air780ep_config_t`:

```c
bool auto_connect;                    /**< ready 后是否自动提交联网请求，不等待网络上线； Whether to submit connect after ready, without waiting online */
```

In the `lwlte_air780ep_config_t` notes, replace any note that says `auto_connect` submits a connection request with notes that say `lwlte_start()` starts the module and network asynchronously. The init function note block should include this content:

```c
 * @note 该函数只创建 LTE 用户门面及内部对象，不启动模块、不等待 RDY、不激活 PDP。
 * @note ESP_OK 返回时 *out_lte 为可用句柄，所有权转移给调用方，必须通过 lwlte_destroy() 释放。
 * @note 调用方应注册事件回调后调用 lwlte_start()；最终 online 结果通过 LWLTE_EVENT_NET_ONLINE 上报。
```

Remove the entire public `lwlte_connect(lwlte_t *me)` declaration and its Doxygen block.

Add this public declaration and Doxygen block before `lwlte_disconnect()`:

```c
/**
 * @brief 启动 LTE 并异步联网
 * @details Start LTE and connect network asynchronously
 * @note 该函数异步提交启动请求，ESP_OK 仅表示请求已提交，不表示模块 ready 或网络 online。
 * @note 成功联网通过 LWLTE_EVENT_NET_ONLINE 上报，也可通过 lwlte_get_net_state() 查询。
 * @note 建议在调用本函数前先调用 lwlte_register_event_callback() 注册事件回调。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许启动或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层启动错误
 */
esp_err_t lwlte_start(lwlte_t *me);
```

- [ ] **Step 2: Replace `lwlte_connect()` implementation with `lwlte_start()`**

In `src/lwlte/lwlte.c`, replace the existing `lwlte_connect()` function with:

```c
esp_err_t lwlte_start(lwlte_t *me)
{
    core_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    ret = core_start(core);
    end_api_call(me);

    return ret;
}
```

Do not call `modem_start()`, `core_connect()`, `lwlte_wait_ready()`, or any `modem_*` API from `lwlte_start()`.

- [ ] **Step 3: Run the new public API test subset**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: still FAIL, but failures related to missing `lwlte_start()` and public `auto_connect` in `lwlte.h` should be gone. Failures for `lwlte_air780ep_init()`, Core start, examples, and docs are expected.

- [ ] **Step 4: Checkpoint without committing**

Run:

```bash
git diff -- src/include/lwlte.h src/lwlte/lwlte.c tests/host/test_lwlte_start_lifecycle.py
```

Expected: diff shows the public API rename/removal and the new test only.

---

### Task 3: Make Air780EP Facade Init Construct Only

**Files:**
- Modify: `src/lwlte/lwlte_air780ep.c`

- [ ] **Step 1: Remove init-time startup timeout bookkeeping**

In `lwlte_air780ep_init()`, remove these local variables and their uses:

```c
const uint32_t total_ready_timeout_ms = ready_timeout_ms(config);
const TickType_t init_start_tick = xTaskGetTickCount();
uint32_t stage_timeout_ms = 0;
```

Keep `ready_timeout_ms(config)` as a helper if it is still used to populate `modem_config.ready_timeout_ms`. If it becomes unused after this task, remove the helper prototype and function in the same file.

- [ ] **Step 2: Keep modem config ready timeout as a plain config value**

Replace the current `modem_air780ep_config_t modem_config` initialization with this shape:

```c
const modem_air780ep_config_t modem_config = {
    .en_pin = config->en_pin,
    .reset_pulse_ms = config->modem_reset_pulse_ms,
    .ready_timeout_ms = config->init_ready_timeout_ms,
    .default_cmd_timeout_ms = config->modem_default_cmd_timeout_ms,
    .event_queue_size = config->modem_event_queue_size,
    .event_task_stack = config->modem_event_task_stack,
    .event_task_priority = config->modem_event_task_priority,
};
```

This preserves the existing public `init_ready_timeout_ms` field as the Air780EP RDY wait timeout passed down to Modem. Do not introduce a new public timeout field in this task.

- [ ] **Step 3: Remove init-time runtime calls**

Delete these blocks from `lwlte_air780ep_init()`:

```c
ret = modem_start(me->modem);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start modem failed: %s", esp_err_to_name(ret));
    return cleanup_after_failure(me, ret);
}
```

```c
ret = core_start(me->core);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start core failed: %s", esp_err_to_name(ret));
    return cleanup_after_failure(me, ret);
}
```

```c
ret = lwlte_wait_ready(me, stage_timeout_ms);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "wait ready failed: %s", esp_err_to_name(ret));
    return cleanup_after_failure(me, ret);
}
```

```c
if (config->auto_connect) {
    ret = lwlte_connect(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "auto connect request failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }
}
```

After MQTT creation and callback registration, the function should set `*out_lte = me;` and return `ESP_OK`.

- [ ] **Step 4: Remove now-unused helpers and includes**

If these are unused after removing init-time waits, delete their prototypes and function bodies:

```c
static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config);
static esp_err_t remaining_timeout_ms(TickType_t start_tick,
                                      uint32_t total_timeout_ms,
                                      uint32_t *out_timeout_ms);
```

If `xTaskGetTickCount()` and `pdMS_TO_TICKS()` are no longer used in `src/lwlte/lwlte_air780ep.c`, remove these includes:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
```

Keep `LWLTE_AIR780EP_DEFAULT_READY_MS` only if `ready_timeout_ms()` remains used. If not used, delete the define as well.

- [ ] **Step 5: Run lifecycle contract test**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: still FAIL, but failures about `lwlte_air780ep_init()` calling runtime startup functions should be gone. Core start, examples, and docs failures are expected.

- [ ] **Step 6: Checkpoint without committing**

Run:

```bash
git diff -- src/lwlte/lwlte_air780ep.c tests/host/test_lwlte_start_lifecycle.py
```

Expected: diff shows init no longer starts modem/core or waits ready.

---

### Task 4: Move Startup Sequencing Into Core FSM

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core_fsm.c`
- Modify: `src/lwlte/lwlte_air780ep.c`

- [ ] **Step 1: Remove Core auto_connect config field**

In `src/core/core.h`, remove this field from `core_config_t`:

```c
bool auto_connect;                   /**< 是否自动联网； Whether to connect automatically */
```

Update the `core_start()` note to describe the new behavior:

```c
 * @note 该函数异步提交启动请求；Core FSM 会调用 modem_start()，随后执行网络激活流程。
 * @note ESP_OK 仅表示请求已提交；最终 online 结果通过 CORE_EVENT_NET_ONLINE 上报。
```

- [ ] **Step 2: Remove Core auto_connect initialization**

In `src/lwlte/lwlte_air780ep.c`, remove this field from the `core_config_t core_config` initializer:

```c
.auto_connect = false,
```

- [ ] **Step 3: Update `handle_start()` to call modem and start network activation**

In `src/core/core_fsm.c`, replace `handle_start()` with:

```c
static void handle_start(core_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state != CORE_STATE_STOPPED) {
        return;
    }

    core_set_state(me, CORE_STATE_STARTING);
    post_event_checked(me, CORE_EVENT_STARTED, NULL);

    esp_err_t ret = modem_start(me->modem);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start modem failed: %s", esp_err_to_name(ret));
        handle_core_error(me, ret);
        return;
    }

    handle_ready(me);
    ret = net_mgr_start_activation(me);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "start network activation failed: %s", esp_err_to_name(ret));
    }
}
```

This is intentionally blocking inside the Core FSM task while `modem_start()` performs RDY/basic AT bring-up. User API remains asynchronous because `lwlte_start()` only submits `CORE_SIG_START`.

- [ ] **Step 4: Remove auto-connect from `handle_ready()`**

In `src/core/core_fsm.c`, replace `handle_ready()` with:

```c
static void handle_ready(core_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state == CORE_STATE_ONLINE ||
        state == CORE_STATE_NET_ACTIVATING ||
        state == CORE_STATE_READY) {
        return;
    }

    core_set_state(me, CORE_STATE_READY);
    post_event_checked(me, CORE_EVENT_READY, NULL);
}
```

- [ ] **Step 5: Run lifecycle contract test**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: still FAIL only on examples and docs. Public API, factory, and Core sequencing checks should pass.

- [ ] **Step 6: Run existing host tests that cover network and MQTT contracts**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
python3 tests/host/test_mqtt_end_to_end_contract.py
```

Expected: PASS or failures that directly identify references to removed public `lwlte_connect()` / `auto_connect`; fix those references in code or tests as part of the next tasks, not by weakening lifecycle behavior.

- [ ] **Step 7: Checkpoint without committing**

Run:

```bash
git diff -- src/core/core.h src/core/core_fsm.c src/lwlte/lwlte_air780ep.c
```

Expected: diff shows Core owns modem start and network activation, and no Core `auto_connect` remains.

---

### Task 5: Update Examples To Explicit Start

**Files:**
- Modify: `examples/basic_connect/main/main.c`
- Modify: `examples/mqtt_client/main/main.c`
- Modify: `main/main.c`

- [ ] **Step 1: Update `examples/basic_connect/main/main.c` config**

Remove this initializer field if present:

```c
.auto_connect = false,
```

After successful `lwlte_air780ep_init()` and callback registration, add:

```c
ret = lwlte_start(lte);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(lwlte_destroy(lte));
    return;
}
```

If the example currently calls `lwlte_connect(lte)`, delete that call because `lwlte_start()` now starts the full online flow.

- [ ] **Step 2: Update `examples/mqtt_client/main/main.c` config and startup**

Remove this initializer field if present:

```c
.auto_connect = false,
```

After successful `lwlte_air780ep_init()` and callback registration, add:

```c
ret = lwlte_start(lte);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(lwlte_destroy(lte));
    return;
}
```

Keep `lwlte_mqtt_start(lte)` explicit. If the example starts MQTT immediately after `lwlte_start()`, that remains acceptable only if the MQTT service waits for network. If the example starts MQTT from the LTE event callback, trigger it when `event_id == LWLTE_EVENT_NET_ONLINE`.

- [ ] **Step 3: Update `main/main.c` if it uses old lifecycle**

Search `main/main.c` for `auto_connect` and `lwlte_connect`. If found, remove `auto_connect` from the config and replace `lwlte_connect(lte)` with:

```c
ret = lwlte_start(lte);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(lwlte_destroy(lte));
    return;
}
```

If `main/main.c` does not use the LTE facade, leave it unchanged.

- [ ] **Step 4: Run lifecycle contract test**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: still FAIL only on docs if source and examples are complete.

- [ ] **Step 5: Checkpoint without committing**

Run:

```bash
git diff -- examples/basic_connect/main/main.c examples/mqtt_client/main/main.c main/main.c
```

Expected: examples no longer mention `auto_connect` or `lwlte_connect` and explicitly call `lwlte_start()`.

---

### Task 6: Update Architecture And Class Documentation

**Files:**
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/oop-design.md`

- [ ] **Step 1: Update architecture layer responsibilities**

In `docs/agents/architecture.md`, revise lifecycle text so it includes these exact statements or equivalent wording:

```markdown
- `lwlte_air780ep_init()` 只负责创建和装配 `lwlte_t`、AT Engine、Modem、Core、Ping/MQTT service，不启动模块、不等待 RDY、不激活 PDP。
- `lwlte_start()` 是用户显式启动入口，异步提交启动请求；最终 online 结果通过 `LWLTE_EVENT_NET_ONLINE` 上报。
- Core 在 `CORE_SIG_START` 中调用 `modem_start()`，随后执行 SIM、注册、附着、APN、PDP 激活和 IP 查询流程。
- `modem_start()` 表示模块动态开机到基础 AT ready：注册 URC、硬复位/等待 RDY、基础 AT 初始化；不负责 APN/PDP/IP。
```

Remove examples that show `auto_connect = true`, `auto_connect = false`, `modem_start()` called from `lwlte_air780ep_init()`, `core_start()` called from `lwlte_air780ep_init()`, or `lwlte_wait_ready()` inside init.

- [ ] **Step 2: Update classes documentation**

In `docs/agents/classes.md`, update the public Facade and Core sections so they include:

```markdown
esp_err_t lwlte_start(lwlte_t *me);
```

and no longer include public:

```markdown
esp_err_t lwlte_connect(lwlte_t *me);
bool auto_connect;
```

Keep internal `core_connect()` only if it is documented as an internal Core command helper rather than a user API. Define `modem_start()` as module bring-up to RDY/basic AT ready, not network online.

- [ ] **Step 3: Update OOP lifecycle examples**

In `docs/agents/oop-design.md`, replace any `lwlte_air780ep_config_t` example containing:

```c
.auto_connect = true,
```

or:

```c
.auto_connect = false,
```

with examples that call:

```c
ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &g_lte));
ESP_ERROR_CHECK(lwlte_register_event_callback(g_lte, app_event_handler, NULL));
ESP_ERROR_CHECK(lwlte_start(g_lte));
```

- [ ] **Step 4: Run lifecycle contract test**

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py
```

Expected: PASS.

- [ ] **Step 5: Checkpoint without committing**

Run:

```bash
git diff -- docs/agents/architecture.md docs/agents/classes.md docs/agents/oop-design.md
```

Expected: docs describe init/create vs start, Core-owned startup, and no public `auto_connect`.

---

### Task 7: Run Full Host And Build Verification

**Files:**
- Verify: all modified source, docs, examples, and tests

- [ ] **Step 1: Run all host contract tests**

Run:

```bash
python3 -m unittest discover -s tests/host -p 'test_*.py'
```

Expected: PASS. If any existing host test fails because it asserts the old public `auto_connect` or `lwlte_connect()` lifecycle, update that test to assert `lwlte_start()` and the new Core-owned startup sequence instead of weakening the new behavior.

- [ ] **Step 2: Run static lifecycle searches**

Run:

```bash
rg "auto_connect|lwlte_connect\(|\.auto_connect" src examples main docs/agents tests/host
```

Expected: no matches for public `auto_connect`, `.auto_connect`, or `lwlte_connect(`. If `core_connect()` remains, it is not matched by this command and can stay internal.

Run:

```bash
rg "modem_start\(|core_start\(|lwlte_wait_ready\(|lwlte_connect\(" src/lwlte/lwlte_air780ep.c
```

Expected: no matches.

Run:

```bash
rg "modem_start\(me->modem\)|net_mgr_start_activation\(me\)" src/core/core_fsm.c
```

Expected: both matches are in `handle_start()`.

- [ ] **Step 3: Build with ESP-IDF MCP**

Run the ESP-IDF build through the available MCP build tool.

Expected: build succeeds.

- [ ] **Step 4: If MCP build is unavailable, build with ESP-IDF shell**

Run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: build succeeds. If it fails, record the first compiler error and fix the referenced source before rerunning.

- [ ] **Step 5: Final diff review without committing**

Run:

```bash
git status --short
git diff --stat
git diff -- src/include/lwlte.h src/lwlte/lwlte.c src/lwlte/lwlte_air780ep.c src/core/core.h src/core/core_fsm.c
```

Expected: diff contains only lifecycle changes from this plan plus the user's pre-existing uncommitted source changes if they overlap. Do not revert or overwrite unrelated existing modifications.

- [ ] **Step 6: Optional hardware verification**

If hardware is connected and the user asks for runtime verification, flash and monitor according to `docs/agents/build-and-debug.md`. Expected successful event order:

```text
LWLTE_EVENT_STARTED
LWLTE_EVENT_READY
LWLTE_EVENT_NET_CONNECTING
LWLTE_EVENT_NET_ONLINE
```

Do not perform flashing or serial monitoring unless the user asks for hardware verification.
