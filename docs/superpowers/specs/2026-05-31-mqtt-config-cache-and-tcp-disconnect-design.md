# MQTT Config Cache and TCP Disconnect Design

## Context

The current MQTT Modem API separates MQTT setup into three parameter structs:

- `modem_mqtt_config_t` for `AT+MCONFIG`
- `modem_mqtt_tcp_config_t` for `AT+MIPSTART`
- `modem_mqtt_connect_config_t` for `AT+MCONNECT`

The current disconnect path only exposes `modem_mqtt_disconnect()`, which maps to `AT+MDISCONNECT`. Air780EP also provides `AT+MIPCLOSE` for closing the MQTT TCP channel, and the project documentation recommends disconnecting in the order `MDISCONNECT` then `MIPCLOSE`.

## Goals

- Merge MQTT client, broker, and session options into one `modem_mqtt_config_t`.
- Make `modem_mqtt_tcp_connect()` and `modem_mqtt_connect()` use cached Modem configuration instead of per-call parameter structs.
- Add explicit MQTT TCP disconnect support for `AT+MIPCLOSE`.
- Keep MQTT session disconnect and MQTT TCP disconnect as separate API operations.
- Allow reconfiguration only when the MQTT TCP channel and MQTT session are both disconnected.

## Non-Goals

- Do not add TLS support in this change.
- Do not add automatic reconnect or retry behavior.
- Do not collapse MQTT session disconnect and TCP close into a single Modem API.
- Do not preserve the removed `modem_mqtt_tcp_config_t` or `modem_mqtt_connect_config_t` types as compatibility wrappers.

## Public Modem API

The MQTT config type becomes the single source of MQTT connection options:

```c
typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
    const char *host;
    uint16_t port;
    bool clean_session;
    uint16_t keepalive_s;
} modem_mqtt_config_t;
```

The Modem MQTT lifecycle API becomes:

```c
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config);

esp_err_t modem_mqtt_tcp_connect(modem_t *me);

esp_err_t modem_mqtt_connect(modem_t *me);

esp_err_t modem_mqtt_disconnect(modem_t *me);

esp_err_t modem_mqtt_tcp_disconnect(modem_t *me);
```

`modem_mqtt_disconnect()` remains MQTT session disconnect and maps to `AT+MDISCONNECT`. `modem_mqtt_tcp_disconnect()` closes the MQTT TCP channel and maps to `AT+MIPCLOSE`.

## Configuration Ownership

`modem_mqtt_configure()` validates and deep-copies the complete MQTT config into the concrete modem instance. Callers do not need to keep string pointers alive after `modem_mqtt_configure()` returns.

The Air780EP modem instance owns the copied config and releases it during modem destruction or when a later valid `modem_mqtt_configure()` replaces it.

## State Rules

The concrete modem tracks MQTT lifecycle state:

```c
bool mqtt_configured;
bool mqtt_tcp_connected;
bool mqtt_session_connected;
```

The state rules are:

| Operation | Required State | Success Effect |
|---|---|---|
| `modem_mqtt_configure()` | MQTT TCP disconnected and MQTT session disconnected | Replace cached config, send `AT+MCONFIG`, set `mqtt_configured` |
| `modem_mqtt_tcp_connect()` | Configured and MQTT TCP disconnected | Send `AT+MIPSTART`, set `mqtt_tcp_connected` |
| `modem_mqtt_connect()` | MQTT TCP connected and MQTT session disconnected | Send `AT+MCONNECT`, set `mqtt_session_connected` and enable MQTT data URCs |
| `modem_mqtt_disconnect()` | MQTT session connected | Disable MQTT data URCs, send `AT+MDISCONNECT`, clear `mqtt_session_connected` |
| `modem_mqtt_tcp_disconnect()` | MQTT session disconnected and MQTT TCP connected | Send `AT+MIPCLOSE`, clear `mqtt_tcp_connected` |

Reconfiguring while either `mqtt_tcp_connected` or `mqtt_session_connected` is true returns `ESP_ERR_INVALID_STATE`. The caller must first disconnect the MQTT session and then close the MQTT TCP channel.

## Stop Flow

The MQTT client stop flow should explicitly close both layers when needed:

```text
if MQTT session is connected:
    submit CORE_CMD_MQTT_DISCONNECT

if MQTT TCP is connected:
    submit CORE_CMD_MQTT_TCP_DISCONNECT

complete stop
```

This keeps software state aligned with Air780EP module state and follows the documented `MDISCONNECT` then `MIPCLOSE` order.

## Core Integration

Core keeps the same service-command pattern and gains a new command:

```c
CORE_CMD_MQTT_TCP_DISCONNECT
```

The existing MQTT connect command sequence becomes:

```text
CORE_CMD_MQTT_CONFIGURE       -> modem_mqtt_configure(me->modem, &config)
CORE_CMD_MQTT_TCP_CONNECT     -> modem_mqtt_tcp_connect(me->modem)
CORE_CMD_MQTT_CONNECT         -> modem_mqtt_connect(me->modem)
```

The stop sequence uses:

```text
CORE_CMD_MQTT_DISCONNECT      -> modem_mqtt_disconnect(me->modem)
CORE_CMD_MQTT_TCP_DISCONNECT  -> modem_mqtt_tcp_disconnect(me->modem)
```

`CORE_CMD_MQTT_CONFIGURE` carries the complete `modem_mqtt_config_t` fields. `CORE_CMD_MQTT_TCP_CONNECT` and `CORE_CMD_MQTT_CONNECT` no longer carry MQTT connection parameters.

## Error Handling

- Missing `me`, missing config, missing `client_id`, missing `host`, or `port == 0` returns `ESP_ERR_INVALID_ARG`.
- Calling connect steps before `modem_mqtt_configure()` returns `ESP_ERR_INVALID_STATE`.
- Calling `modem_mqtt_configure()` while TCP or session state is connected returns `ESP_ERR_INVALID_STATE`.
- Calling `modem_mqtt_tcp_disconnect()` while MQTT session is still connected returns `ESP_ERR_INVALID_STATE`.
- AT command failures are returned from the concrete modem implementation.

## Testing

Static host contract tests should verify:

- `modem_mqtt_tcp_config_t` and `modem_mqtt_connect_config_t` no longer exist.
- `modem_mqtt_config_t` contains client, broker, and session fields.
- `modem_mqtt_tcp_connect()` and `modem_mqtt_connect()` no longer take config arguments.
- `modem_mqtt_tcp_disconnect()` exists and maps through Modem ops, Core command dispatch, and Air780EP `AT+MIPCLOSE`.
- MQTT stop flow submits `CORE_CMD_MQTT_DISCONNECT` before `CORE_CMD_MQTT_TCP_DISCONNECT`.

ESP-IDF build verification remains required when `idf.py` is available.
