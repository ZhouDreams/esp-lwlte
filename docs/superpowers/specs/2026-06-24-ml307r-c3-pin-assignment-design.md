# ML307R ESP32-C3 Pin Assignment Design

## Context

The current Air780EP examples use `UART_NUM_1` with `GPIO0` as TX, `GPIO1` as RX, and `GPIO2` as EN. The ML307R examples currently use the same pins, which prevents wiring both LTE modules to the same ESP32-C3 development board without rewiring or conflict.

The target hardware has `GPIO3`, `GPIO10`, and `GPIO4` brought out and available. The requested runtime model remains the existing single selected example model: `EXAMPLE_SELECTED` chooses one module example to run at a time.

## ESP32-C3 Pin Rationale

ESP32-C3 UART1 signals can be routed through the GPIO matrix. For a second LTE module connection that should not conflict with the existing Air780EP example wiring:

- Keep Air780EP unchanged on `UART_NUM_1`, `TX=GPIO0`, `RX=GPIO1`, `EN=GPIO2`.
- Move ML307R examples to `UART_NUM_1`, `TX=GPIO3`, `RX=GPIO10`, `EN=GPIO4`.
- Avoid `GPIO8` and `GPIO9` because they are strapping pins.
- Avoid `GPIO12` to `GPIO17` because they are flash-related pins and not recommended for general use.
- Avoid `GPIO18` and `GPIO19` because they are normally used by USB Serial/JTAG.
- Avoid `GPIO20` and `GPIO21` because they are normally used by UART0 console.

`GPIO4` is a JTAG-related pin, so it is acceptable here only because the board has it available and the user confirmed it is free. It is used for the modem EN signal, not the UART data path.

## Chosen Approach

Apply the smallest example-only change:

- In `example/ml307r_basic_connect.c`, set `EXAMPLE_LTE_UART_TX_PIN` to `GPIO_NUM_3`, `EXAMPLE_LTE_UART_RX_PIN` to `GPIO_NUM_10`, and `EXAMPLE_LTE_EN_PIN` to `GPIO_NUM_4`.
- Apply the same pin defaults in `example/ml307r_tcp_client.c`.
- Apply the same pin defaults in `example/ml307r_mqtt_client.c`.
- Leave all Air780EP examples unchanged.
- Leave `UART_NUM_1`, baud rate, APN, CID, and example selection behavior unchanged.

This lets both modules remain physically connected to distinct ESP32-C3 IOs while preserving the existing one-example-at-a-time software model.

## Non-Goals

- Do not add a dual-modem simultaneous runtime example.
- Do not move either module to UART0.
- Do not add menuconfig-based pin selection in this change.
- Do not refactor duplicated example config blocks into shared helpers.
- Do not change LWLTE public APIs, modem adapters, Core, AT Engine, TCP, MQTT, or Ping behavior.

## Documentation And Tests

Update example-facing documentation or static checks as needed so the intended default wiring is explicit:

- Air780EP default wiring remains `TX=GPIO0`, `RX=GPIO1`, `EN=GPIO2`.
- ML307R default wiring becomes `TX=GPIO3`, `RX=GPIO10`, `EN=GPIO4`.

If an existing static host test asserts ML307R example wiring, update it to assert the new pins.

## Verification

Verification should include:

- Static check that all three ML307R example files use `GPIO_NUM_3`, `GPIO_NUM_10`, and `GPIO_NUM_4` for TX, RX, and EN respectively.
- Static check that Air780EP example files still use `GPIO_NUM_0`, `GPIO_NUM_1`, and `GPIO_NUM_2`.
- Run the relevant host tests if available.
- Run `git diff --check`.
- Build the ESP-IDF project.

Hardware validation after implementation should first run one ML307R example with the new wiring and confirm that the AT channel initializes. Full LTE network validation is useful but not required to accept the pin-assignment code change.
