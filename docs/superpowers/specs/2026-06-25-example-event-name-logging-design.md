# Example Event Name Logging Design

## Goal

Improve example serial logs so event-related messages include both the numeric ID and the symbolic event name. This applies consistently to Air780EP and ML307R examples.

## Scope

Update the six example programs:

- `example/air780ep_basic_connect.c`
- `example/air780ep_mqtt_client.c`
- `example/air780ep_tcp_client.c`
- `example/ml307r_basic_connect.c`
- `example/ml307r_mqtt_client.c`
- `example/ml307r_tcp_client.c`

Add shared example-only helpers for mapping public lwLTE enum values to stable log names:

- LTE event IDs
- LTE network states used in existing LTE event logs
- MQTT event IDs
- TCP event IDs
- TCP connection states used in existing TCP event logs

## Log Format

Use the user-selected `id(NAME)` format:

```text
LTE event=3(NET_ONLINE) net=2(ONLINE) err=0
MQTT event=7(PUBLISHED)
TCP event=5(DATA) state=2(CONNECTED) err=0 modem=0 reason=0
```

Unknown values must still be printable and should use `UNKNOWN` as the name.

## Design

Create a small shared example helper in `example/example_event_names.c` and `example/example_event_names.h`, compiled only with examples. Each helper function returns a string literal and uses a `switch` over the public enum values from `lwlte.h`.

Example functions:

- `example_lwlte_event_name(lwlte_event_id_t id)`
- `example_lwlte_net_state_name(lwlte_net_state_t state)`
- `example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id)`
- `example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id)`
- `example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state)`

Keep the existing log fields. Do not add MQTT state to MQTT logs because existing MQTT example logs only print the event ID, and the requested change is to name logged event IDs rather than expand telemetry.

## Verification

Static verification must ensure all six examples use the shared name helpers in event logs.

Hardware verification must be run for all six examples, one at a time:

1. Select example in `example/main.c`.
2. Build with ESP-IDF MCP.
3. Flash to the connected ESP32-C3.
4. Capture serial output with `docs/agents/serial_monitor.py`.
5. Confirm event logs include the `id(NAME)` form.

For LTE module behavior, successful smoke criteria remain example-specific:

- Basic connect examples reach network online and complete ping.
- MQTT examples connect, subscribe, and publish at least one telemetry message.
- TCP examples open, send, receive, and close one TCP exchange.

## Non-Goals

- Do not change public lwLTE APIs.
- Do not change event enum numeric values.
- Do not change modem/core/MQTT/TCP runtime behavior.
- Do not change unrelated example configuration beyond temporarily selecting examples for verification.
