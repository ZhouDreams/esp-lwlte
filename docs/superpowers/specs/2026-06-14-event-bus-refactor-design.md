# Event Bus Refactor Design

## Context

The current event/callback architecture has three independent mechanisms that overlap and produce redundant work:

1. **Core has its own `esp_event_loop_handle_t`** (`src/core/core.c:599`). Core FSM `post_event_checked()` posts `CORE_EVENT_*` to this internal loop; a loop-registered adapter `core_event_adapter()` (`src/core/core.c:736`) unpacks the event and synchronously calls `me->event_callback`.
2. **MQTT client has its own `esp_event_loop_handle_t`** (`src/mqtt_client/mqtt_client.c:132`). `post_mqtt_event()` (`src/mqtt_client/mqtt_client.c:488`) posts to this loop **and** synchronously invokes `me->event_callback` from the FSM task — so the loop is effectively unused for dispatch (no handler is registered on `MQTT_CLIENT_EVENT`).
3. **Facade has no loop, only a single callback slot** (`src/lwlte/lwlte_priv.h:50`). `lwlte_register_event_callback` writes the slot; two translator bridges `lwlte_handle_core_event` / `lwlte_handle_mqtt_event` map internal event IDs into the public `lwlte_event_id_t` namespace and invoke the slot.

Three problems follow from this layout:

- **The two internal loops duplicate effort and split a naturally cross-module concept** (an event bus) across two private instances. Core and MQTT both maintain queue + task + register/unregister machinery, and MQTT's loop is dead weight.
- **Facade's translator bridges exist only because the internal layers do not speak the public event contract.** They are a pure mapping tax with no semantic value.
- **There is no public event bus.** The application can only observe events through the single facade callback slot. It cannot register multiple handlers, cannot share the bus with other ESP-IDF components, and cannot choose between callback and `esp_event_handler_register` styles — all of which are standard ESP-IDF patterns (`esp_wifi`, `esp_mqtt`, BLE).

The user's goal is a single shared event bus (default loop preferred) that both core and MQTT client post to directly, with the public event contract defined in `lwlte.h`. This is a breaking change; no compatibility shim will be provided.

## Goals

- Replace the two private event loops (core, MQTT client) with a single shared `esp_event_loop_handle_t` borrowed by the facade and passed down through config. Default = `esp_event_loop_get_default()`; user may supply a custom loop.
- Define two public event bases in `lwlte.h`: `LWLTE_EVENT` (core/lifecycle) and `LWLTE_MQTT_EVENT` (MQTT lifecycle + data).
- Delete the facade callback slot, the translator bridges, and the entire callback synchronization machinery (`callback_done_sema`, `callback_active`, `callback_tasks[]`, `callback_waiting`, `wait_callbacks_idle`, `add/remove_callback_task_locked`).
- Delete the public API `lwlte_register_event_callback` and the `lwlte_event_callback_t` typedef. Users register handlers via standard `esp_event_handler_register[_with]`.
- Move `CORE_EVENT_PROTOCOL_DATA` / `CORE_EVENT_PROTOCOL_CLOSED` off the event bus onto a private synchronous callback API exposed by core (`core_register_protocol_callback`, `core_register_protocol_closed_callback`). These are module-internal data-plane signals, not user-facing events.
- Remove the heap-clone dance in core (`clone_protocol_data`, `release_core_event_payload`) that existed only to survive the core event loop queue transit. The new synchronous callback path does not need this clone. Note: a new clone is introduced later in the path (MQTT FSM → bus dispatch for async user delivery), so the net allocation count per MQTT message is unchanged (3 clone pairs). The semantic improvement is that every clone serves a real purpose — no pure-overhead transit clones remain.
- Ensure `lwlte_mqtt_init` / `lwlte_mqtt_start` work when MQTT is initialized after `LWLTE_EVENT_NET_ONLINE` has already fired (late subscriber). MQTT client queries `core_get_net_state()` on start and subscribes to future changes via the shared bus.
- Define destroy semantics: internal handlers unregister before resource free; user handlers are the user's responsibility (same contract as `esp_wifi` / `esp_mqtt`).

## Non-Goals

- Do not preserve backward compatibility. `lwlte_register_event_callback` and the unified `lwlte_event_id_t` are deleted outright.
- Do not clean up `error_code` semantics. The field stays `int`, documented as "diagnostic only, may carry CME/CMS code or `esp_err_t`, do not branch on it". Cleanup is a separate task.
- Do not refactor `ping_client`. It remains as-is.
- Do not add reference counting for MQTT_DATA payload. Heap copy + explicit release is sufficient.
- Do not add a "state replay" subscription API (register-and-get-current). Late subscribers use query + subscribe-future.
- Do not protect user handlers from post-destroy dispatch. The user must unregister or guard their own handlers.
- Do not split `lwlte.h` into a separate `lwlte_events.h`. Event definitions live in `lwlte.h`; `core.h` and `mqtt_client.h` include it.

## Architecture

### Overview

```
┌─────────────────────────────────────────────────────────────┐
│              Shared event bus                               │
│   esp_event_loop_handle_t (= user-supplied or default loop) │
│                                                             │
│   Bases:                                                    │
│   ├─ LWLTE_EVENT        (core/lte lifecycle)                │
│   └─ LWLTE_MQTT_EVENT   (mqtt lifecycle + MQTT_DATA)        │
└──────▲───────────────────────▲────────────────────▲────────┘
       │ post                  │ post               │ register
       │                       │                    │ + handle
       │                       │                    │
  ┌────┴────────┐       ┌──────┴───────┐     ┌──────┴────────┐
  │ Core FSM    │       │ MQTT client  │     │ Application   │
  │             │       │              │     │ user handlers │
  │             │       │              │     │               │
  │ post:       │       │ post:        │     │ esp_event_    │
  │ LWLTE_EVENT │       │ LWLTE_MQTT_  │     │ handler_      │
  │ *_*         │       │ EVENT_*      │     │ register()    │
  │             │       │              │     │               │
  │             │       │ register:    │     │               │
  │             │       │ LWLTE_EVENT  │     │               │
  │             │       │ NET_ONLINE/  │     │               │
  │             │       │ OFFLINE/...  │     │               │
  │             │       │ → FSM signal │     │               │
  └──────┬──────┘       └──────▲───────┘     └───────────────┘
         │                     │
         │ sync callback       │ protocol data
         │ (private, no bus)   │ (from core's view: push to mqtt)
         │                     │
         └─────────────────────┘
           core_register_protocol_callback
           core_register_protocol_closed_callback
```

The bus is the only public event channel. Protocol data flows over a private synchronous callback, orthogonal to the bus, invisible to the user.

### Why two event bases

Splitting `LWLTE_EVENT` (core) from `LWLTE_MQTT_EVENT` (mqtt) lets the application register handlers independently per concern. It also aligns with the user's original intent of "pull MQTT callbacks out of the unified namespace". The single shared `lwlte_event_id_t` enum is split into two enums; the mixed `lwlte_event_data_t` is split into two structs.

### Why protocol data does not ride the bus

`PROTOCOL_DATA` and `PROTOCOL_CLOSED` are modem↔mqtt_client internal signals: high-volume (DATA), point-to-point, and semantically "transport layer" rather than "application layer". Exposing them on the bus would leak the internal protocol contract to the application and violate the facade boundary (the application should only see `LWLTE_MQTT_EVENT_DATA`, the parsed business message — never the raw protocol stream). Moving them to a private callback also eliminates the heap-clone hop that existed solely to survive the loop queue.

## Event Contract (public, in `lwlte.h`)

```c
#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(LWLTE_EVENT);
ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT);

typedef enum {
    LWLTE_EVENT_STARTED = 0,
    LWLTE_EVENT_READY,
    LWLTE_EVENT_NET_CONNECTING,
    LWLTE_EVENT_NET_ONLINE,
    LWLTE_EVENT_NET_OFFLINE,
    LWLTE_EVENT_NET_ERROR,
    LWLTE_EVENT_STOPPED,
    LWLTE_EVENT_ERROR,
} lwlte_event_id_t;

typedef enum {
    LWLTE_MQTT_EVENT_STARTED = 0,
    LWLTE_MQTT_EVENT_STOPPED,
    LWLTE_MQTT_EVENT_CONNECTING,
    LWLTE_MQTT_EVENT_CONNECTED,
    LWLTE_MQTT_EVENT_DISCONNECTED,
    LWLTE_MQTT_EVENT_SUBSCRIBED,
    LWLTE_MQTT_EVENT_UNSUBSCRIBED,
    LWLTE_MQTT_EVENT_PUBLISHED,
    LWLTE_MQTT_EVENT_DATA,
    LWLTE_MQTT_EVENT_ERROR,
} lwlte_mqtt_event_id_t;

typedef struct {
    lwlte_net_state_t net_state;
    int error_code;
} lwlte_event_data_t;

typedef struct {
    lwlte_mqtt_state_t mqtt_state;
    int error_code;
    lwlte_mqtt_msg_t msg;          /* valid only for LWLTE_MQTT_EVENT_DATA */
    bool owns_payload;             /* true for DATA events, false otherwise */
} lwlte_mqtt_event_data_t;

/* Mandatory release after handling LWLTE_MQTT_EVENT_DATA. */
void lwlte_mqtt_event_data_release(lwlte_mqtt_event_data_t *data);
```

Deleted from the public surface:

- `lwlte_event_callback_t` typedef
- `lwlte_register_event_callback` function
- The old unified `lwlte_event_id_t` (which interleaved `LWLTE_EVENT_*` and `LWLTE_EVENT_MQTT_*`)
- The old `lwlte_event_data_t` (which carried both `net_state`, `mqtt_state`, and a `mqtt_msg` union member)

### `error_code` semantics

`error_code` remains `int` in both data structs. It is a best-effort diagnostic number:

- `0` for non-error events.
- For error events (`LWLTE_EVENT_ERROR`, `LWLTE_EVENT_NET_ERROR`, `LWLTE_MQTT_EVENT_ERROR`), the value may be a CME/CMS code from the AT layer (positive) or an `esp_err_t` value (negative). Callers MUST NOT branch on specific values for control flow. The only place it is consumed as a return value is `lwlte_wait_ready` on init failure, which casts to `esp_err_t` for the failure path — this is best-effort and documented.

Cleanup of the mixed type is a separate task, out of scope here.

### `lwlte_mqtt_msg_t` ownership

The struct definition (`src/include/lwlte.h:81-86`) is unchanged:

```c
typedef struct {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} lwlte_mqtt_msg_t;
```

Semantics change: for `LWLTE_MQTT_EVENT_DATA`, `topic` and `payload` point to heap-owned buffers. The handler must call `lwlte_mqtt_event_data_release()` before returning. The `owns_payload` flag is the signal that release is required. For all other MQTT events, `msg` is zero-initialized and `owns_payload` is false; release is a no-op but still safe to call.

If multiple handlers are registered for `LWLTE_MQTT_EVENT_DATA` on the same loop, only the last one to call `release()` actually frees the buffers. The recommended pattern is a single consumer for `LWLTE_MQTT_EVENT_DATA`; additional observers should copy the data and not release.

## Core Layer Changes

### Deleted

| Item | Location | Reason |
|------|----------|--------|
| `core.event_loop` field | `core_priv.h` | Loop is now borrowed via config |
| `create_event_loop()` / `destroy_event_loop()` | `core.c:593, 627` | No internal loop |
| `core_event_adapter()` | `core.c:736` | No adapter needed without internal loop |
| `CORE_EVENT` base + `core_event_id_t` enum | `core.h:83-100` | Use public `LWLTE_EVENT` + `lwlte_event_id_t` |
| `core_register_event_callback()` / `core_event_callback_t` | `core.h:313, 242` | No callback slot |
| `core_get_event_loop()` | `core.h:325` | No internal loop to expose |
| `core_post_protocol_data()` | `core.c:527` | Replaced by synchronous callback |
| `clone_protocol_data()` / `release_core_event_payload()` | `core.c:1189, 1209` | Loop transit no longer needed |

### Added / Changed

```c
/* core.h */
#include "lwlte.h"   /* pulls in LWLTE_EVENT base, lwlte_event_id_t, lwlte_event_data_t */

typedef void (*core_protocol_callback_t)(core_handle_t *me,
                                         const core_protocol_data_t *data,
                                         void *user_ctx);

typedef void (*core_protocol_closed_callback_t)(core_handle_t *me,
                                                core_protocol_t protocol,
                                                void *user_ctx);

esp_err_t core_register_protocol_callback(core_handle_t *me,
                                          core_protocol_callback_t callback,
                                          void *user_ctx);

esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx);

typedef struct {
    const char *apn;
    uint8_t primary_cid;
    esp_event_loop_handle_t event_loop;   /* NEW: borrowed from facade */
} core_config_t;
```

`core_post_event()` changes implementation but keeps signature shape — it now calls `esp_event_post_to(me->config.event_loop, LWLTE_EVENT, event_id, event_data, sizeof(*event_data), 0)` directly. The `event_id` type becomes `lwlte_event_id_t`.

### FSM behavior change

`core_fsm.c:467-479` (handling of `MODEM_EVENT_PROTOCOL_DATA` / `_CLOSED`) changes from "post to loop" to "synchronous callback":

```c
case MODEM_EVENT_PROTOCOL_DATA: {
    /* Build protocol_data pointing at modem event's heap buffers.
     * No clone here — callback receives direct pointers and must
     * copy what it needs before returning. */
    core_protocol_data_t pd = {
        .protocol    = (core_protocol_t)event->data.protocol_data.protocol,
        .topic       = event->data.protocol_data.topic,
        .topic_len   = event->data.protocol_data.topic_len,
        .payload     = event->data.protocol_data.payload,
        .payload_len = event->data.protocol_data.payload_len,
    };
    if (me->protocol_callback) {
        me->protocol_callback(me, &pd, me->protocol_user_ctx);
    }
    /* modem event payload freed by FSM as before */
    break;
}
case MODEM_EVENT_PROTOCOL_CLOSED: {
    if (me->protocol_closed_callback) {
        me->protocol_closed_callback(me, CORE_PROTOCOL_MQTT,
                                     me->protocol_closed_user_ctx);
    }
    break;
}
```

All `CORE_EVENT_*` constants referenced by the FSM become `LWLTE_EVENT_*`.

### Payload allocation path (before vs after)

Each MQTT message travels through several clone hops. Counting one "clone pair" = topic + payload copied together:

**Current (3 clone pairs, including 1 pure-overhead):**

1. Modem parses URC → heap-alloc topic/payload (original, not counted as clone).
2. Core `modem_event_cb` clones for modem→core FSM queue transit — **clone #1** (necessary, crosses task boundary).
3. Core FSM → `core_post_protocol_data` → `clone_protocol_data` clones for core loop queue transit — **clone #2** (pure overhead: exists only because protocol data crosses the core event loop queue; freed immediately after the loop dispatches it to mqtt's handler).
4. MQTT `handle_core_event` clones into `mqtt_protocol_data_owned_t` — **clone #3** (necessary, crosses into mqtt FSM task).
5. MQTT FSM dispatches to user via synchronous callback — no additional clone (user borrows #3's pointers during callback).

**New (3 clone pairs, all necessary):**

1. Modem parses URC → heap-alloc (original).
2. Core `modem_event_cb` clones for modem→core FSM queue transit — **clone #1** (same as before).
3. Core FSM invokes `protocol_callback` synchronously → MQTT callback clones into `mqtt_protocol_data_owned_t` — **clone #2** (necessary, crosses into mqtt FSM task). Clone #1 freed after callback returns.
4. MQTT FSM posts `LWLTE_MQTT_EVENT_DATA` to the async bus → clones topic/payload into bus-owned buffers — **clone #3** (necessary, because the bus is async and the FSM cannot hold the buffers until the user handler runs). Clone #2 freed after post.
5. User handler runs, calls `lwlte_mqtt_event_data_release()` → clone #3 freed.

**Net change: 3 clone pairs → 3 clone pairs.** The core loop transit clone (old #2, pure overhead) is eliminated; the bus dispatch clone (new #3, necessary for async delivery) is added. Total allocation pressure is identical; the improvement is semantic — every remaining clone serves a real purpose.

## MQTT Client Layer Changes

### Deleted

| Item | Location | Reason |
|------|----------|--------|
| `mqtt.event_loop` field | `mqtt_client_priv.h:93` | Loop borrowed via config |
| `create_event_loop()` / `destroy_event_loop()` | `mqtt_client.c:132, 147` | No internal loop |
| `mqtt_client_register_event_callback()` / `mqtt_client_event_callback_t` | `mqtt_client.h:110, 123` | No callback slot |
| `MQTT_CLIENT_EVENT` base + `mqtt_client_event_id_t` | `mqtt_client.h:63, 65` | Use public `LWLTE_MQTT_EVENT` + `lwlte_mqtt_event_id_t` |
| `wait_event_callbacks_idle()` | `mqtt_client.c:425` | No callback sync needed |
| `event_callback_*` fields | `mqtt_client_priv.h:111-114` | Same |
| `mqtt_client_get_event_loop()` | `mqtt_client.h:126` | No internal loop |

### Added / Changed

```c
/* mqtt_client.h */
#include "lwlte.h"

typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
    /* ... existing fields ... */
    esp_event_loop_handle_t event_loop;   /* NEW: borrowed from facade */
} mqtt_client_config_t;
```

`post_mqtt_event()` simplifies — it just calls `esp_event_post_to(me->config.event_loop, LWLTE_MQTT_EVENT, event_id, payload, sizeof(*payload), 0)`. No more synchronous callback invocation, no `event_callback_active` bookkeeping.

### Internal protocol callbacks

```c
static void mqtt_protocol_data_cb(core_handle_t *core,
                                  const core_protocol_data_t *data,
                                  void *user_ctx)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)user_ctx;
    if (data->protocol != CORE_PROTOCOL_MQTT) return;

    mqtt_protocol_data_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) return;
    owned->topic = clone_string(data->topic);
    owned->topic_len = data->topic_len;
    owned->payload = clone_payload(data->payload, data->payload_len);
    owned->payload_len = data->payload_len;
    if (!owned->topic || !owned->payload) {
        free(owned->topic); free(owned->payload); free(owned);
        return;
    }

    mqtt_fsm_sig_t sig = { .type = MQTT_SIG_PROTOCOL_DATA, .data = owned };
    if (send_fsm_sig(me, &sig) != ESP_OK) {
        free(owned->topic); free(owned->payload); free(owned);
    }
}

static void mqtt_protocol_closed_cb(core_handle_t *core,
                                    core_protocol_t protocol,
                                    void *user_ctx)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)user_ctx;
    if (protocol != CORE_PROTOCOL_MQTT) return;

    mqtt_fsm_sig_t sig = { .type = MQTT_SIG_PROTOCOL_CLOSED };
    (void)send_fsm_sig(me, &sig);
}
```

These are registered in `mqtt_client_create()` via `core_register_protocol_callback(core, mqtt_protocol_data_cb, me)` and `core_register_protocol_closed_callback(core, mqtt_protocol_closed_cb, me)`.

### Bus handler for net state

The old `handle_core_event()` (mqtt_client.c:254) becomes `handle_lwlte_event()` and listens on the shared bus:

```c
static void handle_lwlte_event(void *handler_arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)handler_arg;
    if (base != LWLTE_EVENT) return;
    if (me->destroying) return;

    mqtt_fsm_sig_t sig = {0};
    switch ((lwlte_event_id_t)event_id) {
    case LWLTE_EVENT_NET_ONLINE:
        sig.type = MQTT_SIG_NET_ONLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        sig.type = MQTT_SIG_NET_OFFLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    default:
        break;
    }
}
```

Registered for `LWLTE_EVENT` base (or specifically NET_ONLINE + NET_OFFLINE) in `mqtt_client_create()`.

### Late subscriber handling

On `MQTT_SIG_START`, the FSM queries current net state in addition to relying on future events:

```c
case MQTT_SIG_START: {
    /* active probe: catch the case where NET_ONLINE fired before we registered */
    core_net_state_t ns = CORE_NET_STATE_OFFLINE;
    (void)core_get_net_state(me->core, &ns);
    if (ns == CORE_NET_STATE_ONLINE) {
        begin_connect(me);
    } else {
        set_state(me, MQTT_CLIENT_STATE_WAITING_NET);
    }
    break;
}
```

Race between probe and arriving event is benign: if the probe reads OFFLINE but `LWLTE_EVENT_NET_ONLINE` is already in the bus queue, the FSM transitions to WAITING_NET and then receives `MQTT_SIG_NET_ONLINE` shortly after — at most one wasted connection attempt if the state then flips back. Eventual consistency is guaranteed by FIFO bus dispatch and FSM idempotence on online/offline signals.

## Facade Layer Changes

### `lwlte_handle` struct

Removed: `callback_done_sema`, `event_callback`, `event_user_ctx`, `callback_active`, `callback_task_overflow`, `callback_tasks[]`, `callback_task_counts[]`, `callback_waiting`. Roughly 80 bytes saved.

Added: `esp_event_loop_handle_t event_loop`.

```c
struct lwlte_handle {
    at_engine_handle_t *at;
    modem_handle_t *modem;
    core_handle_t *core;
    mqtt_client_handle_t *mqtt;
    ping_client_handle_t *ping;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t ready_sema;
    SemaphoreHandle_t api_done_sema;
    esp_event_loop_handle_t event_loop;   /* borrowed */
    int init_error_code;
    int active_api_calls;
    int ready_waiter_count;
    bool ready;
    bool init_failed;
    bool destroying;
};
```

### Deleted functions

| Function | Location | Reason |
|---|---|---|
| `lwlte_register_event_callback` | `lwlte.c:311` | API removed |
| `wait_callbacks_idle` | `lwlte.c:1088` | No callback sync |
| `callback_task_active_locked` | `lwlte.c:1132` | Same |
| `add_callback_task_locked` / `remove_callback_task_locked` | `lwlte.c:1147, 1172` | Same |
| `lwlte_handle_core_event` | `lwlte.c:742` | No translator bridge |
| `lwlte_handle_mqtt_event` | `lwlte.c:798` | Same |
| `map_core_event` / `map_mqtt_event` | `lwlte.c:909, 953` | No translation needed |
| `map_core_event_data` | `lwlte.c:980` | Same |

### Facade factory change

`lwlte_air780ep.c` / `lwlte_ml307r.c` no longer call `core_register_event_callback(core, lwlte_handle_core_event, me)`. Instead:

```c
/* Resolve the loop: NULL = default loop, must already exist */
esp_event_loop_handle_t loop = config->event_loop;
if (loop == NULL) {
    loop = esp_event_loop_get_default();
    if (loop == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
}
me->event_loop = loop;

/* Pass loop down to core via config */
core_config_t core_cfg = {
    .apn         = config->apn,
    .primary_cid = config->primary_cid,
    .event_loop  = me->event_loop,
};
me->core = core_create(&core_cfg, modem);
```

`lwlte_mqtt_init` similarly passes `me->event_loop` through `mqtt_client_config_t.event_loop`.

### `lwlte_wait_ready` preservation

`lwlte_wait_ready` (`lwlte.c:695`) is preserved as a convenience API. It no longer relies on the translator bridge to set `ready`/`init_failed`; instead the facade registers an internal handler on the shared bus:

```c
/* Registered once at factory time, unregistered at destroy */
static void facade_ready_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    lwlte_handle_t *me = (lwlte_handle_t *)arg;
    const lwlte_event_data_t *ev = data;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) { xSemaphoreGive(me->lock); return; }
    if ((lwlte_event_id_t)id == LWLTE_EVENT_READY) {
        me->ready = true;
        wake_ready_waiters_locked(me);
    } else if ((lwlte_event_id_t)id == LWLTE_EVENT_ERROR && !me->ready) {
        me->init_failed = true;
        me->init_error_code = ev ? ev->error_code : ESP_FAIL;
        wake_ready_waiters_locked(me);
    }
    xSemaphoreGive(me->lock);
}
```

The user is unaware of this internal handler — it is an implementation detail of `lwlte_wait_ready`.

### `lwlte_destroy` simplification

```c
esp_err_t lwlte_destroy(lwlte_handle_t *me)
{
    /* 1. lock + set destroying=true + wake ready waiters */
    /* 2. wait_api_calls_idle() */
    /* 3. destroy_owned_resources():
     *    - mqtt_client_destroy(mqtt)  → internally: stop FSM task, unregister
     *                                    LWLTE_EVENT handler, free resources
     *    - core_destroy(core)         → internally: stop FSM task, clear protocol
     *                                    callback slots, free resources
     *    - ... (ping, modem, at as before)
     * 4. esp_event_handler_unregister_with(loop, LWLTE_EVENT, READY/ERROR,
     *                                      facade_ready_handler)
     * 5. free semaphores + free(me) */
}
```

No `wait_callbacks_idle` call. The order "stop FSM task THEN unregister handler" guarantees no new events are posted to the bus after unregister (because the producer — the FSM task — is already gone).

### User handler responsibility

`lwlte_destroy` does **not** unregister user handlers. If a user handler is dispatched after destroy, it receives a dangling `lte` pointer. The user MUST either:

1. Call `esp_event_handler_unregister` before `lwlte_destroy`, or
2. Guard the handler with an application-level flag, or
3. Ensure destroy is called from a context where no events are in flight (typical single-threaded `app_main` cleanup path).

This contract matches `esp_wifi` / `esp_mqtt`.

## Destroy Safety

### Internal handler safety

Three internal handlers live on the shared bus after this refactor:

| Handler | Registered by | Listens for | Purpose |
|---|---|---|---|
| `facade_ready_handler` | facade factory | `LWLTE_EVENT_READY` / `ERROR` | `lwlte_wait_ready` synchronization |
| `mqtt_net_online_handler` | `mqtt_client_create` | `LWLTE_EVENT_NET_ONLINE` | Drive MQTT FSM |
| `mqtt_net_offline_handler` | `mqtt_client_create` | `LWLTE_EVENT_NET_OFFLINE` | Drive MQTT FSM |

Each owning layer is responsible for unregistering its handler before freeing the resources the handler touches. The ordering invariant is:

1. Set `destroying = true` (handler checks this on entry and returns early).
2. Stop the FSM task (no new events will be posted).
3. `esp_event_handler_unregister_with(loop, base, id, handler)`.
4. Free resources.

If a handler is mid-dispatch when unregister runs, `esp_event_handler_unregister_with` guarantees it will not be dispatched again, but does not wait for the in-flight one. The `destroying` flag is the safety net for the in-flight case.

### User handler safety

Out of scope for the framework. Same contract as `esp_wifi`. Documented clearly in `lwlte.h` API docs and examples.

## MQTT_DATA Payload Lifecycle

1. Modem parses `+MSUB` URC → heap-allocates topic/payload.
2. modem→core event queue transit: core receives the event, clones topic/payload into core-owned heap (clone #1).
3. core FSM processes the event, invokes `protocol_callback` synchronously. MQTT client's callback clones topic/payload into `mqtt_protocol_data_owned_t` (clone #2), enqueues it as an FSM signal.
4. MQTT FSM processes the signal, then posts `LWLTE_MQTT_EVENT_DATA` to the bus. Because the bus is asynchronous, the FSM clones topic/payload once more into event-data-owned heap (clone #3) before posting. `owns_payload = true`.
5. Bus dispatches to user handler. User reads `event_data->msg.topic` / `.payload`.
6. User calls `lwlte_mqtt_event_data_release(event_data)` → frees the heap buffers from clone #3.

Total: 3 heap alloc/free pairs per MQTT message (same count as the current loop-based design, but with different semantics — see "Payload allocation path" in Core Layer Changes above for the full analysis). The original modem allocation is freed when the modem event is released; the core-recv clone (#1) is freed by core FSM after the protocol callback returns; the mqtt-cb clone (#2) is freed by MQTT FSM after posting the event; the bus clone (#3) is freed by the user via release.

If the user forgets to call release, clone #3 leaks. This is detectable in testing and documented in the API.

## Configuration

### New `event_loop` field

`lwlte_air780ep_config_t` and `lwlte_ml307r_config_t` each gain:

```c
esp_event_loop_handle_t event_loop;   /**< Optional event loop handle. NULL = use default loop. */
```

The user is responsible for ensuring the loop exists before calling `lwlte_*_init`. If `event_loop == NULL`, the factory calls `esp_event_loop_get_default()`; if that also returns NULL, the factory returns `ESP_ERR_INVALID_STATE`.

Typical usage:

```c
/* app_main early */
ESP_ERROR_CHECK(esp_event_loop_create_default());

/* later */
lwlte_air780ep_config_t config = {
    .uart_num = UART_NUM_1,
    /* ... */
    .event_loop = NULL,   /* or esp_event_loop_get_default() */
};
ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
```

## Examples / Tests / Docs

### Examples

All four examples change shape:

| File | Change |
|---|---|
| `example/air780ep_mqtt_client.c` | Replace `lwlte_register_event_callback` with `esp_event_handler_register(LWLTE_EVENT, ...)` and `esp_event_handler_register(LWLTE_MQTT_EVENT, ...)`. Split callback into two handlers (one per base). Call `lwlte_mqtt_event_data_release` after consuming DATA. |
| `example/ml307r_mqtt_client.c` | Same |
| `example/air780ep_basic_connect.c` | Replace callback registration; only LWLTE_EVENT handler needed |
| `example/ml307r_basic_connect.c` | Same |

Callback signature changes from `lwlte_event_callback_t` to the standard `esp_event_handler_t`:

```c
/* Old */
static void lte_event_cb(lwlte_handle_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *ctx);

/* New */
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t id, void *data);
static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data);
```

### Tests

Host contract tests in `tests/host/` update:

- Remove contracts asserting the presence of `lwlte_register_event_callback` in the API sequence (e.g. `test_mqtt_end_to_end_contract.py:832-833`).
- Remove contracts asserting callback-sync fields in `struct lwlte_handle`.
- Add contracts for `core_register_protocol_callback` / `core_register_protocol_closed_callback` in core's API.
- Add contracts for `event_loop` field presence in `lwlte_air780ep_config_t`, `lwlte_ml307r_config_t`, `core_config_t`, `mqtt_client_config_t`.
- Add contracts asserting absence of `core.event_loop`, `mqtt_client.event_loop`, and `lwlte_handle.event_callback`.

### Docs

| Doc | Change |
|---|---|
| `docs/agents/architecture.md` | §6.1 init chain: remove `core_register_event_callback(core, facade_core_event_handler, lte)`, add `event_loop` in config, add new "Event Bus" subsection with the architecture diagram, add "Protocol Private Callback" subsection |
| `docs/agents/classes.md` | Delete `lwlte_event_callback_t`, `lwlte_register_event_callback`; add `LWLTE_EVENT` / `LWLTE_MQTT_EVENT` bases, `lwlte_event_id_t`, `lwlte_mqtt_event_id_t`, `lwlte_mqtt_event_data_release` |
| `docs/agents/err.md` | `error_code` semantics note (diagnostic only, no control flow) |
| `docs/agents/oop-design.md` | Replace `lwlte_register_event_callback` in code samples |
| `AGENTS.md` / `AGENTS_ZH.md` | Sync any index changes |

## Implementation Order

Seven phases, each independently compilable and testable. Order is fixed by dependencies.

### Phase 1 — Define new event contract

1.1 Add `LWLTE_EVENT` / `LWLTE_MQTT_EVENT` bases, new enums, new event_data structs, `lwlte_mqtt_event_data_release` declaration to `lwlte.h`.
1.2 Keep old `lwlte_event_id_t` and `lwlte_register_event_callback` temporarily — the project must still compile.
1.3 Verify build.

### Phase 2 — Core refactor (bottom-up)

2.1 Add `event_loop` field, `protocol_callback` slots to `core_priv.h`.
2.2 Add `event_loop` to `core_config_t` in `core.h`.
2.3 Add `core_register_protocol_callback` / `_closed_callback` declarations to `core.h`.
2.4 Implement protocol callback registration in `core.c`.
2.5 Change `core_fsm.c` PROTOCOL_DATA/CLOSED handling to synchronous callback.
2.6 Delete `create_event_loop` / `destroy_event_loop` / `core_event_adapter` in `core.c`.
2.7 Replace all `CORE_EVENT_*` constants with `LWLTE_EVENT_*` in `core_fsm.c`; change `event_id` type to `lwlte_event_id_t`.
2.8 Change `core_post_event` to use `me->config.event_loop` + `LWLTE_EVENT` directly.
2.9 Delete `core_post_protocol_data` / `clone_protocol_data` / `release_core_event_payload` in `core.c`.
2.10 Delete `core_register_event_callback` / `core_get_event_loop`.
2.11 Build + unit test.

### Phase 3 — MQTT client refactor

3.1 Update `mqtt_client_priv.h`: remove event_callback_* sync fields, change event_loop to borrowed.
3.2 Add `event_loop` to `mqtt_client_config_t` in `mqtt_client.h`.
3.3 Delete `mqtt_client_register_event_callback` / `mqtt_client_get_event_loop` from `mqtt_client.h`.
3.4 Delete `MQTT_CLIENT_EVENT` base + `mqtt_client_event_id_t` enum.
3.5 Implement `mqtt_protocol_data_cb` / `mqtt_protocol_closed_cb`, register them with core in `mqtt_client_create`.
3.6 Simplify `post_mqtt_event` to pure `esp_event_post_to(LWLTE_MQTT_EVENT, ...)`.
3.7 Delete `create_event_loop` / `destroy_event_loop` / `wait_event_callbacks_idle`.
3.8 Rename `handle_core_event` → `handle_lwlte_event`, listen on `LWLTE_EVENT` for NET_ONLINE/OFFLINE only.
3.9 Add `core_get_net_state` probe on `MQTT_SIG_START`.
3.10 Unregister bus handler in `mqtt_client_destroy`.
3.11 Build + unit test.

### Phase 4 — Facade refactor

4.1 Update `lwlte_priv.h`: delete callback_* sync fields, add `event_loop` field.
4.2 Add `event_loop` field to `lwlte_air780ep_config_t` / `lwlte_ml307r_config_t` in `lwlte.h`.
4.3 Pass `me->event_loop` through to core/mqtt config in factory and `lwlte_mqtt_init`.
4.4 Default loop fallback + validation in factory.
4.5 Delete `lwlte_handle_core_event` / `lwlte_handle_mqtt_event` and all `map_*` functions.
4.6 Delete `wait_callbacks_idle` / `callback_task_*` machinery.
4.7 Simplify `lwlte_destroy` (remove `wait_callbacks_idle` call).
4.8 Add facade internal handler for LWLTE_EVENT_READY/ERROR → set ready_sema.
4.9 Unregister facade internal handler in `lwlte_destroy`.
4.10 Build + unit test.

### Phase 5 — Delete old public API

5.1 Delete `lwlte_event_callback_t` and `lwlte_register_event_callback` from `lwlte.h`.
5.2 Delete old `lwlte_event_id_t` (with `LWLTE_EVENT_MQTT_*`).
5.3 Delete old `lwlte_event_data_t` (mixed fields).
5.4 Build — examples and tests will fail at this point.

### Phase 6 — Examples / tests / docs

6.1 Rewrite four examples.
6.2 Update host tests.
6.3 Update `docs/agents/*.md`.
6.4 Update `AGENTS.md` / `AGENTS_ZH.md`.
6.5 Full end-to-end test.

### Phase 7 — Optional cleanup

7.1 Check `esp_event_post_to` return value in `core_post_event` (currently swallowed at `core.c:510`).
7.2 Any small improvements discovered during implementation.

## Testing Strategy

| Layer | What | Tool |
|---|---|---|
| Unit | core callback registration, protocol callback synchronous invocation, event_loop borrowing | Host tests + mocks |
| Integration | End-to-end MQTT publish/subscribe, net online/offline flip, destroy sequence | Host tests + device tests |
| Regression | All examples build and run, host contract tests green | pytest + idf.py build |

Critical regression points:

1. MQTT_DATA payload lifecycle: single handler → release → no leak.
2. Multiple handlers for MQTT_DATA: only last release frees buffers.
3. Destroy with in-flight MQTT_DATA: user responsibility contract holds.
4. Late subscriber: core online first, then `lwlte_mqtt_init` + `lwlte_mqtt_start` → MQTT correctly enters connecting.
5. Destroy order: FSM task stopped before handler unregister.
6. Race between `core_get_net_state` probe and arriving bus event → eventual consistency, no deadlock.

## Risk Register

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| Shared loop queue full, core post fails, state lost | Medium | MQTT stuck in WAITING_NET | Log post failures; document queue sizing; consider retry in future task |
| User handler dispatched after destroy | Low (user responsibility) | UB | Document clearly; provide unregister example |
| Race causes wasted connecting attempt | High | Wastes one command + timeout | Accepted by design; FSM correctly retreats on offline signal |
| MQTT_DATA payload not released | Medium | Memory leak | Document API contract; helper name is explicit; test coverage |
| Other component's handler on default loop blocks | Low | MQTT internal handler delayed | Document; user can supply a dedicated loop via config |
| protocol_callback blocks core FSM | Low | FSM task stalls | MQTT callback implementation must only enqueue, never block; code review enforces |
| Old API removal breaks user code | High (by design) | Breaking change | Unavoidable; migration guide in docs |

## Out of Scope (YAGNI)

- No callback compatibility shim.
- No `error_code` semantic cleanup.
- No `ping_client` refactor.
- No reference-counted buffer for MQTT_DATA.
- No state-replay subscription API.
- No automatic user-handler protection on destroy.
- No splitting `lwlte.h` into separate event header.

## Acceptance Criteria

1. All four examples build and run end-to-end on Air780EP and ML307R hardware (basic_connect + mqtt_client).
2. All host tests green.
3. `lwlte.h` / `core.h` / `mqtt_client.h` public API matches this design.
4. `struct lwlte_handle` contains no callback_* fields.
5. `struct core_handle` and `struct mqtt_client_handle` contain no `event_loop` ownership (only borrowed reference).
6. MQTT_DATA on device: subscribe → publish → handler receives → release → no leak (verified with heap tracing).
7. Destroy sequence on device: after destroy, bus does not dispatch to internal handlers.
8. Late subscriber scenario on device: core online first, then mqtt init+start → MQTT reaches CONNECTED.
9. `docs/agents/*.md` all updated.
