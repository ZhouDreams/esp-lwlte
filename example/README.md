# Unified Examples

All examples build from the repository root. Select the example to run by editing `EXAMPLE_SELECTED` in `example/main.c`:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT
```

Available selections:

| Value | Description |
|-------|-------------|
| `EXAMPLE_AIR780EP_BASIC_CONNECT` | Air780EP LTE basic connect and ping example |
| `EXAMPLE_AIR780EP_MQTT_CLIENT` | Air780EP ThingsBoard MQTT publish/subscribe example |
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

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. The modem adapter toggles EN low then high during start/reset, polls `AT` until `OK`, and then sends basic AT initialization commands.

## Air780EP Basic Connect

`EXAMPLE_AIR780EP_BASIC_CONNECT` demonstrates the minimum useful esp-lwlte Air780EP flow:

1. Create an Air780EP LWLTE facade over UART.
2. Register LTE facade events.
3. Start LTE network activation asynchronously.
4. Wait for `LWLTE_EVENT_NET_ONLINE`.
5. Run one ping after the network is online.

Expected logs include:

```text
Air780EP basic connect example
LTE event=2 net=1 err=0
LTE event=3 net=2 err=0
Air780EP network is online
ping summary: sent=4 recv=4 lost=0 min=... max=... avg=...
```

## Air780EP MQTT Client

`EXAMPLE_AIR780EP_MQTT_CLIENT` demonstrates a ThingsBoard MQTT client over Air780EP LTE.

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
| Publish | `v1/devices/me/telemetry` | Periodic telemetry data |
| Subscribe | `v1/devices/me/attributes` | Downlink/shared attribute updates from ThingsBoard |

The MQTT example intentionally keeps downlink handling simple: received MQTT data is printed in the event callback. It does not implement RPC response logic.

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
