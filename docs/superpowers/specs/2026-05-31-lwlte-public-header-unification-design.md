# LWLTE Public Header Unification Design

## Background

The current public facade API is split across `src/include/lwlte.h` and `src/include/lwlte_air780ep.h`. `lwlte.h` contains the generic `lwlte_t` handle and common runtime APIs, while `lwlte_air780ep.h` contains the Air780EP config and factory function.

The desired external model is simpler: applications include one public header, create an LTE facade through a module-specific factory such as `lwlte_air780ep_init()`, and then use the same `lwlte_*` APIs regardless of the underlying module.

## Goals

- Expose a single public user header: `src/include/lwlte.h`.
- Move Air780EP public config types and `lwlte_air780ep_init()` into `lwlte.h`.
- Keep module factory init functions at the top of the `GLOBAL PROTOTYPES` section, before `lwlte_destroy()`.
- Keep implementation files split by responsibility: common facade logic in `lwlte.c`, Air780EP factory logic in `lwlte_air780ep.c`.
- Preserve the runtime object model: all module factories return `lwlte_t *`; `lwlte_t` internally owns a `modem_t *` whose `ops` determine concrete module behavior.

## Non-Goals

- Do not merge `lwlte_air780ep.c` into `lwlte.c`.
- Do not expose `modem_t`, `core_t`, `at_engine_t`, or concrete modem object types to users.
- Do not add a generic `lwlte_init()` that hides the module choice.
- Do not add compatibility aliases unless a concrete external compatibility requirement appears.

## Public Header Layout

`src/include/lwlte.h` remains the only public LWLTE header. Its high-level order is:

```c
/**********************
 *      TYPEDEFS
 **********************/

/* Generic facade handle, states, events, MQTT message, Ping types. */

/* Air780EP public config types. */
typedef struct {
    bool enabled;
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keepalive_s;
    bool clean_session;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
} lwlte_air780ep_config_mqtt_client_t;

typedef struct {
    uart_port_t uart_num;
    gpio_num_t uart_tx_pin;
    gpio_num_t uart_rx_pin;
    int uart_baud_rate;
    gpio_num_t en_pin;
    const char *apn;
    uint8_t primary_cid;
    bool auto_connect;
    uint32_t init_ready_timeout_ms;
    uint32_t net_activate_timeout_ms;
    uint32_t reconnect_delay_ms;
    int at_rx_buf_size;
    int at_rx_task_stack;
    int at_rx_task_priority;
    int at_rx_line_buf_size;
    int at_cmd_default_timeout_ms;
    int at_max_response_lines;
    uint32_t modem_reset_pulse_ms;
    uint32_t modem_default_cmd_timeout_ms;
    int modem_event_queue_size;
    int modem_event_task_stack;
    int modem_event_task_priority;
    int core_fsm_queue_size;
    int core_fsm_task_stack;
    int core_fsm_task_priority;
    lwlte_air780ep_config_mqtt_client_t mqtt_client;
} lwlte_air780ep_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);

/* Future module factories also go here, before destroy. */
esp_err_t lwlte_xxx_init(const lwlte_xxx_config_t *config,
                         lwlte_t **out_lte);

esp_err_t lwlte_destroy(lwlte_t *me);
esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx);
esp_err_t lwlte_connect(lwlte_t *me);
```

Module-specific config structs live in the `TYPEDEFS` section after generic user-facing types and before `GLOBAL PROTOTYPES`. This keeps required type definitions before their function declarations while making the `GLOBAL PROTOTYPES` section start with creation APIs.

## Implementation Layout

Implementation files remain split:

- `src/lwlte/lwlte.c`: common `lwlte_t` lifecycle, state, event bridge, connect/disconnect, MQTT facade, Ping facade, and destruction logic.
- `src/lwlte/lwlte_air780ep.c`: Air780EP-specific factory assembly, config validation, timeout helpers, cleanup helper, and other Air780EP-only `static` functions.
- Future `src/lwlte/lwlte_xxx.c` files: one module factory implementation per module family, with module-specific `static` helpers kept local to that file.

`lwlte_air780ep.c` includes `lwlte.h` and `lwlte_priv.h` directly. It no longer depends on a public `lwlte_air780ep.h` header.

## Object Model

Applications never create `lwlte_t` directly. They call a concrete module factory:

```c
lwlte_t *lte = NULL;
esp_err_t ret = lwlte_air780ep_init(&config, &lte);
```

The factory creates the concrete modem object, then stores it through the base `modem_t *` inside `lwlte_t`. For concrete modem objects that embed `modem_t base` as their first member, using `&child->base` as a `modem_t *` is upcasting. Recovering the concrete object from a `modem_t *` inside module-specific ops is downcasting and must use the existing container-of pattern.

Core and common facade APIs operate only on `modem_t *` through `modem_*` wrapper APIs. They do not know whether the actual module is Air780EP or a future module.

## Documentation Updates

Architecture and OOP guidance should be updated to describe a single public `lwlte.h` include. Any references that say applications or board initialization include `lwlte_air780ep.h` should change to say they include `lwlte.h` and use the module-specific config and factory declared there.

The documented implementation split should remain `lwlte.c` for common facade behavior and `lwlte_air780ep.c` for the Air780EP factory.

## Testing

Verification should include:

- Build the ESP-IDF component or project after removing the public Air780EP header.
- Search for `#include "lwlte_air780ep.h"` and update users to `#include "lwlte.h"`.
- Confirm `lwlte_air780ep_init()` is declared before `lwlte_destroy()` in `lwlte.h`.
- Confirm `lwlte_air780ep.c` still compiles with module-specific `static` helpers kept local.
