# Internal Config Grouping Design

## Context

The public `lwlte_air780ep_config_t` and `lwlte_ml307r_config_t` have been refactored from flat fields into nested public groups under `base`:

- `base.uart`
- `base.at_engine`
- `base.modem`
- `base.core`
- `base.event`

That shape is appropriate for the Facade because the public module config is a board-level composition object spanning several internal subsystems. The remaining question is whether internal layer config structs should follow the same grouping style.

The preferred decision criterion is architectural consistency. Internal configs should expose clear conceptual boundaries that match their layer responsibilities, but the public `.base` shape should not be copied mechanically into every value object.

## Goals

- Make persistent internal object creation configs match the same grouped-configuration design philosophy as the public Facade config.
- Keep each internal config grouped by the owning layer's real concepts, not by public API names when those names do not fit.
- Reduce future drift when adding module-specific modem fields or expanding MQTT/TLS configuration.
- Preserve existing runtime behavior and zero/default normalization semantics.
- Keep one-shot command/request value objects simple and flat.

## Non-Goals

- Do not change public `lwlte_*_config_t` shape again.
- Do not reuse public `lwlte_*_config_t` sub-structs inside internal layer headers.
- Do not add backward-compatibility aliases for internal struct fields.
- Do not refactor command/request objects such as `modem_mqtt_config_t`, `ping_client_request_t`, or publish/subscribe request structs.
- Do not change object lifetimes, task creation, event routing, or modem startup behavior.

## Scope

Refactor these persistent creation configs:

- `at_engine_config_t`
- `modem_air780ep_config_t`
- `modem_ml307r_config_t`
- `core_config_t`
- `mqtt_client_config_t`

Leave these value objects flat:

- `modem_mqtt_config_t`
- `modem_mqtt_topic_t`
- `modem_mqtt_publish_t`
- `modem_ping_request_t`
- `ping_client_request_t`
- `mqtt_client_publish_t`
- `core_cmd_t` and nested command payload structs
- public request/operation structs in `lwlte.h`

## Design

### AT Engine Config

`at_engine_config_t` currently mixes UART transport fields and AT engine runtime tuning. Refactor it into two groups:

```c
typedef struct {
    uart_port_t uart_num;
    int tx_pin;
    int rx_pin;
    int baud_rate;
    int rx_buf_size;
} at_engine_uart_config_t;

typedef struct {
    int rx_task_stack;
    int rx_task_priority;
    int rx_line_buf_size;
    int cmd_default_timeout_ms;
    int max_response_lines;
} at_engine_runtime_config_t;

typedef struct {
    at_engine_uart_config_t    uart;
    at_engine_runtime_config_t runtime;
} at_engine_config_t;
```

`rx_buf_size` belongs under `uart` internally because `at_engine.c` uses it as the UART driver RX ring buffer size in `uart_driver_install()`. The public API currently groups it under `base.at_engine`; Facade mapping should keep that public decision while mapping the field into internal `at_config.uart.rx_buf_size`.

### Modem Configs

`modem_air780ep_config_t` and `modem_ml307r_config_t` currently have identical fields. Introduce a common modem base config and wrap it in module-specific config structs:

```c
typedef struct {
    gpio_num_t en_pin;
} modem_hardware_config_t;

typedef struct {
    uint32_t reset_pulse_ms;
    uint32_t ready_timeout_ms;
    uint32_t default_cmd_timeout_ms;
} modem_timing_config_t;

typedef struct {
    int event_queue_size;
    int event_task_stack;
    int event_task_priority;
} modem_event_config_t;

typedef struct {
    modem_hardware_config_t hardware;
    modem_timing_config_t   timing;
    modem_event_config_t    event;
} modem_base_config_t;

typedef struct {
    modem_base_config_t base;
} modem_air780ep_config_t;

typedef struct {
    modem_base_config_t base;
} modem_ml307r_config_t;
```

The module wrappers intentionally stay even though there are no module-specific fields today. They preserve a clear extension point for future fields such as ML307R power-key behavior, Air780EP RI wake configuration, or module-specific startup gates.

Defaults remain module-owned. For example, Air780EP and ML307R may both normalize `base.timing.ready_timeout_ms == 0`, but each implementation should keep its own default constants.

### Core Config

`core_config_t` represents Core event posting, network/PDP policy, and FSM resources. Refactor it into service-specific groups:

```c
typedef struct {
    esp_event_loop_handle_t loop;
} core_event_config_t;

typedef struct {
    const char *apn;
    uint8_t primary_cid;
    uint32_t net_activate_timeout_ms;
    uint32_t reconnect_delay_ms;
} core_network_config_t;

typedef struct {
    int queue_size;
    int task_stack;
    int task_priority;
} core_fsm_config_t;

typedef struct {
    core_event_config_t   event;
    core_network_config_t network;
    core_fsm_config_t     fsm;
} core_config_t;
```

Use `event.loop` rather than `event.event_loop` to match public `lwlte_event_config_t` naming. Use `network` instead of `core` or `net` to keep the field self-explanatory at call sites.

### MQTT Client Config

`mqtt_client_config_t` spans endpoint selection, credentials, MQTT session behavior, FSM resources, and event bus integration. Refactor it into MQTT-domain groups:

```c
typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
} mqtt_client_endpoint_config_t;

typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
} mqtt_client_auth_config_t;

typedef struct {
    uint16_t keepalive_s;
    bool clean_session;
} mqtt_client_session_config_t;

typedef struct {
    int queue_size;
    int task_stack;
    int task_priority;
} mqtt_client_fsm_config_t;

typedef struct {
    esp_event_loop_handle_t loop;
} mqtt_client_event_config_t;

typedef struct {
    mqtt_client_endpoint_config_t endpoint;
    mqtt_client_auth_config_t     auth;
    mqtt_client_session_config_t  session;
    mqtt_client_fsm_config_t      fsm;
    mqtt_client_event_config_t    event;
} mqtt_client_config_t;
```

TLS is not added in this refactor. A future TLS feature can add a `tls` group without disrupting endpoint/auth/session naming.

## Data Flow

Facade factories keep responsibility for translating public config into internal config:

```text
lwlte_*_config_t.base.uart       -> at_engine_config_t.uart
lwlte_*_config_t.base.at_engine  -> at_engine_config_t.runtime plus uart.rx_buf_size
lwlte_*_config_t.base.modem      -> modem_*_config_t.base.hardware/timing/event
lwlte_*_config_t.base.core       -> core_config_t.network/fsm
lwlte_*_config_t.base.event.loop -> core_config_t.event.loop and facade event_loop
```

`lwlte_mqtt_init()` maps public `lwlte_mqtt_config_t` into the grouped `mqtt_client_config_t`:

```text
host/port plus default transport -> mqtt_client_config_t.endpoint
client_id/username/password      -> mqtt_client_config_t.auth
keepalive/clean_session          -> mqtt_client_config_t.session
fsm_*                            -> mqtt_client_config_t.fsm
lte event loop                   -> mqtt_client_config_t.event.loop
```

The internal configs remain copied and normalized by their owning layers as they are today.

## Error Handling And Defaults

- Preserve all existing `ESP_ERR_INVALID_ARG`, `ESP_ERR_NO_MEM`, `ESP_ERR_NOT_SUPPORTED`, and `ESP_ERR_INVALID_STATE` behavior.
- Preserve zero-default behavior for queue sizes, task stacks, task priorities, timeouts, line-buffer sizes, and MQTT keepalive/session defaults.
- Keep each owning layer responsible for normalizing its own config snapshot.
- Do not make Facade apply internal defaults except for existing module-specific bridging behavior, such as preserving the current ML307R line-buffer fallback.

## Documentation Updates

Update these documents with the new internal grouped config shapes:

- `docs/agents/classes.md`
- `docs/agents/architecture.md`
- `docs/agents/oop-design.md` if examples mention internal create configs

Also fix the stale `core_config_t` documentation that currently describes `event_loop` as not being part of config even though the implementation already carries it.

`AGENTS.md` and `AGENTS_ZH.md` do not need changes unless the document index itself changes.

## Testing

Host contract tests should verify:

- Public `lwlte_*_config_t` shape remains unchanged.
- Facade maps public nested groups into internal grouped configs correctly.
- AT Engine normalization still applies defaults through nested fields.
- Air780EP and ML307R modem configs preserve ready/default command timeout behavior.
- Core receives event loop via `core_config_t.event.loop` and posts to the intended loop.
- MQTT client receives endpoint/auth/session/fsm/event values through grouped config fields.
- Ping and command request contracts remain flat.

Build verification should include:

- `git diff --check`
- `python3 -m pytest tests/host/ -q`
- ESP-IDF build using the project documented environment flow

## Implementation Decisions

No user-facing API change is planned. Carry these naming choices into implementation:

- Use `runtime` for AT Engine non-UART parameters.
- Use `hardware`, `timing`, and `event` inside `modem_base_config_t`.
- Use `network`, `fsm`, and `event` inside `core_config_t`.
- Use `endpoint`, `auth`, `session`, `fsm`, and `event` inside `mqtt_client_config_t`.
