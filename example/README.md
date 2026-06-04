# Unified Examples

All examples now build from the repository root. Select the example to run by editing `EXAMPLE_SELECTED` in `example/main.c`:

```c
#define EXAMPLE_SELECTED    EXAMPLE_BASIC_CONNECT
```

Available selections:

| Value | Description |
|-------|-------------|
| `EXAMPLE_BASIC_CONNECT` | Air780EP LTE basic connect and ping example |
| `EXAMPLE_MQTT_CLIENT` | Air780EP ThingsBoard MQTT client example |
| `EXAMPLE_ML307R_PROBE` | ML307R raw UART probe example |

## Build

From the repository root:

```bash
idf.py set-target esp32c3
idf.py build
```

## Flash And Monitor

Replace `/dev/cu.usbserial-XXXX` with the board serial port:

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

In a non-interactive agent environment, use the repository helper after flashing:

```bash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-XXXX
```

## Air780EP Wiring

Default wiring targets the ESP32-C3 Pro DevKit setup used during development.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Controlled by modem adapter |
| GND | GND | Common ground required |

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. The modem adapter registers URC handlers first, toggles EN low then high during init, waits for the module `RDY` URC, and only then sends AT initialization commands.

## Basic Connect

`EXAMPLE_BASIC_CONNECT` demonstrates the minimum useful esp-lwlte flow:

1. Create an Air780EP LWLTE facade over UART.
2. Let the modem adapter reset EN, wait RDY, and run AT init internally.
3. Register LTE facade events.
4. Start LTE network activation.
5. Run ping after the network is online.
6. Print LTE/network events and periodic state.

Expected logs include:

```text
esp-lwlte basic connect example
LTE event: NET_CONNECTING net=ACTIVATING err=0
LTE event: NET_ONLINE net=ONLINE err=0
LTE network is online
periodic: lte=ONLINE net=ONLINE
```

## MQTT Client

`EXAMPLE_MQTT_CLIENT` demonstrates a ThingsBoard MQTT client over LTE.

Configure MQTT settings with `idf.py menuconfig` under **Example MQTT Settings**:

| Setting | Default | Description |
|---------|---------|-------------|
| MQTT broker host | `admin.jovisdreams.site` | ThingsBoard server hostname |
| MQTT broker port | `1883` | Plain TCP MQTT port |
| MQTT client ID | `esp-lwlte-mqtt-example` | Device client identifier |
| ThingsBoard device access token | `Air780EP` | Used as MQTT username |
| MQTT keepalive seconds | `120` | Keepalive interval |

Topics used by the example:

| Direction | Topic | Purpose |
|-----------|-------|---------|
| Publish | `v1/devices/me/telemetry` | Telemetry data |
| Subscribe | `v1/devices/me/rpc/request/+` | RPC commands from ThingsBoard |
| Publish | `v1/devices/me/rpc/response/{id}` | RPC response |
| Subscribe | `v1/devices/me/attributes` | Attribute updates from ThingsBoard |

## ML307R UART Probe

`EXAMPLE_ML307R_PROBE` probes an ML307R module over UART without using esp-lwlte library initialization.

Default wiring:

| ESP32-C3 | ML307R | Notes |
|----------|--------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN or power enable | Held high by this example |
| GND | GND | Common ground required |

Probe flow:

1. Configure `UART_NUM_1` at `115200` baud.
2. Drive GPIO2 high.
3. Listen for startup UART data for 3 seconds.
4. Send `ATE0` and print raw response bytes.
5. Send `AT` up to 3 times and print raw response bytes.
6. Stay in an idle loop and keep printing unsolicited UART data.
