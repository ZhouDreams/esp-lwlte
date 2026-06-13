# Air780EP MQTT MSGSET/MODE/STATU Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement three Air780EP MQTT AT commands in `modem_air780ep`: defensive mode reset (`AT+MQTTMSGSET=0` + `AT+MQTTMODE=0`) inside `mqtt_configure`, and a status query op (`AT+MQTTSTATU`) exposed at the modem layer.

**Architecture:** The defensive commands are fixed-value and embedded at the start of `air780ep_mqtt_configure()` (after the connected check, before config copy) with hard-fail semantics. The status query follows the existing `modem_get_info()` pattern: new public type + ops slot + wrapper + Air780EP implementation. No changes to mqtt_client FSM, Core, or lwlte facade.

**Tech Stack:** ESP-IDF v5.x, C99, FreeRTOS, existing `modem` OOP layer (ops vtable + `modem_base_init`), existing AT engine helpers (`send_cmd`, `ensure_at_ok`, `find_line_with_prefix`, `parse_int_after_prefix`).

**Spec:** `docs/superpowers/specs/2026-06-13-air780ep-mqtt-msgset-mode-statu-design.md`

**Verification approach:** This codebase has no unit-test framework for modem hardware interaction. Each task gates on `idf.py build` passing with zero errors/warnings. A final on-device verification checklist (Task 6) covers runtime behavior.

**Commit policy:** Do NOT run `git commit` anywhere in this plan. The user commits manually after review. `git add` for inspection is fine.

---

## File Structure

| File | Responsibility | Change Type |
|------|----------------|-------------|
| `src/modem/modem.h` | Public modem types & API | Add `modem_mqtt_status_t` enum + `modem_mqtt_get_status()` prototype |
| `src/modem/modem_priv.h` | Internal ops vtable | Add `modem_mqtt_get_status_fn` typedef + `mqtt_get_status` slot |
| `src/modem/modem.c` | Wrapper functions calling ops | Add `modem_mqtt_get_status()` wrapper |
| `src/modem/modem_air780ep.c` | Air780EP concrete implementation | Add `reset_mqtt_modes()`, `air780ep_mqtt_get_status()`, `map_mqtt_status()`; register op; modify `air780ep_mqtt_configure()` |

Tasks are ordered by dependency: type definitions (Task 1) before ops slot (Task 2) before implementation (Tasks 3-5), so the build stays compilable at each task boundary.

---

## Task 1: Add `modem_mqtt_status_t` enum and prototype to `modem.h`

**Files:**
- Modify: `src/modem/modem.h` (insert after `modem_mqtt_publish_t` struct, before `modem_ping_request_t`)

- [ ] **Step 1: Locate the insertion point**

The new enum goes in the MQTT-related types region. `modem_mqtt_publish_t` ends at L159, followed by a blank line, then `modem_ping_request_t` doxygen at L162. Insert the new enum between them.

- [ ] **Step 2: Add the enum definition**

In `src/modem/modem.h`, replace this block (L160-162):

```c


/**
 * @brief Ping 请求参数
```

with:

```c


/**
 * @brief MQTT 模块硬件状态
 * @details MQTT module hardware status
 * @note 对应 Air780EP AT+MQTTSTATU 查询结果。
 */
typedef enum {
    MODEM_MQTT_STATUS_OFFLINE = 0,       /**< 离线； Offline */
    MODEM_MQTT_STATUS_AUTHENTICATED,     /**< 已认证，可发布； Authenticated, can publish */
    MODEM_MQTT_STATUS_TCP_CONNECTED,     /**< TCP 已连接，未认证； TCP connected, not authenticated */
} modem_mqtt_status_t;

/**
 * @brief Ping 请求参数
```

- [ ] **Step 3: Add the `modem_mqtt_get_status()` prototype**

The prototype goes after `modem_mqtt_publish()` (which ends at L566) and before the `modem_ping()` doxygen block (starts at L568). In `src/modem/modem.h`, replace this block (L566-568):

```c
                              const modem_mqtt_publish_t *publish);

/**
 * @brief 执行 Ping 诊断
```

with:

```c
                              const modem_mqtt_publish_t *publish);

/**
 * @brief 查询 MQTT 模块硬件状态
 * @details Query MQTT module hardware status
 * @param[in] me 调制解调器句柄
 * @param[out] status MQTT 状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_mqtt_get_status(modem_handle_t *me, modem_mqtt_status_t *status);

/**
 * @brief 执行 Ping 诊断
```

- [ ] **Step 4: Build to verify header compiles**

Run: `idf.py build`
Expected: build succeeds (the new type and prototype are declared but unused; no warning because they are public API declarations).

---

## Task 2: Add `modem_mqtt_get_status_fn` typedef and ops slot to `modem_priv.h`

**Files:**
- Modify: `src/modem/modem_priv.h` (add typedef near L121, add ops slot near L164)

- [ ] **Step 1: Add the function pointer typedef**

In `src/modem/modem_priv.h`, the MQTT-related typedefs are `modem_mqtt_configure_fn` (L106), `modem_mqtt_topic_fn` (L113), `modem_mqtt_publish_fn` (L120). Add the new typedef after `modem_mqtt_publish_fn`. Replace this block (L120-124):

```c
typedef esp_err_t (*modem_mqtt_publish_fn)(modem_handle_t *me,
                                           const modem_mqtt_publish_t *publish);

/**
 * @brief Ping 诊断操作函数
```

with:

```c
typedef esp_err_t (*modem_mqtt_publish_fn)(modem_handle_t *me,
                                           const modem_mqtt_publish_t *publish);

/**
 * @brief 查询 MQTT 状态操作函数
 * @details Query MQTT status operation function
 */
typedef esp_err_t (*modem_mqtt_get_status_fn)(modem_handle_t *me,
                                              modem_mqtt_status_t *status);

/**
 * @brief Ping 诊断操作函数
```

- [ ] **Step 2: Add the ops table slot**

In the `modem_ops_t` struct, the MQTT section ends with `mqtt_publish` (L164) and the diagnostics section starts with `ping` (L167). Add the new slot between them. Replace this block (L164-167):

```c
    modem_mqtt_publish_fn mqtt_publish;              /**< 发布 MQTT 消息； Publish MQTT message */

    /* ── 诊断； Diagnostics ──────────────────────────────── */
    modem_ping_fn ping;                              /**< 执行 Ping 诊断； Execute ping diagnostic */
```

with:

```c
    modem_mqtt_publish_fn mqtt_publish;              /**< 发布 MQTT 消息； Publish MQTT message */
    modem_mqtt_get_status_fn mqtt_get_status;        /**< 查询 MQTT 状态； Query MQTT status */

    /* ── 诊断； Diagnostics ──────────────────────────────── */
    modem_ping_fn ping;                              /**< 执行 Ping 诊断； Execute ping diagnostic */
```

- [ ] **Step 3: Build to verify**

Run: `idf.py build`
Expected: build **fails** with a warning/error about `s_air780ep_ops` missing the `mqtt_get_status` initializer (designated initializer requires all non-defaulted fields once the struct has a new member that is not at the end... actually C allows partial initialization, fields not listed are zero-initialized). 

**Correction:** C99 designated initializers zero-initialize unlisted fields, so `s_air780ep_ops` (which does not list `mqtt_get_status`) will compile with `mqtt_get_status = NULL`. Build should **succeed**. Expected: build succeeds. (The NULL pointer is safe because `modem_mqtt_get_status()` wrapper checks `me->ops->mqtt_get_status` before dereferencing.)

---

## Task 3: Add `modem_mqtt_get_status()` wrapper to `modem.c`

**Files:**
- Modify: `src/modem/modem.c` (insert after `modem_mqtt_publish()`, before `modem_ping()`)

- [ ] **Step 1: Locate the insertion point**

`modem_mqtt_publish()` ends at L566 (closing brace), followed by a blank line, then `modem_ping()` doxygen at L568. The wrapper goes between them.

- [ ] **Step 2: Add the wrapper function**

In `src/modem/modem.c`, replace this block (L566-568):

```c
    return me->ops->mqtt_publish(me, publish);
}

/**
 * @brief 执行 Ping 诊断
```

with:

```c
    return me->ops->mqtt_publish(me, publish);
}

esp_err_t modem_mqtt_get_status(modem_handle_t *me, modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_get_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_get_status not supported");
    return me->ops->mqtt_get_status(me, status);
}

/**
 * @brief 执行 Ping 诊断
```

This follows the exact pattern of `modem_get_info()` (L403) and `modem_mqtt_publish()` (L553-566): arg check, `check_ready`, ops-existence check, call through vtable.

- [ ] **Step 3: Build to verify**

Run: `idf.py build`
Expected: build succeeds. The wrapper is defined but calls through a NULL op pointer at runtime — that's expected; the Air780EP implementation is added in Task 5.

---

## Task 4: Add `reset_mqtt_modes()` and wire it into `air780ep_mqtt_configure()`

**Files:**
- Modify: `src/modem/modem_air780ep.c` (add static prototype ~L279, add helper implementation near other mqtt helpers, modify `air780ep_mqtt_configure()` at L2860)

- [ ] **Step 1: Add the static prototype**

The MQTT op static prototypes are grouped at L268-279 (`air780ep_mqtt_configure` through `air780ep_mqtt_publish`). Add the new prototype after `air780ep_mqtt_publish` (L278). Replace this block (L278-280):

```c
static esp_err_t air780ep_mqtt_publish(modem_handle_t *me,
                                        const modem_mqtt_publish_t *publish);
static esp_err_t air780ep_ping(modem_handle_t *me,
```

with:

```c
static esp_err_t air780ep_mqtt_publish(modem_handle_t *me,
                                        const modem_mqtt_publish_t *publish);
static esp_err_t air780ep_mqtt_get_status(modem_handle_t *me,
                                           modem_mqtt_status_t *status);
static esp_err_t reset_mqtt_modes(modem_air780ep_t *self);
static modem_mqtt_status_t map_mqtt_status(int state);
static esp_err_t air780ep_ping(modem_handle_t *me,
```

(Adding all four new prototypes together here — `air780ep_mqtt_get_status`, `reset_mqtt_modes`, `map_mqtt_status` — since Task 5 will define the other two. This avoids editing the prototype block twice.)

- [ ] **Step 2: Add the `reset_mqtt_modes()` implementation**

Place it immediately before `air780ep_mqtt_configure()` (which starts at L2844). The function uses existing helpers `send_cmd`, `ensure_at_ok`, and the existing constant `AIR780EP_MQTT_CMD_TIMEOUT_MS` (L59).

Insert this block right before the line `static esp_err_t air780ep_mqtt_configure(modem_handle_t *me,`:

```c
static esp_err_t reset_mqtt_modes(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTMSGSET=0", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTMSGSET=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTTMSGSET to direct mode failed");

    ret = send_cmd(self, "AT+MQTTMODE=0", &ctx,
                   AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTMODE=0");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTTMODE to ASCII failed");

    return ESP_OK;
}

```

- [ ] **Step 3: Call `reset_mqtt_modes()` at the start of `air780ep_mqtt_configure()`**

In `air780ep_mqtt_configure()`, the `connected` check ends at L2860. The config copy starts at L2862. Insert the call between them. Replace this block (L2859-2862):

```c
    ESP_RETURN_ON_FALSE(!connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT is connected");

    modem_mqtt_config_t new_config = {0};
```

with:

```c
    ESP_RETURN_ON_FALSE(!connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT is connected");

    esp_err_t ret = reset_mqtt_modes(self);
    ESP_RETURN_ON_ERROR(ret, TAG, "reset MQTT modes failed");

    modem_mqtt_config_t new_config = {0};
```

**Note:** This introduces `ret` before the existing `esp_err_t ret = copy_mqtt_config(...)` at L2863. Since C allows redeclaration in the same block is NOT allowed, the existing L2863 line `esp_err_t ret = copy_mqtt_config(&new_config, config);` must be changed to `ret = copy_mqtt_config(&new_config, config);` (drop the `esp_err_t`). Replace L2863:

```c
    esp_err_t ret = copy_mqtt_config(&new_config, config);
```

with:

```c
    ret = copy_mqtt_config(&new_config, config);
```

- [ ] **Step 4: Build to verify**

Run: `idf.py build`
Expected: build succeeds with zero errors/warnings. `reset_mqtt_modes()` is now called on every `mqtt_configure`, before the existing `AT+MCONFIG`.

---

## Task 5: Implement `air780ep_mqtt_get_status()`, `map_mqtt_status()`, and register the op

**Files:**
- Modify: `src/modem/modem_air780ep.c` (add `map_mqtt_status()` and `air780ep_mqtt_get_status()` implementation, register in `s_air780ep_ops`)

- [ ] **Step 1: Add `map_mqtt_status()` helper implementation**

Place it right after the `reset_mqtt_modes()` function added in Task 4 (and before `air780ep_mqtt_configure()`). Insert after the closing brace of `reset_mqtt_modes()`:

```c
static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 0:  return MODEM_MQTT_STATUS_OFFLINE;
    case 1:  return MODEM_MQTT_STATUS_AUTHENTICATED;
    case 2:  return MODEM_MQTT_STATUS_TCP_CONNECTED;
    default: return MODEM_MQTT_STATUS_OFFLINE;
    }
}
```

- [ ] **Step 2: Add `air780ep_mqtt_get_status()` implementation**

Place it right after `map_mqtt_status()`. It uses existing helpers: `send_cmd`, `ensure_at_ok`, `find_line_with_prefix`, `parse_int_after_prefix`, and the constant `AIR780EP_MQTT_CMD_TIMEOUT_MS`. Insert:

```c
static esp_err_t air780ep_mqtt_get_status(modem_handle_t *me,
                                           modem_mqtt_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MQTTSTATU", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MQTTSTATU");
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "AT+MQTTSTATU failed");

    const char *line = find_line_with_prefix(&ctx.response, "+MQTTSTATU");
    ESP_RETURN_ON_FALSE(line, ESP_ERR_INVALID_RESPONSE, TAG,
                        "+MQTTSTATU line missing");

    int state = 0;
    ret = parse_int_after_prefix(line, "+MQTTSTATU", &state);
    ESP_RETURN_ON_ERROR(ret, TAG, "parse +MQTTSTATU failed");
    ESP_RETURN_ON_FALSE(state >= 0 && state <= 2, ESP_ERR_INVALID_RESPONSE,
                        TAG, "invalid MQTT status %d", state);

    *status = map_mqtt_status(state);
    return ESP_OK;
}

```

- [ ] **Step 3: Register the op in `s_air780ep_ops`**

In the `s_air780ep_ops` static initializer (L797-819), `mqtt_publish` is at L817 and `ping` is at L818. Add `mqtt_get_status` between them. Replace this block (L817-818):

```c
    .mqtt_publish = air780ep_mqtt_publish,
    .ping = air780ep_ping,
```

with:

```c
    .mqtt_publish = air780ep_mqtt_publish,
    .mqtt_get_status = air780ep_mqtt_get_status,
    .ping = air780ep_ping,
```

- [ ] **Step 4: Build to verify full integration**

Run: `idf.py build`
Expected: build succeeds with zero errors/warnings. All four new symbols are now defined and wired: `modem_mqtt_status_t` (type), `modem_mqtt_get_status()` (wrapper), `air780ep_mqtt_get_status()` (impl), `reset_mqtt_modes()` (helper called from `air780ep_mqtt_configure`).

---

## Task 6: On-device verification checklist

**Files:** None (verification only)

This task is manual on-device verification. Run the existing Air780EP MQTT example (`example/air780ep_mqtt_client.c`) against real hardware. No code changes in this task.

- [ ] **Step 1: Normal connect flow — verify 3-command sequence**

Flash and monitor `air780ep_mqtt_client`. During MQTT configure, observe the monitor log shows three AT commands sent in sequence before `MCONFIG`:
1. `AT+MQTTMSGSET=0` → `OK`
2. `AT+MQTTMODE=0` → `OK`
3. `AT+MCONFIG=...` → `OK`

Then MQTT connects normally and telemetry publishes work.

Expected: all three return OK; MQTT reaches CONNECTED state; telemetry publishes succeed.

- [ ] **Step 2: Mode reset from cached state — verify MSGSET pulls back to direct**

Before starting lwlte, use an external serial tool (or the monitor) to send `AT+MQTTMSGSET=1` to the module manually. Then reset the MCU / restart lwlte.

Expected: during configure, `AT+MQTTMSGSET=0` returns OK. After subscribing, received messages arrive as `+MSUB:<topic>,<len>,<message>` (direct format), NOT `+MSUB:<store_addr>` (cached format). This confirms the module was pulled back to direct mode.

- [ ] **Step 3: Mode reset from HEX encoding — verify MODE pulls back to ASCII**

Before starting lwlte, use an external serial tool to send `AT+MQTTMODE=1`. Then restart lwlte.

Expected: during configure, `AT+MQTTMODE=0` returns OK. Publish payloads are received by the broker as correct ASCII content (not HEX-decoded garbage).

- [ ] **Step 4: MQTTSTATU query — verify three states**

Add a temporary call to `modem_mqtt_get_status(handle, &status)` at three points in a test program (or extend the example temporarily), where `handle` is the `modem_handle_t*` (note: this is modem-layer, not the lwlte handle; access requires being inside the modem/core layer, so this step is best done by temporarily logging from within `air780ep_mqtt_tcp_connect` / `air780ep_mqtt_connect` / after disconnect).

Expected values:
- Before MQTT connect (module freshly online, no MQTT session): `MODEM_MQTT_STATUS_OFFLINE` (0)
- After `AT+MIPSTART` succeeds (`CONNECT OK`) but before `AT+MCONNECT`: `MODEM_MQTT_STATUS_TCP_CONNECTED` (2)
- After `AT+MCONNECT` succeeds (`CONNACK OK`): `MODEM_MQTT_STATUS_AUTHENTICATED` (1)

- [ ] **Step 5: Hard-fail path — verify configure aborts if mode reset fails**

This step is conditional on being able to simulate a failure. If the module is in a state where `AT+MQTTMSGSET=0` returns ERROR (difficult to simulate reliably; skip if not feasible), verify that `modem_mqtt_configure()` returns an error (not `ESP_OK`) and does not cache the config. The monitor log should show `ESP_LOGE` from `ensure_at_ok` with command name `AT+MQTTMSGSET=0`.

Expected: `modem_mqtt_configure` returns non-OK; no `mqtt_configured` flag set; `AT+MCONFIG` is not sent.

---

## Self-Review Notes

**Spec coverage check:**
- Defensive mode reset (MSGSET/MODE) → Task 4 ✓
- MQTT status query type → Task 1 ✓
- Ops slot → Task 2 ✓
- Wrapper → Task 3 ✓
- Air780EP impl (MQTTSTATU) → Task 5 ✓
- Error handling (hard-fail, invalid response, range check) → Tasks 4 & 5 ✓
- On-device verification → Task 6 ✓

**Placeholder scan:** No TBD/TODO. All code blocks are complete.

**Type consistency:** `modem_mqtt_status_t`, `modem_mqtt_get_status_fn`, `modem_mqtt_get_status`, `air780ep_mqtt_get_status`, `reset_mqtt_modes`, `map_mqtt_status` — names consistent across all tasks.

**Build-order correctness:** Tasks 1→2→3→4→5 each leave the tree compilable. Task 2 adds an ops slot that `s_air780ep_ops` will zero-initialize (C99 designated initializer semantics), so build succeeds even before Task 5 registers the impl.
