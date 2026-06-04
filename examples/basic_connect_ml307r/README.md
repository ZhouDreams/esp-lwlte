# Basic Connect ML307R UART Probe

This example probes an ML307R module over UART without using esp-lwlte library initialization.

It is intended to verify the startup behavior before adding an ML307R modem subclass. The ML307R communication-flow guide documents `+MATREADY` as the startup indication, not Air780EP-style `RDY`. The same guide also notes that when the module is in autobaud mode, there is no `+MATREADY` report and the UART must first send `AT`; after `OK`, later commands can be executed.

## Hardware Wiring

Default wiring matches the existing development setup.

| ESP32-C3 | ML307R | Notes |
|----------|--------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN or power enable | Held high by this example |
| GND | GND | Common ground required |

## Probe Flow

1. Configure `UART_NUM_1` at `115200` baud.
2. Drive GPIO2 high.
3. Listen for startup UART data for 3 seconds.
4. Send `ATE0` and print raw response bytes.
5. Send `AT` up to 3 times and print raw response bytes.
6. Stay in an idle loop and keep printing unsolicited UART data.

## Build

From the repository root:

```bash
idf.py -C examples/basic_connect_ml307r set-target esp32c3
idf.py -C examples/basic_connect_ml307r build
```

## Flash And Monitor

Replace `/dev/cu.usbserial-XXXX` with the board's serial port:

```bash
idf.py -C examples/basic_connect_ml307r -p /dev/cu.usbserial-XXXX flash monitor
```

In a non-interactive agent environment, use the repository serial monitor helper after flashing:

```bash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-XXXX
```

## Expected Logs

Possible fixed-baud startup:

```text
RX startup: len=11 text='+MATREADY..'
TX ATE0: ATE0
RX ATE0: len=...
TX AT: AT
RX AT: len=...
```

Possible autobaud startup:

```text
no startup URC received; ML307R autobaud mode may require AT first
TX ATE0: ATE0
RX ATE0: len=...
TX AT: AT
RX AT: len=...
```

## Troubleshooting

- No response: check TX/RX cross-wiring, common ground, module power, EN wiring, and baud rate `115200`.
- Startup data appears as garbled bytes: check baud rate or whether autobaud is active.
- `ATE0` has no response but later `AT` responds: keep the raw log; the first command may have been consumed by autobaud synchronization.
