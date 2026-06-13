# ML307R MQTTSTATE Query and MQTTPUB dup Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `AT+MQTTSTATE` status query support to `modem_ml307r` and make the `<dup>` parameter in `AT+MQTTPUB` explicit, mirroring the Air780EP MQTTSTATE implementation.

**Architecture:** Single-file change to `src/modem/modem_ml307r.c`. Reuses the `modem_mqtt_status_t` enum, `mqtt_get_status` ops slot, and `modem_mqtt_get_status()` wrapper that were added to the public/modem layer in the Air780EP task. ML307R's state values (1/2/3) are normalized to the existing enum via a `map_mqtt_status()` helper.

**Tech Stack:** ESP-IDF (C99), AT command engine, FreeRTOS.

**Spec:** `docs/superpowers/specs/2026-06-14-ml307r-mqttstate-dup-design.md`

---

### Task 1: Add Doxygen prototypes for new static functions

**Files:**
- Modify: `src/modem/modem_ml307r.c` (forward declarations around L125-126)

This task adds the forward declarations + Doxygen comments for the two new static functions (`map_mqtt_status` and `ml307r_mqtt_get_status`) so the rest of the file can reference them. No implementation yet.

- [ ] **Step 1: Read the current prototype section to find the exact insertion point**

Run: `grep -n "ml307r_mqtt_publish\|ml307r_ping\|to_ml307r" src/modem/modem_ml307r.c | head -5`

Expected: see L125-126 (`ml307r_mqtt_publish` prototype) followed by L127 (`ml307r_ping` prototype), then L132 (`to_ml307r`). The new prototypes go between `ml307r_mqtt_publish` (L126) and `ml307r_ping` (L127), keeping MQTT functions grouped.

- [ ] **Step 2: Add the two new prototypes with Doxygen comments**

Insert after L126 (the closing `);` of `ml307r_mqtt_publish`) and before L127 (`static esp_err_t ml307r_ping`):

```c
static esp_err_t ml307r_mqtt_publish(modem_handle_t *me,
                                      const modem_mqtt_publish_t *publish);
/**
 * @brief 查询 MQTT 连接状态
 * @details Query MQTT connection state via AT+MQTTSTATE
 * @param[in] me 调制解调器句柄
 * @param[out] status MQTT 状态枚举
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_FAIL: AT 命令失败
 *         - ESP_ERR_INVALID_RESPONSE: 响应解析失败
 */
static esp_err_t ml307r_mqtt_get_status(modem_handle_t *me,
                                         modem_mqtt_status_t *status);
/**
 * @brief 映射 MQTT 状态值
 * @details Map integer MQTT state (1/2/3) to enum; others map to OFFLINE
 * @param[in] state AT 状态值
 * @return MQTT 状态枚举
 */
static modem_mqtt_status_t map_mqtt_status(int state);
static esp_err_t ml307r_ping(modem_handle_t *me,
```

Note: `ml307r_ping` line is shown for context — it already exists, do not duplicate it. The insertion is the two Doxygen blocks + two prototype lines between `ml307r_mqtt_publish`'s closing `;` and `ml307r_ping`'s `static`.

- [ ] **Step 3: Verify build compiles (prototypes only, no implementations yet)**

Build will fail at link time because the functions are declared but not defined — but the compiler stage (syntax check) should pass. If the build system compiles `.c` to `.o` before linking, the `.o` for `modem_ml307r.c` should build successfully because the prototypes are not yet referenced anywhere.

Run: ESP-IDF MCP `build_project`
Expected: Success (prototypes are not called yet, so no link error). If a link error appears about undefined `ml307r_mqtt_get_status` or `map_mqtt_status`, it means something already references them — investigate before continuing.

- [ ] **Step 4: Commit**

```bash
git add src/modem/modem_ml307r.c
git commit -m "feat(ml307r): add MQTTSTATE op and map_mqtt_status prototypes"
```

---

### Task 2: Implement `map_mqtt_status()` helper

**Files:**
- Modify: `src/modem/modem_ml307r.c` (new function, placed near the MQTT implementation section)

This task adds the mapping function that converts ML307R's integer state values (1/2/3) into the shared `modem_mqtt_status_t` enum.

- [ ] **Step 1: Find the insertion point — after `ml307r_mqtt_publish` implementation**

Run: `grep -n "^static esp_err_t ml307r_mqtt_publish\|^static esp_err_t ml307r_ping" src/modem/modem_ml307r.c`

Expected: `ml307r_mqtt_publish` starts around L2464, `ml307r_ping` starts around L2529. The new `map_mqtt_status` goes between them (after `ml307r_mqtt_publish`'s closing `}`, before `ml307r_ping`).

- [ ] **Step 2: Read the end of `ml307r_mqtt_publish` to confirm exact insertion location**

Read lines 2525-2530 to find the `return ret;` + `}` closing `ml307r_mqtt_publish`, then the blank line, then `static esp_err_t ml307r_ping`.

- [ ] **Step 3: Insert `map_mqtt_status()` implementation**

Insert between the closing `}` of `ml307r_mqtt_publish` (around L2527) and `static esp_err_t ml307r_ping` (around L2529):

```c
static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 2:  return MODEM_MQTT_STATUS_AUTHENTICATED;
    case 1:  return MODEM_MQTT_STATUS_TCP_CONNECTED;
    case 3:  return MODEM_MQTT_STATUS_OFFLINE;
    default: return MODEM_MQTT_STATUS_OFFLINE;
    }
}
```

- [ ] **Step 4: Build to verify**

Run: ESP-IDF MCP `build_project`
Expected: Success (function is defined but not yet called — no link error).

- [ ] **Step 5: Commit**

```bash
git add src/modem/modem_ml307r.c
git commit -m "feat(ml307r): add map_mqtt_status normalization helper"
```

---

### Task 3: Implement `ml307r_mqtt_get_status()` op

**Files:**
- Modify: `src/modem/modem_ml307r.c` (new function, placed right after `map_mqtt_status`)

This task adds the op that sends `AT+MQTTSTATE=0`, parses the response, and returns the normalized status.

- [ ] **Step 1: Confirm the helpers this function depends on all exist**

Run: `grep -n "parse_int_after_prefix\|find_line_with_prefix\|ensure_at_ok\|ML307R_MQTT_CMD_TIMEOUT_MS\|ml307r_cmd_ctx_t" src/modem/modem_ml307r.c | head -10`

Expected: all four helpers + the timeout macro + the context type exist (they are used throughout the file).

- [ ] **Step 2: Insert `ml307r_mqtt_get_status()` implementation**

Insert immediately after `map_mqtt_status()` (added in Task 2) and before `ml307r_ping`:

```c
static esp_err_t ml307r_mqtt_get_status(modem_handle_t *me,
                                         modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_ml307r_t *self = to_ml307r(me);

    ml307r_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTSTATE=0", &ctx,
                             ML307R_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTSTATE=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MQTTSTATE=0 failed");

    const char *line = find_line_with_prefix(&ctx.response, "+MQTTSTATE");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                        "+MQTTSTATE line missing");

    int state = 0;
    ret = parse_int_after_prefix(line, "+MQTTSTATE", &state);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +MQTTSTATE failed");
    ESP_RETURN_ON_FALSE(state >= 1 && state <= 3, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid MQTT state %d", state);

    *status = map_mqtt_status(state);
    return ESP_OK;
}
```

- [ ] **Step 3: Build to verify**

Run: ESP-IDF MCP `build_project`
Expected: Success (function is defined; not yet registered in ops table so not yet callable via the wrapper, but compilation must pass).

- [ ] **Step 4: Commit**

```bash
git add src/modem/modem_ml307r.c
git commit -m "feat(ml307r): add ml307r_mqtt_get_status op implementation"
```

---

### Task 4: Register `mqtt_get_status` in `s_ml307r_ops`

**Files:**
- Modify: `src/modem/modem_ml307r.c` (ops table around L251)

This task wires the new op into the ops table so `modem_mqtt_get_status()` (the public wrapper in `modem.c`) can dispatch to it.

- [ ] **Step 1: Read the current ops table to confirm exact location**

Read lines 244-253. The MQTT section currently ends with `.mqtt_publish = ml307r_mqtt_publish,` (L251) followed by `.ping = ml307r_ping,` (L252).

- [ ] **Step 2: Add the `.mqtt_get_status` entry**

Edit the ops table to insert `.mqtt_get_status` between `.mqtt_publish` and `.ping`:

Old:
```c
    .mqtt_publish = ml307r_mqtt_publish,
    .ping = ml307r_ping,
```

New:
```c
    .mqtt_publish = ml307r_mqtt_publish,
    .mqtt_get_status = ml307r_mqtt_get_status,
    .ping = ml307r_ping,
```

- [ ] **Step 3: Build to verify**

Run: ESP-IDF MCP `build_project`
Expected: Success. The op is now registered; `modem_mqtt_get_status()` called on an ML307R modem will dispatch to `ml307r_mqtt_get_status()`.

- [ ] **Step 4: Commit**

```bash
git add src/modem/modem_ml307r.c
git commit -m "feat(ml307r): register mqtt_get_status op in s_ml307r_ops"
```

---

### Task 5: Fix `ml307r_mqtt_publish()` `<dup>` parameter

**Files:**
- Modify: `src/modem/modem_ml307r.c` (around L2495-2515)

This task makes the hardcoded `<dup>` value explicit in the `AT+MQTTPUB` format string. No behavioral change — the AT command produced is byte-identical.

- [ ] **Step 1: Read the current `ml307r_mqtt_publish` AT command construction**

Read lines 2494-2516 to see both `snprintf` calls (L2496 length measurement and L2511 actual write). Both use the format string `AT+MQTTPUB=0,\"%s\",%u,%u,0,%u,\"%s\"` with a hardcoded `0` in the `<dup>` position.

- [ ] **Step 2: Replace the comment and first `snprintf` (length measurement)**

Old (L2495-2499):
```c
    /* AT command shape: AT+MQTTPUB=0,"%s",%u,%u,0,%u,"%s". */
    int needed = snprintf(NULL, 0, "AT+MQTTPUB=0,\"%s\",%u,%u,0,%u,\"%s\"",
                           escaped_topic, (unsigned int)publish->qos,
                           publish->retain ? 1U : 0U,
                           (unsigned int)publish->payload_len, hex_payload);
```

New:
```c
    /* AT+MQTTPUB=<connect_id>,"<topic>",<qos>,<retain>,<dup>,<msg_len>,"<message>"
     * lwlte 只发新消息，<dup> 恒为 0；模组自动重传由 MQTTCFG="retrans" 控制。
     * lwlte only publishes new messages; <dup> is always 0. Module auto-retransmit
     * is controlled by MQTTCFG="retrans" and does not use this parameter. */
    const unsigned int dup_flag = 0U;
    int needed = snprintf(NULL, 0, "AT+MQTTPUB=0,\"%s\",%u,%u,%u,%u,\"%s\"",
                           escaped_topic, (unsigned int)publish->qos,
                           publish->retain ? 1U : 0U,
                           dup_flag,
                           (unsigned int)publish->payload_len, hex_payload);
```

- [ ] **Step 3: Replace the second `snprintf` (actual write)**

Old (L2511-2515):
```c
    snprintf(cmd, (size_t)needed + 1,
             "AT+MQTTPUB=0,\"%s\",%u,%u,0,%u,\"%s\"",
             escaped_topic, (unsigned int)publish->qos,
             publish->retain ? 1U : 0U,
             (unsigned int)publish->payload_len, hex_payload);
```

New:
```c
    snprintf(cmd, (size_t)needed + 1,
             "AT+MQTTPUB=0,\"%s\",%u,%u,%u,%u,\"%s\"",
             escaped_topic, (unsigned int)publish->qos,
             publish->retain ? 1U : 0U,
             dup_flag,
             (unsigned int)publish->payload_len, hex_payload);
```

Note: `dup_flag` was declared as `const unsigned int dup_flag = 0U;` in Step 2 — it is in scope for this second `snprintf` because both are inside the same function body.

- [ ] **Step 4: Build to verify**

Run: ESP-IDF MCP `build_project`
Expected: Success with zero warnings.

- [ ] **Step 5: Commit**

```bash
git add src/modem/modem_ml307r.c
git commit -m "refactor(ml307r): make MQTTPUB <dup> parameter explicit

Change format string from hardcoded 0 to named dup_flag=0U constant.
Behavior is identical; this improves readability and aligns the code
with the corrected AT command reference (AT+MQTTPUB has a mandatory
<dup> parameter between <retain> and <msg_len>)."
```

---

### Task 6: Build verification and final review

**Files:**
- No code changes — verification only

This task confirms the complete change set builds cleanly and the git log is coherent.

- [ ] **Step 1: Full build**

Run: ESP-IDF MCP `build_project`
Expected: Success with zero errors and zero warnings.

- [ ] **Step 2: Review the complete diff**

Run: `git log --oneline -6 && git diff HEAD~5 --stat`

Expected: 5 commits, all touching only `src/modem/modem_ml307r.c`. Diff stat should show roughly +60/-10 lines.

- [ ] **Step 3: Verify no stray debug code or TODOs**

Run: `git diff HEAD~5 -- src/modem/modem_ml307r.c`

Review the full diff for: no `printf`/`ESP_LOGI` debug additions beyond what the existing pattern uses, no `TODO`/`FIXME`/`XXX` comments, no commented-out code blocks.

- [ ] **Step 4: Verify the ops table has the new entry**

Run: `grep -A2 "mqtt_publish" src/modem/modem_ml307r.c | grep mqtt_get_status`

Expected: shows `.mqtt_get_status = ml307r_mqtt_get_status,` in the ops table.

- [ ] **Step 5: Verify the dup fix is present**

Run: `grep "dup_flag" src/modem/modem_ml307r.c`

Expected: 3 lines — 1 declaration (`const unsigned int dup_flag = 0U;`) and 2 usages (in the two `snprintf` calls).

---

## On-Device Verification (deferred to user)

The repository has no unit test framework for modem hardware interaction. These checks require physical ML307R hardware and are performed by the user after implementation:

1. **MQTTSTATE query**: Call `modem_mqtt_get_status()` at three points:
   - Before MQTT connect: expect `MODEM_MQTT_STATUS_OFFLINE` (normalized from state=3).
   - While MQTT is connecting: expect `MODEM_MQTT_STATUS_TCP_CONNECTED` (normalized from state=1).
   - After `MQTTCONN` succeeds (`+MQTTURC:"conn",0,0`): expect `MODEM_MQTT_STATUS_AUTHENTICATED` (normalized from state=2).

2. **MQTTPUB dup fix**: Publish a message via `modem_mqtt_publish()`. Verify the broker receives the correct payload (content identical to pre-fix behavior). Inspect monitor log to confirm the AT command format is `AT+MQTTPUB=0,"topic",0,0,0,<len>,"<hex>"` (dup=0 in the correct positional slot).
