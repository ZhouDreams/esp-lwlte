# Basic Connect Example

This example demonstrates the minimum useful esp-lwlte flow:

1. Enable the Air780EP module.
2. Create AT Engine over UART.
3. Create and initialize the Air780EP modem adapter.
4. Create Core Service.
5. Start Core and connect LTE.
6. Print Core/network events and periodic state.

## Hardware Wiring

Default wiring targets the ESP32-C3 Pro DevKit setup used during development.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Held high by the example |
| GND | GND | Common ground required |

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. This example holds GPIO2 high before creating the modem.

Do not wire GPIO2 to `pwrkey_pin` in this example. The current Air780EP adapter treats `pwrkey_pin` as a pulse signal and drives it low after the pulse.

## Defaults

| Setting | Value |
|---------|-------|
| UART | `UART_NUM_1` |
| Baud rate | `115200` |
| APN | empty string, using Air780EP/operator default path |
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
Air780EP EN GPIO2 held high
Core event: STARTED
Core event: READY
Core ready reached
Core event: NET_CONNECTING net=ACTIVATING err=0
Core event: NET_ONLINE net=ONLINE err=0
LTE network is online
periodic: core=ONLINE net=ONLINE
```

If the network does not become online, the example logs `NET_ERROR` when Core reports one, or logs `LTE network did not become online` and prints current Core/network state when the wait ends.

## Troubleshooting

- No AT response: check TX/RX cross-wiring, common ground, EN held high, and baud rate `115200`.
- SIM not ready: check SIM insertion, SIM PIN state, and module antenna/power.
- Registration timeout/error: check antenna, signal, LTE coverage, and SIM network availability.
- PDP/APN errors: this example uses an empty APN to request the module/operator default. If your SIM requires an explicit APN, change `EXAMPLE_LTE_APN` in `main/main.c`.
- Serial monitor unavailable: check whether another monitor process is holding the port.
