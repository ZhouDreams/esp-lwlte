# ML307R MQTTSTATE Query and MQTTPUB dup Fix Design

## Context

The Air780EP modem received `AT+MQTTSTATU` status query support and defensive `MQTTMSGSET=0` / `MQTTMODE=0` mode reset in a prior task (see `2026-06-13-air780ep-mqtt-msgset-mode-statu-design.md`). This task is the ML307R counterpart — adding `AT+MQTTSTATE` query support and fixing the `AT+MQTTPUB` command's `<dup>` parameter.

Unlike Air780EP, ML307R's defensive mode reset already exists inside `ml307r_mqtt_configure()` (L2124-2128 sends `encoding,0,1,0`; L2170 sends `cached,0,0` on every configure call). So this task has a narrower scope: only the status query op and the dup fix.

The ML307R MQTT AT command reference was verified against manual V6.8.3 (36 pages of images read via vision-reader subagents) and corrected in commit `533b437`. The key differences from Air780EP are:

- `AT+MQTTSTATE=<connect_id>` — correct spelling (Air780EP's `AT+MQTTSTATU` lacks the trailing `S`), takes a mandatory `connect_id` parameter, returns `+MQTTSTATE: <state>` **without** echoing `connect_id`.
- State values differ: ML307R uses `1`(connecting/reconnecting), `2`(connected), `3`(disconnected), `4..255`(reserved). Air780EP uses `0`(offline), `1`(authenticated), `2`(TCP connected).
- `AT+MQTTPUB` has a mandatory `<dup>` parameter between `<retain>` and `<msg_len>` that Air780EP's `AT+MPUBEX` does not expose.

## Goals

- Add `AT+MQTTSTATE` as a modem-layer query that returns the module's MQTT connection state, normalized to the existing `modem_mqtt_status_t` enum (shared with Air780EP).
- Fix `ml307r_mqtt_publish()` to make the `<dup>` parameter explicit in the AT command format string, improving readability and aligning the code with the corrected MD reference.
- Keep changes within `modem_ml307r.c` only; no changes to public headers or `modem.c` (the `mqtt_get_status` ops slot and `modem_mqtt_get_status()` wrapper were added in the Air780EP task).

## Non-Goals

- Do not change ML307R's HEX input encoding (`encoding,0,1,0`). It is a deliberate design choice: `MQTTPUB` embeds payload inline in the AT command line (not a prompt-mode like Air780EP's `MPUBEX`), so HEX encoding is required for binary safety. See "Why HEX Input Encoding" below.
- Do not extract ML307R's defensive `encoding`/`cached` commands into a `reset_mqtt_modes()` helper. They are already correctly placed in `mqtt_configure()` as fixed commands; refactoring adds churn without value.
- Do not add a `dup` field to `modem_mqtt_publish_t`. lwlte only ever publishes new messages; `dup` is always 0. Making it a struct field would change the public API and affect Air780EP for no functional gain.
- Do not modify the `mqtt_client` FSM, Core, or lwlte facade.
- Do not change the existing `modem_mqtt_status_t` enum (3 values). ML307R normalizes its 1/2/3 values into the existing enum.

## Why HEX Input Encoding

ML307R's `AT+MQTTPUB` embeds the payload directly in the AT command line as a quoted string: `AT+MQTTPUB=0,"topic",0,0,0,4,"68656C6C6F"`. This is fundamentally different from Air780EP's `AT+MPUBEX`, which returns a `>` prompt and accepts a raw byte stream.

An inline AT command line is a C string. If the payload contains:
- `0x00` (null byte) — truncates the C string, silently losing data after it.
- `"` (double quote) — breaks the AT command's quoting structure.
- `,` (comma) or `\r\n` — corrupts the AT command's field/line structure.

HEX input encoding (`encoding=1`) sidesteps all of these: `hex_encode_payload()` converts every byte to two ASCII hex characters (`0-9A-F`). The payload `"hello"` becomes `"68656C6C6F"`, which is always AT-command-safe regardless of content.

Output stays at `encoding=0` (raw) because `parse_mqtt_publish_urc()` reads the received payload via `memcpy` bounded by `<payload_len>`, not by string terminator — so raw bytes are handled correctly on RX.

## Affected Files

| File | Change |
|------|--------|
| `src/modem/modem_ml307r.c` | (1) New `map_mqtt_status()` helper; (2) New `ml307r_mqtt_get_status()` op; (3) Register `.mqtt_get_status` in `s_ml307r_ops`; (4) Fix `ml307r_mqtt_publish()` dup parameter; (5) Doxygen prototypes for new static functions |

No other files change. The public types (`modem_mqtt_status_t`), ops slot (`modem_ops_t.mqtt_get_status`), and wrapper (`modem_mqtt_get_status()`) were added to `modem.h` / `modem_priv.h` / `modem.c` in the Air780EP task and are reused directly.

## MQTTSTATE Status Query

### Public Type (existing, unchanged)

The `modem_mqtt_status_t` enum from `modem.h`:

```c
typedef enum {
    MODEM_MQTT_STATUS_OFFLINE = 0,       /**< 离线； Offline */
    MODEM_MQTT_STATUS_AUTHENTICATED,     /**< 已认证，可发布； Authenticated, can publish */
    MODEM_MQTT_STATUS_TCP_CONNECTED,     /**< TCP 已连接，未认证； TCP connected, not authenticated */
} modem_mqtt_status_t;
```

This enum was designed for Air780EP's 0/1/2 values. ML307R normalizes its 1/2/3 values into the same enum (see mapping below).

### Mapping Helper

```c
static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 2:  return MODEM_MQTT_STATUS_AUTHENTICATED;   /* 连接成功； connected */
    case 1:  return MODEM_MQTT_STATUS_TCP_CONNECTED;   /* 连接/重连中 → 近似底层就绪； connecting ≈ transport ready */
    case 3:  return MODEM_MQTT_STATUS_OFFLINE;         /* 断开； disconnected */
    default: return MODEM_MQTT_STATUS_OFFLINE;         /* 4..255 保留； reserved */
    }
}
```

Rationale for the `1 → TCP_CONNECTED` mapping: ML307R `state=1` means "正在连接或重连" (connecting/reconnecting). This is not literally "TCP connected but unauthenticated" like Air780EP's state=2. However, both share the operational meaning "not fully authenticated yet, but transport-layer activity is in progress." Since the lwlte-level caller only distinguishes "authenticated" (can publish) from "not authenticated" (cannot publish), collapsing `connecting` into `TCP_CONNECTED` preserves the actionable distinction without losing a decision path.

`state=3` (断开/disconnected) and reserved `4..255` both map to `OFFLINE`. The caller treats OFFLINE as "MQTT session not usable, need recovery."

The caller (`ml307r_mqtt_get_status`) validates `state >= 1 && state <= 3` before calling this helper, so the `default` branch is unreachable but returns a safe value for defensive consistency.

### Op Implementation

```c
static esp_err_t ml307r_mqtt_get_status(modem_handle_t *me,
                                         modem_mqtt_status_t *status);
```

Logic:

1. Validate `me` and `status` non-NULL (`ESP_RETURN_ON_FALSE`).
2. Cast via `to_ml307r(me)`.
3. Send `AT+MQTTSTATE=0` via `send_cmd()` with `ML307R_MQTT_CMD_TIMEOUT_MS`. The `connect_id` is fixed at `0` (ML307R implementation uses a single MQTT connection).
4. `ensure_at_ok()` on the response. On failure, return the error.
5. Find the response line via `find_line_with_prefix(&ctx.response, "+MQTTSTATE")`. If missing, return `ESP_ERR_INVALID_RESPONSE`.
6. Parse the integer via `parse_int_after_prefix(line, "+MQTTSTATE", &state)`. On failure, return `ESP_ERR_INVALID_RESPONSE`.
7. Validate `state >= 1 && state <= 3`. Out of range, return `ESP_ERR_INVALID_RESPONSE`.
8. Map via `map_mqtt_status(state)` and write to `*status`.

### Response Format

The manual response format is `+MQTTSTATE: <state>` (standard colon-space separator, no `connect_id` echoed). The existing `find_line_with_prefix("+MQTTSTATE")` and `parse_int_after_prefix("+MQTTSTATE")` helpers handle this directly. Unlike Air780EP's `+MQTTSTATU :<state>` (space before colon), no special whitespace handling is needed.

### Ops Table Registration

In `s_ml307r_ops` (around L231), add the entry in the MQTT section after `mqtt_publish` and before `ping`:

```c
.mqtt_publish       = ml307r_mqtt_publish,
.mqtt_get_status    = ml307r_mqtt_get_status,   /* new */
.ping               = ml307r_ping,
```

## MQTTPUB dup Parameter Fix

### Current State

`ml307r_mqtt_publish()` at L2495-2499 (length measurement) and L2511-2515 (actual write) uses a format string with a hardcoded `0` in the `<dup>` position:

```c
/* AT command shape: AT+MQTTPUB=0,"%s",%u,%u,0,%u,"%s". */
int needed = snprintf(NULL, 0, "AT+MQTTPUB=0,\"%s\",%u,%u,0,%u,\"%s\"",
                       escaped_topic, (unsigned int)publish->qos,
                       publish->retain ? 1U : 0U,
                       (unsigned int)publish->payload_len, hex_payload);
```

The format string fields are: `topic, qos, retain, dup(hardcoded 0), msg_len, message`. The `0` is `<dup>`, positionally correct but undocumented.

### Fix

Replace the hardcoded `0` with an explicit `0U` parameter, making the format string's `%u` placeholders correspond 1:1 to the manual's parameters:

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

Both `snprintf` calls (L2496 length measurement + L2511 actual write) are modified identically.

### Why a Named Local Instead of a Literal

`const unsigned int dup_flag = 0U;` is self-documenting: a reader scanning the `snprintf` arguments immediately sees that the fourth `%u` is `dup`, not some other field. If a future task adds `dup` to `modem_mqtt_publish_t`, only this one line changes.

### Behavioral Invariant

The AT command string produced before and after the fix is **byte-identical**. Both produce `AT+MQTTPUB=0,"<topic>",<qos>,<retain>,0,<msg_len>,"<message>"`. This is a pure readability/maintainability change with zero functional impact.

## Doxygen Prototypes

New static functions need Doxygen comment blocks on their forward declarations, following the file's existing bilingual style (`中文； English`). The prototypes are added near the existing `ml307r_mqtt_publish` prototype (around L125).

## Error Handling Summary

| Scenario | Return Code |
|----------|-------------|
| `ml307r_mqtt_get_status`: `AT+MQTTSTATE=0` returns ERROR/timeout | `ESP_FAIL` (from `ensure_at_ok`) |
| `ml307r_mqtt_get_status`: response missing `+MQTTSTATE` line | `ESP_ERR_INVALID_RESPONSE` |
| `ml307r_mqtt_get_status`: state integer parse failure | `ESP_ERR_INVALID_RESPONSE` |
| `ml307r_mqtt_get_status`: state value outside 1..3 | `ESP_ERR_INVALID_RESPONSE` |
| `modem_mqtt_get_status` wrapper: modem not ready | `ESP_ERR_INVALID_STATE` (from `check_ready`) |
| `modem_mqtt_get_status` wrapper: op not implemented | `ESP_ERR_NOT_SUPPORTED` (ML307R now registers the op; only triggers if a future modem omits it) |
| `ml307r_mqtt_publish` dup fix | No new error paths; behavioral invariant preserved |

## Testing

The repository has no unit test framework for modem hardware interaction. Verification is build-based plus manual on-device checks.

### Build Verification

ESP-IDF MCP `build_project` must complete with zero errors and zero warnings after the changes.

### On-Device Verification Checklist

1. **MQTTSTATE query**: Call `modem_mqtt_get_status()` at three points:
   - Before MQTT connect: expect `MODEM_MQTT_STATUS_OFFLINE` (normalized from state=3).
   - While MQTT is connecting: expect `MODEM_MQTT_STATUS_TCP_CONNECTED` (normalized from state=1).
   - After `MQTTCONN` succeeds (`+MQTTURC:"conn",0,0`): expect `MODEM_MQTT_STATUS_AUTHENTICATED` (normalized from state=2).

2. **MQTTPUB dup fix**: Publish a message via `modem_mqtt_publish()`. Verify the broker receives the correct payload (content identical to pre-fix behavior). Inspect monitor log to confirm the AT command format is `AT+MQTTPUB=0,"topic",0,0,0,<len>,"<hex>"` (dup=0 in the correct positional slot).
