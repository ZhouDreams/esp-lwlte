# Air780EP CPIN SIM Busy Polling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent early `AT+CPIN?` `+CME ERROR: 14` / SIM busy responses from causing an immediate `NET_ERROR` by polling CPIN at the documented Air780EP interval.

**Architecture:** Keep the fix inside the Air780EP modem adapter so Core does not need to understand module-specific CME codes. `air780ep_get_sim_status()` becomes responsible for classifying definite SIM errors and retrying only transient SIM busy responses. The public API and Core activation FSM remain unchanged.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS tasks/ticks, C, project serial monitor helper, Python 3 standard library for a focused host regression check.

---

## File Structure

- Create: `tests/host/test_air780ep_cpin_policy.py`
  - Static regression check for the Air780EP CPIN busy policy because this repository currently has no host or ESP-IDF unit test target.
  - Verifies production source contains the documented timeout, poll interval, CME constants, busy retry branch, and definite SIM CME mappings.
- Modify: `src/modem/modem_air780ep.c`
  - Add Air780EP private CPIN/CME constants.
  - Add small helpers for SIM status caching and definite CME-to-SIM-status mapping.
  - Replace `air780ep_get_sim_status()` with a loop that retries only `+CME ERROR: 14` at 1 second intervals for up to 10 seconds.
  - Preserve unrelated existing changes, including any current additions in `run_basic_init_cmds()`.
- Modify: `docs/agents/at_cmd_air780ep.md`
  - Document that `+CME ERROR: 14` from `AT+CPIN?` means SIM busy and should be polled rather than treated as immediate fatal failure.

Current workspace note: the worktree already contains unrelated changes in `AGENTS.md`, `src/modem/modem_air780ep.c`, and `docs/agents/cme_error_air780ep.md`. Do not revert or rewrite those changes while implementing this plan.

---

### Task 1: Add CPIN Busy Regression Test

**Files:**
- Create: `tests/host/test_air780ep_cpin_policy.py`

- [ ] **Step 1: Write the failing host regression test**

Create `tests/host/test_air780ep_cpin_policy.py` with this exact content:

```python
#!/usr/bin/env python3
"""Static regression checks for the Air780EP AT+CPIN? SIM busy policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src/modem/modem_air780ep.c"
DOC = ROOT / "docs/agents/at_cmd_air780ep.md"


class Air780EpCpinPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = SRC.read_text(encoding="utf-8")
        cls.doc = DOC.read_text(encoding="utf-8")

    def test_busy_polling_constants_are_defined(self):
        self.assertRegex(self.src, r"#define\s+AIR780EP_SIM_READY_TIMEOUT_MS\s+10000")
        self.assertRegex(self.src, r"#define\s+AIR780EP_SIM_READY_POLL_INTERVAL_MS\s+1000")
        self.assertRegex(self.src, r"#define\s+AIR780EP_CME_SIM_BUSY\s+14")

    def test_definite_cme_status_mapping_is_explicit(self):
        expected_pairs = [
            ("AIR780EP_CME_SIM_NOT_INSERTED", "MODEM_SIM_NOT_INSERTED"),
            ("AIR780EP_CME_SIM_PIN_REQUIRED", "MODEM_SIM_PIN_REQUIRED"),
            ("AIR780EP_CME_SIM_PUK_REQUIRED", "MODEM_SIM_PUK_REQUIRED"),
            ("AIR780EP_CME_SIM_FAILURE", "MODEM_SIM_ERROR"),
            ("AIR780EP_CME_SIM_WRONG", "MODEM_SIM_ERROR"),
        ]

        self.assertIn("static bool sim_status_from_cme_error", self.src)
        for cme_name, status_name in expected_pairs:
            self.assertIn(cme_name, self.src)
            self.assertIn(status_name, self.src)

    def test_air780ep_get_sim_status_retries_only_sim_busy(self):
        self.assertIn("ctx.response.status == AT_RESP_CME_ERROR", self.src)
        self.assertIn("ctx.response.error_code == AIR780EP_CME_SIM_BUSY", self.src)
        self.assertIn("AIR780EP_SIM_READY_POLL_INTERVAL_MS", self.src)
        self.assertIn("vTaskDelay(timeout_ticks(wait_ms))", self.src)
        self.assertIn("return ESP_ERR_TIMEOUT", self.src)

    def test_cpin_documentation_mentions_sim_busy_policy(self):
        self.assertIn("+CME ERROR: 14", self.doc)
        self.assertIn("SIM busy", self.doc)
        self.assertIn("1 秒", self.doc)
        self.assertIn("10 秒", self.doc)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and verify it fails before implementation**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py
```

Expected: FAIL. At least `test_busy_polling_constants_are_defined` fails because `AIR780EP_SIM_READY_TIMEOUT_MS` and `AIR780EP_SIM_READY_POLL_INTERVAL_MS` are not defined yet.

- [ ] **Step 3: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add tests/host/test_air780ep_cpin_policy.py
git commit -m "test(air780ep): cover CPIN SIM busy policy"
```

Expected: one commit containing only the new host regression test.

---

### Task 2: Add Air780EP CPIN Policy Constants And Helpers

**Files:**
- Modify: `src/modem/modem_air780ep.c:31-49`
- Modify: `src/modem/modem_air780ep.c:480-500`
- Modify: `src/modem/modem_air780ep.c:1360-1410`

- [ ] **Step 1: Add private constants near the existing Air780EP defines**

In `src/modem/modem_air780ep.c`, extend the define block near `AIR780EP_CIPSHUT_TIMEOUT_MS` with these lines:

```c
#define AIR780EP_SIM_READY_TIMEOUT_MS     10000
#define AIR780EP_SIM_READY_POLL_INTERVAL_MS 1000
#define AIR780EP_CME_SIM_NOT_INSERTED     10
#define AIR780EP_CME_SIM_PIN_REQUIRED     11
#define AIR780EP_CME_SIM_PUK_REQUIRED     12
#define AIR780EP_CME_SIM_FAILURE          13
#define AIR780EP_CME_SIM_BUSY             14
#define AIR780EP_CME_SIM_WRONG            15
```

Keep the existing `AIR780EP_URC_*` definitions below this block unchanged.

- [ ] **Step 2: Add helper prototypes near `parse_sim_status_line()`**

In the static prototype section, immediately after the `parse_sim_status_line()` prototype, add:

```c
/**
 * @brief 缓存 SIM 状态
 * @details Cache SIM status
 * @param[in] self Air780EP 调制解调器实例
 * @param[in] status SIM 状态
 */
static void cache_sim_status(modem_air780ep_t *self, modem_sim_status_t status);

/**
 * @brief 从 CME 错误码映射明确 SIM 状态
 * @details Map definite CME errors to SIM status
 * @param[in] error_code CME 错误码
 * @param[out] status SIM 状态
 * @return true: 已映射为明确 SIM 状态； false: 非明确 SIM 状态
 */
static bool sim_status_from_cme_error(int error_code, modem_sim_status_t *status);
```

- [ ] **Step 3: Add helper implementations after `parse_sim_status_line()`**

In `src/modem/modem_air780ep.c`, immediately after `parse_sim_status_line()` ends and before `query_cgatt()`, add:

```c
static void cache_sim_status(modem_air780ep_t *self, modem_sim_status_t status)
{
    if (!self) {
        return;
    }

    if (!self->base.lock) {
        self->last_sim_status = status;
        return;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->last_sim_status = status;
    xSemaphoreGive(self->base.lock);
}

static bool sim_status_from_cme_error(int error_code, modem_sim_status_t *status)
{
    if (!status) {
        return false;
    }

    switch (error_code) {
    case AIR780EP_CME_SIM_NOT_INSERTED:
        *status = MODEM_SIM_NOT_INSERTED;
        return true;
    case AIR780EP_CME_SIM_PIN_REQUIRED:
        *status = MODEM_SIM_PIN_REQUIRED;
        return true;
    case AIR780EP_CME_SIM_PUK_REQUIRED:
        *status = MODEM_SIM_PUK_REQUIRED;
        return true;
    case AIR780EP_CME_SIM_FAILURE:
    case AIR780EP_CME_SIM_WRONG:
        *status = MODEM_SIM_ERROR;
        return true;
    default:
        return false;
    }
}
```

- [ ] **Step 4: Run the host regression test and verify remaining failures**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py
```

Expected: FAIL. Constant and CME mapping assertions should now pass, while the retry branch and documentation assertions still fail.

---

### Task 3: Implement CPIN SIM Busy Polling

**Files:**
- Modify: `src/modem/modem_air780ep.c:2063-2087`

- [ ] **Step 1: Replace `air780ep_get_sim_status()` with polling implementation**

Replace the entire existing `air780ep_get_sim_status()` function with:

```c
static esp_err_t air780ep_get_sim_status(modem_t *me, modem_sim_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    const uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    bool sim_busy_seen = false;

    while (true) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, "AT+CPIN?", &ctx, 0);
        ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CPIN? failed");

        if (ctx.response.status == AT_RESP_OK) {
            const char *line = find_line_with_prefix(&ctx.response, "+CPIN:");
            ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                                "+CPIN line missing");

            modem_sim_status_t parsed = parse_sim_status_line(line);
            cache_sim_status(self, parsed);
            *status = parsed;
            return ESP_OK;
        }

        if (ctx.response.status == AT_RESP_CME_ERROR) {
            modem_sim_status_t parsed = MODEM_SIM_UNKNOWN;
            if (sim_status_from_cme_error(ctx.response.error_code, &parsed)) {
                cache_sim_status(self, parsed);
                *status = parsed;
                return ESP_OK;
            }

            if (ctx.response.error_code == AIR780EP_CME_SIM_BUSY) {
                sim_busy_seen = true;
                uint32_t elapsed_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) -
                    start_ms;
                if (elapsed_ms >= AIR780EP_SIM_READY_TIMEOUT_MS) {
                    break;
                }

                uint32_t remaining_ms = AIR780EP_SIM_READY_TIMEOUT_MS - elapsed_ms;
                uint32_t wait_ms = AIR780EP_SIM_READY_POLL_INTERVAL_MS;
                if (remaining_ms < wait_ms) {
                    wait_ms = remaining_ms;
                }
                if (wait_ms == 0) {
                    break;
                }

                ESP_LOGW(TAG, "AT+CPIN? returned SIM busy, retry in %u ms",
                         (unsigned int)wait_ms);
                vTaskDelay(timeout_ticks(wait_ms));
                continue;
            }
        }

        ret = ensure_at_ok(&ctx.response, "AT+CPIN?");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CPIN? failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (sim_busy_seen) {
        cache_sim_status(self, MODEM_SIM_UNKNOWN);
        *status = MODEM_SIM_UNKNOWN;
        ESP_LOGE(TAG, "AT+CPIN? SIM busy timeout after %u ms",
                 (unsigned int)AIR780EP_SIM_READY_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_FAIL;
}
```

- [ ] **Step 2: Run the host regression test and verify documentation remains failing**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py
```

Expected: FAIL only in `test_cpin_documentation_mentions_sim_busy_policy`, because production code now contains the busy polling policy but the Air780EP AT command doc has not been updated yet.

- [ ] **Step 3: Build-check the C change**

Run:

```bash
source "$HOME/.espressif/v6.0/esp-idf/export.sh" && idf.py -C examples/basic_connect build
```

Expected: build completes successfully with no C compiler errors from `src/modem/modem_air780ep.c`.

---

### Task 4: Update Air780EP AT Command Documentation

**Files:**
- Modify: `docs/agents/at_cmd_air780ep.md:76`

- [ ] **Step 1: Replace the `AT+CPIN?` table row**

In `docs/agents/at_cmd_air780ep.md`, replace the existing `AT+CPIN?` row with this row:

```markdown
| 查询 PIN/SIM 状态 | `AT+CPIN?` | `+CPIN: <code>` + `OK`；SIM 初始化忙时可能返回 `+CME ERROR: 14` | `READY` 可用；`SIM PIN` 等待 PIN；`SIM REMOVED` 未检出；`+CME ERROR: 14` 为 `SIM busy` | 9s | SIM-ready 初始化内部步骤 | 单次查询保持 9s；SIM-ready 等待应由上层轮询/重试总预算实现，而不是一次 `AT+CPIN?` 等待 180s。按合宙快速入门建议，遇到 SIM busy 应每 1 秒轮询，10 秒内仍未 ready 再按超时处理；也存在 URC `+CPIN:<code>` |
```

- [ ] **Step 2: Run the host regression test and verify it passes**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py
```

Expected: PASS with output ending in `OK`.

- [ ] **Step 3: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add src/modem/modem_air780ep.c docs/agents/at_cmd_air780ep.md tests/host/test_air780ep_cpin_policy.py
git commit -m "fix(air780ep): poll CPIN while SIM is busy"
```

Expected: one commit containing the modem CPIN policy, documentation update, and regression test.

---

### Task 5: Verify Build And Hardware Behavior

**Files:**
- Read only: `docs/agents/build-and-debug.md`
- Runtime verification: `examples/basic_connect`

- [ ] **Step 1: Run full example build**

Run:

```bash
source "$HOME/.espressif/v6.0/esp-idf/export.sh" && idf.py -C examples/basic_connect build
```

Expected: build succeeds and produces `examples/basic_connect/build/basic_connect.bin`.

- [ ] **Step 2: Check current USB serial port**

Run:

```bash
ls /dev/tty.usb* /dev/cu.usb* 2>/dev/null
```

Expected: output includes `/dev/cu.usbserial-1130` and `/dev/tty.usbserial-1130` on the current development machine. If the suffix differs, use the printed `/dev/cu.usbserial-*` path in Step 3.

- [ ] **Step 3: Monitor serial output for 30 seconds**

Run:

```bash
source "$HOME/.espressif/v6.0/esp-idf/export.sh" && python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-1130
```

Expected when the SIM is initially busy: repeated `AT+CPIN?` attempts are separated by approximately 1 second, not by tens of milliseconds. Expected successful end state remains:

```text
LTE event: NET_ONLINE net=ONLINE err=0
periodic: lte=ONLINE net=ONLINE
```

- [ ] **Step 4: Verify no immediate false NET_ERROR from SIM busy**

Inspect the serial log. Expected: no `LTE event: NET_ERROR` is emitted solely because the first few `AT+CPIN?` responses are `+CME ERROR: 14`. If `NET_ERROR` appears after roughly 10 seconds of continuous SIM busy, that matches the timeout policy.

- [ ] **Step 5: Capture final working tree summary**

Run:

```bash
git status --short
```

Expected: only intended files from this plan are modified or created, plus any unrelated pre-existing user changes that were already present before implementation.
