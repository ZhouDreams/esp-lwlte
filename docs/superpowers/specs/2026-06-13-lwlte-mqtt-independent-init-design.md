# lwlte MQTT Independent Init Design

## Context

Today MQTT client configuration is embedded inside the LTE initialization config structs:

- `lwlte_air780ep_config_t.mqtt_client` of type `lwlte_air780ep_config_mqtt_client_t` (`src/include/lwlte.h:236`)
- `lwlte_ml307r_config_t.mqtt_client` of type `lwlte_ml307r_config_mqtt_client_t` (`src/include/lwlte.h:293`)

The two `..._config_mqtt_client_t` typedefs have **identical fields**, differing only in name. The factory functions `lwlte_air780ep_init` / `lwlte_ml307r_init` read `config->mqtt_client.enabled` and conditionally create `me->mqtt` inside the LTE init call (`src/lwlte/lwlte_air780ep.c:193-218` for Air780EP, the same shape in ML307R).

This couples two unrelated concerns:

- LTE bring-up (AT Engine + Modem + Core + Ping) is mandatory and modem-specific.
- MQTT client is an optional, modem-agnostic service.

The user wants `lwlte_*_config_t` to carry only Core/Modem/AT related fields, and to instantiate the MQTT client through a dedicated API.

## Goals

- Remove the `mqtt_client` field from both `lwlte_air780ep_config_t` and `lwlte_ml307r_config_t`.
- Unify the two duplicate `..._config_mqtt_client_t` typedefs into one modem-agnostic `lwlte_mqtt_config_t`.
- Add a public `lwlte_mqtt_init(lwlte_handle_t*, const lwlte_mqtt_config_t*)` that creates the MQTT client object and wires the internal event bridge.
- Add a public `lwlte_mqtt_destroy(lwlte_handle_t*)` that releases the MQTT client object, safe from any FSM state.
- Keep `lwlte_mqtt_start` / `lwlte_mqtt_stop` semantics unchanged: they still drive the FSM lifecycle only.
- Keep `lwlte_destroy` as a safety net: it still cleans up any MQTT client the user did not explicitly destroy.
- Match the existing repo-wide convention that every `*_destroy` is safe from any runtime state (mirrors `mqtt_client_destroy`'s internal `wait_stop_before_destroy`, and `lwlte_destroy`'s `destroy_owned_resources`).

## Non-Goals

- Do not change the lower-layer `mqtt_client_*`, `modem_*_mqtt_*`, or `modem_ops_t` interfaces.
- Do not add `lwlte_mqtt_reconfigure`. Reconfiguration is destroy + init.
- Do not add TLS, will message, or topic persistence. Those are lower-layer extension points and out of scope.
- Do not add `lwlte_mqtt_get_config`. The caller owns the config.
- Do not preserve backward compatibility for the deleted typedefs or fields. This is a hard break.

## Lifecycle Semantics

The facade now exposes two orthogonal lifecycle pairs for MQTT:

| Layer | Pair | Meaning |
|-------|------|---------|
| Object lifecycle | `lwlte_mqtt_init` ↔ `lwlte_mqtt_destroy` | Create/destroy the MQTT client object, its FSM task, queues, event loop, and event bridge |
| FSM lifecycle | `lwlte_mqtt_start` ↔ `lwlte_mqtt_stop` | Submit asynchronous connect / disconnect requests to the FSM |

### When each API may be called

- `lwlte_mqtt_init`: after `lwlte_*_init` returned a handle, before `lwlte_destroy`. `lwlte_start` may have been called already or not — init only constructs the object, it does not depend on LTE network state. Calling init twice without an intervening destroy returns `ESP_ERR_INVALID_STATE`.
- `lwlte_mqtt_start` / `lwlte_mqtt_stop` / `lwlte_mqtt_subscribe` / `lwlte_mqtt_unsubscribe` / `lwlte_mqtt_publish` / `lwlte_mqtt_get_state`: require `me->mqtt != NULL`, otherwise return `ESP_ERR_INVALID_STATE`. This already is the existing behavior.
- `lwlte_mqtt_destroy`: after init, in any FSM state. If the FSM is still running, the lower-layer `mqtt_client_destroy` performs `wait_stop_before_destroy` internally (auto-stop). Calling destroy when `me->mqtt == NULL` returns `ESP_ERR_INVALID_STATE`.
- `lwlte_destroy`: still safe in all cases. If `me->mqtt != NULL`, `destroy_owned_resources` cleans it up (`src/lwlte/lwlte.c:1113-1120`); if the user already destroyed it, the cleanup is skipped.

### Recommended App-layer ordering

```
lwlte_air780ep_init / lwlte_ml307r_init
lwlte_register_event_callback
lwlte_mqtt_init              // create MQTT object, register internal event bridge
lwlte_start                  // begin LTE bring-up
... wait for LWLTE_EVENT_NET_ONLINE ...
lwlte_mqtt_start             // begin MQTT FSM
... use lwlte_mqtt_subscribe / lwlte_mqtt_publish ...
lwlte_mqtt_destroy (optional; auto-stop)
lwlte_destroy
```

`lwlte_mqtt_init` is decoupled from `lwlte_start`, so the user may also call init after LTE is online. The only requirement is that the LTE handle exists.

## Public API

### `lwlte_mqtt_config_t`

New unified config, placed in `src/include/lwlte.h` before `lwlte_air780ep_config_t`. The `enabled` flag is removed because calling `lwlte_mqtt_init` is itself the act of enabling MQTT.

```c
typedef struct {
    const char *host;            /**< Required MQTT broker host */
    uint16_t    port;            /**< Required MQTT broker port */
    const char *client_id;       /**< Required MQTT client ID */
    const char *username;        /**< Optional username */
    const char *password;        /**< Optional password */
    uint16_t    keepalive_s;     /**< Keepalive seconds, 0 uses lower-layer default */
    bool        clean_session;   /**< Clean session flag */
    int         fsm_queue_size;  /**< MQTT FSM queue size, 0 uses default */
    int         fsm_task_stack;  /**< MQTT FSM task stack, 0 uses default */
    int         fsm_task_priority; /**< MQTT FSM task priority, 0 uses default */
} lwlte_mqtt_config_t;
```

### New APIs

```c
esp_err_t lwlte_mqtt_init   (lwlte_handle_t *me, const lwlte_mqtt_config_t *config);
esp_err_t lwlte_mqtt_destroy(lwlte_handle_t *me);
```

#### `lwlte_mqtt_init`

- Args: facade handle (non-NULL), config (non-NULL).
- Validation: `host` and `client_id` non-empty, `port > 0`, integer tuning fields non-negative. Returns `ESP_ERR_INVALID_ARG` otherwise.
- Facade gate: goes through `begin_api_call(me, true, &core)` exactly like the other runtime APIs, so it cannot race with `lwlte_destroy`.
- Re-init guard: after acquiring the gate, checks `me->mqtt != NULL` and returns `ESP_ERR_INVALID_STATE` if so.
- Internal behavior:
  - Builds a `mqtt_client_config_t` with `transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP` and the supplied fields.
  - Calls `mqtt_client_create(&mqtt_config, core)`. On failure returns `ESP_FAIL` and releases the gate.
  - Calls `mqtt_client_register_event_callback(mqtt, lwlte_handle_mqtt_event, me)`. On failure calls `mqtt_client_destroy(mqtt)` (auto-stop, safe), releases the gate, and returns the error.
  - Stores the handle into `me->mqtt` and releases the gate.
- Ownership: `config` and its string fields are borrowed for the duration of the call only. The lower layer deep-copies the strings, so the caller may free or reuse them once `lwlte_mqtt_init` returns.
- Return codes: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_STATE` (already initialized, or facade destroying), `ESP_ERR_NO_MEM`, `ESP_FAIL`.

#### `lwlte_mqtt_destroy`

- Args: facade handle (non-NULL).
- Facade gate: goes through `begin_api_call(me, false, NULL)` (destroy does not need core).
- Guard: under lock, reads `me->mqtt` into a local `mqtt` and sets `me->mqtt = NULL`. If `mqtt` was NULL, releases the gate and returns `ESP_ERR_INVALID_STATE`.
  - Calls `mqtt_client_destroy(mqtt)` **inside** the gate (before `end_api_call`). The lower layer may block on `wait_stop_before_destroy`; this keeps `active_api_calls > 0` so `lwlte_destroy` waits for MQTT teardown to finish. No self-deadlock: the facade lock is not held during the call, and `lwlte_handle_mqtt_event` does not use the api-call gate.
- Return codes: `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_INVALID_STATE` (not initialized, or facade destroying), plus any error from the lower-layer destroy (logged as warning).

## Internal Changes

### `lwlte_air780ep_init` / `lwlte_ml307r_init`

Both factory functions drop their MQTT creation step entirely:

- `src/lwlte/lwlte_air780ep.c:190-218` (step 6, the entire `if (config->mqtt_client.enabled)` block) is deleted.
- The ML307R factory's matching block is deleted.
- `validate_config` in `src/lwlte/lwlte_air780ep.c:258-273` drops the `if (config->mqtt_client.enabled) { ... }` clause; the ML307R validator's matching clause is dropped.

After this change the factories only create AT Engine / Modem / Core / Ping. `me->mqtt` is NULL by virtue of `lwlte_create_empty` zeroing the handle.

### `lwlte_mqtt_init` / `lwlte_mqtt_destroy` implementation

Placed in `src/lwlte/lwlte.c`, adjacent to the existing `lwlte_mqtt_start` / `lwlte_mqtt_stop`. Skeleton:

```c
esp_err_t lwlte_mqtt_init(lwlte_handle_t *me, const lwlte_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(config->host && config->host[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT host is required");
    ESP_RETURN_ON_FALSE(config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "MQTT port is required");
    ESP_RETURN_ON_FALSE(config->client_id && config->client_id[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT client_id is required");
    ESP_RETURN_ON_FALSE(non_negative_int(config->fsm_queue_size) &&
                        non_negative_int(config->fsm_task_stack) &&
                        non_negative_int(config->fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "MQTT task fields must be non-negative");

    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool already_initialized = (me->mqtt != NULL);
    xSemaphoreGive(me->lock);
    if (already_initialized) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    const mqtt_client_config_t mqtt_config = {
        .transport         = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
        .host              = config->host,
        .port              = config->port,
        .client_id         = config->client_id,
        .username          = config->username,
        .password          = config->password,
        .keepalive_s       = config->keepalive_s,
        .clean_session     = config->clean_session,
        .fsm_queue_size    = config->fsm_queue_size,
        .fsm_task_stack    = config->fsm_task_stack,
        .fsm_task_priority = config->fsm_task_priority,
    };
    mqtt_client_handle_t *mqtt = mqtt_client_create(&mqtt_config, core);
    if (!mqtt) {
        end_api_call(me);
        ESP_LOGE(TAG, "create MQTT client failed");
        return ESP_FAIL;
    }

    ret = mqtt_client_register_event_callback(mqtt, lwlte_handle_mqtt_event, me);
    if (ret != ESP_OK) {
        mqtt_client_destroy(mqtt);
        end_api_call(me);
        ESP_LOGE(TAG, "register MQTT event bridge failed: %s", esp_err_to_name(ret));
        return ret;
    }

    me->mqtt = mqtt;
    end_api_call(me);
    return ESP_OK;
}

esp_err_t lwlte_mqtt_destroy(lwlte_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t *mqtt = me->mqtt;
    me->mqtt = NULL;
    xSemaphoreGive(me->lock);

    if (!mqtt) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    ret = mqtt_client_destroy(mqtt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "destroy MQTT client failed: %s", esp_err_to_name(ret));
    }
    end_api_call(me);
    return ret;
}
```

`non_negative_int` is already available in `src/lwlte/lwlte_air780ep.c:288` as a static helper; the implementation plan will decide whether to move it to a shared location (e.g. `lwlte_priv.h` or a small static in `lwlte.c`) or duplicate it locally in `lwlte.c`.

### Concurrency invariants

| Invariant | Why |
|-----------|-----|
| Both init and destroy go through `begin_api_call` / `end_api_call` | Prevents racing with `lwlte_destroy`, consistent with all other runtime APIs |
| `me->mqtt = NULL` happens (under lock) before the lower-layer `mqtt_client_destroy` call | Prevents concurrent `lwlte_mqtt_*` runtime calls from grabbing the stale handle via `begin_mqtt_api_call`, which reads `me->mqtt` under lock and returns `ESP_ERR_INVALID_STATE` when it is NULL |
| The lower-layer `mqtt_client_destroy` is called **inside** the facade gate (before `end_api_call`) | This keeps `active_api_calls > 0` during teardown, which blocks `lwlte_destroy` at `wait_api_calls_idle` until MQTT cleanup finishes — the desired protection. There is no self-deadlock: `begin_api_call` releases `me->lock` before returning, and `lwlte_handle_mqtt_event` (which may fire during teardown) takes `me->lock` briefly and does not touch `active_api_calls` |
| init failure after `mqtt_client_create` succeeds must call `mqtt_client_destroy(mqtt)` before returning | Never leave an MQTT object whose event bridge was never registered |

### `lwlte_destroy` fallback

`destroy_owned_resources` (`src/lwlte/lwlte.c:1109-1120`) is unchanged. If the user called `lwlte_mqtt_destroy` earlier, `me->mqtt == NULL` and the fallback skips. If the user did not, the fallback destroys it. Both paths are safe.

## Documentation

### Examples

Two examples must be updated:

- `example/air780ep_mqtt_client.c`
- `example/ml307r_mqtt_client.c`

Each follows the same migration:

- Drop the `.mqtt_client = { ... }` initializer from the `lwlte_*_config_t` literal.
- After `lwlte_*_init` (and after `lwlte_register_event_callback`), construct a `lwlte_mqtt_config_t` literal and call `lwlte_mqtt_init(lte, &mqtt_cfg)`.
- The cleanup path may call `lwlte_mqtt_destroy(lte)` before `lwlte_destroy(lte)`, or omit it and rely on `lwlte_destroy`'s fallback.

### `docs/agents/`

| Document | Change |
|----------|--------|
| `classes.md` | Move the MQTT client "configure and create" section from "nested in `lwlte_*_config_t`" to "via `lwlte_mqtt_init(lte, &mqtt_config)`". Document both lifecycle pairs and the `lwlte_destroy` fallback. |
| `architecture.md` | Update the App-layer sequence diagram: `lwlte_*_init` → `lwlte_register_event_callback` → `lwlte_mqtt_init` → `lwlte_start` → `lwlte_mqtt_start`. Note that `lwlte_*_config_t` now only carries Core/Modem/AT fields. |
| `feature-roadmap.md` | Update any wording that describes MQTT as configured inside the LTE init config. |
| `coding-style.md` | If it shows a facade API annotation template, add an example of the "init/destroy vs start/stop two-layer lifecycle" pattern. |

`AGENTS.md` itself does not change (no new or removed document links).

### Public header doxygen

The `lwlte.h` doxygen blocks for `lwlte_mqtt_init` / `lwlte_mqtt_destroy` / `lwlte_mqtt_config_t` must state:

- `lwlte_mqtt_init`: when it may be called; duplicate init returns `ESP_ERR_INVALID_STATE`; config strings are borrowed for the call only; the internal event bridge is registered automatically (user-transparent); the handle must be released via `lwlte_mqtt_destroy` or `lwlte_destroy`.
- `lwlte_mqtt_destroy`: safe from any FSM state (auto-stop); duplicate destroy / not-initialized returns `ESP_ERR_INVALID_STATE`; `lwlte_destroy` is the fallback.
- `lwlte_mqtt_config_t`: `host` / `port` / `client_id` required; remaining fields optional; 0 uses lower-layer default; the `enabled` field is removed (calling init is the enable action).

## Verification

- The build must pass `esp-idf-eim_build_project`.
- `example/air780ep_mqtt_client.c` and `example/ml307r_mqtt_client.c` must compile after migration.
- Grep must confirm no remaining references to `lwlte_air780ep_config_mqtt_client_t`, `lwlte_ml307r_config_mqtt_client_t`, or any `.mqtt_client` field access.
- Grep must confirm `lwlte_mqtt_init` and `lwlte_mqtt_destroy` are declared in `lwlte.h` and defined in `lwlte.c`.
