# Basic Connect Example Design

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
| Air780EP EN | `GPIO2`, held high |
| UART baud rate | `115200` |
| APN | empty string, meaning module/operator default in the Air780EP activation path |

The Air780EP `EN` pin is level-controlled: high means the module runs, low powers it down. The example must configure GPIO2 as output and keep it high before creating AT Engine/Modem/Core.

Do not pass GPIO2 as `modem_air780ep_config_t.pwrkey_pin`. The current Air780EP adapter treats `pwrkey_pin` as a pulse output and returns it low after the pulse, which would power down this hardware wiring. Use `GPIO_NUM_NC` for `pwrkey_pin`, `reset_pin`, and `status_pin` in the modem config unless a future adapter adds explicit EN support.

## Behavior

The example flow is:

1. Configure logging and print a startup banner with pin/baud/APN defaults.
2. Drive Air780EP EN (`GPIO2`) high and wait briefly for hardware power stabilization.
3. Create `at_engine_t` with UART1, GPIO0/GPIO1, baud 115200, and practical RX/command buffer defaults.
4. Create `modem_t` via `modem_air780ep_create()` with pulse/control pins set to `GPIO_NUM_NC`.
5. Call `modem_init()`.
6. Create `lwlte_core_t` with `apn = ""`, `primary_cid = 1`, `auto_connect = false`, and default timeout/task values where zero is supported.
7. Register a Core event callback that logs each event and updates simple volatile flags.
8. Call `lwlte_core_start()`.
9. Wait for `LWLTE_CORE_EVENT_READY` with a timeout.
10. Call `lwlte_core_connect()`.
11. Wait for `LWLTE_CORE_EVENT_NET_ONLINE` or `LWLTE_CORE_EVENT_NET_ERROR`.
12. Keep the task alive and print Core/network state every 5 seconds.

The example should not call lower-level blocking modem operations from its public Core control path except for construction and `modem_init()`. Runtime network control should go through Core APIs.

## Event Logging

The Core event callback should log at least:

- `LWLTE_CORE_EVENT_STARTED`
- `LWLTE_CORE_EVENT_READY`
- `LWLTE_CORE_EVENT_NET_CONNECTING`
- `LWLTE_CORE_EVENT_NET_ONLINE`
- `LWLTE_CORE_EVENT_NET_OFFLINE`
- `LWLTE_CORE_EVENT_NET_ERROR`
- `LWLTE_CORE_EVENT_STOPPED`
- `LWLTE_CORE_EVENT_ERROR`

For network events with data, include `net_state` and `error_code` in the log.

## Error Handling

Keep the example linear and explicit:

- If a create/init/start/connect step fails, log the failing step and error name.
- Cleanup in reverse order for resources that were successfully created.
- If Core start/connect times out, log the current Core and network state.
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
- Note that Air780EP EN on GPIO2 is held high by the example.
- Build/flash/monitor commands.
- Expected serial log milestones: startup, Core ready, network connecting, network online or error.
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
