# MQTT Client Example

This example demonstrates a ThingsBoard MQTT client over LTE using esp-lwlte:

1. Create an Air780EP LTE facade with MQTT client enabled.
2. Connect to the LTE network.
3. Start the MQTT client and connect to ThingsBoard.
4. Subscribe to ThingsBoard RPC and attribute topics.
5. Publish telemetry data every 5 seconds.
6. Respond to RPC requests from ThingsBoard.

## Hardware Wiring

Same as `basic_connect` — targets the ESP32-C3 Pro DevKit.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Controlled by modem adapter |
| GND | GND | Common ground required |

## Configuration

Run `idf.py menuconfig` and navigate to **Example MQTT Settings**:

| Setting | Default | Description |
|---------|---------|-------------|
| MQTT broker host | `iot.jovisdreams.site` | ThingsBoard server hostname |
| MQTT broker port | `1883` | Plain TCP MQTT port |
| MQTT client ID | `esp-lwlte-mqtt-example` | Device client identifier |
| ThingsBoard device access token | `""` | Used as MQTT username; **must be set** |
| MQTT keepalive seconds | `120` | Keepalive interval |

**Important:** You must set the ThingsBoard device access token before flashing. Create a device in ThingsBoard and copy its access token into the `EXAMPLE_MQTT_TOKEN` config.

## Build

```bash
idf.py -C examples/mqtt_client set-target esp32c3
idf.py -C examples/mqtt_client menuconfig
idf.py -C examples/mqtt_client build
```

## Flash And Monitor

```bash
idf.py -C examples/mqtt_client -p /dev/cu.usbserial-XXXX flash monitor
```

For non-interactive agent environments:

```bash
python3 docs/agents/serial_monitor.py --timeout 60 --port /dev/cu.usbserial-XXXX
```

## Expected Logs

```text
esp-lwlte MQTT client example
UART1 TX=0 RX=1 baud=115200 EN=2 APN=''
MQTT host=iot.jovisdreams.site port=1883 client_id=esp-lwlte-mqtt-example
LTE event: NET_CONNECTING net=ACTIVATING mqtt=STOPPED err=0
LTE event: NET_ONLINE net=ONLINE mqtt=STOPPED err=0
LTE network is online, starting MQTT client
LTE event: MQTT_CONNECTING net=ONLINE mqtt=CONNECTING err=0
LTE event: MQTT_CONNECTED net=ONLINE mqtt=CONNECTED err=0
MQTT connected, subscribing to ThingsBoard topics
subscription confirmed (1/2)
subscription confirmed (2/2)
All ThingsBoard subscriptions confirmed
telemetry published: {"temperature":25.5,"counter":0}
telemetry published: {"temperature":25.5,"counter":1}
RPC response sent to v1/devices/me/rpc/response/1
```

## ThingsBoard Topics

| Direction | Topic | Purpose |
|-----------|-------|---------|
| Publish | `v1/devices/me/telemetry` | Telemetry data (`temperature`, `counter`) |
| Subscribe | `v1/devices/me/rpc/request/+` | RPC commands from ThingsBoard |
| Publish | `v1/devices/me/rpc/response/{id}` | RPC response (`{"status":"ok"}`) |
| Subscribe | `v1/devices/me/attributes` | Attribute updates from ThingsBoard |

## Troubleshooting

- **LTE issues:** See `basic_connect` troubleshooting.
- **MQTT connection refused:** Verify the device token is correct and the device exists in ThingsBoard.
- **MQTT timeout:** Ensure port 1883 is correct for plain TCP; ThingsBoard also supports TLS on port 8883, which requires a different setup.
- **No telemetry in ThingsBoard:** Check the ThingsBoard device "Latest telemetry" tab; verify the payload format matches expected keys.
- **RPC not working:** Verify the subscription to `v1/devices/me/rpc/request/+` succeeded (check logs for "All ThingsBoard subscriptions confirmed").
