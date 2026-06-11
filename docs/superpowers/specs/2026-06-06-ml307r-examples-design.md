# ML307R Examples Design

## Context

The repository now has Air780EP examples for basic LTE connectivity and ThingsBoard MQTT publish/subscribe. ML307R is already implemented as a first-class LWLTE facade with `lwlte_ml307r_init()`, Ping, and MQTT support, but the example directory still contains the older raw UART probe. The probe no longer matches the desired example set.

## Goals

- Add two ML307R examples that mirror the Air780EP examples.
- Replace the raw ML307R UART probe with facade-based examples.
- Keep example selection explicit by module name.
- Update example build wiring and documentation so no `ml307r_probe` entry remains.
- Avoid changes to public APIs, Core, Ping, MQTT service, or AT Engine behavior.
- Fix ML307R modem MQTT publish encoding so the facade can publish JSON telemetry payloads.

## Non-Goals

- Do not refactor Air780EP and ML307R examples into shared helpers.
- Do not add new ML307R API fields or board configuration mechanisms.
- Do not change MQTT topics, ThingsBoard assumptions, or Kconfig settings.
- Do not implement ML307R hardware validation beyond build/static verification in this change.

## Chosen Approach

Create ML307R examples by following the current Air780EP example structure and replacing only module-specific facade/config types:

- `lwlte_air780ep_init()` becomes `lwlte_ml307r_init()`.
- `lwlte_air780ep_config_t` becomes `lwlte_ml307r_config_t`.
- File names, log tags, function names, and example selection macros use `ml307r` explicitly.

This keeps each module example readable as a standalone file and avoids a larger helper abstraction while the example set is still small.

## Example Files

### `example/ml307r_basic_connect.c`

The basic-connect example will:

1. Configure UART1 on GPIO0/GPIO1, EN on GPIO2, baud 115200, empty APN, and primary CID 1.
2. Create an ML307R facade with `lwlte_ml307r_init()`.
3. Register an LTE event callback.
4. Call `lwlte_start()` and wait for `LWLTE_EVENT_NET_ONLINE` with a bounded polling loop.
5. Run one `lwlte_ping()` to `8.8.8.8` after the network is online.
6. Enter `idle_forever()` after success or failure.

The event handling and ping request shape will match `air780ep_basic_connect.c`.

### `example/ml307r_mqtt_client.c`

The MQTT example will:

1. Configure the same ML307R UART/EN/APN/CID defaults as the basic example.
2. Enable MQTT in `lwlte_ml307r_config_t.mqtt_client` using the existing `CONFIG_EXAMPLE_MQTT_*` settings.
3. Create the ML307R facade with `lwlte_ml307r_init()`.
4. Register one callback for LTE and MQTT events.
5. Start LTE and wait for network online.
6. Start MQTT and wait for `LWLTE_EVENT_MQTT_CONNECTED`.
7. Subscribe to `v1/devices/me/attributes` and wait for `LWLTE_EVENT_MQTT_SUBSCRIBED`.
8. Publish periodic telemetry to `v1/devices/me/telemetry`.
9. Print received MQTT downlink data in the event callback.

The example intentionally keeps receive handling simple: it logs the topic and payload only, with no RPC response implementation.

## ML307R MQTT Payload Encoding

The ML307R MQTT manual does not provide an Air780EP-style `MPUBEX` prompt command for raw MQTT payload writes. It uses `AT+MQTTPUB` and supports `AT+MQTTCFG="encoding",<connect_id>,<input_format>,<output_format>`. For `input_format=1`, the `<message>` argument is a HEX string and `<msg_len>` is the converted/original payload byte length.

The ML307R modem adapter will configure MQTT input encoding with `AT+MQTTCFG="encoding",0,1,0` before enabling direct receive mode. Upper layers continue passing the original non-empty payload bytes to `lwlte_mqtt_publish()`. `modem_ml307r.c` converts those bytes to uppercase HEX only at the modem adapter boundary, then sends `AT+MQTTPUB=0,"<topic>",<qos>,<retain>,0,<raw_len>,"<hex_payload>"`.

This preserves the public API and lets the example publish normal ThingsBoard JSON such as `{"temperature":25.5,"counter":0}`.

## Entry And Build Integration

Update `example/example.h`:

- Keep `EXAMPLE_AIR780EP_BASIC_CONNECT` and `EXAMPLE_AIR780EP_MQTT_CLIENT`.
- Add `EXAMPLE_ML307R_BASIC_CONNECT` and `EXAMPLE_ML307R_MQTT_CLIENT`.
- Remove `EXAMPLE_ML307R_PROBE`.
- Add declarations for `example_ml307r_basic_connect_run()` and `example_ml307r_mqtt_client_run()`.
- Remove `example_ml307r_probe_run()`.

Update `example/main.c`:

- Add switch cases for both ML307R examples.
- Remove the probe switch case.
- Leave the default selected example as `EXAMPLE_AIR780EP_BASIC_CONNECT`.

Update `example/CMakeLists.txt`:

- Add `ml307r_basic_connect.c` and `ml307r_mqtt_client.c`.
- Remove `ml307r_probe.c`.

Delete `example/ml307r_probe.c`.

## Documentation Updates

Update `example/README.md`:

- Replace `EXAMPLE_ML307R_PROBE` in the available selections table with the two new ML307R examples.
- Replace the obsolete standalone UART diagnostic section with ML307R wiring, ML307R basic-connect flow, and ML307R MQTT flow.
- Reuse the same MQTT setting table and ThingsBoard topic description model used by Air780EP.

Update `docs/agents/directory-structure.md`:

- List `ml307r_basic_connect.c` and `ml307r_mqtt_client.c` as examples.
- Remove `ml307r_probe.c` from the example file list.

## Error Handling

The ML307R examples will use the same error-handling policy as the Air780EP examples:

- Log initialization, callback registration, LTE start, MQTT start, and subscribe failures with `esp_err_to_name()`.
- Destroy the facade after callback registration or start failures where the current Air780EP example already does so.
- Treat network and MQTT wait timeouts as terminal example failures and enter `idle_forever()`.
- Keep callback state in file-local `static volatile` flags.

## Testing And Verification

Verification should include:

- Static check that `ml307r_probe` file, macro, function, switch case, and README references are gone.
- Static check that new ML307R example files, macros, run functions, CMake entries, and README entries exist.
- Static check that ML307R MQTT publish uses HEX input encoding and converts caller payloads in the modem layer.
- `git diff --check`.
- ESP-IDF project build through the repository build helper.

Hardware validation is useful but not required for this change. If hardware is available, test `EXAMPLE_ML307R_BASIC_CONNECT` first, then `EXAMPLE_ML307R_MQTT_CLIENT` with valid ThingsBoard credentials.
