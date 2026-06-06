# ML307R Modem Subclass Design

## Context

The project already has a complete Air780EP modem subclass, a unified `modem_ops_t` interface, Core-driven network activation, Ping and MQTT services, and an ML307R AT command reference. ML307R currently only has a raw UART probe example. This design adds ML307R as a first-class modem implementation while preserving the existing layering: App uses the LWLTE facade, Core uses `modem_t`, and module-specific AT differences stay inside the Modem Adapter layer.

The ML307R implementation should follow Air780EP's structure and coding style, but it must use ML307R-specific data-plane and MQTT commands. Startup readiness is confirmed by repeated `AT` probes returning `OK`; `+MATREADY` is not used as the startup gate.

## Goals

- Add `modem_ml307r.[ch]` as a concrete `modem_t` subclass.
- Add public facade factory `lwlte_ml307r_init()` and user config types.
- Implement all current `modem_ops_t` methods, including network activation, Ping, and MQTT.
- Keep Core, MQTT Client Service, Ping Client Service, AT Engine, and common Air780EP behavior unchanged except for build registration and public type additions.
- Use ML307R `MIPCALL` as the primary data-plane activation path.
- Use ML307R `MQTT*` commands for MQTT instead of Air780EP `MCONFIG/MIPSTART/MCONNECT/MSUB/MPUBEX` commands.
- Use EN low pulse then high as the ML307R hardware reset/start sequence.
- Do not wait for `+MATREADY`; startup readiness is based only on `AT OK` probing.

## Non-Goals

- Do not add TCP, HTTP, HTTPS, SSL certificate, DNS, or socket public APIs in this change.
- Do not refactor Air780EP into shared helpers unless a tiny local helper is clearly needed for ML307R implementation.
- Do not change `modem_ops_t` shape or Core network activation policy.
- Do not make `+MATREADY` advance startup state.
- Do not send AT commands from an AT Engine URC callback.

## Public API

Add `lwlte_ml307r_config_mqtt_client_t` and `lwlte_ml307r_config_t` to `src/include/lwlte.h`. The shape should mirror `lwlte_air780ep_config_t` so board code can switch module factories with minimal changes:

- UART hardware fields: `uart_num`, `uart_tx_pin`, `uart_rx_pin`, `uart_baud_rate`.
- Module control field: `en_pin`.
- Network fields: `apn`, `primary_cid`, `init_ready_timeout_ms`, `net_activate_timeout_ms`, `reconnect_delay_ms`.
- AT Engine tuning fields: RX buffer, task, line buffer, default timeout, max response lines.
- Modem tuning fields: reset pulse, default command timeout, event queue/task settings.
- Core task tuning fields.
- MQTT client configuration fields.

Add:

```c
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,
                            lwlte_t **out_lte);
```

`primary_cid` is limited to `1` in this first implementation. This matches the current Core default data-plane model and the ML307R documentation recommendation that CID 1 is the default bearer. Other CIDs return validation failure at the facade layer or `ESP_ERR_NOT_SUPPORTED` in modem operations.

## Modem Subclass

Add `src/modem/modem_ml307r.h` with `modem_ml307r_config_t` and `modem_ml307r_create()`.

`modem_ml307r_t` is private to `src/modem/modem_ml307r.c` and embeds `modem_t base` as the first field. It owns:

- A `modem_ml307r_config_t` snapshot.
- URC handlers for `+CPIN:`, `+CREG:`, `+CEREG:`, `+CGREG:`, `+MIPCALL:`, and `+MQTTURC:`.
- Cached `modem_info_t`, last SIM status, last registration status, last signal quality.
- ML307R PDP/MIPCALL context cache for the supported CID.
- Cached MQTT configuration and MQTT connection state for `connect_id=0`.
- Flags for URC registration, initialization, MQTT configured, MQTT connected, and MQTT data enabled.

`modem_ml307r_create()` normalizes zero-valued timeout/task fields, initializes the ML307R private cache, then calls `modem_base_init(&self->base, "ml307r", at, &s_ml307r_ops, ...)`. It returns `&self->base` on success.

## Facade Factory

Add `src/lwlte/lwlte_ml307r.c`. It mirrors the existing Air780EP factory:

1. Validate config and set `*out_lte = NULL`.
2. Create an empty `lwlte_t` facade.
3. Create `at_engine_t` from UART config.
4. Create `modem_ml307r_t` through `modem_ml307r_create()`.
5. Create Core with `primary_cid` and APN.
6. Register Core event bridge.
7. Create Ping Client.
8. Optionally create MQTT Client and register MQTT event bridge.
9. Return the facade handle.

Failure cleanup uses the existing `lwlte_destroy()` reverse-order ownership model.

## Startup And Reset

ML307R `start` and `reset` use the same readiness boundary:

1. Set modem state to `MODEM_STATE_INITIALIZING`.
2. Clear volatile PDP and MQTT runtime flags.
3. If `en_pin != GPIO_NUM_NC`, configure it as output, drive it low for `reset_pulse_ms`, then drive it high.
4. Within `ready_timeout_ms`, repeatedly send `AT` with a short per-attempt timeout until the response status is `AT_RESP_OK`.
5. Do not wait for `+MATREADY`.
6. Run basic init commands: `ATE0`, `AT+CMEE=1`, `AT+CEREG=2`, `AT+CGREG=2`, `AT+CREG=2`.
7. Register runtime URC handlers.
8. Set `MODEM_STATE_READY`, set initialized flag, and post `MODEM_EVENT_READY`.

`+MATREADY` may still appear in UART logs, but it is not registered as a startup synchronization mechanism and does not update modem/Core startup progress.

## Network Operations

### Information And Status

- `get_info`: query `AT+CGSN`, `AT+CIMI`, `AT+MCCID`, `AT+CGMM`, and `AT+CGMR`. Copy available values into `modem_info_t`; leave unavailable fields empty if a non-critical identity query fails after SIM-dependent commands.
- `get_sim_status`: query `AT+CPIN?`, parse `+CPIN: READY`, `SIM PIN`, `SIM PUK`, missing SIM, or other statuses into `MODEM_SIM_*`.
- `get_signal`: query `AT+CSQ`, parse `+CSQ: <rssi>,<ber>`, and calculate dBm with `-113 + 2 * rssi` when RSSI is `0..31`.
- `get_registration`: prefer `AT+CEREG?`; fall back to `AT+CGREG?` and `AT+CREG?` if needed. Map `stat=1` to home, `5` to roaming, `2` to searching, `3` to denied, and unknown values to unknown.
- `get_packet_attach_status`: query `AT+CGATT?` and require `+CGATT: 1` for attached.

### PDP And MIPCALL

- `set_apn`: send `AT+CGDCONT=<cid>,"IPV4V6","<apn>"` and cache APN plus PDP type.
- `activate_pdp`: query `AT+MIPCALL?` first. If CID 1 is already active and has an IP, cache it and post/return success. Otherwise send `AT+MIPCALL=1,<cid>` and parse a `+MIPCALL: <cid>,1,"<ip>"` line if it appears in the command response.
- `deactivate_pdp`: send `AT+MIPCALL=0,<cid>`, clear cached active/IP state when command succeeds, and post a PDP deactivation event if state changed.
- `get_pdp_context`: query `AT+MIPCALL?` and refresh cached active/IP state. It may also query `AT+CGDCONT?` if APN/PDP type cache is empty.

Because the current AT Engine sends non-final lines to the active command response and only dispatches URCs when idle, `+MIPCALL:` may be seen either in the command response or as an idle URC after `OK`. The modem implementation must handle both paths. It must not assume command-period URC dispatch.

## MQTT Operations

ML307R MQTT uses `connect_id=0` and the `MQTT*` command family.

- `mqtt_configure`: validate and deep-copy config. Send `AT+MQTTCFG="version",0,4`, `AT+MQTTCFG="cid",0,1`, `AT+MQTTCFG="keepalive",0,<keepalive>`, `AT+MQTTCFG="clean",0,<clean_session>`, and `AT+MQTTCFG="cached",0,0` for direct receive mode.
- `mqtt_tcp_connect`: return `ESP_OK` if MQTT config exists. ML307R MQTT does not need a separate Air780EP-style `MIPSTART`; transport setup is included in `AT+MQTTCONN`.
- `mqtt_connect`: send `AT+MQTTCONN=0,"<host>",<port>,"<client_id>","<user>","<password>"`. Treat `+MQTTURC: "conn",0,0` as connected. Other `conn_state` values mean disconnected or failed.
- `mqtt_disconnect`: send `AT+MQTTDISC=0`; `+MQTTURC: "conn",0,2` confirms client-initiated disconnect when observed.
- `mqtt_tcp_disconnect`: no separate transport close is needed for ML307R MQTT; it may return `ESP_OK` after local state cleanup.
- `mqtt_subscribe`: send `AT+MQTTSUB=0,"<topic>",<qos>` and require the immediate command response to be valid. Subscription completion ACK may arrive as `+MQTTURC: "suback"`; the first implementation may rely on command acceptance and update higher-layer completion consistently with existing service expectations.
- `mqtt_unsubscribe`: send `AT+MQTTUNSUB=0,"<topic>"`.
- `mqtt_publish`: send `AT+MQTTPUB=0,"<topic>",<qos>,<retain>,<payload_len>,"<payload>"` for safe textual payloads. Payload escaping must reject or escape quotes, CR, and LF. Binary payload support can use HEX mode later if needed.

The `+MQTTURC:` handler handles:

- `"conn"`: update MQTT connection state. `0` is connected; `2`, `3`, `4`, `5`, `6`, and `255` clear connected state and may post `MODEM_EVENT_PROTOCOL_CLOSED`.
- `"publish"`: parse direct-mode topic and payload length, copy topic/payload to heap, and post `MODEM_EVENT_PROTOCOL_DATA`.
- `"pubnmi"`: log cache-mode notification only. Do not send `AT+MQTTREAD` from the URC callback.
- ACK events such as `suback`, `unsuback`, `puback`, `pubrec`, `pubcomp`, and `timeout`: log/update local diagnostics only unless the existing service contract requires a completion event.

## Ping

`ping` sends:

```text
AT+MPING="<host>",<timeout_s>,<count>,<packet_len>,1
```

It parses per-packet lines:

```text
+MPING: <result>,"<ip>",<packet_len>,<time>,<ttl>
```

and summary lines:

```text
+MPING: "statistics",<sent>,<lost>,<rtt_min>,<rtt_max>,<rtt_avg>
```

Only `result=0` is considered a successful reply. The summary fills `sent`, `lost`, `received`, `min_time_ms`, `max_time_ms`, and `avg_time_ms`. If no summary is present but replies were parsed, the modem computes a fallback summary from replies.

## URC Handling

Runtime URC handlers are registered only after basic init succeeds. Handlers must be short and non-blocking because they run in the AT Engine RX task context under AT Engine handler dispatch rules.

System-level handlers:

- `+CPIN:` updates cached SIM status and posts `MODEM_EVENT_SIM_CHANGED`.
- `+CREG:`, `+CEREG:`, and `+CGREG:` update cached registration state and post `MODEM_EVENT_REG_CHANGED`.
- `+MIPCALL:` updates cached PDP active/IP state and posts `MODEM_EVENT_PDP_ACTIVATED` or `MODEM_EVENT_PDP_DEACTIVATED`.
- `+MQTTURC:` updates MQTT state and posts MQTT protocol events when needed.

Connection-layer URCs such as `+MIPOPEN:`, `+MIPCLOSE:`, and `+MIPURC:` are not system network events in this change. They are reserved for future socket APIs.

## Error Handling

The implementation uses standard ESP-IDF errors:

- `ESP_ERR_INVALID_ARG` for null pointers, unsafe AT arguments, invalid CID, invalid QoS, missing MQTT required fields, or invalid config.
- `ESP_ERR_INVALID_STATE` for operations before initialization or without required cached MQTT config.
- `ESP_ERR_NOT_SUPPORTED` for unsupported CIDs or unimplemented ML307R-specific variants.
- `ESP_ERR_TIMEOUT` for AT Engine timeout or derived operation timeout.
- `ESP_ERR_INVALID_RESPONSE` for malformed AT response lines.
- `ESP_FAIL` for generic `ERROR`, `+CME ERROR`, `+CMS ERROR`, and other module-side failures.

AT command helpers log the command, response status, and CME/CMS code when available. Response parsers preserve cached previous values only when a query fails before a new valid value is parsed.

## Build Integration

Update `src/CMakeLists.txt` to include:

- `modem/modem_ml307r.c`
- `lwlte/lwlte_ml307r.c`

No example change is required for the modem subclass itself, but a later ML307R basic-connect example can reuse `lwlte_ml307r_init()`.

## Testing And Verification

Host tests should cover:

- Public API and CMake source entries for ML307R are present.
- Startup does not wait for or gate on `+MATREADY`; it uses repeated `AT` and requires `OK`.
- Basic init commands are ordered after `AT OK`.
- `+MIPCALL: <cid>,1,"<ip>"` activates and caches PDP/IP.
- `+MIPCALL: <cid>,0` clears PDP/IP and posts deactivation.
- `+MQTTURC: "conn",0,0` sets MQTT connected state; non-zero states clear it.
- Direct-mode `+MQTTURC: "publish"` creates a `MODEM_EVENT_PROTOCOL_DATA` event with copied topic/payload.
- `AT+MPING` response parsing fills replies and summary.

Final verification should run the host test suite and ESP-IDF project build. Hardware verification, if available, should confirm the startup sequence: EN low pulse, EN high, repeated `AT` until `OK`, basic init commands, Core SIM/register/attach checks, `MIPCALL`, then `NET_ONLINE` only after IP acquisition.
