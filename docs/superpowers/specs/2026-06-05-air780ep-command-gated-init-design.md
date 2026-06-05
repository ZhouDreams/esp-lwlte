# Air780EP Command-Gated Initialization Design

## Context

Air780EP startup currently waits for the `RDY` URC before sending basic AT initialization commands. The newer `docs/modem-init-min-flow.md` separates modem AT-channel readiness from later SIM, registration, attach, PDP, and IP readiness, and favors command-confirmed state transitions over URC-driven initialization.

This design changes Air780EP initialization so the startup path is driven only by AT command results. URCs may still be useful after initialization for runtime state observation and recovery, but they must not advance, cache, or accelerate the initialization flow in this phase.

## Goals

- Make `modem_start()` confirm module readiness by hard reset followed by `AT` returning `OK`.
- Keep `modem_start()` scoped to AT-channel readiness and basic AT parser configuration only.
- Keep SIM, signal, registration, packet attach, APN, PDP activation, and IP acquisition in Core network activation.
- Prevent initialization-stage URCs from affecting modem or core startup decisions.
- Define clear retry and error behavior for command-driven initialization.
- Update code comments and agent documentation so the flow no longer describes `RDY` as a startup gate.

## Non-Goals

- Do not design the future runtime URC state model in this change.
- Do not use `RDY`, `+CGEV`, `^MODE`, `+E_UTRAN Service`, `+NITZ`, registration URCs, or SIM URCs to advance initialization.
- Do not change the public API shape.
- Do not move network activation responsibilities into `modem_start()`.

## Approach

Use a command-gated startup sequence:

1. Reset internal volatile runtime flags.
2. Set modem state to `MODEM_STATE_INITIALIZING`.
3. Perform EN hardware reset, or flush RX when EN control is disabled.
4. Poll `AT` until it returns `OK`, within `ready_timeout_ms`.
5. Run basic initialization commands in order: `ATE0`, `AT+CMEE=1`, `AT+CEREG=2`, `AT+CGREG=2`, `AT+CREG=2`, `AT*I`.
6. Register runtime URC handlers only after basic command initialization succeeds.
7. Mark modem ready and post `MODEM_EVENT_READY`.

Runtime URC registration is deliberately after the command-gated init sequence. This makes the initialization boundary explicit: unsolicited lines received before ready are ignored by the URC subsystem and cannot update modem/core state.

## Modem Start Flow

`air780ep_start()` and `air780ep_reset()` should share the same startup semantics:

- They do not wait for `RDY`.
- They do not register an `RDY` synchronization handler.
- They do not use `ATE0` as the readiness probe.
- They use only `AT` as the readiness probe and require `OK`.
- They enter the basic init phase only after `AT OK`.

If a spontaneous `RDY`, `^MODE`, `+E_UTRAN Service`, `+CGEV`, or `+NITZ` line arrives during the startup probe, it is treated as irrelevant startup noise. It may appear in IO logs, but it must not update cached state, post events, release waits, or change core progress.

## Core Start Flow

`core_start()` remains an asynchronous public-facing startup request: it only submits `CORE_SIG_START` and returns. The Core FSM `handle_start()` path then calls `modem_start()` synchronously inside the Core task. `modem_start()` is blocking and returns `ESP_OK` only after `AT OK`, basic init commands, runtime URC registration, modem ready state, and `MODEM_EVENT_READY` posting have completed. Only after that `ESP_OK` does Core mark itself ready and start network activation.

If `modem_start()` returns an error, Core does not start network activation. It enters `CORE_STATE_ERROR` and posts `CORE_EVENT_ERROR`.

Network activation remains command-driven:

- SIM status is confirmed by `AT+CPIN?`.
- Signal is queried by `AT+CSQ`.
- Registration is confirmed by `AT+CEREG?`, then `AT+CGREG?`, then `AT+CREG?` fallback.
- Packet attach is confirmed by `AT+CGATT?`.
- PDP/IP readiness is confirmed by `AT+CSTT`, `AT+CIICR`, and `AT+CIFSR` returning a valid IP.

No startup-time URC may cause Core to skip any of these command-confirmed steps.

## Retry Policy

### AT Ready Probe

The `WAIT_AT_READY` phase uses the configured `ready_timeout_ms` total budget. Within that budget, it repeatedly sends `AT` and requires an `OK` response. Each individual `AT` probe uses a 1000 ms per-attempt timeout, separate from the total ready budget, so startup can retry regularly while the module is booting.

Each failed `AT` probe is followed by a 500 ms retry interval before the next attempt. The interval prevents a tight loop and avoids sending probe commands back-to-back while the module is still booting or while startup noise is crossing the UART.

Failures counted for a probe attempt include:

- AT engine send timeout.
- AT response status `ERROR`.
- AT response status `+CME ERROR`.
- AT response status `+CMS ERROR`.
- Aborted or invalid response status.

If the total ready timeout expires before `AT OK`, startup fails with `ESP_ERR_TIMEOUT`.

### Basic Init Commands

Each basic initialization command is attempted up to 3 times:

1. Send the command.
2. Require AT response `OK`.
3. On failure, log the command name, attempt number, response status, and error code when available.
4. Wait 500 ms before the next attempt.

The 3 attempts must not be sent back-to-back without delay.

Failures counted for a basic init attempt include:

- AT engine send timeout.
- AT response status `ERROR`.
- AT response status `+CME ERROR`.
- AT response status `+CMS ERROR`.
- Missing or invalid final response.

If any command still fails after 3 attempts, startup stops immediately and returns the last error for that command.

## Error Handling

When startup fails:

- `air780ep_start()` sets `MODEM_STATE_ERROR`.
- `air780ep_start()` does not post `MODEM_EVENT_READY`.
- Runtime URCs are not registered before initialization succeeds, so no URC rollback is needed on initialization failure.
- `modem_start()` returns the failure code to Core.
- Core handles the failure by entering `CORE_STATE_ERROR` and posting `CORE_EVENT_ERROR`.
- Facade maps the Core error to `LWLTE_EVENT_ERROR` through the existing event bridge.

## URC Boundary

This change intentionally postpones runtime URC semantics.

During initialization:

- `RDY` is ignored.
- `+CPIN:` is ignored.
- `+CREG:`, `+CEREG:`, and `+CGREG:` are ignored.
- `+CGEV:` is ignored.
- `+PDP DEACT` and `+PDP:DEACT` are ignored.
- `^MODE`, `+E_UTRAN Service`, and `+NITZ` are ignored.

After initialization, runtime URCs may later be used to update modem/core state, detect negative transitions, or trigger recovery. That runtime URC design is outside this initialization-focused change.

## Documentation Updates

Update repository documentation that currently describes Air780EP startup as waiting for `RDY`:

- `docs/modem-init-min-flow.md`
- `docs/agents/architecture.md`
- `docs/agents/classes.md`
- `docs/agents/at_cmd_air780ep.md`
- `src/include/lwlte.h` comments for `init_ready_timeout_ms`
- `src/core/core_fsm.c` comments around `modem_start()`

The new wording should consistently say that Air780EP startup readiness is confirmed by `AT OK` after hard reset, not by `RDY`.

## Verification

Verification should include:

- Build the ESP-IDF project.
- Search the codebase to confirm no Air780EP startup path waits for `RDY`.
- Search docs and comments to confirm `RDY wait` is no longer described as the startup gate.
- If hardware is available, flash and monitor startup logs to verify the sequence is hard reset, repeated `AT` until `OK`, basic init commands, Core network activation, then `NET_ONLINE` only after IP acquisition.
