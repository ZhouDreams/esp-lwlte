# MQTT Client Example Design

## Overview

A new example `examples/mqtt_client` that demonstrates full ThingsBoard MQTT interaction over LTE using the esp-lwlte library. Follows the same structure as `examples/basic_connect`.

## File Structure

```
examples/mqtt_client/
├── CMakeLists.txt
├── Kconfig.projbuild
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    └── main.c
```

## Kconfig.projbuild Parameters

| Config | Type | Default | Description |
|--------|------|---------|-------------|
| `EXAMPLE_MQTT_HOST` | string | `iot.jovisdreams.site` | ThingsBoard MQTT broker host |
| `EXAMPLE_MQTT_PORT` | int (1-65535) | `1883` | MQTT broker port (plain TCP) |
| `EXAMPLE_MQTT_CLIENT_ID` | string | `esp-lwlte-mqtt-example` | MQTT client ID |
| `EXAMPLE_MQTT_TOKEN` | string | `""` | ThingsBoard device access token (used as MQTT username) |
| `EXAMPLE_MQTT_KEEPALIVE_S` | int (10-1200) | `120` | MQTT keepalive seconds |

## Execution Flow

1. **LTE Init** — `lwlte_air780ep_init()` with `mqtt_client.enabled = true`, host/port/client_id/username from Kconfig
2. **Register event callback** — `lwlte_register_event_callback()`
3. **Connect LTE** — `lwlte_connect()`, wait for `LWLTE_EVENT_NET_ONLINE` (timeout 120s)
4. **Start MQTT** — `lwlte_mqtt_start()`, wait for `LWLTE_EVENT_MQTT_CONNECTED` (timeout 30s)
5. **Subscribe** — `lwlte_mqtt_subscribe()` to ThingsBoard topics:
   - `v1/devices/me/rpc/request/+` (QoS 0) — RPC requests
   - `v1/devices/me/attributes` (QoS 0) — shared attribute updates
6. **Periodic telemetry** — Every 5 seconds, publish to `v1/devices/me/telemetry`:
   - Payload: `{"temperature":25.5,"counter":N}` (hardcoded temperature, incrementing counter)
7. **Event handling** in callback:
   - `LWLTE_EVENT_MQTT_DATA` — If topic starts with `v1/devices/me/rpc/request/`, extract request_id from topic suffix, publish response `{"status":"ok"}` to `v1/devices/me/rpc/response/{id}`
   - `LWLTE_EVENT_MQTT_SUBSCRIBED` — Log confirmation
   - `LWLTE_EVENT_MQTT_DISCONNECTED` / `LWLTE_EVENT_NET_OFFLINE` — Update state flags
   - `LWLTE_EVENT_MQTT_ERROR` / `LWLTE_EVENT_NET_ERROR` — Log errors

## API Usage

The example only uses public APIs from `lwlte.h` and `lwlte_air780ep.h`:
- `lwlte_air780ep_init()`, `lwlte_destroy()`
- `lwlte_register_event_callback()`
- `lwlte_connect()`
- `lwlte_get_state()`, `lwlte_get_net_state()`
- `lwlte_mqtt_start()`, `lwlte_mqtt_subscribe()`, `lwlte_mqtt_publish()`
- `lwlte_mqtt_get_state()`

No internal headers are included.

## State Tracking

Static volatile flags:
- `s_net_online` — set on `LWLTE_EVENT_NET_ONLINE`, cleared on `NET_OFFLINE`/`NET_ERROR`/`ERROR`
- `s_mqtt_connected` — set on `LWLTE_EVENT_MQTT_CONNECTED`, cleared on `MQTT_DISCONNECTED`/`MQTT_ERROR`
- `s_rpc_subscribed` — set on first `LWLTE_EVENT_MQTT_SUBSCRIBED` for RPC topic
- `s_attr_subscribed` — set on second `LWLTE_EVENT_MQTT_SUBSCRIBED` for attributes topic
- `s_counter` — incrementing telemetry counter

## Telemetry Timer

Use a simple `vTaskDelay` loop in the main task (same pattern as basic_connect's periodic log). The loop checks `s_mqtt_connected && s_rpc_subscribed` before publishing.

## RPC Response Logic

On `LWLTE_EVENT_MQTT_DATA`:
1. Check if `data->data.mqtt_msg.topic` starts with `"v1/devices/me/rpc/request/"`
2. Extract the numeric suffix as request_id using a simple scan
3. Format response topic: `v1/devices/me/rpc/response/{request_id}`
4. Publish fixed payload `{"status":"ok"}` with QoS 0, retain=false

No JSON parsing of the RPC request payload — the example only acknowledges receipt.

## Error Handling

- Init/connect/start failures → log error and enter idle loop (same as basic_connect)
- MQTT publish failure → log warning, continue loop
- Network offline during MQTT → state flags cleared, telemetry loop skips publishes, waits for reconnect

## Hardware Wiring

Same as basic_connect (ESP32-C3 Pro DevKit + Air780EP):
| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | UART1 TX |
| GPIO1 | TX | UART1 RX |
| GPIO2 | EN | Module enable |
| GND | GND | Common ground |

## Build & Flash

```bash
idf.py -C examples/mqtt_client set-target esp32c3
idf.py -C examples/mqtt_client menuconfig  # Configure MQTT token
idf.py -C examples/mqtt_client build
idf.py -C examples/mqtt_client -p /dev/cu.usbserial-XXXX flash monitor
```
