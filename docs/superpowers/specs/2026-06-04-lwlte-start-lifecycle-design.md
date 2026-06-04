# lwlte Start Lifecycle Design

## Context

The current Air780EP facade factory performs object creation and runtime startup in one call. `lwlte_air780ep_init()` creates the AT Engine, Modem, Core, Ping, and MQTT objects, then directly calls `modem_start()`, `core_start()`, waits for ready, and optionally submits a connect request through `auto_connect`.

This mixes two lifecycle phases that should remain separate:

- `init` / `create`: instantiate objects and wire dependencies.
- `start`: perform dynamic runtime bring-up.

The intended architecture is that the Facade exposes user APIs and owns dependency assembly, Core owns the LTE startup and network state machine, and Modem translates semantic module operations into concrete AT commands and URC events.

## Goals

- Make `lwlte_air780ep_init()` only construct and configure hidden objects.
- Add a public `lwlte_start()` API for users to explicitly start LTE service.
- Keep `modem_start()` as the internal operation that powers/resets the LTE module and waits for RDY/basic AT readiness.
- Make Core own the full startup sequence from module start through PDP activation and IP acquisition.
- Define `lwlte_start()` success as asynchronous submission; the online result is reported by event callback or state query.
- Remove the public `auto_connect` option because `lwlte_start()` is the public connect-to-online action.

## Non-Goals

- Do not move APN, PDP, retry, or network state-machine ownership into Modem.
- Do not make `lwlte_start()` block until online.
- Do not auto-start MQTT from `lwlte_start()`; MQTT keeps its own explicit `lwlte_mqtt_start()` lifecycle.
- Do not add backward compatibility for `auto_connect`; this project has no stated external compatibility requirement for that field.

## Lifecycle Semantics

### Object Creation

`lwlte_air780ep_init()` validates user configuration, allocates `lwlte_t`, creates AT Engine, creates Air780EP Modem, creates Core, creates Ping Client, optionally creates MQTT Client, registers internal event bridges, and returns the facade handle.

It must not:

- call `modem_start()`;
- call `core_start()`;
- call `lwlte_wait_ready()`;
- call `lwlte_connect()`;
- wait for RDY, Core ready, PDP activation, or IP acquisition.

### Public Start

`lwlte_start(lwlte_t *me)` is a public asynchronous API. It validates the facade, obtains the Core handle, and calls `core_start()`.

`ESP_OK` from `lwlte_start()` means the start request was submitted. It does not mean the modem is ready or the network is online.

The application observes final success through `lwlte_register_event_callback()` and `LWLTE_EVENT_NET_ONLINE`, or by polling `lwlte_get_net_state()` for `LWLTE_NET_STATE_ONLINE`.

Failures are reported through `LWLTE_EVENT_ERROR` or `LWLTE_EVENT_NET_ERROR`, depending on the failing stage.

### Modem Start

`modem_start()` keeps its name and means dynamic module bring-up: register URC handlers, perform hardware reset or controlled RDY wait, wait for `RDY`, execute basic AT initialization commands, and emit `MODEM_EVENT_READY`.

`modem_start()` does not configure APN, activate PDP, or query IP. Those remain Core-owned network tasks.

### Core Start

Core receives `CORE_SIG_START` in its FSM. The start handler changes Core to `CORE_STATE_STARTING`, posts `CORE_EVENT_STARTED`, calls `modem_start()`, then continues into Core network activation.

After `modem_start()` succeeds, Core posts `CORE_EVENT_READY` to indicate basic module readiness. Core then runs the existing network flow: SIM check, signal check, registration wait, packet attach wait, APN setup, PDP activation, and IP query. PDP active with a valid IP is the condition for `CORE_EVENT_NET_ONLINE` and `LWLTE_EVENT_NET_ONLINE`.

## API Changes

### Public Header

Add:

```c
esp_err_t lwlte_start(lwlte_t *me);
```

Remove from `lwlte_air780ep_config_t`:

```c
bool auto_connect;
```

Update `lwlte_air780ep_init()` comments to state that it only creates the facade and internal objects. Update `lwlte_start()` comments to state that it submits an asynchronous startup and online request.

### Existing API Roles

`lwlte_connect()` is removed from the public API because `lwlte_start()` now covers connect-to-PDP/IP-online semantics. Internal Core helpers may keep `core_connect()` if they are still useful for FSM implementation, but users should not see a separate connect operation.

`lwlte_wait_ready()` remains internal unless a separate public wait API is intentionally designed later. There is no event replay requirement: applications should register callbacks before calling `lwlte_start()` if they need asynchronous notification, or query state afterward.

## State And Error Behavior

- Calling `lwlte_start()` when the facade is valid and Core is stopped submits `CORE_SIG_START`.
- Repeated `lwlte_start()` while Core is starting, ready, activating, online, error-recovering, or destroying returns `ESP_ERR_INVALID_STATE` through `core_start()` state checks.
- If `modem_start()` fails, Core enters `CORE_STATE_ERROR` and posts `CORE_EVENT_ERROR`; Facade maps it to `LWLTE_EVENT_ERROR`.
- If SIM, registration, attach, APN, PDP activation, or IP acquisition fails, Core reports network failure through `CORE_EVENT_NET_ERROR`; Facade maps it to `LWLTE_EVENT_NET_ERROR`.
- `lwlte_destroy()` must succeed after `lwlte_air780ep_init()` even if `lwlte_start()` was never called. Destroy still releases resources in reverse ownership order: MQTT, Ping, Core, Modem, AT Engine, facade synchronization primitives.
- `lwlte_mqtt_start()` remains explicit. If it is called before network online, MQTT service keeps its existing waiting-network behavior rather than being auto-started by LTE start.

## Event Flow

Expected user-visible event order for a successful startup is:

```text
LWLTE_EVENT_STARTED
LWLTE_EVENT_READY
LWLTE_EVENT_NET_CONNECTING
LWLTE_EVENT_NET_ONLINE
```

`LWLTE_EVENT_READY` means Core observed the modem's basic module readiness. It is not the final success event for `lwlte_start()`; `LWLTE_EVENT_NET_ONLINE` is.

## Component Responsibilities

### Facade

- Owns dependency construction and destruction.
- Exposes `lwlte_air780ep_init()`, `lwlte_start()`, event callback registration, state query, MQTT, and Ping user APIs.
- Does not directly call Modem runtime operations outside factory construction/destruction paths.
- Does not encode LTE startup sequencing.

### Core

- Owns the startup sequence after `lwlte_start()` delegates to `core_start()`.
- Calls `modem_start()` during `CORE_SIG_START` handling.
- Owns SIM/register/attach/APN/PDP/IP flow and reconnect policy.
- Emits Core events that Facade maps to user events.

### Modem

- Owns module-specific bring-up implementation behind `modem_start()`.
- Translates Core semantic requests such as APN setup, PDP activation, IP query, MQTT commands, and ping into AT commands.
- Translates URC strings into `modem_event_t` events.
- Does not own the network state machine.

### AT Engine

- Remains a generic AT protocol and UART engine.
- Does not know module semantics or LTE network states.

## Documentation Impact

Update repository agent docs so they match the new lifecycle:

- `docs/agents/architecture.md`: revise Facade factory sequence, callback timing, and startup data flow.
- `docs/agents/classes.md`: revise `lwlte_air780ep_init()`, add `lwlte_start()`, remove `auto_connect`, and define `modem_start()` semantics precisely.
- `docs/agents/oop-design.md`: update lifecycle examples if they still show `auto_connect` or startup inside init.

## Example Impact

Examples should use this pattern:

```c
lwlte_t *lte = NULL;
ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
ESP_ERROR_CHECK(lwlte_register_event_callback(lte, app_event_handler, NULL));
ESP_ERROR_CHECK(lwlte_start(lte));
```

The callback should treat `LWLTE_EVENT_NET_ONLINE` as LTE startup success. MQTT examples should start MQTT explicitly after LTE online, or start MQTT early only if the MQTT service intentionally waits for network.

## Verification Plan

- Build the ESP-IDF project.
- Search the codebase to confirm `lwlte_air780ep_init()` no longer calls `modem_start()`, `core_start()`, `lwlte_wait_ready()`, or `lwlte_connect()`.
- Search the public config to confirm `auto_connect` is removed.
- Confirm `lwlte_start()` exists in the public header and delegates only to Core.
- Confirm Core start path calls `modem_start()` before network activation.
- With hardware, verify event order: `STARTED`, `READY`, `NET_CONNECTING`, `NET_ONLINE`.

## Open Decisions

No open design decisions remain.
