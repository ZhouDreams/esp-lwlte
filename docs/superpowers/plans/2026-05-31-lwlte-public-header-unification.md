# LWLTE Public Header Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `src/include/lwlte.h` the single public LWLTE header while keeping module-specific factory implementations in separate `.c` files.

**Architecture:** Move Air780EP public config typedefs and `lwlte_air780ep_init()` into `lwlte.h`, with module factory init declarations first in `GLOBAL PROTOTYPES`. Keep `lwlte.c` as the common facade implementation and `lwlte_air780ep.c` as the Air780EP composition root with its own `static` helpers. Update examples, docs, and host contract checks so public users include only `lwlte.h`.

**Tech Stack:** ESP-IDF component in C, FreeRTOS, ESP-IDF GPIO/UART public types, Python `unittest` host contract checks.

---

## File Structure

- Modify: `src/include/lwlte.h` — add ESP-IDF GPIO/UART includes, move Air780EP config typedefs into the public typedef section, and place `lwlte_air780ep_init()` at the top of `GLOBAL PROTOTYPES` before `lwlte_destroy()`.
- Delete: `src/include/lwlte_air780ep.h` — remove the split public module header.
- Modify: `src/lwlte/lwlte_air780ep.c` — include `lwlte.h` instead of `lwlte_air780ep.h`; keep all Air780EP-specific `static` helpers in this file.
- Modify: `examples/basic_connect/main/main.c` — replace `#include "lwlte_air780ep.h"` with `#include "lwlte.h"`.
- Modify: `examples/mqtt_client/main/main.c` — replace `#include "lwlte_air780ep.h"` with `#include "lwlte.h"`.
- Modify: `tests/host/test_mqtt_end_to_end_contract.py` — replace the old readable `AIR780EP_H` fixture with a nonexistence check path and assert Air780EP config tokens in `lwlte.h`.
- Modify: `docs/agents/architecture.md` — document single-header public usage.
- Modify: `docs/agents/oop-design.md` — document single-header public usage while preserving upcasting/downcasting guidance.
- Modify: `docs/agents/directory-structure.md` — document `src/include/` exporting only `lwlte.h`.
- Optional documentation updates: update superseded historical specs/plans only if their current wording is actively used as project guidance. Do not rewrite old plans solely for history cleanup.

### Task 1: Add A Failing Contract Check

**Files:**
- Modify: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Update the host contract to expect single-header public API**

In `tests/host/test_mqtt_end_to_end_contract.py`, replace the old `AIR780EP_H` readable fixture with `LWLTE_AIR780EP_H` used only for the nonexistence assertion, and remove the `cls.air780ep_h` load. Change `test_public_api_and_air780ep_mqtt_config_exist()` so Air780EP tokens are checked in `self.lwlte_h`, and add a new test that asserts the split public header is gone and the init declaration appears before destroy.

Use this exact shape for the affected top-level constants and `setUpClass()` fields:

```python
LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_AIR780EP_H = ROOT / "src/include/lwlte_air780ep.h"
LWLTE_PRIV = ROOT / "src/lwlte/lwlte_priv.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"
CLASSES_MD = ROOT / "docs/agents/classes.md"
```

Use this exact `setUpClass()` beginning:

```python
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional(LWLTE_H)
        cls.lwlte_priv = read_optional(LWLTE_PRIV)
        cls.lwlte_c = read_optional(LWLTE_C)
        cls.lwlte_air780ep_c = read_optional(LWLTE_AIR780EP_C)
        cls.classes_md = read_optional(CLASSES_MD)
```

Replace the final loop in `test_public_api_and_air780ep_mqtt_config_exist()` with:

```python
        for token in [
            "lwlte_air780ep_config_mqtt_client_t",
            "bool enabled;",
            "const char *host;",
            "uint16_t port;",
            "const char *client_id;",
            "lwlte_air780ep_config_mqtt_client_t mqtt_client;",
            "esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,",
        ]:
            self.assertIn(token, self.lwlte_h)
```

Add this test method after `test_public_api_and_air780ep_mqtt_config_exist()`:

```python
    def test_lwlte_h_is_the_only_public_lwlte_header(self):
        self.assertFalse(
            LWLTE_AIR780EP_H.exists(),
            "Air780EP public declarations must live in lwlte.h",
        )
        self.assertIn('#include "driver/gpio.h"', self.lwlte_h)
        self.assertIn('#include "driver/uart.h"', self.lwlte_h)
        self.assertIn("esp_err_t lwlte_air780ep_init", self.lwlte_h)
        self.assertIn("esp_err_t lwlte_destroy", self.lwlte_h)
        self.assertLess(
            self.lwlte_h.index("esp_err_t lwlte_air780ep_init"),
            self.lwlte_h.index("esp_err_t lwlte_destroy"),
        )
        self.assertIn('#include "lwlte.h"', self.lwlte_air780ep_c)
        self.assertNotIn('#include "lwlte_air780ep.h"', self.lwlte_air780ep_c)
```

- [ ] **Step 2: Run the focused host contract and verify it fails**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_lwlte_h_is_the_only_public_lwlte_header
```

Expected: FAIL because `src/include/lwlte_air780ep.h` still exists, `lwlte.h` does not include `driver/gpio.h` or `driver/uart.h`, and `lwlte_air780ep.c` still includes `lwlte_air780ep.h`.

### Task 2: Move Public Air780EP Declarations Into `lwlte.h`

**Files:**
- Modify: `src/include/lwlte.h`
- Delete: `src/include/lwlte_air780ep.h`

- [ ] **Step 1: Add public ESP-IDF type includes to `lwlte.h`**

In `src/include/lwlte.h`, add these includes after the existing standard C includes and before `esp_err.h`:

```c
#include "driver/gpio.h"
#include "driver/uart.h"
```

The include block should become:

```c
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
```

- [ ] **Step 2: Insert Air780EP config typedefs before `GLOBAL PROTOTYPES`**

In `src/include/lwlte.h`, insert the current `lwlte_air780ep_config_mqtt_client_t` and `lwlte_air780ep_config_t` definitions after `lwlte_event_callback_t` and before the `GLOBAL PROTOTYPES` section. Copy the definitions and comments from the current `src/include/lwlte_air780ep.h` exactly, preserving field order and notes.

The inserted declarations must include these exact field lines:

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

typedef struct {
    uart_port_t uart_num;
    gpio_num_t uart_tx_pin;
    gpio_num_t uart_rx_pin;
    int uart_baud_rate;
    gpio_num_t en_pin;
    const char *apn;
    uint8_t primary_cid;
    bool auto_connect;
    uint32_t init_ready_timeout_ms;
    uint32_t net_activate_timeout_ms;
    uint32_t reconnect_delay_ms;
    int at_rx_buf_size;
    int at_rx_task_stack;
    int at_rx_task_priority;
    int at_rx_line_buf_size;
    int at_cmd_default_timeout_ms;
    int at_max_response_lines;
    uint32_t modem_reset_pulse_ms;
    uint32_t modem_default_cmd_timeout_ms;
    int modem_event_queue_size;
    int modem_event_task_stack;
    int modem_event_task_priority;
    int core_fsm_queue_size;
    int core_fsm_task_stack;
    int core_fsm_task_priority;
    lwlte_air780ep_config_mqtt_client_t mqtt_client;
} lwlte_air780ep_config_t;
```

- [ ] **Step 3: Put `lwlte_air780ep_init()` first in `GLOBAL PROTOTYPES`**

In `src/include/lwlte.h`, insert the current `lwlte_air780ep_init()` documentation and prototype from `src/include/lwlte_air780ep.h` immediately after the `GLOBAL PROTOTYPES` banner and before the `lwlte_destroy()` documentation.

The prototype must be:

```c
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);
```

- [ ] **Step 4: Remove the split public Air780EP header**

Delete `src/include/lwlte_air780ep.h`.

- [ ] **Step 5: Run the focused contract again**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_lwlte_h_is_the_only_public_lwlte_header
```

Expected: still FAIL because `src/lwlte/lwlte_air780ep.c` still includes `lwlte_air780ep.h`.

### Task 3: Update Source And Example Includes

**Files:**
- Modify: `src/lwlte/lwlte_air780ep.c`
- Modify: `examples/basic_connect/main/main.c`
- Modify: `examples/mqtt_client/main/main.c`

- [ ] **Step 1: Update the Air780EP factory source include**

In `src/lwlte/lwlte_air780ep.c`, replace:

```c
#include "lwlte_air780ep.h"
#include "lwlte_priv.h"
```

with:

```c
#include "lwlte.h"
#include "lwlte_priv.h"
```

- [ ] **Step 2: Update basic connect example include**

In `examples/basic_connect/main/main.c`, replace:

```c
#include "lwlte_air780ep.h"
```

with:

```c
#include "lwlte.h"
```

If the file already includes `lwlte.h`, remove only the `lwlte_air780ep.h` include and do not duplicate `lwlte.h`.

- [ ] **Step 3: Update MQTT client example include**

In `examples/mqtt_client/main/main.c`, replace:

```c
#include "lwlte_air780ep.h"
```

with:

```c
#include "lwlte.h"
```

If the file already includes `lwlte.h`, remove only the `lwlte_air780ep.h` include and do not duplicate `lwlte.h`.

- [ ] **Step 4: Run the focused contract and verify it passes**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_lwlte_h_is_the_only_public_lwlte_header
```

Expected: PASS.

### Task 4: Update Project Documentation

**Files:**
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/oop-design.md`
- Modify: `docs/agents/directory-structure.md`

- [ ] **Step 1: Update architecture public include wording**

In `docs/agents/architecture.md`, replace public usage wording that says board initialization may include `lwlte_air780ep.h` with wording that says board initialization includes `lwlte.h` and uses module-specific config and factory declarations from that header.

Use these replacements:

```markdown
| App | 用户业务逻辑只操作 `lwlte_t`；板级初始化代码 include `lwlte.h` 并填写 `lwlte_air780ep_config_t` 等模块配置 |
```

```markdown
- 业务 App 代码只应 include `lwlte.h` 并调用 `lwlte_*` 操作；板级初始化或 App 自有配置代码也 include `lwlte.h`，并填写其中声明的模块配置如 `lwlte_air780ep_config_t`。
```

- [ ] **Step 2: Update OOP design public include wording**

In `docs/agents/oop-design.md`, replace wording that lists `#include "lwlte.h" / "lwlte_air780ep.h"` with:

```markdown
    │  #include "lwlte.h"
```

Replace the board initialization sentence with:

```markdown
业务 App 代码只 include `lwlte.h` 并操作 `lwlte_t`，不暴露任何子类类型。板级初始化或 App 自有配置代码也 include `lwlte.h`，并填写其中声明的模块配置如 `lwlte_air780ep_config_t`。
```

Do not change the existing upcasting/downcasting section except if nearby wording references the removed public header.

- [ ] **Step 3: Update directory structure public header wording**

In `docs/agents/directory-structure.md`, replace the `src/include/` comment that says it exports `lwlte.h、lwlte_air780ep.h` with:

```markdown
├── include/       # 用户公共头文件，仅导出 lwlte.h
```

- [ ] **Step 4: Verify no live source or agent docs include the removed header**

Run:

```bash
rg -n 'lwlte_air780ep\.h|#include "lwlte_air780ep\.h"' src examples docs/agents
```

Expected: no matches in `src/`, `examples/`, or `docs/agents/`.

The host contract test may still mention `lwlte_air780ep.h` as an intentional nonexistence assertion and should not be included in this zero-match check.

Historical matches under `docs/superpowers/specs/` and `docs/superpowers/plans/` may remain because they document past decisions, except the new design and this plan intentionally mention the old header as migration context.

### Task 5: Full Verification

**Files:**
- No additional planned edits.

- [ ] **Step 1: Run the full host contract test**

Run:

```bash
python3 -m unittest tests.host.test_mqtt_end_to_end_contract
```

Expected: PASS.

- [ ] **Step 2: Build the ESP-IDF project**

Run the ESP-IDF MCP build tool if available. In this environment, call `esp-idf-eim_build_project`.

If the MCP build tool is unavailable, run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: build succeeds without missing `lwlte_air780ep.h` include errors and without unknown `uart_port_t` or `gpio_num_t` type errors in `lwlte.h`.

- [ ] **Step 3: Check worktree diff**

Run:

```bash
git diff -- src/include/lwlte.h src/include/lwlte_air780ep.h src/lwlte/lwlte_air780ep.c examples/basic_connect/main/main.c examples/mqtt_client/main/main.c tests/host/test_mqtt_end_to_end_contract.py docs/agents/architecture.md docs/agents/oop-design.md docs/agents/directory-structure.md
```

Expected: diff only shows the single-header public API migration, include updates, test updates, and documentation wording updates.
