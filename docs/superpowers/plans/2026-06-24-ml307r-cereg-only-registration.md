# ML307R CEREG-Only Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove ML307R active `CGREG`/`CREG` dependencies so startup and registration polling use only `CEREG`, matching the tested ML307R firmware.

**Architecture:** Keep this change ML307R-specific. Update the ML307R static contract first, then change `modem_ml307r.c` registration init/query/URC wiring to the CEREG-only path. Update ML307R docs to describe the tested firmware behavior while leaving Air780EP behavior unchanged.

**Tech Stack:** C, ESP-IDF, Python `unittest`, Markdown documentation.

---

## File Structure

- Modify `tests/host/test_ml307r_contract.py`: change ML307R static expectations so startup, registration queries, and URC registration no longer require `CGREG` or `CREG`.
- Modify `src/modem/modem_ml307r.c`: remove ML307R `CGREG/CREG` init commands, query fallback entries, and runtime URC registration/unregistration entries.
- Modify `docs/agents/at_cmd_ml307r.md`: document that the tested ML307R firmware uses CEREG only for active registration status and that `CGREG/CREG` return `ERROR` on the test module.
- Modify `docs/modem-init-min-flow.md`: update the ML307R minimum flow and module comparison rows to CEREG-only while preserving Air780EP broader behavior.

No commit is included in required tasks. Repository policy requires explicit user authorization before every `git commit`.

### Task 1: Update ML307R Static Contract

**Files:**
- Modify: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Update startup init command expectations**

In `test_basic_init_commands_and_start_reset_order()`, replace:

```python
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"', '"AT+CGREG=2"', '"AT+CREG=2"']
```

with:

```python
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"']
```

Immediately after the `for token in expected_order` loop, add:

```python
        self.assertNotIn('"AT+CGREG=2"', init_body)
        self.assertNotIn('"AT+CREG=2"', init_body)
```

- [ ] **Step 2: Update registration query expectations**

In `test_identity_status_and_registration_mapping_exists()`, remove these two tokens from the token list:

```python
            "AT+CGREG?",
            "AT+CREG?",
```

After the token loop in the same test, add:

```python
        reg_body = function_body(
            self.ml307r_c,
            "static esp_err_t ml307r_get_registration(modem_handle_t *me,",
        )
        self.assertIn('"AT+CEREG?"', reg_body)
        self.assertNotIn('"AT+CGREG?"', reg_body)
        self.assertNotIn('"AT+CREG?"', reg_body)
```

- [ ] **Step 3: Update URC registration expectations**

In `test_urc_registration_and_callback_constraints()`, replace the token list:

```python
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
```

with:

```python
        for token in [
            "ML307R_URC_CPIN",
            "ML307R_URC_CEREG",
            "ML307R_URC_MIPCALL",
            "ML307R_URC_MQTTURC",
            "at_engine_register_urc",
        ]:
            self.assertIn(token, register_body)
        self.assertNotIn("ML307R_URC_CREG", register_body)
        self.assertNotIn("ML307R_URC_CGREG", register_body)
```

Add an unregister-body check below the register-body assertions:

```python
        unregister_body = function_body(
            self.ml307r_c,
            "static esp_err_t ml307r_unregister_urcs(modem_ml307r_t *self)",
        )
        self.assertIn("ML307R_URC_CEREG", unregister_body)
        self.assertNotIn("ML307R_URC_CREG", unregister_body)
        self.assertNotIn("ML307R_URC_CGREG", unregister_body)
```

- [ ] **Step 4: Run the updated static contract and verify it fails**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_contract.py -v
```

Expected: FAIL. Failures should point to `AT+CGREG=2` / `AT+CREG=2` still being present in `run_basic_init_cmds()`, `AT+CGREG?` / `AT+CREG?` still being present in `ml307r_get_registration()`, or `ML307R_URC_CREG` / `ML307R_URC_CGREG` still being present in ML307R URC registration.

### Task 2: Make ML307R Registration CEREG-Only

**Files:**
- Modify: `src/modem/modem_ml307r.c`
- Test: `tests/host/test_ml307r_contract.py`

- [ ] **Step 1: Remove unused ML307R CREG/CGREG defines**

In the defines section near the top of `src/modem/modem_ml307r.c`, replace:

```c
#define ML307R_URC_CREG                  "+CREG:"
#define ML307R_URC_CEREG                 "+CEREG:"
#define ML307R_URC_CGREG                 "+CGREG:"
```

with:

```c
#define ML307R_URC_CEREG                 "+CEREG:"
```

- [ ] **Step 2: Update the registration prototype comment**

In the comment for `ml307r_get_registration()`, replace:

```c
 * @details Query EPS/GSM/CS registration via AT+CEREG?, AT+CGREG? and AT+CREG?;
 *          the first non-UNKNOWN result wins and updates the modem state
```

with:

```c
 * @details Query EPS/LTE registration via AT+CEREG? and update the modem state
```

- [ ] **Step 3: Remove unsupported init commands**

In `run_basic_init_cmds()`, replace:

```c
    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
        "AT+CGREG=2",
        "AT+CREG=2",
    };
```

with:

```c
    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
    };
```

- [ ] **Step 4: Remove unsupported ML307R URC handlers from registration**

In `register_urcs()`, replace the start of the `urcs[]` initializer:

```c
        { ML307R_URC_CPIN, &self->cpin_handler, cpin_urc_handler },
        { ML307R_URC_CREG, &self->creg_handler, reg_urc_handler },
        { ML307R_URC_CEREG, &self->cereg_handler, reg_urc_handler },
        { ML307R_URC_CGREG, &self->cgreg_handler, reg_urc_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler, mipcall_urc_handler },
```

with:

```c
        { ML307R_URC_CPIN, &self->cpin_handler, cpin_urc_handler },
        { ML307R_URC_CEREG, &self->cereg_handler, reg_urc_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler, mipcall_urc_handler },
```

In `ml307r_unregister_urcs()`, replace the start of its `urcs[]` initializer:

```c
        { ML307R_URC_CPIN, &self->cpin_handler },
        { ML307R_URC_CREG, &self->creg_handler },
        { ML307R_URC_CEREG, &self->cereg_handler },
        { ML307R_URC_CGREG, &self->cgreg_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler },
```

with:

```c
        { ML307R_URC_CPIN, &self->cpin_handler },
        { ML307R_URC_CEREG, &self->cereg_handler },
        { ML307R_URC_MIPCALL, &self->mipcall_handler },
```

- [ ] **Step 5: Make `ml307r_get_registration()` query only CEREG**

Replace the `queries[]` block in `ml307r_get_registration()`:

```c
    const struct {
        const char *cmd;
        const char *prefix;
    } queries[] = {
        { "AT+CEREG?", ML307R_URC_CEREG },
        { "AT+CGREG?", ML307R_URC_CGREG },
        { "AT+CREG?", ML307R_URC_CREG },
    };
```

with:

```c
    const struct {
        const char *cmd;
        const char *prefix;
    } queries[] = {
        { "AT+CEREG?", ML307R_URC_CEREG },
    };
```

- [ ] **Step 6: Run ML307R contract and verify it passes**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_contract.py -v
```

Expected: OK.

### Task 3: Update ML307R Registration Documentation

**Files:**
- Modify: `docs/agents/at_cmd_ml307r.md`
- Modify: `docs/modem-init-min-flow.md`
- Test: documentation grep/static review

- [ ] **Step 1: Update `docs/agents/at_cmd_ml307r.md` SIM/network table**

In the SIM/network state table, replace the three registration rows:

```markdown
| 网络注册状态 | `AT+CREG=<n>`，`AT+CREG?` | `+CREG: <n>,<stat>[,<lac>,<ci>,<act>]` + `OK` | `stat=1/5` 已注册；`2` 搜索；`3` 拒绝 | 9s | 通用注册状态和 URC | 同前缀也用于查询响应 |
| EPS 注册状态 | `AT+CEREG=<n>`，`AT+CEREG?` | `+CEREG: <n>,<stat>[,<tac>,<ci>,<act>...]` + `OK` | LTE/EPS 注册状态；`stat=1/5` 可用 | 9s | ML307R LTE 主注册状态 | 建议初始化启用 URC，再轮询查询兜底 |
| GPRS 注册状态 | `AT+CGREG=<n>`，`AT+CGREG?` | `+CGREG: <n>,<stat>[,<lac>,<ci>[,...]]` + `OK` | 分组域注册状态 | 9s | 数据域注册状态和 URC | 同前缀也用于查询响应 |
```

with:

```markdown
| EPS 注册状态 | `AT+CEREG=<n>`，`AT+CEREG?` | `+CEREG: <n>,<stat>[,<tac>,<ci>,<act>...]` + `OK` | LTE/EPS 注册状态；`stat=1/5` 可用 | 9s | ML307R 当前注册主路径 | 实测 ML307R 固件支持 CEREG；`CGREG/CREG` 命令族返回 `ERROR`，实现不要主动调用 |
```

- [ ] **Step 2: Update `docs/agents/at_cmd_ml307r.md` system URC table**

Replace these rows:

```markdown
| `+CREG:` | 网络注册 | CREG URC 开启后注册状态变化 | 更新通用注册状态 | 同时也是查询响应前缀 |
| `+CEREG:` | EPS 注册 | CEREG URC 开启后注册状态变化 | 优先用于 LTE 注册状态 | 同时也是查询响应前缀 |
| `+CGREG:` | GPRS 注册 | CGREG URC 开启后分组域注册状态变化 | 更新分组域注册状态 | 同时也是查询响应前缀 |
```

with:

```markdown
| `+CEREG:` | EPS 注册 | CEREG URC 开启后注册状态变化 | ML307R 当前唯一主动注册状态事件 | 同时也是 `AT+CEREG?` 查询响应前缀；实测 `CGREG/CREG` 不可用 |
```

- [ ] **Step 3: Update `docs/agents/at_cmd_ml307r.md` recommended registration flow**

Replace the registration and SIM check list:

```markdown
1. `AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2` 启用注册状态 URC。
2. 轮询 `AT+CPIN?`，要求 `+CPIN: READY`。
3. `AT+CFUN?`，确认驻网前功能模式为 `1`。
4. `AT+CSQ` 或 `AT+CESQ` 检查信号。
5. 轮询 `AT+CEREG?` 或 `AT+CGREG?`，要求 `stat=1` 或 `stat=5`。
6. `AT+CGATT?`，要求 `+CGATT: 1`。
7. 可选 `AT+MUESTATS="radio"` 或 `AT+MUESTATS="cell"` 打印诊断。
```

with:

```markdown
1. `AT+CEREG=2` 启用 LTE/EPS 注册状态 URC。
2. 轮询 `AT+CPIN?`，要求 `+CPIN: READY`。
3. `AT+CFUN?`，确认驻网前功能模式为 `1`。
4. `AT+CSQ` 或 `AT+CESQ` 检查信号。
5. 轮询 `AT+CEREG?`，要求 `stat=1` 或 `stat=5`。
6. `AT+CGATT?`，要求 `+CGATT: 1`。
7. 可选 `AT+MUESTATS="radio"` 或 `AT+MUESTATS="cell"` 打印诊断。
```

- [ ] **Step 4: Update `docs/modem-init-min-flow.md` ML307R flow**

In the ML307R minimum flow table and surrounding ML307R rows, update ML307R wording so it says:

```markdown
| Enable registration URC | `AT+CEREG=2` | 返回 `OK` | 只开启 LTE/EPS 注册变化 URC | 当前实测 ML307R 固件 `AT+CGREG*` 和 `AT+CREG*` 返回 `ERROR`，不要在主动初始化路径调用。 |
| Registered | `AT+CEREG?` 轮询 | `stat=1` 或 `stat=5` | 需先 `AT+CEREG=<n>` 才有注册变化 URC | 通信流程手册明确正常上电后自动驻网，可用 `AT+CEREG?` 查询是否成功。 |
```

In the ML307R URC classification table, remove the `+CREG:` and `+CGREG:` ML307R rows, and update the `+CEREG:` row note to:

```markdown
| `+CEREG:` | 否，除非已开启 | `AT+CEREG=<n>`，`n=1/2/3/4/5` | LTE/EPS 注册状态变化 | ML307R 当前主动注册路径只使用 CEREG；同前缀也用于查询响应。 |
```

In the module-difference quick table, replace the ML307R cells so they state:

```markdown
| LTE 注册主判断 | `CEREG`，辅以 `CGREG/CREG` | `CEREG` only（当前实测 `CGREG/CREG` 返回 `ERROR`） |
| 注册 URC 开启 | `AT+CEREG=<n>`、`AT+CGREG=<n>`、`AT+CREG=<n>` | `AT+CEREG=<n>` |
```

In the recommended minimum implementation strategy, replace:

```markdown
2. 进入 `CONFIG_AT`，关闭 echo，开启 `CMEE=1`，设置注册 URC 为 `CEREG=2`、`CGREG=2`、`CREG=2`。
4. 进入 `WAIT_REGISTERED`，消费 `+CEREG/+CGREG/+CREG`，同时周期查询 `AT+CEREG?`，直到 `stat=1/5`。
```

with:

```markdown
2. 进入 `CONFIG_AT`，关闭 echo，开启 `CMEE=1`。Air780EP 设置注册 URC 为 `CEREG=2`、`CGREG=2`、`CREG=2`；ML307R 只设置 `CEREG=2`。
4. 进入 `WAIT_REGISTERED`。Air780EP 消费 `+CEREG/+CGREG/+CREG`；ML307R 只消费 `+CEREG`。两者都周期查询 `AT+CEREG?`，直到 `stat=1/5`。
```

- [ ] **Step 5: Run focused documentation searches**

Run:

```bash
rg "ML307R.*CGREG|ML307R.*CREG|AT\+CGREG=2|AT\+CREG=2|CGREG/CREG" docs/agents/at_cmd_ml307r.md docs/modem-init-min-flow.md
```

Expected: remaining matches, if any, must state that ML307R `CGREG/CREG` is unsupported or Air780EP-only. There must be no ML307R recommendation to actively call `AT+CGREG=2`, `AT+CREG=2`, `AT+CGREG?`, or `AT+CREG?`.

### Task 4: Verification And Hardware Smoke Test

**Files:**
- Verify only: `tests/host/test_ml307r_contract.py`
- Verify only: `tests/host/test_air780ep_command_gated_init.py`
- Verify only: `src/modem/modem_ml307r.c`
- Verify only: `docs/agents/at_cmd_ml307r.md`
- Verify only: `docs/modem-init-min-flow.md`

- [ ] **Step 1: Run host tests**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_contract.py -v
python3 -m unittest tests/host/test_air780ep_command_gated_init.py -v
```

Expected: both commands end with `OK`.

- [ ] **Step 2: Run whitespace validation**

Run:

```bash
git diff --check
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Build for ESP32-C3**

Use MCP tools:

```text
esp-idf-eim_set_target target=esp32c3
esp-idf-eim_build_project
```

Expected: target selection succeeds and build succeeds.

- [ ] **Step 4: Flash and monitor ML307R basic connect**

Ensure `example/main.c` selects ML307R basic connect:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_BASIC_CONNECT
```

Build and flash with MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 45 --port /dev/cu.usbserial-1130
```

Expected: serial log must show `AT+CEREG=2` and must not show `AT+CGREG=2` or `AT+CREG=2`. The log should pass beyond the previous failure point where `AT+CGREG=2` returned `ERROR`. If later stages fail, record the new stage separately.

### Task 5: Optional User-Authorized Commit

**Files:**
- Stage only after user authorization:
- `docs/superpowers/specs/2026-06-24-ml307r-cereg-only-registration-design.md`
- `docs/superpowers/plans/2026-06-24-ml307r-cereg-only-registration.md`
- `tests/host/test_ml307r_contract.py`
- `src/modem/modem_ml307r.c`
- `docs/agents/at_cmd_ml307r.md`
- `docs/modem-init-min-flow.md`

- [ ] **Step 1: Ask for explicit commit authorization**

Ask the user whether to commit these changes. Do not commit unless the user explicitly says to commit.

- [ ] **Step 2: Inspect status and diff before committing**

Run:

```bash
git status --short
git diff -- docs/superpowers/specs/2026-06-24-ml307r-cereg-only-registration-design.md docs/superpowers/plans/2026-06-24-ml307r-cereg-only-registration.md tests/host/test_ml307r_contract.py src/modem/modem_ml307r.c docs/agents/at_cmd_ml307r.md docs/modem-init-min-flow.md
git log --oneline -10
```

Expected: the diff contains only the CEREG-only registration fix, docs, tests, and spec/plan files. Existing unrelated local edits remain unstaged.

- [ ] **Step 3: Commit only after authorization**

Run only after explicit user approval:

```bash
git add docs/superpowers/specs/2026-06-24-ml307r-cereg-only-registration-design.md docs/superpowers/plans/2026-06-24-ml307r-cereg-only-registration.md tests/host/test_ml307r_contract.py src/modem/modem_ml307r.c docs/agents/at_cmd_ml307r.md docs/modem-init-min-flow.md
git commit -m "fix(modem): use CEREG-only registration for ML307R"
```

Expected: commit succeeds and does not include unrelated local changes.
