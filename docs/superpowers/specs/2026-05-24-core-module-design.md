# Core Module MVP Design

## Goal

Implement the Core Service layer described in `docs/agents/classes.md` section 3. The MVP provides Core public APIs, an internal FSM task, network activation management, PDP context caching, Modem event handling, and App-facing event dispatch through `esp_event`.

Core remains the third layer in the four-layer architecture. It may call only the Modem public wrapper APIs (`modem_*`) and must not include AT Engine or concrete module headers such as `modem_air780ep.h`.

## Scope

Included:

- `lwlte_core.h` public API with opaque `lwlte_core_t`.
- Core lifecycle APIs: create, destroy, start, stop.
- Network control APIs: connect, disconnect.
- State query APIs for Core lifecycle state and network state.
- Core-owned `esp_event` loop and convenience event callback registration.
- Internal FSM task and signal queue.
- Modem event callback adapter that only queues FSM signals.
- `net_mgr` activation steps: SIM check, signal check, registration check, APN set, PDP activation.
- Fixed-delay reconnect when the primary PDP context is deactivated.
- `pdp_mgr` primary PDP context cache.
- `src/CMakeLists.txt` integration and build verification.

Excluded from this MVP:

- MQTT/HTTP clients.
- Keepalive probing.
- Exponential backoff.
- Multi-PDP policy beyond a small context cache.
- Unit test framework setup, because the repository currently has no test harness.

## File Layout

Add these files:

- `src/include/lwlte_core.h`: public Core API and public types.
- `src/core/core_priv.h`: internal structs, FSM signals, and internal function declarations.
- `src/core/core.c`: public API, lifecycle, state helpers, event loop, callback adapters.
- `src/core/core_fsm.c`: FSM queue/task lifecycle and signal dispatch.
- `src/core/net_mgr.c`: network activation flow and reconnect timer.
- `src/core/pdp_mgr.c`: PDP context cache helpers.

Update:

- `src/CMakeLists.txt`: add Core source files and the `esp_event` dependency.

## Public API

`lwlte_core.h` exposes:

```c
typedef struct lwlte_core lwlte_core_t;

typedef struct {
    const char *apn;
    uint8_t     primary_cid;
    uint32_t    net_activate_timeout_ms;
    uint32_t    reconnect_delay_ms;
    bool        auto_connect;
    int         fsm_queue_size;
    int         fsm_task_stack;
    int         fsm_task_priority;
} lwlte_core_config_t;

typedef enum {
    LWLTE_CORE_STATE_STOPPED = 0,
    LWLTE_CORE_STATE_STARTING,
    LWLTE_CORE_STATE_READY,
    LWLTE_CORE_STATE_NET_ACTIVATING,
    LWLTE_CORE_STATE_ONLINE,
    LWLTE_CORE_STATE_ERROR,
    LWLTE_CORE_STATE_DESTROYING,
} lwlte_core_state_t;

typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,
    LWLTE_NET_STATE_ACTIVATING,
    LWLTE_NET_STATE_ONLINE,
    LWLTE_NET_STATE_ERROR,
} lwlte_net_state_t;

ESP_EVENT_DECLARE_BASE(LWLTE_CORE_EVENT);

typedef enum {
    LWLTE_CORE_EVENT_STARTED = 0,
    LWLTE_CORE_EVENT_READY,
    LWLTE_CORE_EVENT_NET_CONNECTING,
    LWLTE_CORE_EVENT_NET_ONLINE,
    LWLTE_CORE_EVENT_NET_OFFLINE,
    LWLTE_CORE_EVENT_NET_ERROR,
    LWLTE_CORE_EVENT_STOPPED,
    LWLTE_CORE_EVENT_ERROR,
} lwlte_core_event_id_t;

typedef struct {
    lwlte_net_state_t net_state;
    int               error_code;
} lwlte_core_event_data_t;

typedef void (*lwlte_core_event_callback_t)(lwlte_core_t *core,
                                             lwlte_core_event_id_t event_id,
                                             const lwlte_core_event_data_t *data,
                                             void *user_ctx);

lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 modem_t *modem);
esp_err_t lwlte_core_destroy(lwlte_core_t *me);
esp_err_t lwlte_core_start(lwlte_core_t *me);
esp_err_t lwlte_core_stop(lwlte_core_t *me);
esp_err_t lwlte_core_register_event_callback(lwlte_core_t *me,
                                              lwlte_core_event_callback_t callback,
                                              void *user_ctx);
esp_event_loop_handle_t lwlte_core_get_event_loop(lwlte_core_t *me);
esp_err_t lwlte_core_get_state(lwlte_core_t *me, lwlte_core_state_t *state);
esp_err_t lwlte_core_get_net_state(lwlte_core_t *me, lwlte_net_state_t *state);
esp_err_t lwlte_core_connect(lwlte_core_t *me);
esp_err_t lwlte_core_disconnect(lwlte_core_t *me);
```

The header follows the existing Doxygen and bilingual line-comment style used by `modem.h`.

## Internal Architecture

`struct lwlte_core` owns the Core resources:

- Normalized config copy.
- Borrowed `modem_t *` dependency.
- Core-owned `esp_event_loop_handle_t`.
- `core_fsm_t`, `net_mgr_t`, and `pdp_mgr_t` submodules.
- Lifecycle state, destroying flag, and short-field mutex.

`core_fsm_t` owns the FSM task, queue, task-done semaphore, and stop flags. The task serializes all Core state transitions and all blocking `modem_*` calls.

`net_mgr_t` owns activation progress, retry counters, reconnect timer, and network state.

`pdp_mgr_t` owns a fixed-size cache of `modem_pdp_context_t` values and the primary CID.

## Threading Model

App-facing APIs do not perform blocking modem operations. They validate arguments and state, then enqueue a FSM signal with zero wait.

Modem event task calls the Core modem callback registered through `modem_register_event_callback()`. The callback copies the incoming `modem_event_t` into the FSM signal payload and enqueues it with zero wait. It does not call Core public APIs and does not mutate Core state directly.

The FSM task owns state transitions and calls blocking `modem_*` APIs. It must not hold `core->lock` while calling Modem APIs.

Core event notifications are posted to the Core-owned `esp_event` loop. App callbacks registered through `lwlte_core_register_event_callback()` run from the event loop task, not from the FSM task or Modem event task.

The convenience callback API supports one callback slot per Core instance. Consumers that need multiple independent subscriptions should use `lwlte_core_get_event_loop()` and call `esp_event_handler_register_with()` directly.

## Lifecycle Flow

Create:

- Validate `config` and `modem`.
- Allocate and normalize defaults.
- Create lock, event loop, FSM queue/task resources, and net reconnect timer.
- Initialize PDP cache.
- Register Core modem callback with the borrowed Modem instance.
- Initial state is `LWLTE_CORE_STATE_STOPPED`; network state is `LWLTE_NET_STATE_OFFLINE`.

Start:

- Queue `CORE_SIG_START`.
- FSM sets Core state to `STARTING` and posts `LWLTE_CORE_EVENT_STARTED`.
- FSM checks Modem state. If Modem is already ready or beyond ready, Core moves to `READY`; otherwise it waits for `MODEM_EVENT_READY`.
- If `auto_connect` is true after becoming ready, FSM starts network activation.

Stop:

- Queue `CORE_SIG_STOP`.
- FSM cancels reconnect timer, optionally deactivates the primary PDP context if online or activating, sets network state to offline, posts `STOPPED`, and returns to `STOPPED`.

Destroy:

- Reject destruction from the FSM task.
- Mark destroying.
- Stop FSM task and timers.
- Unregister the Modem callback by registering `NULL`.
- Delete event loop, synchronization objects, and allocated memory.
- Core does not destroy the borrowed Modem instance.

## Network Activation Flow

Activation is driven by the FSM task:

1. `CHECK_SIM`: call `modem_get_sim_status()` and require `MODEM_SIM_READY`.
2. `CHECK_SIGNAL`: call `modem_get_signal()`; command failure fails activation. The MVP does not enforce a signal-strength threshold.
3. `CHECK_REGISTRATION`: call `modem_get_registration()` and require home or roaming registration.
4. `SET_APN`: call `modem_set_apn(primary_cid, apn)`.
5. `ACTIVATE_PDP`: call `modem_activate_pdp(primary_cid)`, then `modem_get_pdp_context()` when activation succeeds.
6. `DONE`: update PDP cache, set network state online, set Core state online, and post `LWLTE_CORE_EVENT_NET_ONLINE`.

On activation error:

- Increment retry count.
- Retry until `max_retry` is reached.
- When retries are exhausted, set Core state to `ERROR`, network state to `ERROR`, and post `LWLTE_CORE_EVENT_NET_ERROR` with the error code.

The MVP uses a fixed internal max retry count of 3. It does not expose retry count in `lwlte_core_config_t` because that field is not in the approved class design.

## Modem Event Mapping

The FSM handles queued Modem events as follows:

- `MODEM_EVENT_READY`: set Core state to ready, post `READY`, optionally auto-connect.
- `MODEM_EVENT_SIM_CHANGED`: use as advisory state for future activation decisions; no public event in MVP unless it causes activation failure.
- `MODEM_EVENT_REG_CHANGED`: use as advisory state for future activation decisions; no public event in MVP unless it causes activation failure.
- `MODEM_EVENT_PDP_ACTIVATED`: update PDP cache, set online, post `NET_ONLINE`.
- `MODEM_EVENT_PDP_DEACTIVATED`: if primary CID or unknown CID is affected, set offline, post `NET_OFFLINE`, and start fixed-delay reconnect if Core is not stopping or destroying.
- `MODEM_EVENT_SIGNAL_CHANGED`: cache only if needed by `net_mgr`; no public event in MVP.
- `MODEM_EVENT_ERROR`: post `LWLTE_CORE_EVENT_ERROR` and let activation logic decide whether to retry or enter error state.

## Error Handling

Except for pointer-returning getters/constructors, Core public APIs return `esp_err_t` and use ESP-IDF standard error codes only.

- `lwlte_core_create()` returns `NULL` on invalid input or resource allocation failure.
- `lwlte_core_get_event_loop()` returns `NULL` on invalid input.
- Invalid pointers or invalid config values return `ESP_ERR_INVALID_ARG` from `esp_err_t` APIs.
- Calls that conflict with lifecycle state return `ESP_ERR_INVALID_STATE`.
- Resource allocation failures return `ESP_ERR_NO_MEM`.
- Queue send failures return `ESP_ERR_TIMEOUT`.
- Modem operation failures propagate their `esp_err_t` value where the public API is directly affected. Internal activation failures are reported through Core events.

The implementation uses `ESP_RETURN_ON_*` and `ESP_GOTO_ON_*` macros consistently with `docs/agents/err.md`. Functions using `ESP_GOTO_ON_*` name the local return variable `ret` and use the cleanup label `err`.

## Concurrency and Ownership

- Core borrows `modem_t *`; Board Init remains responsible for Modem lifecycle.
- Core owns its event loop, FSM queue/task resources, lock, semaphores, and reconnect timer.
- `core->lock` protects short fields only: lifecycle state, destroying flag, and task stop flags.
- Blocking Modem calls happen only from the FSM task and never while holding `core->lock`.
- FSM signal payloads are value-copied. Modem event payloads are copied into queue items so there is no borrowed pointer lifetime issue.

## Verification

Implementation verification uses the project build path from `docs/agents/build-and-debug.md`:

- Prefer the ESP-IDF MCP build tool.
- If MCP build is unavailable, use `idf.py build` after sourcing the configured ESP-IDF environment.
- Report whether verification is compile-only or includes hardware/serial validation. The MVP requires compile verification; hardware validation is not required unless requested.

## Acceptance Criteria

- Project builds with the new Core source files included.
- Public Core header compiles from an App include path.
- Core implementation does not include `at_engine.h` or `modem_air780ep.h`.
- `lwlte_core_start()`, `lwlte_core_stop()`, `lwlte_core_connect()`, and `lwlte_core_disconnect()` enqueue FSM signals and return without performing blocking Modem operations directly.
- Modem callbacks only enqueue FSM signals.
- Core events are posted through `esp_event_post_to()` on the Core-owned event loop.
- Destroy releases Core-owned resources and does not destroy the borrowed Modem.
