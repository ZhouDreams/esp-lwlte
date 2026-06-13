# Air780EP MQTT MSGSET/MODE/STATU Implementation Design

## Context

The current `modem_air780ep` MQTT implementation supports eight AT commands (MCONFIG / MIPSTART / MCONNECT / MDISCONNECT / MIPCLOSE / MSUB / MUNSUB / MPUBEX). Three commands from manual section 16 (pages 246-248) are documented in `docs/agents/at_cmd_air780ep.md` but not yet implemented:

- `AT+MQTTMSGSET=<mode>` — sets subscription message print mode
- `AT+MQTTMODE=<mode>` — sets MQTT payload encoding (ASCII/HEX)
- `AT+MQTTSTATU` — queries MQTT connection state

The lwlte implementation only handles **direct mode** (`MSGSET=0`, where received messages arrive as `+MSUB:<topic>,<len>,<message>`) and **ASCII encoding** (`MODE=0`, where `AT+MPUBEX` payload is sent verbatim). If the Air780EP module is left in cached mode (`MSGSET=1`) or HEX encoding (`MODE=1`) by a prior session or external tool, the lwlte RX parser and TX escape path silently break: `+MSUB:<store_addr>` cannot be parsed, and HEX-mode publish corrupts payloads.

## Goals

- Force the module into direct mode (`AT+MQTTMSGSET=0`) and ASCII encoding (`AT+MQTTMODE=0`) at the start of every MQTT configure step, so lwlte always operates on a known module state.
- Add `AT+MQTTSTATU` as a modem-layer query that returns the module's hardware MQTT connection state.
- Keep the changes within the modem layer; do not modify the mqtt_client FSM, Core, or lwlte facade in this task.

## Non-Goals

- Do not implement cached mode (`MSGSET=1`) or its companion `AT+MQTTMSGGET`.
- Do not implement HEX encoding (`MODE=1`) for the publish path.
- Do not expose `modem_mqtt_get_status()` at the lwlte layer. A future lwlte-level `mqtt_state` aggregate data structure is planned but out of scope.
- Do not add new MQTT ops for `mqtt_set_rx_mode` / `mqtt_set_encoding`. The defensive commands are fixed-value and belong inside `air780ep_mqtt_configure()`.
- Do not change the mqtt_client connect FSM step sequence (CONFIGURE -> TCP_CONNECT -> CONNECT).

## Affected Files

| File | Change |
|------|--------|
| `src/modem/modem.h` | Add `modem_mqtt_status_t` enum and `modem_mqtt_get_status()` prototype |
| `src/modem/modem_priv.h` | Add `modem_mqtt_get_status_fn` typedef and `mqtt_get_status` slot in `modem_ops_t` |
| `src/modem/modem.c` | Add `modem_mqtt_get_status()` wrapper |
| `src/modem/modem_air780ep.c` | Add `reset_mqtt_modes()` helper, `air780ep_mqtt_get_status()` op, `map_mqtt_status()` helper, register op in `s_air780ep_ops`, modify `air780ep_mqtt_configure()` to call `reset_mqtt_modes()` |

## Defensive Mode Reset (MSGSET/MODE)

### Placement

Inside `air780ep_mqtt_configure()`, **after** the `connected` check (L2859 `ESP_RETURN_ON_FALSE(!connected, ...)`) and **before** `copy_mqtt_config()` (L2863). This placement guarantees:

- If the module is already connected, the function returns `ESP_ERR_INVALID_STATE` before touching modes (safe; connected state is untouched).
- If mode reset fails, the function returns immediately with the cached config untouched (no half-updated state).
- The mode reset runs on every call to `air780ep_mqtt_configure()`, which is the first step of every connect and reconnect sequence (mqtt_client submits `CORE_CMD_MQTT_CONFIGURE` as the first connect step).

### Helper Function

```c
static esp_err_t reset_mqtt_modes(modem_air780ep_t *self);
```

Logic:

1. Send `AT+MQTTMSGSET=0` via `send_cmd()` with `AIR780EP_MQTT_CMD_TIMEOUT_MS`, then `ensure_at_ok()`. On failure, return immediately (hard fail).
2. Send `AT+MQTTMODE=0` via `send_cmd()` with `AIR780EP_MQTT_CMD_TIMEOUT_MS`, then `ensure_at_ok()`. On failure, return immediately (hard fail).
3. Return `ESP_OK`.

Both commands use standard OK/ERROR termination (manual set-command response). No `success_matches` customization is needed.

### Hard-Fail Semantics

If either command returns ERROR or times out, `reset_mqtt_modes()` returns the error from `ensure_at_ok()` (typically `ESP_FAIL`), and `air780ep_mqtt_configure()` returns that error immediately without executing `AT+MCONFIG` or caching the config. The `ensure_at_ok()` helper already logs an `ESP_LOGE` with the command name and response line, so `reset_mqtt_modes()` adds no extra logging to avoid redundancy.

Rationale: if the module cannot be placed in direct mode + ASCII encoding, MQTT RX parsing and TX escaping cannot work correctly. Proceeding would cause the "silent break" scenario this feature exists to prevent.

### Why Not Soft-Fail

Soft-fail (log a warning and continue) was rejected because it directly contradicts the feature's purpose: if the module is stuck in cached mode and `MSGSET=0` fails, continuing would let `+MSUB:<store_addr>` URCs reach a parser that only understands `+MSUB:<topic>,<len>,<message>`, silently dropping all received messages. Hard-fail makes the broken state visible to the caller.

## MQTT Status Query (MQTTSTATU)

### Public Type

New enum in `modem.h`, placed near the other MQTT types (`modem_mqtt_config_t` region):

```c
typedef enum {
    MODEM_MQTT_STATUS_OFFLINE = 0,       /**< 离线； Offline */
    MODEM_MQTT_STATUS_AUTHENTICATED,     /**< 已认证，可发布； Authenticated, can publish */
    MODEM_MQTT_STATUS_TCP_CONNECTED,     /**< TCP 已连接，未认证； TCP connected, not authenticated */
} modem_mqtt_status_t;
```

Naming uses `STATUS` (not `STATE`) to avoid confusion with the existing `modem_state_t` lifecycle enum and to match the command name `AT+MQTTSTATU` and the manual's "状态" terminology.

Numeric values map directly to the manual's `0/1/2`:
- `0` offline
- `1` authenticated (already logged in, can publish)
- `2` TCP connected but not authenticated (needs `AT+MCONNECT`)

### Ops Signature

In `modem_priv.h`:

```c
typedef esp_err_t (*modem_mqtt_get_status_fn)(modem_handle_t *me,
                                              modem_mqtt_status_t *status);
```

New slot in `modem_ops_t`, placed at the end of the MQTT section (after `mqtt_publish`, before `ping`):

```c
modem_mqtt_get_status_fn mqtt_get_status;   /**< 查询 MQTT 状态； Query MQTT status */
```

### Air780EP Implementation

```c
static esp_err_t air780ep_mqtt_get_status(modem_handle_t *me,
                                          modem_mqtt_status_t *status);
```

Logic:

1. Validate `me` and `status` non-NULL (`ESP_RETURN_ON_FALSE`).
2. Cast via `to_air780ep(me)`.
3. Send `AT+MQTTSTATU` via `send_cmd()` with `AIR780EP_MQTT_CMD_TIMEOUT_MS`. Note the manual command name lacks the trailing `S`.
4. `ensure_at_ok()` on the response. On failure, return the error.
5. Find the response line via `find_line_with_prefix(&ctx.response, "+MQTTSTATU")`. If missing, return `ESP_ERR_INVALID_RESPONSE`.
6. Parse the integer via the existing `parse_int_after_prefix(line, "+MQTTSTATU", &state)`. On failure, return `ESP_ERR_INVALID_RESPONSE`.
7. Validate `state >= 0 && state <= 2`. Out of range, return `ESP_ERR_INVALID_RESPONSE`.
8. Map via `map_mqtt_status(state)` and write to `*status`.

### Space Handling in Response

The manual response format is `+MQTTSTATU :<state>` (space before the colon). The existing `skip_prefix_value()` helper matches the prefix `+MQTTSTATU`, then skips whitespace, then skips a `:` if present, then skips whitespace again. This handles both `+MQTTSTATU :<state>` (space before colon) and `+MQTTSTATU: <state>` (space after colon) formats. Passing the prefix without a colon (`"+MQTTSTATU"`) to both `find_line_with_prefix()` and `parse_int_after_prefix()` works correctly.

### Mapping Helper

```c
static modem_mqtt_status_t map_mqtt_status(int state)
{
    switch (state) {
    case 0: return MODEM_MQTT_STATUS_OFFLINE;
    case 1: return MODEM_MQTT_STATUS_AUTHENTICATED;
    case 2: return MODEM_MQTT_STATUS_TCP_CONNECTED;
    default: return MODEM_MQTT_STATUS_OFFLINE;  /* unreachable; caller validates range first */
    }
}
```

The caller (`air780ep_mqtt_get_status`) validates the range before calling this helper, so the `default` branch is unreachable but returns a safe value for defensive consistency.

### Wrapper (modem.c)

`modem_mqtt_get_status()` follows the exact pattern of `modem_get_info()`:

1. `ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, ...)`.
2. `check_ready(me, false)` -> `ESP_RETURN_ON_ERROR`.
3. `ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_get_status, ESP_ERR_NOT_SUPPORTED, ...)`.
4. `return me->ops->mqtt_get_status(me, status)`.

## Ops Table Registration

In `s_air780ep_ops`, add the new entry at the end of the MQTT section:

```c
.mqtt_publish       = air780ep_mqtt_publish,
.mqtt_get_status    = air780ep_mqtt_get_status,   /* new */
.ping               = air780ep_ping,
```

## Error Handling Summary

| Scenario | Return Code |
|----------|-------------|
| `reset_mqtt_modes`: MSGSET or MODE returns ERROR/timeout | `ESP_FAIL` (from `ensure_at_ok`); `air780ep_mqtt_configure` returns immediately, config not cached |
| `air780ep_mqtt_get_status`: response missing `+MQTTSTATU` line | `ESP_ERR_INVALID_RESPONSE` |
| `air780ep_mqtt_get_status`: state integer parse failure | `ESP_ERR_INVALID_RESPONSE` |
| `air780ep_mqtt_get_status`: state value outside 0..2 | `ESP_ERR_INVALID_RESPONSE` |
| `modem_mqtt_get_status` wrapper: modem not ready | `ESP_ERR_INVALID_STATE` (from `check_ready`) |
| `modem_mqtt_get_status` wrapper: op not implemented | `ESP_ERR_NOT_SUPPORTED` |

## Testing

The repository has no unit test framework for modem hardware interaction. Verification is build-based plus manual on-device checks.

### Build Verification

`idf.py build` must complete with zero errors and zero warnings after the changes.

### On-Device Verification Checklist

1. **Normal connect flow**: Start the air780ep MQTT example. In monitor output, observe that the MQTT configure step sends three AT commands in sequence: `AT+MQTTMSGSET=0`, `AT+MQTTMODE=0`, `AT+MCONFIG=...`. All three return OK. MQTT connects and operates normally.

2. **Mode reset from cached state**: Before starting lwlte, use an external serial tool to send `AT+MQTTMSGSET=1` to the module. Then start lwlte. Verify in monitor that `AT+MQTTMSGSET=0` returns OK during configure, and that after subscribing, received messages arrive as `+MSUB:<topic>,<len>,<message>` (direct format), not `+MSUB:<store_addr>` (cached format). This confirms the module was pulled back to direct mode.

3. **Mode reset from HEX encoding**: Before starting lwlte, use an external serial tool to send `AT+MQTTMODE=1`. Then start lwlte. Verify that publish payloads sent by lwlte are received correctly by the broker (ASCII content, not HEX-decoded garbage), confirming the module was pulled back to ASCII mode.

4. **MQTTSTATU query**: Call `modem_mqtt_get_status()` at three points:
   - Before MQTT connect: expect `MODEM_MQTT_STATUS_OFFLINE` (0).
   - After `MIPSTART` succeeds but before `MCONNECT`: expect `MODEM_MQTT_STATUS_TCP_CONNECTED` (2).
   - After `MCONNECT` succeeds (`CONNACK OK`): expect `MODEM_MQTT_STATUS_AUTHENTICATED` (1).

5. **Hard-fail path** (if feasible to simulate): If the module is in a state where `AT+MQTTMSGSET=0` returns ERROR (e.g., MQTT session active from a prior unclean state), verify that `modem_mqtt_configure()` returns an error and does not cache the config.
