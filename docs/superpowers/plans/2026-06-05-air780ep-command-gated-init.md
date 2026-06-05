# Air780EP Command-Gated Initialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Air780EP `RDY`-gated startup with command-gated initialization: hard reset, `AT OK`, basic init commands, then Core command-driven network activation.

**Architecture:** `core_start()` stays asynchronous and only submits `CORE_SIG_START`; Core FSM `handle_start()` blocks inside the Core task while `modem_start()` runs. `modem_start()` blocks until Air780EP responds to `AT` with `OK`, finishes basic init commands, registers runtime URCs after initialization, marks modem ready, and returns `ESP_OK`; only then does Core start SIM/register/attach/PDP/IP activation by command responses.

**Tech Stack:** C, ESP-IDF, FreeRTOS, Python `unittest` static regression tests, project ESP-IDF MCP build tool.

**Commit Policy:** Do not commit during this plan unless the user explicitly requests it. Use `git status --short` checkpoints instead of commit steps.

---

## Scope Check

This plan covers one subsystem boundary: Air780EP startup and the Core FSM handoff into existing network activation. It does not implement the future runtime URC state model. Runtime URC handlers may remain in the file, but initialization and network activation success must not rely on URC state.

## File Structure

- Modify: `tests/host/test_air780ep_cpin_policy.py` - update SIM busy contract so `AT+CPIN?` retry is timer-driven, not URC-accelerated.
- Create: `tests/host/test_air780ep_command_gated_init.py` - static regression contract for `AT` probing, retry delay, no `RDY` synchronization, startup sequencing, and doc wording.
- Modify: `src/modem/modem_air780ep.c` - remove `RDY` wait path, add `AT` ready polling, add retry interval constants, move URC registration after command init, simplify CPIN busy retry wait.
- Modify: `src/core/core_fsm.c` - update comments to describe blocking `modem_start()` followed by command-driven network activation.
- Modify: `src/include/lwlte.h` - update `init_ready_timeout_ms` comments from `RDY` timeout to `AT OK` timeout.
- Modify: `docs/modem-init-min-flow.md` - align Air780EP flow with `AT OK` ready gate and command-only network activation.
- Modify: `docs/agents/architecture.md` - update lifecycle descriptions that currently say `等待 RDY`.
- Modify: `docs/agents/classes.md` - update modem/core class design text and Air780EP operation table.
- Modify: `docs/agents/at_cmd_air780ep.md` - update Air780EP AT/URC reference so `RDY` is not a startup gate.

---

### Task 1: Add Failing Static Contracts

**Files:**
- Create: `tests/host/test_air780ep_command_gated_init.py`
- Modify: `tests/host/test_air780ep_cpin_policy.py`

- [ ] **Step 1: Add the command-gated init regression test**

Create `tests/host/test_air780ep_command_gated_init.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static regression checks for Air780EP command-gated startup."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
AIR780EP = ROOT / "src/modem/modem_air780ep.c"
CORE_FSM = ROOT / "src/core/core_fsm.c"
LWLTE_H = ROOT / "src/include/lwlte.h"
INIT_FLOW_DOC = ROOT / "docs/modem-init-min-flow.md"
ARCH_DOC = ROOT / "docs/agents/architecture.md"
CLASSES_DOC = ROOT / "docs/agents/classes.md"
AT_DOC = ROOT / "docs/agents/at_cmd_air780ep.md"


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


class Air780EpCommandGatedInitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.air780ep = AIR780EP.read_text(encoding="utf-8")
        cls.core_fsm = CORE_FSM.read_text(encoding="utf-8")
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.docs = "\n".join(
            path.read_text(encoding="utf-8")
            for path in [INIT_FLOW_DOC, ARCH_DOC, CLASSES_DOC, AT_DOC]
        )

    def test_startup_constants_define_command_probe_policy(self):
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_AT_READY_PROBE_TIMEOUT_MS\s+1000")
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_INIT_RETRY_DELAY_MS\s+500")
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_INIT_CMD_MAX_ATTEMPTS\s+3")

    def test_rdy_synchronization_path_is_removed(self):
        forbidden_symbols = [
            "AIR780EP_URC_RDY",
            "rdy_handler",
            "rdy_sema",
            "rdy_seen",
            "waiting_rdy",
            "clear_rdy_state",
            "begin_wait_rdy",
            "cancel_wait_rdy",
            "wait_rdy",
            "rdy_urc_handler",
        ]
        for symbol in forbidden_symbols:
            self.assertNotIn(symbol, self.air780ep)

    def test_wait_at_ready_uses_only_at_command(self):
        body = function_body(self.air780ep, "static esp_err_t wait_at_ready(modem_air780ep_t *self)")
        self.assertIn('send_cmd(self, "AT", &ctx, AIR780EP_AT_READY_PROBE_TIMEOUT_MS)', body)
        self.assertIn("AIR780EP_INIT_RETRY_DELAY_MS", body)
        self.assertIn("vTaskDelay", body)
        self.assertIn("ESP_ERR_TIMEOUT", body)
        self.assertNotIn('"ATE0"', body)
        self.assertNotIn("RDY", body)

    def test_basic_init_commands_start_with_ate0_and_retry_with_delay(self):
        body = function_body(self.air780ep, "static esp_err_t run_basic_init_cmds(modem_air780ep_t *self)")
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"', '"AT+CGREG=2"', '"AT+CREG=2"', '"AT*I"']
        last_index = -1
        for token in expected_order:
            index = body.find(token)
            self.assertGreater(index, last_index, token)
            last_index = index
        self.assertIn("AIR780EP_INIT_CMD_MAX_ATTEMPTS", body)
        self.assertIn("AIR780EP_INIT_RETRY_DELAY_MS", body)
        self.assertIn("vTaskDelay", body)

    def test_start_and_reset_sequence_is_command_gated(self):
        for signature in [
            "static esp_err_t air780ep_start(modem_t *me)",
            "static esp_err_t air780ep_reset(modem_t *me)",
        ]:
            body = function_body(self.air780ep, signature)
            for token in [
                "hardware_reset(self)",
                "wait_at_ready(self)",
                "run_basic_init_cmds(self)",
                "register_urcs(self)",
                "finish_modem_ready(me, self)",
            ]:
                self.assertIn(token, body, signature)
            self.assertLess(body.index("hardware_reset(self)"), body.index("wait_at_ready(self)"), signature)
            self.assertLess(body.index("wait_at_ready(self)"), body.index("run_basic_init_cmds(self)"), signature)
            self.assertLess(body.index("run_basic_init_cmds(self)"), body.index("register_urcs(self)"), signature)
            self.assertLess(body.index("register_urcs(self)"), body.index("finish_modem_ready(me, self)"), signature)
            self.assertNotIn("wait_rdy", body)
            self.assertNotIn("begin_wait_rdy", body)

    def test_core_start_comment_describes_blocking_modem_start_then_network_activation(self):
        handle_start = function_body(self.core_fsm, "static void handle_start(core_t *me)")
        self.assertIn("modem_start", handle_start)
        self.assertIn("AT OK", handle_start)
        self.assertIn("基础 AT", handle_start)
        self.assertIn("net_mgr_start_activation(me)", handle_start)
        self.assertLess(handle_start.index("modem_start(me->modem)"), handle_start.index("net_mgr_start_activation(me)"))

    def test_public_config_comments_use_at_ready_not_rdy_wait(self):
        self.assertIn("AT OK", self.lwlte_h)
        self.assertNotIn("RDY 等待超时", self.lwlte_h)
        self.assertNotIn("RDY wait timeout", self.lwlte_h)

    def test_docs_describe_at_ok_gate_not_rdy_gate(self):
        for required in [
            "AT OK",
            "AT 通道",
            "命令返回",
            "modem_start()",
            "core_start()",
        ]:
            self.assertIn(required, self.docs)
        forbidden_patterns = [
            r"等待\s*RDY",
            r"等\s*RDY",
            r"wait\s+RDY",
            r"RDY\s+wait",
            r"RDY\s*等待超时",
        ]
        for pattern in forbidden_patterns:
            self.assertIsNone(re.search(pattern, self.docs, re.IGNORECASE), pattern)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Update the CPIN static contract to remove URC acceleration**

In `tests/host/test_air780ep_cpin_policy.py`, replace `test_air780ep_get_sim_status_retries_only_sim_busy()` with:

```python
    def test_air780ep_get_sim_status_retries_sim_busy_by_timer_only(self):
        self.assertIn("ctx.response.status == AT_RESP_CME_ERROR", self.src)
        self.assertIn("ctx.response.error_code == AIR780EP_CME_SIM_BUSY", self.src)
        self.assertIn("AIR780EP_SIM_READY_POLL_INTERVAL_MS", self.src)
        self.assertIn("vTaskDelay(timeout_ticks(wait_ms))", self.src)
        self.assertIn("return ESP_ERR_TIMEOUT", self.src)
        self.assertNotIn("cpin_ready_sema", self.src)
        self.assertNotIn("waiting_cpin_ready", self.src)
        self.assertNotIn("wait URC or", self.src)
        self.assertNotIn("+CPIN: READY URC received", self.src)
```

Then delete the entire existing `test_cpin_ready_wait_drains_stale_signal_before_arming_waiter()` method from the same file.

- [ ] **Step 3: Run the new tests and confirm they fail**

Run:

```bash
python3 tests/host/test_air780ep_command_gated_init.py -v
```

Expected: FAIL. The failures should mention missing `AIR780EP_AT_READY_PROBE_TIMEOUT_MS`, missing `wait_at_ready`, and existing `RDY` synchronization symbols.

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py -v
```

Expected: FAIL. The failure should mention existing `cpin_ready_sema` / `waiting_cpin_ready` or missing timer-only wait code.

- [ ] **Step 4: Check working tree**

Run:

```bash
git status --short
```

Expected: shows the new test file, the modified CPIN test, and the existing uncommitted spec file.

---

### Task 2: Rewrite Air780EP Startup Helpers

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Remove RDY synchronization state from the Air780EP instance**

In `modem_air780ep_t`, remove these fields:

```c
    at_urc_handler_t rdy_handler;
    SemaphoreHandle_t rdy_sema;
    bool rdy_seen;
    bool waiting_rdy;
```

Also remove this macro:

```c
#define AIR780EP_URC_RDY                 "RDY"
```

- [ ] **Step 2: Add startup retry constants**

Near the existing timeout defines in `src/modem/modem_air780ep.c`, add:

```c
#define AIR780EP_AT_READY_PROBE_TIMEOUT_MS 1000
#define AIR780EP_INIT_RETRY_DELAY_MS       500
#define AIR780EP_INIT_CMD_MAX_ATTEMPTS     3
```

- [ ] **Step 3: Replace RDY helper prototypes with AT-ready prototypes**

Remove prototypes for:

```c
static void clear_rdy_state(modem_air780ep_t *self);
static esp_err_t begin_wait_rdy(modem_air780ep_t *self);
static void cancel_wait_rdy(modem_air780ep_t *self);
static esp_err_t wait_rdy(modem_air780ep_t *self);
static void rdy_urc_handler(const char *prefix, const char *line, void *user_ctx);
```

Add these prototypes in their place:

```c
static uint32_t now_ms(void);
static bool elapsed_at_least(uint32_t start_ms, uint32_t timeout_ms);
static void delay_init_retry(void);
static esp_err_t wait_at_ready(modem_air780ep_t *self);
```

- [ ] **Step 4: Replace RDY helper implementations with command-ready helpers**

Delete the full implementations of `clear_rdy_state()`, `begin_wait_rdy()`, `cancel_wait_rdy()`, and `wait_rdy()`.

Insert this code before `run_basic_init_cmds()`:

```c
static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool elapsed_at_least(uint32_t start_ms, uint32_t timeout_ms)
{
    return (uint32_t)(now_ms() - start_ms) >= timeout_ms;
}

static void delay_init_retry(void)
{
    vTaskDelay(timeout_ticks(AIR780EP_INIT_RETRY_DELAY_MS));
}

static esp_err_t wait_at_ready(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const uint32_t timeout_ms = self->config.ready_timeout_ms;
    const uint32_t start_ms = now_ms();
    unsigned int attempt = 1;

    while (!elapsed_at_least(start_ms, timeout_ms)) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, "AT", &ctx,
                                 AIR780EP_AT_READY_PROBE_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT");
        }
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "AT ready probe failed (attempt %u): %s",
                 attempt, esp_err_to_name(ret));
        attempt++;

        if (!elapsed_at_least(start_ms, timeout_ms)) {
            delay_init_retry();
        }
    }

    ESP_LOGE(TAG, "AT ready probe timeout after %u ms",
             (unsigned int)timeout_ms);
    return ESP_ERR_TIMEOUT;
}
```

- [ ] **Step 5: Simplify `hardware_reset()` so it only controls EN and flushes RX**

Replace the existing `hardware_reset()` body with:

```c
static esp_err_t hardware_reset(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    ret = at_engine_flush_rx_exclusive(self->base.at);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "flush RX input before reset failed");

    if (self->config.en_pin == GPIO_NUM_NC) {
        at_engine_end_exclusive(self->base.at);
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << (uint32_t)self->config.en_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure EN GPIO failed");

    ret = gpio_set_level(self->config.en_pin, 0);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set EN GPIO low failed");

    if (self->config.reset_pulse_ms > 0) {
        vTaskDelay(timeout_ticks(self->config.reset_pulse_ms));
    }

    ret = at_engine_flush_rx_exclusive(self->base.at);
    ESP_GOTO_ON_ERROR(ret, err_restore_en, TAG, "flush RX input after EN low failed");

    ret = gpio_set_level(self->config.en_pin, 1);
    ESP_GOTO_ON_ERROR(ret, err_restore_en, TAG, "set EN GPIO high failed");

    at_engine_end_exclusive(self->base.at);
    return ESP_OK;

err_restore_en:
    {
        esp_err_t restore_ret = gpio_set_level(self->config.en_pin, 1);
        if (restore_ret != ESP_OK) {
            ESP_LOGW(TAG, "restore EN GPIO high failed: %s",
                     esp_err_to_name(restore_ret));
        }
    }
err:
    at_engine_end_exclusive(self->base.at);
    return ret;
}
```

- [ ] **Step 6: Remove RDY handler implementation**

Delete the full `rdy_urc_handler()` function.

- [ ] **Step 7: Remove RDY resource creation and destroy cleanup**

In `modem_air780ep_create()`, remove the block that creates `self->rdy_sema` and its failure cleanup path. Keep `cpin_ready_sema` removal for Task 4.

In `air780ep_destroy()`, remove this cleanup:

```c
    if (self->rdy_sema) {
        vSemaphoreDelete(self->rdy_sema);
        self->rdy_sema = NULL;
    }
    self->rdy_seen = false;
    self->waiting_rdy = false;
```

- [ ] **Step 8: Run command-gated init test and confirm remaining failures are sequencing-related**

Run:

```bash
python3 tests/host/test_air780ep_command_gated_init.py -v
```

Expected: still FAIL until `air780ep_start()`, `air780ep_reset()`, `register_urcs()`, and docs are updated.

---

### Task 3: Make Start/Reset Block on AT OK Then Init Commands

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Update basic init command retry loop**

In `run_basic_init_cmds()`, replace the hard-coded `3` loop with constants and delayed retry:

```c
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 1; attempt <= AIR780EP_INIT_CMD_MAX_ATTEMPTS; attempt++) {
            air780ep_cmd_ctx_t ctx;
            ret = send_cmd(self, cmds[i], &ctx, 0);
            if (ret == ESP_OK) {
                ret = ensure_at_ok(&ctx.response, cmds[i]);
            }
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "%s failed (attempt %d/%d): %s", cmds[i], attempt,
                     AIR780EP_INIT_CMD_MAX_ATTEMPTS, esp_err_to_name(ret));
            if (attempt < AIR780EP_INIT_CMD_MAX_ATTEMPTS) {
                delay_init_retry();
            }
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed after %d attempts", cmds[i],
                            AIR780EP_INIT_CMD_MAX_ATTEMPTS);
    }
```

- [ ] **Step 2: Remove RDY from URC registration lists**

In `register_urcs()`, remove this entry from the `urcs[]` array:

```c
        { AIR780EP_URC_RDY, &self->rdy_handler, rdy_urc_handler },
```

In `air780ep_unregister_urcs()`, remove this entry:

```c
        { AIR780EP_URC_RDY, &self->rdy_handler },
```

- [ ] **Step 3: Replace `air780ep_start()` sequence**

In `air780ep_start()`, use this operation order after `modem_set_state(me, MODEM_STATE_INITIALIZING)`:

```c
    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    ret = wait_at_ready(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait AT ready failed");

    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");
```

Remove the old pre-reset `register_urcs(self)`, `wait_rdy(self)`, and `cancel_wait_rdy(self)` calls.

Keep this error cleanup, without any RDY cancellation:

```c
err:
    if (!urc_registered_before && self->urc_registered) {
        unregister_urcs(self);
    }
    set_initialized(self, false);
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
```

- [ ] **Step 4: Replace `air780ep_reset()` sequence**

Apply the same sequence to `air780ep_reset()`:

```c
    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    ret = wait_at_ready(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait AT ready failed");

    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");
```

Remove the old pre-reset `register_urcs(self)`, `wait_rdy(self)`, and `cancel_wait_rdy(self)` calls.

- [ ] **Step 5: Run command-gated init test and confirm source checks pass**

Run:

```bash
python3 tests/host/test_air780ep_command_gated_init.py -v
```

Expected: source-order tests pass; doc/comment tests may still fail until Task 5.

---

### Task 4: Remove CPIN URC Acceleration From Command Flow

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Remove CPIN ready waiter state from the instance**

In `modem_air780ep_t`, remove:

```c
    SemaphoreHandle_t cpin_ready_sema;
    bool cpin_ready_seen;
    bool waiting_cpin_ready;
```

- [ ] **Step 2: Remove CPIN semaphore creation and cleanup**

In `modem_air780ep_create()`, remove the block that creates `self->cpin_ready_sema`.

In `air780ep_destroy()`, remove:

```c
    if (self->cpin_ready_sema) {
        vSemaphoreDelete(self->cpin_ready_sema);
        self->cpin_ready_sema = NULL;
    }
    self->cpin_ready_seen = false;
    self->waiting_cpin_ready = false;
```

- [ ] **Step 3: Simplify SIM busy retry wait**

In `air780ep_get_sim_status()`, replace the SIM busy wait block with timer-only waiting:

```c
                ESP_LOGW(TAG, "AT+CPIN? returned SIM busy, retry in %u ms",
                         (unsigned int)wait_ms);
                vTaskDelay(timeout_ticks(wait_ms));
                continue;
```

Remove the code that drains `cpin_ready_sema`, sets `waiting_cpin_ready`, waits on the semaphore, and logs `+CPIN: READY URC received`.

- [ ] **Step 4: Simplify `cpin_urc_handler()` so it cannot release command waiters**

Replace the beginning of `cpin_urc_handler()` with:

```c
static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    modem_sim_status_t status = parse_sim_status_line(line);

    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
        self->last_sim_status = status;
        xSemaphoreGive(self->base.lock);
    }

    const modem_event_t event = {
        .id = MODEM_EVENT_SIM_CHANGED,
        .data.sim_status = status,
    };
    esp_err_t ret = modem_post_event(&self->base, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post SIM changed event failed: %s", esp_err_to_name(ret));
    }
}
```

- [ ] **Step 5: Run CPIN policy test**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py -v
```

Expected: PASS.

---

### Task 5: Update Core Comments And Public Timeout Wording

**Files:**
- Modify: `src/core/core_fsm.c`
- Modify: `src/include/lwlte.h`

- [ ] **Step 1: Update `handle_start()` comment**

In `src/core/core_fsm.c`, replace the comment above `modem_start(me->modem)` with:

```c
    /* modem_start 阻塞完成：EN 硬复位 -> AT OK -> 基础 AT 初始化
     * -> 注册运行期 URC -> MODEM_EVENT_READY。只有返回 ESP_OK 后，
     * Core 才进入命令驱动的 SIM/注册/附着/PDP/IP 网络激活流程。 */
```

- [ ] **Step 2: Update `init_ready_timeout_ms` comments**

In `src/include/lwlte.h`, change the note at the Air780EP config block to:

```c
 * @note init_ready_timeout_ms 为 0 时使用下层默认值；该值在 lwlte_start() 触发 modem_start() 时作为 Air780EP 硬复位后等待 AT OK 的总超时。
```

Change the field comment to:

```c
    uint32_t init_ready_timeout_ms;        /**< Air780EP AT OK 等待总超时，0 使用下层默认值； Air780EP AT OK wait timeout, 0 uses lower-layer default */
```

- [ ] **Step 3: Run command-gated init test**

Run:

```bash
python3 tests/host/test_air780ep_command_gated_init.py -v
```

Expected: source/comment checks pass except any documentation checks left for Task 6.

---

### Task 6: Update Agent Documentation

**Files:**
- Modify: `docs/modem-init-min-flow.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/at_cmd_air780ep.md`

- [ ] **Step 1: Update `docs/modem-init-min-flow.md` Air780EP power-on wording**

Change the Air780EP `Power on` table row to state:

```markdown
| Power on | 拉 EN/供电后轮询 `AT` | `AT` 返回 `OK` | `RDY` 可能自发出现，但不参与初始化判定 | 实测会出现重复 `RDY` 和其它启动期 URC；初始化状态机忽略这些行，只以 `AT OK` 作为 AT 通道 ready 条件。失败后按间隔重试，直到总超时。 |
```

- [ ] **Step 2: Update `docs/modem-init-min-flow.md` URC classification**

Change the `RDY` row in Air780EP URC classification to:

```markdown
| `RDY` | 是 | 否 | 启动期日志现象 | 当前初始化流程不使用它做 gate；只以 `AT OK` 确认 AT 通道可用。 |
```

Change the conclusion sentence to include:

```markdown
当前实现的最小初始化以命令确认闭环为准：硬复位后 `AT OK`、`ATE0 OK`、`CMEE/注册 URC 开关 OK`，随后 Core 继续用 `CPIN/CEREG/CGATT/CIFSR` 等命令推进网络状态。
```

- [ ] **Step 3: Update `docs/agents/architecture.md` lifecycle wording**

Replace startup descriptions that say `硬复位/等待 RDY/基础 AT 初始化` with:

```markdown
硬复位/等待 `AT OK`/基础 AT 初始化
```

Ensure the Core section says:

```markdown
Core 在 `CORE_SIG_START` 中调用阻塞式 `modem_start()`；`modem_start()` 完成硬复位、`AT OK` 和基础 AT 初始化后返回 `ESP_OK`，Core 随后执行 SIM、注册、附着、APN、PDP 激活和 IP 查询流程。
```

- [ ] **Step 4: Update `docs/agents/classes.md` Air780EP operation table**

Replace operation descriptions for `start` and `reset` so they state:

```markdown
| `start` | 模块动态开机到基础 AT ready：硬复位后轮询 `AT` 直到 `OK`，执行基础 AT 初始化；不激活 PDP | 硬复位后在 ready 总超时内轮询 `AT`，成功后执行 `ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`、`AT*I`，再注册运行期 URC 并发布 ready |
| `reset` | 通过 EN 执行硬复位，并在 `AT OK` 后恢复基础 AT 工作环境 | 拉低 EN、等待 `reset_pulse_ms`、拉高 EN、轮询 `AT` 到 `OK`，再执行基础 AT 初始化命令 |
```

Also replace field comments that describe `rdy_sema`, `rdy_seen`, or `waiting_rdy` with no text; these fields should no longer be documented.

- [ ] **Step 5: Update `docs/agents/at_cmd_air780ep.md`**

Change the `RDY` row in the system URC table to:

```markdown
| `RDY` | 模块启动 | 模块重启过程中可能自发出现 | 当前初始化不消费；仅作为串口日志现象 | PDF 片段未系统列出，实机常见，但启动 gate 改为硬复位后 `AT OK` |
```

In the recommended initialization flow, insert the new first step:

```markdown
1. 硬复位后轮询 `AT`，直到返回 `OK`
```

Then keep `ATE0` as the next step.

- [ ] **Step 6: Run documentation static checks**

Run:

```bash
python3 tests/host/test_air780ep_command_gated_init.py -v
```

Expected: PASS.

Run:

```bash
python3 tests/host/test_lwlte_start_lifecycle.py -v
```

Expected: PASS.

---

### Task 7: Full Verification

**Files:**
- No new edits expected.

- [ ] **Step 1: Run all host regression tests**

Run:

```bash
python3 -m unittest discover -s tests/host -p 'test_*.py' -v
```

Expected: all tests pass.

- [ ] **Step 2: Build with ESP-IDF MCP**

Use the ESP-IDF build tool:

```text
esp-idf-eim_build_project
```

Expected: project builds successfully.

- [ ] **Step 3: Search for stale RDY-gated startup wording**

Run these searches:

```bash
rg "wait RDY|RDY wait|等待 RDY|等 RDY|RDY 等待超时" src docs/agents docs/modem-init-min-flow.md tests/host
```

Expected: no matches.

Run:

```bash
rg "wait_rdy|begin_wait_rdy|cancel_wait_rdy|rdy_sema|waiting_rdy|AIR780EP_URC_RDY" src/modem/modem_air780ep.c
```

Expected: no matches.

- [ ] **Step 4: Check final diff and status**

Run:

```bash
git status --short
git diff -- src/modem/modem_air780ep.c src/core/core_fsm.c src/include/lwlte.h docs/modem-init-min-flow.md docs/agents/architecture.md docs/agents/classes.md docs/agents/at_cmd_air780ep.md tests/host/test_air780ep_command_gated_init.py tests/host/test_air780ep_cpin_policy.py docs/superpowers/specs/2026-06-05-air780ep-command-gated-init-design.md docs/superpowers/plans/2026-06-05-air780ep-command-gated-init.md
```

Expected: diff only contains the planned test, source, docs, spec, and plan changes. No commit is made unless the user asks for it.
