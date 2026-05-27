# net_mgr Activation Flow Design

## Context

The `basic_connect` example currently reaches `NET_ONLINE`, but the serial log shows a transient incorrect sequence:

```text
NET_CONNECTING
activation attempt 1 failed: ESP_ERR_INVALID_STATE
activation attempt 2 failed: ESP_ERR_INVALID_STATE
activation attempt 3 failed: ESP_ERR_INVALID_STATE
NET_ERROR
NET_ONLINE
```

The module was not actually failing. It reported registration status `2`, meaning searching/registering, and then registered shortly after:

```text
+CREG: 2
+CEREG: 2
+CGREG: 2
...
+CEREG: 1,"2797","02B82E08",7
+CGREG: 1,"2797","02B82E08",7
+CGEV: ME PDN ACT 1
```

The current `net_mgr` activation logic treats not-ready states as `ESP_ERR_INVALID_STATE` and exhausts three attempts too quickly. The design must distinguish waiting states from real failures.

## Online Definition

For the current project stage, `NET_ONLINE` means the local cellular data path is ready for application protocols such as MQTT and HTTP. It does not require ping or any external reachability test.

The required conditions are:

- SIM is ready.
- Cellular registration is complete with registration status `1` or `5`.
- Packet domain is attached with `CGATT=1`.
- PDP/PDN is active.
- A valid local IP address has been obtained.

Ping remains a diagnostic or optional health-check feature, not a prerequisite for `NET_ONLINE`.

## Layer Responsibilities

The activation flow keeps responsibilities separated by layer:

| Layer | Responsibility |
|-------|----------------|
| `at_engine` | Send and receive AT lines, collect command responses, dispatch idle-period URCs. |
| `modem_air780ep` | Translate Air780EP AT commands and URCs into stable modem state, cached snapshots, and modem events. |
| `net_mgr` | Own the network activation state machine, timeouts, retry decisions, online/offline transitions, and application-facing network events. |
| Application | Consume `NET_CONNECTING`, `NET_ONLINE`, `NET_OFFLINE`, and `NET_ERROR` without parsing modem details. |

`net_mgr` must not depend on any URC being delivered exactly once. URCs accelerate state discovery, but active queries and cached modem state provide the authoritative snapshot.

This boundary is important because `+CREG:`, `+CEREG:`, and `+CGREG:` can appear both as query responses and as URCs. The current AT engine intentionally treats matching lines during an active command as command response lines, not independent URCs.

## Activation State Machine

Replace the current “run a full activation attempt up to three times” behavior with an explicit staged activation loop:

```text
IDLE
-> CHECK_SIM
-> CHECK_SIGNAL
-> WAIT_REGISTRATION
-> WAIT_PACKET_ATTACH
-> SET_APN
-> ACTIVATE_PDP
-> QUERY_IP
-> ONLINE
```

The state machine starts by posting one `NET_CONNECTING` event and setting LTE/network state to activating. It should not post repeated `NET_CONNECTING` events while polling intermediate conditions.

## Stage Semantics

| Stage | Active operation | Success condition | Waiting condition | Failure condition |
|-------|------------------|-------------------|-------------------|-------------------|
| `CHECK_SIM` | Query `AT+CPIN?`. | `+CPIN: READY`. | SIM busy or short-lived unknown. | SIM removed, PIN/PUK required, SIM error, total timeout. |
| `CHECK_SIGNAL` | Query `AT+CSQ`. | Response parses successfully. | None in the first implementation. | AT failure or invalid response after retry policy. |
| `WAIT_REGISTRATION` | Query `AT+CEREG?`, then `AT+CGREG?`/`AT+CREG?` as fallback. | `stat=1` or `stat=5`. | `stat=2` searching/registering, short-lived unknown. | `stat=3` denied or total timeout. |
| `WAIT_PACKET_ATTACH` | Query `AT+CGATT?`. | `CGATT=1`. | `CGATT=0`. | Total timeout or hard AT error. |
| `SET_APN` | If configured APN is non-empty, configure APN; otherwise skip. | APN set or skipped. | None. | Command failure. |
| `ACTIVATE_PDP` | Use Air780EP TCPIP activation path: `AT+CSTT` and `AT+CIICR`. | Command sequence succeeds or PDP active is confirmed. | Activation command in progress. | Command failure or timeout. |
| `QUERY_IP` | Query `AT+CIFSR` or equivalent cached PDP context. | Valid local IP address. | None. | Missing, invalid, or timed-out IP response. |
| `ONLINE` | Maintain online state. | Stay online. | None. | PDP deactivation URC or explicit disconnect. |

Registration status `2` is not an error. It keeps the state machine in `WAIT_REGISTRATION` until registration succeeds, a terminal status appears, or the activation timeout expires.

## Active Query And URC Policy

Use a hybrid strategy:

- Active query establishes the authoritative state snapshot.
- URC updates cached modem state and can wake or accelerate the state machine.
- Periodic polling provides the fallback if URC is missed, swallowed into a command response, or never emitted.
- The total activation timeout provides the final boundary.

Rules:

- On entering each waiting stage, query once immediately.
- If the result is a waiting state, remain in the current stage and wait for either a relevant modem event or the next poll interval.
- On receiving a relevant URC/modem event, re-check the current stage condition instead of blindly declaring success.
- If a later active query observes a completed condition, advance even if no URC was delivered.
- If the total activation timeout expires, emit `NET_ERROR` with a stage-appropriate error.

Example with URC acceleration:

```text
WAIT_REGISTRATION
-> AT+CEREG? returns stat=2
-> remain in WAIT_REGISTRATION
-> receive +CEREG: 1
-> re-check registration condition
-> advance to WAIT_PACKET_ATTACH
```

Example with polling fallback:

```text
WAIT_REGISTRATION
-> AT+CEREG? returns stat=2
-> URC is not dispatched independently
-> next poll AT+CEREG? returns stat=1
-> advance to WAIT_PACKET_ATTACH
```

## Error Classification

The design uses three categories instead of treating every not-ready state as `ESP_ERR_INVALID_STATE`:

| Category | Examples | `net_mgr` behavior |
|----------|----------|--------------------|
| Waiting state | SIM busy, registration searching, `CGATT=0`. | Stay in the current stage and wait for URC or next poll. |
| Recoverable error | Temporary AT timeout, PDP activation failure, IP query failure. | Clean up the affected data path if needed, then retry according to reconnect policy. |
| Terminal error | SIM removed, PIN/PUK required, registration denied, total timeout. | Post one `NET_ERROR` and enter error state. |

The application-facing event stream should be stable:

```text
NET_CONNECTING
NET_ONLINE
```

or, on true failure:

```text
NET_CONNECTING
NET_ERROR
```

It should not produce `NET_ERROR` followed by `NET_ONLINE` for a normal registration-in-progress sequence.

## Initial Implementation Shape

The first implementation should be the smallest correct change:

- Keep activation execution inside the core FSM task.
- Replace the three rapid full activation retries with a staged loop using the existing `net_activate_timeout_ms` budget.
- Poll waiting stages at a controlled interval, initially 1 second for SIM busy, registration searching, and `CGATT=0`.
- Post `NET_CONNECTING` once at activation start.
- Keep existing modem URC parsing and cached modem state behavior.
- Do not add ping to the online condition.
- Avoid adding a complex wait object or semaphore until the staged polling flow is proven.

The first implementation may not fully exploit URCs for wakeup. It must still be correct through polling. Later work can use `MODEM_EVENT_REG_CHANGED`, `MODEM_EVENT_SIM_CHANGED`, and `MODEM_EVENT_PDP_ACTIVATED` to wake the state machine faster.

## Recovery Behavior

When activation reaches PDP/TCPIP stages and fails, do not blindly repeat the same command in a tight loop. For Air780EP TCPIP activation, recovery should clean the TCPIP scene before retrying data activation when appropriate.

Recommended behavior:

- If `CSTT`, `CIICR`, or `CIFSR` fails after registration and attach are ready, perform Air780EP data-path cleanup such as `AT+CIPSHUT` through the existing modem deactivation path before retrying.
- Preserve the total activation timeout for initial connect.
- Use the existing reconnect delay policy for later online-to-offline recovery.

## Deferred Work

These items are intentionally outside the first implementation:

- Mandatory ping or external reachability checks.
- DNS validation.
- Signal-quality threshold enforcement beyond parsing and logging `CSQ`.
- A fully event-driven wait primitive for `net_mgr`.
- Multi-CID PDP activation beyond the current Air780EP `cid=1` TCPIP path.
- APN authentication with username/password.

## Expected Result

For the observed startup sequence, the corrected log should look like:

```text
LTE event: NET_CONNECTING net=ACTIVATING err=0
... SIM busy handled as waiting ...
... CEREG/CGREG stat=2 handled as waiting ...
... CEREG/CGREG stat=1 observed ...
... PDP active and IP acquired ...
LTE event: NET_ONLINE net=ONLINE err=0
periodic: lte=ONLINE net=ONLINE
```

The corrected flow should not emit `NET_ERROR` while the module is simply searching/registering and still within `net_activate_timeout_ms`.
