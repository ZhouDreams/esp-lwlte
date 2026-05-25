# LWLTE Facade API Design

## 背景

当前项目已有 AT Engine、Modem Adapter、Core Service 三层，并且 `examples/basic_connect` 已能通过 `lwlte_core_*` API 发起基础联网流程。但示例代码同时 include 并直接使用了 `at_engine.h`、`modem.h`、`modem_air780ep.h` 和 `lwlte_core.h`，这使示例承担了 Board Init、模块装配和用户业务三种角色。

项目目标是让 App 用户只看到真正的 esp-lwlte 用户 API。用户不应该知道 AT Engine、Modem Adapter、Core Service 的存在，也不应该直接操作 `core`。`core`、未来 `mqtt`、`tcp`、`http` 都应作为组件内部 service，被统一的 LWLTE facade 句柄封装。

## 已确认决策

- 新增真正的用户门面层，命名为 LWLTE Facade。
- `lwlte_air780ep_init()` 是 Air780EP 模块专用 factory，但返回通用 `lwlte_t *`。
- 后续连接、断开、销毁、状态查询和未来 MQTT/TCP/HTTP API 都使用通用 `lwlte_t *`。
- `lwlte_air780ep_init()` 显式传入完整配置，不依赖 Kconfig。
- `lwlte_air780ep_init()` 阻塞执行到底层 ready 或超时；ready 表示 AT Engine、Modem、Core 已创建并启动，Core 已发布 ready。
- `lwlte_air780ep_init()` 默认不联网；联网由 `lwlte_connect()` 显式发起。保留 `auto_connect` 配置字段时，其语义应由 facade 在 ready 后调用 `lwlte_connect()` 实现。
- 销毁使用通用 `lwlte_destroy(lwlte_t *lte)`，不使用模块专用 destroy。
- `src/include` 只放真正导出给 App 的 public headers。
- 层间 headers 不放在 `src/include`，应放回对应模块目录并通过 `PRIV_INCLUDE_DIRS` 供组件内部使用。
- `core` 现有 `lwlte_core_*` 命名和文档中的“用户 API”定位必须降级为层间 service API。
- 实现阶段如果现有 `docs/agents/` 文档与本设计不一致，必须同步修改对应文档。

## 架构

组件内部新增一层 LWLTE Facade，位于 App 与各 service 之间：

```text
App
  ↓ 只 include lwlte*.h
LWLTE Facade
  ↓
Service Layer: core, future mqtt, future tcp, future http
  ↓
Modem Adapter
  ↓
AT Engine
```

App 不算组件内部层。组件内部层次为：Facade -> Service -> Modem -> AT Engine。

Facade 是用户 API 的唯一入口，也是模块装配的 composition root。普通 service 代码仍必须遵守只调用紧邻下层的规则；Facade 的模块 factory 文件允许认识 AT Engine、Modem 和 Core，因为它负责创建和持有整棵依赖树。

## 文件结构

目标结构：

```text
src/
├── include/
│   ├── lwlte.h
│   └── lwlte_air780ep.h
├── lwlte/
│   ├── lwlte.c
│   ├── lwlte_air780ep.c
│   └── lwlte_priv.h
├── core/
│   ├── core.h
│   ├── core_priv.h
│   ├── core.c
│   ├── core_fsm.c
│   ├── net_mgr.c
│   └── pdp_mgr.c
├── modem/
│   ├── modem.h
│   ├── modem_air780ep.h
│   ├── modem_priv.h
│   ├── modem.c
│   └── modem_air780ep.c
└── at_engine/
    ├── at_engine.h
    └── at_engine.c
```

CMake include policy:

```cmake
idf_component_register(
    SRCS ...
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS lwlte core modem at_engine
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
```

Only `src/include/lwlte.h` and `src/include/lwlte_air780ep.h` are public to applications and dependent components. `core.h`, `modem.h`, `modem_air780ep.h`, and `at_engine.h` are component-private inter-layer headers.

## Public API

`src/include/lwlte.h` defines the generic user handle, event model, state model, and operations:

```c
typedef struct lwlte lwlte_t;

typedef enum {
    LWLTE_STATE_STOPPED = 0,
    LWLTE_STATE_STARTING,
    LWLTE_STATE_READY,
    LWLTE_STATE_NET_ACTIVATING,
    LWLTE_STATE_ONLINE,
    LWLTE_STATE_ERROR,
    LWLTE_STATE_DESTROYING,
} lwlte_state_t;

typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,
    LWLTE_NET_STATE_ACTIVATING,
    LWLTE_NET_STATE_ONLINE,
    LWLTE_NET_STATE_ERROR,
} lwlte_net_state_t;

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

typedef struct {
    lwlte_net_state_t net_state;
    int error_code;
} lwlte_event_data_t;

typedef void (*lwlte_event_callback_t)(lwlte_t *lte,
                                       lwlte_event_id_t event_id,
                                       const lwlte_event_data_t *data,
                                       void *user_ctx);

esp_err_t lwlte_destroy(lwlte_t *lte);
esp_err_t lwlte_register_event_callback(lwlte_t *lte,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx);
esp_err_t lwlte_connect(lwlte_t *lte);
esp_err_t lwlte_disconnect(lwlte_t *lte);
esp_err_t lwlte_get_state(lwlte_t *lte, lwlte_state_t *state);
esp_err_t lwlte_get_net_state(lwlte_t *lte, lwlte_net_state_t *state);
```

`src/include/lwlte_air780ep.h` defines only the Air780EP user-facing factory config and init function. It must not expose `at_engine_t`, `modem_t`, or `core_t`. The init API returns `esp_err_t` and writes the created handle through `out_lte` so failures can report precise ESP-IDF error codes instead of collapsing to NULL.

```c
typedef struct {
    uart_port_t uart_num;
    gpio_num_t uart_tx_pin;
    gpio_num_t uart_rx_pin;
    int uart_baud_rate;

    gpio_num_t en_pin;
    gpio_num_t pwrkey_pin;
    gpio_num_t reset_pin;
    gpio_num_t status_pin;

    const char *apn;
    uint8_t primary_cid;
    bool auto_connect;

    uint32_t init_ready_timeout_ms;
    uint32_t module_power_stable_ms;
    uint32_t net_activate_timeout_ms;
    uint32_t reconnect_delay_ms;

    int at_rx_buf_size;
    int at_rx_task_stack;
    int at_rx_task_priority;
    int at_rx_line_buf_size;
    int at_cmd_default_timeout_ms;
    int at_max_response_lines;

    uint32_t modem_power_on_pulse_ms;
    uint32_t modem_reset_pulse_ms;
    uint32_t modem_boot_wait_ms;
    uint32_t modem_default_cmd_timeout_ms;
    int modem_event_queue_size;
    int modem_event_task_stack;
    int modem_event_task_priority;

    int core_fsm_queue_size;
    int core_fsm_task_stack;
    int core_fsm_task_priority;
} lwlte_air780ep_config_t;

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);
```

The exact field names can be adjusted during implementation to match coding style, but the public config must remain explicit and must contain enough information to build AT Engine, Air780EP Modem, and Core without exposing lower-layer handles.

## Core Service Demotion

Core is no longer user API. Rename or re-scope its public-looking symbols to service-level inter-layer API:

```text
lwlte_core_t                    -> core_t
lwlte_core_config_t             -> core_config_t
lwlte_core_state_t              -> core_state_t
lwlte_net_state_t               -> core_net_state_t
lwlte_core_event_id_t           -> core_event_id_t
lwlte_core_event_data_t         -> core_event_data_t
lwlte_core_event_callback_t     -> core_event_callback_t
lwlte_core_create()             -> core_create()
lwlte_core_destroy()            -> core_destroy()
lwlte_core_start()              -> core_start()
lwlte_core_stop()               -> core_stop()
lwlte_core_connect()            -> core_connect()
lwlte_core_disconnect()         -> core_disconnect()
lwlte_core_get_state()          -> core_get_state()
lwlte_core_get_net_state()      -> core_get_net_state()
lwlte_core_register_event_callback() -> core_register_event_callback()
LWLTE_CORE_EVENT_*              -> CORE_EVENT_*
```

This is an intentional breaking internal rename. No backward-compatible aliases should be added unless a concrete external compatibility requirement appears.

## Internal `lwlte_t`

`src/lwlte/lwlte_priv.h` owns the complete runtime object:

```c
struct lwlte {
    at_engine_t *at;
    modem_t *modem;
    core_t *core;
    lwlte_event_callback_t event_callback;
    void *event_user_ctx;
    SemaphoreHandle_t ready_sema;
    SemaphoreHandle_t lock;
    bool destroying;
};
```

The exact fields may evolve, but ownership is fixed: `lwlte_t` owns AT Engine, Modem, and Core created by its factory and destroys them in reverse order.

## Init Flow

`lwlte_air780ep_init()` performs:

1. Validate `config` and `out_lte`.
2. Allocate `lwlte_t` and internal synchronization primitives.
3. Configure optional Air780EP EN GPIO if `en_pin` is valid, set it high, and wait `module_power_stable_ms`.
4. Create AT Engine from UART fields.
5. Create Air780EP modem from modem GPIO/task/timeout fields.
6. Call `modem_init()`.
7. Create Core from APN, primary CID, timeouts, and FSM fields.
8. Register an internal Core event callback to translate `core_event_id_t` into `lwlte_event_id_t`.
9. Call `core_start()`.
10. Block until Core ready, Core error, or `init_ready_timeout_ms` expires.
11. If ready and `auto_connect` is true, call `lwlte_connect()` and return after the connect request is submitted, not after network online.
12. On any failure, destroy partially-created resources in reverse order and return the error.

`lwlte_air780ep_init()` returning `ESP_OK` means `*out_lte` has been set to a usable handle and Core is ready. It does not mean the LTE network is online unless `lwlte_get_net_state()` later reports online or `LWLTE_EVENT_NET_ONLINE` is received.

## Event Flow

Core remains the source of network lifecycle events. Facade translates Core events to LWLTE events and dispatches the user callback registered through `lwlte_register_event_callback()`.

During `lwlte_air780ep_init()`, the internal event callback must be installed before `core_start()` so it can observe `CORE_EVENT_READY`. If the user has not yet registered a callback, events are still used internally for readiness; they are not queued for later user replay.

## Error Handling

- Invalid public arguments return `ESP_ERR_INVALID_ARG`.
- Factory allocation failures return `ESP_ERR_NO_MEM` where possible.
- Ready wait timeout returns `ESP_ERR_TIMEOUT`.
- Lower-layer failures propagate their original `esp_err_t` when available.
- `lwlte_destroy()` is responsible for best-effort ordered cleanup. If a lower-layer destroy fails, it returns the first failure and leaves ownership state conservative; it must not double-free later resources.
- Public API should reject operations while destroy is in progress with `ESP_ERR_INVALID_STATE`.

## Documentation Updates Required During Implementation

Implementation must update existing docs that currently describe `lwlte_core_*` as user API or place layer headers under `src/include`:

- `docs/agents/directory-structure.md`: add `src/lwlte/`, define `src/include` as public-only, move layer headers to module directories.
- `docs/agents/architecture.md`: change from four-layer App/Core/Modem/AT view to App + four internal layers: Facade, Service, Modem, AT Engine. Clarify Facade composition-root exception.
- `docs/agents/classes.md`: update visibility table, add `lwlte_t` and `lwlte_air780ep_config_t`, demote Core classes to service inter-layer API, rename `lwlte_core_*` to `core_*`.
- Any existing spec or example documentation that instructs user examples to include `at_engine.h`, `modem.h`, `modem_air780ep.h`, or `lwlte_core.h` must be updated or superseded.

Do not modify top-level `AGENTS.md` unless adding or changing the document index.

## Example Target Shape

The basic connect example should become a user-layer example:

```c
#include "lwlte.h"
#include "lwlte_air780ep.h"

static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx);

void app_main(void)
{
    lwlte_t *lte = NULL;
    const lwlte_air780ep_config_t config = {
        .uart_num = UART_NUM_1,
        .uart_tx_pin = GPIO_NUM_0,
        .uart_rx_pin = GPIO_NUM_1,
        .uart_baud_rate = 115200,
        .en_pin = GPIO_NUM_2,
        .apn = "",
        .primary_cid = 1,
        .auto_connect = false,
    };

    ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
    ESP_ERROR_CHECK(lwlte_register_event_callback(lte, lte_event_cb, NULL));
    ESP_ERROR_CHECK(lwlte_connect(lte));
}
```

The example must not include or call AT Engine, Modem, Air780EP modem factory, or Core Service APIs directly.

## Testing And Verification

Minimum verification for implementation:

- Build the component and basic connect example with ESP-IDF.
- Verify `examples/basic_connect` only includes `lwlte.h` and `lwlte_air780ep.h` from esp-lwlte.
- Verify no public header in `src/include` includes `core.h`, `modem.h`, `modem_air780ep.h`, or `at_engine.h` unless that dependency is intentionally public. `lwlte_air780ep.h` may include ESP-IDF GPIO/UART types because the public config uses them.
- Verify `idf_component_register()` uses `INCLUDE_DIRS include` and private include dirs for `lwlte`, `core`, `modem`, and `at_engine`.
- Verify Core source files use `core_*` names and do not expose `lwlte_core_*` names as user API.
- Run static searches to ensure App/example code does not include private layer headers.

## Non-Goals

- Do not implement MQTT/TCP/HTTP in this change.
- Do not support multiple modem modules beyond keeping the factory pattern ready for future modules.
- Do not add backward-compatible `lwlte_core_*` aliases unless explicitly required.
- Do not convert the project to C++ or introduce a platform abstraction layer.
