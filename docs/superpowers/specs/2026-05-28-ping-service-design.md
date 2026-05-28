# Ping Service Design

## Context

`esp-lwlte` already has a layered service boundary:

- App code uses only Facade APIs in `src/include/lwlte*.h`.
- Facade owns composition and delegates runtime work to internal services.
- MQTT Client Service sits above Core and submits protocol commands through `core_submit_cmd()`.
- Core FSM is the single serialized executor for Modem operations.
- Modem Adapter translates semantic operations into Air780EP AT commands.
- AT Engine only sends commands and parses generic AT responses.

Ping should follow the same layering but stay much lighter than MQTT. It must be exposed as a user-callable Facade API, but it should not own a dedicated FSM task or queue.

## Goals

- Add a user-callable synchronous ping API through the LWLTE Facade.
- Keep Ping Service lightweight: no Ping FSM, no Ping task, no Ping queue.
- Reuse Core command queue so ping AT commands are serialized with MQTT and network commands.
- Support detailed per-packet ping replies with caller-owned output storage.
- Keep layer boundaries intact: Ping Service depends only on Core, not Modem or AT Engine.
- Leave a clear path for future asynchronous ping without implementing it now.

## Non-Goals

- Do not make ping part of Core online detection or network activation.
- Do not add a Ping state machine similar to MQTT.
- Do not allocate the user's per-reply result array inside the component.
- Do not implement asynchronous ping in the first version.
- Do not let Facade or Ping Service call `modem_*`, `at_engine_*`, or Air780EP helpers directly.

## Architecture

Add `Ping Service` after `MQTT Client Service` in `docs/agents/classes.md`. It is an internal service above Core:

```text
App
  -> lwlte_ping()
       -> Facade
            -> ping_client_ping()
                 -> core_submit_cmd(CORE_CMD_PING)
                      -> Core FSM
                           -> modem_ping()
                                -> Air780EP AT+CIPPING
```

Ping Service owns only short-lived synchronization for a single synchronous request. It does not maintain a long-lived protocol connection, subscription list, reconnect policy, or protocol data path.

Core FSM remains the only place that executes `modem_ping()`. This keeps AT command ordering deterministic and prevents ping from racing MQTT publish/subscribe or network-management commands.

## Public Facade API

The first version exposes a synchronous blocking API:

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} lwlte_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} lwlte_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} lwlte_ping_summary_t;

esp_err_t lwlte_ping(lwlte_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);
```

`replies` is caller-owned storage. `max_replies` must be at least `request->count`. The component fills up to `request->count` reply entries and never allocates or frees this array. `summary` may be `NULL`.

`host` may be a domain name or IP address. The first Air780EP implementation passes it to `AT+CIPPING` as a quoted host string.

## Ping Service API

Ping Service has its own internal request type instead of exposing the Facade request type directly. Reply and summary storage use Core command-boundary result types so Ping Service can pass output buffers through `core_submit_cmd()` without depending on Modem types:

```c
typedef struct ping_client ping_client_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} ping_client_request_t;

ping_client_t *ping_client_create(core_t *core);
esp_err_t ping_client_destroy(ping_client_t *me);
esp_err_t ping_client_ping(ping_client_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary);
```

`ping_client_t` holds a borrowed `core_t *` and a small lifecycle lock/destroying flag. It does not create an event loop, task, FSM queue, or timer. `ping_client_ping()` creates short-lived completion synchronization for the current call, submits one Core command, waits for completion, then returns.

Facade, Core, and Modem each keep their own layer-visible value types. The implementation must copy fields at layer boundaries and must not cast between unrelated struct pointer types. Any transient scratch array used for this mapping is internal and freed before `lwlte_ping()` returns; the returned reply array remains caller-owned and has no free function.

## Core Command Boundary

Core adds a command type:

```c
CORE_CMD_PING
```

Core owns the command-boundary result types:

```c
typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} core_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} core_ping_summary_t;
```

Core command data gains ping fields:

```c
struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    core_ping_reply_t *replies;
    size_t max_replies;
    core_ping_summary_t *summary;
} ping;
```

The ownership rules are fixed:

- Core deep-copies `host` when `core_submit_cmd()` accepts the command.
- `replies` and `summary` are output buffers owned by the synchronous Ping Service call.
- `ping_client_ping()` waits until Core command completion before returning, so the output buffers remain valid through `done_cb`.
- If Core rejects, drops, or times out the command, it completes the command through the existing `core_cmd_done_callback_t` result path.

Core FSM handles `CORE_CMD_PING` by validating network state, calling `modem_ping()`, mapping the returned `esp_err_t` to `core_cmd_result_t`, then invoking `done_cb` without holding `core->lock`.

## Modem Adapter Boundary

Modem adds a semantic ping operation:

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
} modem_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} modem_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} modem_ping_summary_t;

esp_err_t modem_ping(modem_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary);
```

`modem_ops_t` gains a `ping` method. Core uses only `modem_ping()` and does not know Air780EP command syntax.

## Air780EP Mapping

Air780EP maps `modem_ping()` to:

```text
AT+CIPPING="<host>",<count>,<data_len>,<timeout_100ms>,<ttl>
```

The response contains one line per reply plus final `OK`:

```text
+CIPPING: <replyId>,<IpAddress>,<replyTime>,<ttl>
OK
```

Parsing rules:

- Fill one reply entry for each valid `+CIPPING:` line up to `request->count`.
- `seq` comes from `<replyId>`.
- `ip` comes from `<IpAddress>` and is truncated only if necessary to fit the fixed buffer with NUL termination.
- `time_ms` comes from `<replyTime>`.
- `ttl` comes from `<ttl>`.
- `success` is true for replies that are not recognized as timeout/loss.
- The Air780EP documented no-response pattern is `replyTime == timeout_100ms * 100` and `ttl == 255`; treat that pattern as a lost packet.
- If `summary` is non-NULL, `summary.sent = request->count`.
- If `summary` is non-NULL, `summary.received` counts successful replies.
- If `summary` is non-NULL, `summary.lost = sent - received`.
- If `summary` is non-NULL, `min_time_ms`, `max_time_ms`, and `avg_time_ms` are calculated only over successful replies. If no packet succeeds, these fields are `0`.

Air780EP should use the AT Engine command path that can retain intermediate `+CIPPING:` lines until the final `OK`. It should return `ESP_ERR_INVALID_RESPONSE` if the command succeeds but required response lines cannot be parsed consistently.

## Validation And Defaults

Validation applies at the Facade/Ping boundary before submitting to Core:

- `me`, `request`, and `replies` must be non-NULL.
- `request->host` must be non-NULL and non-empty.
- `request->count` must be in `1..100`.
- `max_replies` must be at least `request->count`.
- `request->data_len` must be `0..1024`.
- `request->timeout_100ms` must be `1..600`.
- `request->ttl` must be `1..255`.
- `request->total_timeout_ms == 0` means Ping Service derives a conservative default from `count * timeout_100ms * 100` plus command overhead. Non-zero values are used as the caller's total wait budget.

Core must return `ESP_ERR_INVALID_STATE` if ping is requested while the network is not online.

## Error Handling

- Invalid arguments return `ESP_ERR_INVALID_ARG` before command submission.
- Unsupported modem implementations return `ESP_ERR_NOT_SUPPORTED` through `modem_ping()`.
- Core network offline returns `ESP_ERR_INVALID_STATE`.
- `core_submit_cmd()` enqueue failure returns `ESP_FAIL`.
- Waiting longer than the effective total timeout returns `ESP_ERR_TIMEOUT`; the effective timeout is either the caller's non-zero `total_timeout_ms` or the derived default when `total_timeout_ms == 0`.
- AT command timeout returns `ESP_ERR_TIMEOUT`.
- AT `ERROR`, `+CME ERROR`, or `+CMS ERROR` follows existing Modem error mapping.
- Unparseable `+CIPPING:` responses return `ESP_ERR_INVALID_RESPONSE`.

Partial packet loss is not an API error if the AT command completes normally. Packet loss is represented in `replies[].success`; when `summary` is non-NULL, it is also represented in `summary.lost`.

## Threading And Lifetime

`lwlte_ping()` is synchronous and may block the caller until Core finishes the ping command or the total timeout expires. It should not be called from time-sensitive callbacks.

The synchronous waiting context is short-lived:

- Ping Service creates a completion primitive for the call.
- Ping Service submits `CORE_CMD_PING` with output buffer pointers and a done callback.
- Core FSM executes `modem_ping()`.
- Core invokes the done callback.
- The callback stores the result and signals completion.
- Ping Service returns to Facade, and Facade returns to App.

Destroy rules:

- `ping_client_destroy()` must reject or fail any new ping call once destruction starts.
- The first version does not need to support destroying a `ping_client_t` concurrently with an active `ping_client_ping()` on the same handle; this follows the broader component expectation that callers do not destroy a handle while another task is using it.
- Core command ownership remains unchanged: accepted commands are released by Core after completion or drop handling.

## Facade Integration

Facade factory creates `ping_client_t` after Core creation and before `core_start()`:

```text
core_create(...)
ping_client_create(core)
optional mqtt_client_create(...)
core_start(core)
```

`lwlte_t` gains a `ping_client_t *ping` member. `lwlte_destroy()` destroys Ping Service before Core, following reverse dependency order.

Facade maps between public `lwlte_ping_*` types and internal Ping/Core value types. App code still does not include `src/ping_client/ping_client.h` or `src/core/core.h`.

## Future Async API

The first implementation only documents the future async shape:

```c
esp_err_t lwlte_ping_async(lwlte_t *me,
                           const lwlte_ping_request_t *request,
                           void *user_ctx);
```

Future async ping should still reuse `CORE_CMD_PING`, but it will need Ping Service owned request/result storage and a defined cancellation/destroy policy. It may report completion through `LWLTE_EVENT_PING_DONE` or a dedicated callback. No async code or event IDs should be added in the first synchronous version.

## Documentation Updates

`docs/agents/classes.md` should be updated as follows:

- Add Ping Service to the visibility table as a layer-internal API under `src/ping_client/ping_client.h` with prefix `ping_client_`.
- Add `modem_ping_*` types, `modem_ping()`, and `modem_ops_t.ping` to the Modem section.
- Add `core_ping_*` types, `CORE_CMD_PING`, and ping command data/result ownership rules to the Core command queue section.
- Add a new `## 5. Ping Service` section immediately after MQTT Client Service.
- Renumber App section to follow Ping Service.
- Add public Facade ping types/functions to the App section.
- Update the prior note that `AT+CIPPING` was only an internal helper; it is now the Air780EP implementation mapping for `modem_ping()`.

## Testing Strategy

Documentation/static contract tests should verify:

- `classes.md` contains the new Ping Service section after MQTT.
- `CORE_CMD_PING` is documented in Core command queue.
- `modem_ping()` and Air780EP `AT+CIPPING` mapping are documented.
- Ping Service boundary says no dedicated FSM/task/queue.
- Facade public API uses caller-owned reply buffer.
- Ping Service does not include Modem or AT Engine headers.

Implementation tests should later cover:

- Facade argument validation.
- Ping Service synchronous command completion.
- Core command queue deep-copy of `host` and safe output buffer usage.
- Air780EP parser for success replies, timeout/loss replies, malformed lines, and final `OK`.
- Network-offline rejection through Core.
