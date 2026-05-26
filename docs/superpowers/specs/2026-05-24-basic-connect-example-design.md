# Basic Connect Example Design

**后续修订**: Air780EP 用户侧示例已迁移到 LWLTE Facade；EN/RDY 初始化、空 APN 语义和 public config 字段以后续设计 `docs/superpowers/specs/2026-05-25-air780ep-rdy-init-flow-design.md` 为准。

## Goal

Add a minimal ESP-IDF example that demonstrates the library's smallest useful path after the Core MVP:

- Power-enable the Air780EP hardware on the ESP32-C3 Pro DevKit setup.
- Create the AT Engine, Air780EP Modem, and Core objects.
- Start Core, connect LTE, and print lifecycle/network events.
- Keep running and periodically print Core/network state for manual serial-log verification.

This example is a usage demonstration, not a board support layer or a production app template.

## Location

Create a standalone ESP-IDF project at:

```text
examples/basic_connect/
```

Expected structure:

```text
examples/basic_connect/
├── CMakeLists.txt
├── README.md
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    └── main.c
```

The example should reference the component through `EXTRA_COMPONENT_DIRS`, pointing at the repository `src` component directory.

## Hardware Defaults

The default configuration targets the user's current ESP32-C3 Pro DevKit and Air780EP wiring:

| Signal | Default |
|--------|---------|
| UART port | `UART_NUM_1` |
| ESP32-C3 TX to Air780EP RX | `GPIO0` |
| ESP32-C3 RX from Air780EP TX | `GPIO1` |
| Air780EP EN | `GPIO2`, controlled by Air780EP facade init |
| UART baud rate | `115200` |
| APN | empty string; facade does not send an APN configuration command |

The Air780EP `EN` pin is level-controlled: high means the module runs, low powers it down. Current example code does not manually hold EN high before creating low-level objects; it calls `lwlte_air780ep_init()`, which registers URCs, toggles EN low/high, waits for `RDY`, and then sends AT initialization commands.

Old low-level `pwrkey_pin` / `reset_pin` / `status_pin` guidance is superseded. Current app code only configures the public Air780EP facade `en_pin`.

## Behavior

The current example flow is:

1. Configure logging and print a startup banner with pin/baud/APN defaults.
2. Call `lwlte_air780ep_init()` with UART, EN, CID, APN, reset pulse and timeout settings.
3. The facade creates AT Engine, Air780EP Modem and Core internally.
4. Air780EP Modem registers URCs, toggles EN, waits for `RDY`, sends AT initialization commands, then reports ready.
5. Register an LTE facade event callback that logs events and updates simple volatile flags.
6. Call `lwlte_connect()`.
7. Wait for `LWLTE_EVENT_NET_ONLINE` or `LWLTE_EVENT_NET_ERROR`.
8. Keep the task alive and print LTE/network state every 5 seconds.

The example should not call lower-level AT Engine, Modem or Core APIs directly. Runtime network control should go through LWLTE facade APIs.

## Event Logging

The LTE facade event callback should log at least:

- `LWLTE_EVENT_STARTED`
- `LWLTE_EVENT_READY`
- `LWLTE_EVENT_NET_CONNECTING`
- `LWLTE_EVENT_NET_ONLINE`
- `LWLTE_EVENT_NET_OFFLINE`
- `LWLTE_EVENT_NET_ERROR`
- `LWLTE_EVENT_STOPPED`
- `LWLTE_EVENT_ERROR`

For network events with data, include `net_state` and `error_code` in the log.

## Error Handling

Keep the example linear and explicit:

- If a create/init/start/connect step fails, log the failing step and error name.
- Cleanup in reverse order for resources that were successfully created.
- If connect times out, log the current LTE and network state.
- After a fatal setup failure, keep the task alive with periodic delay so serial logs remain readable.

Because the example is intended for manual board validation, it should prefer clear logs over complex recovery logic.

## Configuration Scope

Do not add Kconfig in the first version. Use local `#define` defaults in `main.c` and document them in `README.md`.

Rationale:

- The immediate target board/wiring is known.
- A minimal example should be easy to read without menuconfig.
- Kconfig can be added later if multiple boards or wiring variants need first-class support.

## README Content

The example README should include:

- What the example demonstrates.
- Hardware wiring table.
- Note that Air780EP EN on GPIO2 is controlled by the modem adapter during facade init.
- Build/flash/monitor commands.
- Expected serial log milestones: startup, LTE network connecting, network online or error.
- Troubleshooting notes for SIM/APN/signal/registration failures.

## Validation

Minimum validation for this task:

- Build the root project after adding the example only if root build is still relevant.
- Build the example as an independent ESP-IDF project.
- Run boundary checks to ensure Core still has no forbidden AT Engine/Air780EP/private dependency.
- Do not claim hardware success unless flash/serial monitor is actually run and logs show the expected events.

Hardware runtime validation is optional unless explicitly requested after the example compiles.

## Out Of Scope

- MQTT/HTTP examples.
- Kconfig-based board abstraction.
- OLED display output.
- Air780EP adapter changes for explicit EN semantics.
- Automated unit tests for hardware behavior.
- Changing the existing root `main/main.c` demo stub.
