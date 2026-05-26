# Basic Connect Example

This example demonstrates the minimum useful esp-lwlte flow:

1. Create an Air780EP LWLTE facade over UART.
2. Let the modem adapter reset EN, wait RDY, and run AT init internally.
3. Register LTE facade events.
4. Connect LTE.
5. Print LTE/network events and periodic state.

## Hardware Wiring

Default wiring targets the ESP32-C3 Pro DevKit setup used during development.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Controlled by modem adapter; held high to keep module running |
| GND | GND | Common ground required |

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. The modem adapter registers URC handlers first, toggles EN low then high during init, waits for the module `RDY` URC, and only then sends AT initialization commands.

## Defaults

| Setting | Value |
|---------|-------|
| UART | `UART_NUM_1` |
| Baud rate | `115200` |
| EN low reset hold | `500 ms` |
| APN | empty string; the facade does not send an APN configuration command and Air780EP uses its module/operator default path |
| Primary CID | `1` |

## Build

From the repository root:

```bash
idf.py -C examples/basic_connect set-target esp32c3
idf.py -C examples/basic_connect build
```

## Flash And Monitor

Replace `/dev/cu.usbserial-XXXX` with the board's serial port:

```bash
idf.py -C examples/basic_connect -p /dev/cu.usbserial-XXXX flash monitor
```

In a non-interactive agent environment, use the repository serial monitor helper after flashing:

```bash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-XXXX
```

## Expected Logs

Successful startup should show milestones similar to:

```text
esp-lwlte basic connect example
LTE event: NET_CONNECTING net=ACTIVATING err=0
LTE event: NET_ONLINE net=ONLINE err=0
LTE network is online
periodic: lte=ONLINE net=ONLINE
```

If the network does not become online, the example logs `NET_ERROR` when the facade reports one, or logs `LTE network did not become online` and prints current LTE/network state when the wait ends.

## Troubleshooting

- No AT response: check TX/RX cross-wiring, common ground, EN held high, and baud rate `115200`.
- SIM not ready: check SIM insertion, SIM PIN state, and module antenna/power.
- Registration timeout/error: check antenna, signal, LTE coverage, and SIM network availability.
- PDP/APN errors: this example uses an empty APN, so esp-lwlte does not send `AT+CGDCONT`. If your SIM requires an explicit APN, change `EXAMPLE_LTE_APN` in `main/main.c`.
- Serial monitor unavailable: check whether another monitor process is holding the port.
