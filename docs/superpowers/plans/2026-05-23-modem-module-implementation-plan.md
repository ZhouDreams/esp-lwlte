# Modem Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Modem Adapter layer with common `modem_*` APIs, event dispatch, and Air780EP system/network AT command support.

**Architecture:** Add a common opaque `modem_t` base class with internal `modem_ops_t` dispatch, then implement `modem_air780ep_t` as the first concrete subclass. Core will depend only on `modem.h`; Board Init will use `modem_air780ep.h` to create the concrete module. URCs are translated in Air780EP handlers, queued in the Modem base, and delivered by a Modem event task rather than the AT Engine RX task.

**Tech Stack:** C99, ESP-IDF 6.0, FreeRTOS queues/tasks/semaphores, ESP-IDF GPIO driver, existing `at_engine` API, `esp_err_t`, `esp_check.h`, project C OOP conventions.

---

## User Constraints

- Do not create git commits unless the user explicitly asks for a commit.
- Work in the main workspace; do not create a git worktree unless explicitly requested.
- Follow `docs/agents/coding-style.md`, `docs/agents/oop-design.md`, and `docs/agents/err.md`.
- `reference/` is read-only.

## Scope Check

The spec covers one subsystem: the Modem Adapter layer. It includes public Modem APIs, the Modem base/event mechanism, Air780EP concrete implementation, and build integration. It excludes Core Service, MQTT/HTTP/FTP/SMS/GNSS/socket data path, and real hardware verification.

## File Structure

- Create: `src/include/modem.h`
  - Public inter-layer API used by Core and Board Init.
  - Defines `modem_t`, state enums, value objects, event types, callback type, and `modem_*` wrapper functions.
- Create: `src/include/modem_air780ep.h`
  - Board Init-only Air780EP factory API.
  - Defines `modem_air780ep_config_t` and `modem_air780ep_create()`.
- Create: `src/modem/modem_priv.h`
  - Internal Modem layer header.
  - Defines `struct modem`, `modem_ops_t`, base lifecycle helpers, state helper, and event post helper.
- Create: `src/modem/modem.c`
  - Implements common wrapper APIs, base resource lifecycle, event task, and callback registration.
- Create: `src/modem/modem_air780ep.c`
  - Implements Air780EP subclass, AT command helpers, response parsing, PDP cache, and URC translation.
- Modify: `src/CMakeLists.txt`
  - Add the two new source files and GPIO driver dependency.
- Modify: `docs/agents/at_cmd_air780ep.md`
  - Replace stale wording that says special responses require a future wrapper with the current `at_engine_send_cmd_with_options()` API.

## Shared Implementation Decisions

- `MODEM_EVENT_PDP_ACTIVATED` and `MODEM_EVENT_PDP_DEACTIVATED` are the canonical event names.
- `modem_info_t` field name for firmware is `fw_revision`.
- `modem_destroy()` calls subclass `ops->destroy()` for subclass cleanup, then frees base resources and the object only if subclass destroy succeeds. If subclass destroy returns an error, keep base resources and object memory alive so any still-registered URC handler cannot point at freed memory.
- Air780EP cache, APN, query, and URC `cid` values supported by the first implementation are `1..4`, mapped to `pdp[cid - 1]`. The Air780EP global TCPIP activation/deactivation path (`AT+CSTT`/`AT+CIICR`/`AT+CIFSR` and `AT+CIPSHUT`) supports only `cid=1`; valid `cid=2..4` returns `ESP_ERR_NOT_SUPPORTED` before sending global commands.
- APN strings must be non-NULL, shorter than `MODEM_APN_MAX_LEN`, and must not contain `"`, `\r`, or `\n`.
- Event posting from URC handlers uses `xQueueSend(queue, &event, 0)` and drops on full queue with a warning.
- Air780EP mutable caches (`pdp[]`, `cached_info`, `last_sim_status`, `last_reg_status`, and `last_signal`) are protected by `modem_t.lock`. Normal API methods use blocking cache locks around cache reads/writes without holding them across AT commands or event posting. URC handlers use non-blocking cache locks and drop the cache update/event if the lock is busy.
- `CLOSED` and `+CIPRXGET:` are not registered in this implementation because they belong to connection/socket data handling.

## Task 1: Public Modem Headers

**Files:**
- Create: `src/include/modem.h`
- Create: `src/include/modem_air780ep.h`

- [ ] **Step 1: Create `src/include/modem.h` with public types**

Use `apply_patch` to add `src/include/modem.h` following the project header template. The file must include `<stdbool.h>`, `<stdint.h>`, and `esp_err.h`.

The public constants must be:

```c
#define MODEM_IMEI_MAX_LEN      16
#define MODEM_IMSI_MAX_LEN      16
#define MODEM_ICCID_MAX_LEN     24
#define MODEM_MODEL_MAX_LEN     32
#define MODEM_FW_REV_MAX_LEN    64
#define MODEM_APN_MAX_LEN       64
#define MODEM_PDP_TYPE_MAX_LEN  8
#define MODEM_IP_ADDR_MAX_LEN   48
```

The public type block must define these names and fields:

```c
typedef struct modem modem_t;

typedef enum {
    MODEM_STATE_CREATED = 0,
    MODEM_STATE_INITIALIZING,
    MODEM_STATE_READY,
    MODEM_STATE_REGISTERING,
    MODEM_STATE_REGISTERED,
    MODEM_STATE_PDP_ACTIVE,
    MODEM_STATE_ERROR,
    MODEM_STATE_DESTROYING,
} modem_state_t;

typedef enum {
    MODEM_REG_NOT_REGISTERED = 0,
    MODEM_REG_REGISTERED_HOME,
    MODEM_REG_SEARCHING,
    MODEM_REG_DENIED,
    MODEM_REG_UNKNOWN,
    MODEM_REG_REGISTERED_ROAMING,
} modem_reg_status_t;

typedef enum {
    MODEM_SIM_UNKNOWN = 0,
    MODEM_SIM_READY,
    MODEM_SIM_PIN_REQUIRED,
    MODEM_SIM_PUK_REQUIRED,
    MODEM_SIM_NOT_INSERTED,
    MODEM_SIM_ERROR,
} modem_sim_status_t;

typedef struct {
    char imei[MODEM_IMEI_MAX_LEN];
    char imsi[MODEM_IMSI_MAX_LEN];
    char iccid[MODEM_ICCID_MAX_LEN];
    char model[MODEM_MODEL_MAX_LEN];
    char fw_revision[MODEM_FW_REV_MAX_LEN];
} modem_info_t;

typedef struct {
    int rssi;
    int ber;
    int rssi_dbm;
    bool rssi_dbm_valid;
} modem_signal_t;

typedef struct {
    uint8_t cid;
    char apn[MODEM_APN_MAX_LEN];
    char pdp_type[MODEM_PDP_TYPE_MAX_LEN];
    bool active;
    char ip_addr[MODEM_IP_ADDR_MAX_LEN];
} modem_pdp_context_t;

typedef enum {
    MODEM_EVENT_READY = 0,
    MODEM_EVENT_SIM_CHANGED,
    MODEM_EVENT_REG_CHANGED,
    MODEM_EVENT_PDP_ACTIVATED,
    MODEM_EVENT_PDP_DEACTIVATED,
    MODEM_EVENT_SIGNAL_CHANGED,
    MODEM_EVENT_ERROR,
} modem_event_id_t;

typedef struct {
    modem_event_id_t id;
    union {
        modem_sim_status_t sim_status;
        modem_reg_status_t reg_status;
        modem_pdp_context_t pdp;
        modem_signal_t signal;
        int error_code;
    } data;
} modem_event_t;

typedef void (*modem_event_callback_t)(modem_t *modem,
                                       const modem_event_t *event,
                                       void *user_ctx);
```

- [ ] **Step 2: Add public function prototypes to `modem.h`**

Add Doxygen comments for each function and declare exactly these prototypes:

```c
esp_err_t modem_destroy(modem_t *me);
esp_err_t modem_init(modem_t *me);
esp_err_t modem_reset(modem_t *me);

esp_err_t modem_register_event_callback(modem_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

esp_err_t modem_get_state(modem_t *me, modem_state_t *state);
esp_err_t modem_get_info(modem_t *me, modem_info_t *info);
esp_err_t modem_get_sim_status(modem_t *me, modem_sim_status_t *status);
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal);
esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status);

esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn);
esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp);
```

Expected: `modem.h` exposes no `struct modem` definition, no `modem_ops_t`, and no Air780EP private type.

- [ ] **Step 3: Create `src/include/modem_air780ep.h`**

Use `apply_patch` to add `src/include/modem_air780ep.h` following the project header template. It must include `driver/gpio.h`, `at_engine.h`, and `modem.h`.

Define exactly this public config and factory:

```c
typedef struct {
    gpio_num_t pwrkey_pin;
    gpio_num_t reset_pin;
    gpio_num_t status_pin;
    uint32_t power_on_pulse_ms;
    uint32_t reset_pulse_ms;
    uint32_t boot_wait_ms;
    uint32_t default_cmd_timeout_ms;
    int event_queue_size;
    int event_task_stack;
    int event_task_priority;
} modem_air780ep_config_t;

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config);
```

- [ ] **Step 4: Verify public headers are self-contained enough for parsing**

Run: `git diff -- src/include/modem.h src/include/modem_air780ep.h`

Expected: both new headers use `#pragma once`, `extern "C"`, project section headers, and public Doxygen comments. No source file is added yet.

## Task 2: Internal Modem Base API

**Files:**
- Create: `src/modem/modem_priv.h`

- [ ] **Step 1: Create internal private header**

Use `apply_patch` to add `src/modem/modem_priv.h`. The file must include `<stdbool.h>`, `<stddef.h>`, `at_engine.h`, `modem.h`, `freertos/FreeRTOS.h`, `freertos/queue.h`, `freertos/semphr.h`, and `freertos/task.h`.

Add the internal helper macro:

```c
#define MODEM_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

Define `modem_ops_t` exactly as:

```c
typedef struct modem_ops {
    esp_err_t (*destroy)(modem_t *me);
    esp_err_t (*init)(modem_t *me);
    esp_err_t (*reset)(modem_t *me);
    esp_err_t (*get_info)(modem_t *me, modem_info_t *info);
    esp_err_t (*get_sim_status)(modem_t *me, modem_sim_status_t *status);
    esp_err_t (*get_signal)(modem_t *me, modem_signal_t *signal);
    esp_err_t (*get_registration)(modem_t *me, modem_reg_status_t *status);
    esp_err_t (*set_apn)(modem_t *me, uint8_t cid, const char *apn);
    esp_err_t (*activate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*deactivate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*get_pdp_context)(modem_t *me, uint8_t cid,
                                  modem_pdp_context_t *pdp);
} modem_ops_t;
```

- [ ] **Step 2: Define `struct modem` and base helper prototypes**

In `modem_priv.h`, define `struct modem` with these fields:

```c
struct modem {
    const modem_ops_t *ops;
    at_engine_t *at;
    SemaphoreHandle_t lock;
    QueueHandle_t event_queue;
    TaskHandle_t event_task;
    SemaphoreHandle_t event_task_done_sema;
    modem_event_callback_t event_cb;
    void *event_user_ctx;
    modem_state_t state;
    bool destroying;
    bool event_task_stop_requested;
    const char *name;
};
```

Declare these internal helpers:

```c
esp_err_t modem_base_init(modem_t *me, const char *name, at_engine_t *at,
                          const modem_ops_t *ops, int event_queue_size,
                          int event_task_stack, int event_task_priority);
void modem_base_deinit(modem_t *me);
esp_err_t modem_base_stop_event_task(modem_t *me);
esp_err_t modem_post_event(modem_t *me, const modem_event_t *event);
esp_err_t modem_set_state(modem_t *me, modem_state_t state);
```

Expected: `modem_priv.h` is not included by any public header.

## Task 3: Common Modem Implementation

**Files:**
- Create: `src/modem/modem.c`

- [ ] **Step 1: Create `modem.c` structure and static prototypes**

Create `src/modem/modem.c` with the project source template. Include `modem_priv.h`, `<stdlib.h>`, `<string.h>`, `esp_check.h`, and `esp_log.h`.

Add these defines:

```c
#define TAG "modem"
#define MODEM_DEFAULT_EVENT_QUEUE_SIZE     8
#define MODEM_DEFAULT_EVENT_TASK_STACK     4096
#define MODEM_DEFAULT_EVENT_TASK_PRIORITY  9
#define MODEM_EVENT_TASK_WAIT_MS           100
```

Add static prototypes for exactly these helpers:

```c
static void event_task(void *arg);
static esp_err_t check_ready(modem_t *me, bool allow_created);
static esp_err_t call_no_arg(modem_t *me, esp_err_t (*fn)(modem_t *me));
```

- [ ] **Step 2: Implement base initialization and event task**

Implement `modem_base_init()` with this behavior:

- Return `ESP_ERR_INVALID_ARG` if `me`, `name`, `at`, or `ops` is NULL.
- Normalize non-positive event settings to defaults.
- Set `me->ops`, `me->at`, `me->name`, and `MODEM_STATE_CREATED`.
- Create `lock`, `event_queue`, and `event_task_done_sema`.
- Create `event_task` with task name `modem_evt`.
- On any failure, delete resources created so far and return `ESP_ERR_NO_MEM` or the failing `esp_err_t`.

Implement `event_task()` with this loop:

```c
while (!me->event_task_stop_requested) {
    modem_event_t event = {0};
    if (xQueueReceive(me->event_queue, &event,
                      pdMS_TO_TICKS(MODEM_EVENT_TASK_WAIT_MS)) != pdTRUE) {
        continue;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    modem_event_callback_t cb = me->event_cb;
    void *user_ctx = me->event_user_ctx;
    xSemaphoreGive(me->lock);

    if (cb) {
        cb(me, &event, user_ctx);
    }
}

xSemaphoreGive(me->event_task_done_sema);
vTaskDelete(NULL);
```

- [ ] **Step 3: Implement base deinit, stop, post, and state helpers**

Implement these behaviors:

- `modem_base_stop_event_task()` sets `event_task_stop_requested = true`, waits on `event_task_done_sema` if `event_task` is non-NULL, then sets `event_task = NULL`.
- `modem_base_deinit()` deletes `event_queue`, `event_task_done_sema`, and `lock` if non-NULL, then sets fields to NULL.
- `modem_post_event()` validates arguments and calls `xQueueSend(me->event_queue, event, 0)`. If the queue is full, log a warning and return `ESP_ERR_TIMEOUT`.
- `modem_set_state()` validates arguments, locks, updates `me->state`, unlocks, and returns `ESP_OK`.

- [ ] **Step 4: Implement wrapper APIs**

Implement all functions declared in `modem.h`.

Wrapper rules:

- Every wrapper checks `me` and output arguments with `ESP_RETURN_ON_FALSE`.
- `modem_destroy()` allows `MODEM_STATE_CREATED`, `MODEM_STATE_READY`, `MODEM_STATE_REGISTERING`, `MODEM_STATE_REGISTERED`, `MODEM_STATE_PDP_ACTIVE`, and `MODEM_STATE_ERROR`.
- `modem_destroy()` sets state to `MODEM_STATE_DESTROYING`, sets `destroying = true`, stops event task, calls `me->ops->destroy(me)` if non-NULL, and returns immediately without base deinit/free if subclass destroy returns an error.
- `modem_destroy()` deinitializes base resources and calls `free(me)` only after subclass destroy succeeds.
- `modem_register_event_callback()` allows `callback == NULL` to clear the callback.
- `modem_get_state()` reads state under lock.
- Operational wrappers return `ESP_ERR_INVALID_STATE` if `destroying` is true.
- Operational wrappers return `ESP_ERR_NOT_SUPPORTED` if the corresponding ops method is NULL.

Expected wrapper-to-ops mapping:

```c
modem_init              -> ops->init
modem_reset             -> ops->reset
modem_get_info          -> ops->get_info
modem_get_sim_status    -> ops->get_sim_status
modem_get_signal        -> ops->get_signal
modem_get_registration  -> ops->get_registration
modem_set_apn           -> ops->set_apn
modem_activate_pdp      -> ops->activate_pdp
modem_deactivate_pdp    -> ops->deactivate_pdp
modem_get_pdp_context   -> ops->get_pdp_context
```

- [ ] **Step 5: Verify common implementation diff**

Run: `git diff -- src/modem/modem_priv.h src/modem/modem.c`

Expected: private header and common implementation compile conceptually, with no Air780EP-specific AT command strings in `modem.c`.

## Task 4: Air780EP Skeleton, Factory, and Build Integration

**Files:**
- Create: `src/modem/modem_air780ep.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create Air780EP source structure**

Create `src/modem/modem_air780ep.c` with the project source template. Include `modem_air780ep.h`, `modem_priv.h`, `<stdbool.h>`, `<stdlib.h>`, `<string.h>`, `driver/gpio.h`, `esp_check.h`, `esp_log.h`, and `freertos/task.h`.

Add defines:

```c
#define TAG "modem_air780ep"
#define AIR780EP_MAX_PDP_CONTEXTS        4
#define AIR780EP_MAX_RESPONSE_LINES      8
#define AIR780EP_PARSE_BUF_SIZE          128
#define AIR780EP_DEFAULT_CMD_TIMEOUT_MS  9000
#define AIR780EP_CSTT_TIMEOUT_MS         60000
#define AIR780EP_CIICR_TIMEOUT_MS        90000
#define AIR780EP_CIPSHUT_TIMEOUT_MS      90000
```

Add `air780ep_cmd_ctx_t`:

```c
typedef struct {
    char *lines[AIR780EP_MAX_RESPONSE_LINES];
    at_response_t response;
} air780ep_cmd_ctx_t;
```

Add `modem_air780ep_t` with these fields:

```c
typedef struct {
    modem_t base;
    modem_air780ep_config_t config;
    at_urc_handler_t rdy_handler;
    at_urc_handler_t cpin_handler;
    at_urc_handler_t creg_handler;
    at_urc_handler_t cereg_handler;
    at_urc_handler_t cgreg_handler;
    at_urc_handler_t cgev_handler;
    at_urc_handler_t pdp_deact_handler;
    at_urc_handler_t pdp_colon_deact_handler;
    modem_info_t cached_info;
    modem_sim_status_t last_sim_status;
    modem_reg_status_t last_reg_status;
    modem_signal_t last_signal;
    modem_pdp_context_t pdp[AIR780EP_MAX_PDP_CONTEXTS];
    bool urc_registered;
    bool initialized;
} modem_air780ep_t;
```

- [ ] **Step 2: Add Air780EP ops table and method prototypes**

Add static prototypes for all ops methods and helper functions that later tasks fill:

```c
static esp_err_t air780ep_destroy(modem_t *me);
static esp_err_t air780ep_init(modem_t *me);
static esp_err_t air780ep_reset(modem_t *me);
static esp_err_t air780ep_get_info(modem_t *me, modem_info_t *info);
static esp_err_t air780ep_get_sim_status(modem_t *me, modem_sim_status_t *status);
static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal);
static esp_err_t air780ep_get_registration(modem_t *me, modem_reg_status_t *status);
static esp_err_t air780ep_set_apn(modem_t *me, uint8_t cid, const char *apn);
static esp_err_t air780ep_activate_pdp(modem_t *me, uint8_t cid);
static esp_err_t air780ep_deactivate_pdp(modem_t *me, uint8_t cid);
static esp_err_t air780ep_get_pdp_context(modem_t *me, uint8_t cid,
                                          modem_pdp_context_t *pdp);
```

Add the `static const modem_ops_t s_air780ep_ops` table assigning every method above.

- [ ] **Step 3: Implement factory and minimal cleanup**

Implement `modem_air780ep_create()` with this behavior:

- Validate `at` and `config`.
- Allocate `modem_air780ep_t` with `calloc`.
- Copy config and normalize `default_cmd_timeout_ms`, `event_queue_size`, `event_task_stack`, and `event_task_priority` through `modem_base_init()` defaults.
- Initialize each `pdp[i].cid = i + 1`, `pdp[i].pdp_type = "IP"`, and `last_*` values to unknown.
- Call `modem_base_init(&self->base, "air780ep", at, &s_air780ep_ops, config->event_queue_size, config->event_task_stack, config->event_task_priority)`.
- On failure, free `self` and return NULL.
- On success, return `&self->base`.

Implement `air780ep_destroy()` as subclass cleanup only:

- If URCs are registered, unregister every prefix registered by `air780ep_init()`.
- Set `urc_registered = false` and `initialized = false`.
- Do not delete base FreeRTOS resources and do not free the object.

- [ ] **Step 4: Update CMake**

Modify `src/CMakeLists.txt` to register all sources and dependencies:

```cmake
idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
    INCLUDE_DIRS include
    REQUIRES esp_driver_uart esp_driver_gpio
)
```

- [ ] **Step 5: Run build to expose missing helper implementations**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build may fail because Air780EP methods are declared and in the ops table but not fully implemented yet. Continue only with compile errors related to this new file; do not edit unrelated files.

## Task 5: Air780EP Command Helpers and Initialization

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add common command helpers**

Implement these helpers in `modem_air780ep.c`:

```c
static modem_air780ep_t *to_air780ep(modem_t *me);
static void init_cmd_ctx(air780ep_cmd_ctx_t *ctx);
static esp_err_t send_cmd(modem_air780ep_t *self, const char *cmd,
                          air780ep_cmd_ctx_t *ctx, uint32_t timeout_ms);
static esp_err_t send_cmd_with_options(modem_air780ep_t *self, const char *cmd,
                                       air780ep_cmd_ctx_t *ctx,
                                       const at_cmd_options_t *options);
static esp_err_t ensure_at_ok(const at_response_t *response, const char *cmd);
static const char *find_line_with_prefix(const at_response_t *response,
                                         const char *prefix);
static const char *first_data_line(const at_response_t *response);
static esp_err_t copy_str_field(char *dst, size_t dst_size, const char *src);
static bool cid_valid(uint8_t cid);
static modem_pdp_context_t *pdp_by_cid(modem_air780ep_t *self, uint8_t cid);
static bool at_arg_safe(const char *value);
```

Required helper behavior:

- `to_air780ep()` uses `MODEM_CONTAINER_OF(me, modem_air780ep_t, base)`.
- `init_cmd_ctx()` zeroes the context and sets `response.lines` and `response.max_lines`.
- `send_cmd()` uses the configured default timeout when `timeout_ms == 0`.
- `ensure_at_ok()` returns `ESP_OK` only for `response.status == AT_RESP_OK`; it logs and returns `ESP_FAIL` for `AT_RESP_ERROR`, `AT_RESP_CME_ERROR`, or `AT_RESP_CMS_ERROR`.
- `copy_str_field()` uses `strlcpy`, rejects NULL destination/source, and returns `ESP_ERR_INVALID_RESPONSE` if truncation occurs.
- `at_arg_safe()` returns false if the string contains `"`, `\r`, or `\n`.

- [ ] **Step 2: Add GPIO power/reset helpers**

Implement:

```c
static esp_err_t pulse_gpio(gpio_num_t pin, uint32_t active_ms);
static esp_err_t maybe_power_on(modem_air780ep_t *self);
```

Required behavior:

- If a pin is `GPIO_NUM_NC`, skip it and return `ESP_OK`.
- `pulse_gpio()` calls `gpio_reset_pin(pin)`, `gpio_set_direction(pin, GPIO_MODE_OUTPUT)`, drives the pin low, delays 10 ms, drives high for `active_ms`, then drives low again.
- `maybe_power_on()` pulses `pwrkey_pin` when configured, then delays `boot_wait_ms` when non-zero.

- [ ] **Step 3: Implement URC registration helpers**

Implement:

```c
static esp_err_t register_urcs(modem_air780ep_t *self);
static void unregister_urcs(modem_air780ep_t *self);
```

`register_urcs()` must initialize handler nodes with these prefixes and callbacks:

```c
RDY                  -> rdy_urc_handler
+CPIN:              -> cpin_urc_handler
+CREG:              -> reg_urc_handler
+CEREG:             -> reg_urc_handler
+CGREG:             -> reg_urc_handler
+CGEV:              -> cgev_urc_handler
+PDP DEACT          -> pdp_deact_urc_handler
+PDP:DEACT          -> pdp_deact_urc_handler
```

Use `at_engine_register_urc(self->base.at, prefix, &handler)` for each one. If any registration fails, unregister previously registered handlers and return the error.

- [ ] **Step 4: Implement `air780ep_init()` and `air780ep_reset()`**

`air780ep_init()` must:

- Set state to `MODEM_STATE_INITIALIZING`.
- Call `maybe_power_on()`.
- Register URCs once.
- Send `ATE0`, `AT+CMEE=1`, `AT+CGEREP=1`, `AT+CEREG=2`, `AT+CGREG=2`, and `AT+CREG=2`, checking `ensure_at_ok()` for each.
- Set `initialized = true`.
- Set state to `MODEM_STATE_READY`.
- Post `MODEM_EVENT_READY`.

`air780ep_reset()` must:

- Send `AT+RESET`.
- Check `ensure_at_ok()`.
- Set `initialized = false`.
- Set state to `MODEM_STATE_CREATED`.

- [ ] **Step 5: Build after initialization helpers**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build may still fail because information, signal, registration, PDP, and URC parsing helpers are not complete. Failures should now be limited to functions scheduled in later tasks.

## Task 6: Air780EP Info, SIM, Signal, and Registration Methods

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add parser helpers for common line formats**

Implement:

```c
static const char *skip_prefix_value(const char *line, const char *prefix);
static esp_err_t parse_int_after_prefix(const char *line, const char *prefix, int *out);
static esp_err_t parse_two_ints_after_prefix(const char *line, const char *prefix,
                                             int *first, int *second);
static modem_reg_status_t map_reg_status(int stat);
static esp_err_t parse_registration_line(const char *line, const char *prefix,
                                         modem_reg_status_t *status);
static modem_sim_status_t parse_sim_status_line(const char *line);
```

Parsing rules:

- `skip_prefix_value()` returns the first non-space character after the prefix and optional colon.
- `parse_registration_line()` supports query responses (`+CEREG: <n>,<stat>[,...]`) and uses the second integer as `stat`. URC handlers must use a separate URC parser that treats the first integer after the prefix as `stat` and validates trailing optional fields.
- `map_reg_status()` maps `0 -> MODEM_REG_NOT_REGISTERED`, `1 -> MODEM_REG_REGISTERED_HOME`, `2 -> MODEM_REG_SEARCHING`, `3 -> MODEM_REG_DENIED`, `5 -> MODEM_REG_REGISTERED_ROAMING`, and all other values to `MODEM_REG_UNKNOWN`.
- `parse_sim_status_line()` maps strings containing `READY`, `SIM PIN`, `SIM PUK`, `NOT INSERTED`, or `REMOVED` to the matching enum; unknown values map to `MODEM_SIM_ERROR`.

- [ ] **Step 2: Implement `air780ep_get_info()`**

Implement command sequence and field mapping:

```text
AT+CGSN   -> first data line -> info.imei
AT+CIMI   -> first data line -> info.imsi
AT+ICCID  -> +ICCID: value if present, otherwise first data line -> info.iccid
AT+CGMM   -> +CGMM: value if present, otherwise first data line -> strip one surrounding quote pair -> info.model
AT+CGMR   -> +CGMR: value if present, otherwise first data line -> strip one surrounding quote pair -> info.fw_revision
```

For each command:

- Call `send_cmd()` with default timeout.
- Check `ensure_at_ok()`.
- Copy the parsed value with `copy_str_field()`.

On full success, copy the result to `self->cached_info` and to `*info`.

- [ ] **Step 3: Implement `air780ep_get_sim_status()`**

Implementation requirements:

- Send `AT+CPIN?`.
- Require `ensure_at_ok()`.
- Find `+CPIN:` line.
- Parse with `parse_sim_status_line()`.
- Store `self->last_sim_status` and return it.
- If no `+CPIN:` line exists, return `ESP_ERR_INVALID_RESPONSE`.

- [ ] **Step 4: Implement `air780ep_get_signal()`**

Implementation requirements:

- Send `AT+CSQ`.
- Require `ensure_at_ok()`.
- Parse `+CSQ: <rssi>,<ber>` using `parse_two_ints_after_prefix()`.
- Fill `signal->rssi` and `signal->ber`.
- If `rssi` is `0..31`, set `rssi_dbm = -113 + (2 * rssi)` and `rssi_dbm_valid = true`.
- If `rssi == 99`, set `rssi_dbm = 0` and `rssi_dbm_valid = false`.
- Reject all other RSSI/BER values with `ESP_ERR_INVALID_RESPONSE`.
- Store `self->last_signal`.

- [ ] **Step 5: Implement `air780ep_get_registration()`**

Implementation requirements:

- Try `AT+CEREG?` first and parse `+CEREG:`.
- If command fails, response status is not OK, line is missing, or parsed status is `MODEM_REG_UNKNOWN`, try `AT+CGREG?` and parse `+CGREG:`.
- If still unknown, try `AT+CREG?` and parse `+CREG:`.
- Return the first parsed non-unknown status, or `MODEM_REG_UNKNOWN` with `ESP_OK` if all commands responded but status remained unknown.
- Store `self->last_reg_status`.
- Set state to `MODEM_STATE_REGISTERED` when status is home or roaming.

- [ ] **Step 6: Build after query methods**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: remaining failures, if any, are only in PDP methods and URC handlers scheduled below.

## Task 7: Air780EP PDP and APN Methods

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Implement `air780ep_set_apn()`**

Implementation requirements:

- Validate `cid` with `cid_valid()`.
- Validate `apn` is non-NULL, length `< MODEM_APN_MAX_LEN`, and `at_arg_safe(apn)` is true.
- Format command into a stack buffer:

```c
char cmd[96];
snprintf(cmd, sizeof(cmd), "AT+CGDCONT=%u,\"IP\",\"%s\"", cid, apn);
```

- Reject truncation from `snprintf()` with `ESP_ERR_INVALID_ARG`.
- Send command and require `ensure_at_ok()`.
- Update PDP cache for `cid`: copy APN, set `pdp_type` to `IP`, preserve current active/IP fields.

- [ ] **Step 2: Implement attach and status helpers**

Implement:

```c
static esp_err_t query_cgatt(modem_air780ep_t *self, bool *attached);
static esp_err_t query_cgact(modem_air780ep_t *self, uint8_t cid, bool *active);
static esp_err_t query_cgpaddr(modem_air780ep_t *self, uint8_t cid,
                               char *ip_addr, size_t ip_addr_size);
static bool looks_like_ip_addr(const char *line);
```

Required behavior:

- `query_cgatt()` sends `AT+CGATT?` and parses `+CGATT: <state>`.
- `query_cgact()` sends `AT+CGACT?` and searches response lines for matching `+CGACT: <cid>,<state>`.
- `query_cgpaddr()` sends `AT+CGPADDR=<cid>`, parses `+CGPADDR: <cid>,<addr>`, and copies the address when non-empty.
- `looks_like_ip_addr()` returns true for IPv4 dotted decimal strings. IPv6 address validation is future scope.

- [ ] **Step 3: Implement `air780ep_activate_pdp()`**

Implementation requirements:

- Validate `cid`, then return `ESP_ERR_NOT_SUPPORTED` for valid but unsupported `cid=2..4` before sending any global TCPIP commands.
- Call `air780ep_get_sim_status()` and require `MODEM_SIM_READY`; otherwise return `ESP_ERR_INVALID_STATE`.
- Call `air780ep_get_registration()` and require home or roaming registration; otherwise return `ESP_ERR_INVALID_STATE`.
- Call `query_cgatt()` and require attached; otherwise return `ESP_ERR_INVALID_STATE`.
- If cached APN for `cid` is non-empty, send `AT+CSTT="<apn>"`; otherwise send `AT+CSTT`.
- Send `AT+CIICR` with `AIR780EP_CIICR_TIMEOUT_MS`.
- Send `AT+CIFSR` using `at_engine_send_cmd_with_options()` with one `AT_CMD_SUCCESS_MATCH_ANY_LINE` rule and `AT_CMD_FLAG_NO_STANDARD_OK_FINAL`.
- Require the returned line to satisfy `looks_like_ip_addr()`.
- Update PDP cache: `active = true`, `ip_addr = returned IP`, `pdp_type = "IP"`.
- Set state to `MODEM_STATE_PDP_ACTIVE`.
- Post `MODEM_EVENT_PDP_ACTIVATED` with the PDP snapshot.

- [ ] **Step 4: Implement `air780ep_deactivate_pdp()`**

Implementation requirements:

- Validate `cid`, then return `ESP_ERR_NOT_SUPPORTED` for valid but unsupported `cid=2..4` before sending `AT+CIPSHUT`.
- Send `AT+CIPSHUT` using `at_engine_send_cmd_with_options()` with one exact success rule `SHUT OK` and timeout `AIR780EP_CIPSHUT_TIMEOUT_MS`.
- On success, clear `active` and `ip_addr` for all active PDP cache entries because `AT+CIPSHUT` is a global TCP/IP shutdown.
- Set state to `MODEM_STATE_READY`.
- Post `MODEM_EVENT_PDP_DEACTIVATED` for affected active contexts.

- [ ] **Step 5: Implement `air780ep_get_pdp_context()`**

Implementation requirements:

- Validate `cid` and output pointer.
- Start with cached PDP data.
- Call `query_cgact()`; if it succeeds, update cached `active`.
- If `query_cgact()` succeeds with inactive state, clear cached `ip_addr`, copy the cache snapshot to `*pdp`, and return `ESP_OK` without requiring `query_cgpaddr()`.
- If `query_cgact()` returns `ESP_ERR_NOT_FOUND`, keep cached state and continue conservatively; propagate real AT/send/parser errors.
- Call `query_cgpaddr()` only when PDP is known or cached as active. On success, copy the returned address including an empty value. Propagate real AT/send/parser errors while active; if no address is found, return a cached inactive context.
- Copy the final cache snapshot to `*pdp`.

- [ ] **Step 6: Build after PDP methods**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: source may still fail only if URC handler prototypes are unresolved. Command helper, query method, and PDP method compile errors must be fixed before moving on.

## Task 8: Air780EP URC Translation

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add URC handler prototypes and implementations**

Implement handlers with the AT Engine callback signature:

```c
static void rdy_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void cpin_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void reg_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void cgev_urc_handler(const char *prefix, const char *line, void *user_ctx);
static void pdp_deact_urc_handler(const char *prefix, const char *line, void *user_ctx);
```

Behavior:

- Ignore NULL `user_ctx`.
- Cast `user_ctx` to `modem_air780ep_t *`.
- Never call `at_engine_send_cmd()` or any AT Engine registration API from these handlers.
- Post translated events through `modem_post_event(&self->base, &event)`.

- [ ] **Step 2: Implement handler translation rules**

Required handler behavior:

- `rdy_urc_handler()` posts `MODEM_EVENT_READY`.
- `cpin_urc_handler()` parses SIM status, updates `last_sim_status`, posts `MODEM_EVENT_SIM_CHANGED`.
- `reg_urc_handler()` parses registration with the matched prefix, updates `last_reg_status`, updates state to `MODEM_STATE_REGISTERED` when home/roaming, posts `MODEM_EVENT_REG_CHANGED`.
- `cgev_urc_handler()` detects substrings `PDN ACT` and `PDN DEACT`. For ACT, mark the parsed cid active and post `MODEM_EVENT_PDP_ACTIVATED`. For DEACT, mark the parsed cid inactive, clear IP, set state to `MODEM_STATE_READY` with the non-blocking state helper, and post `MODEM_EVENT_PDP_DEACTIVATED`.
- `pdp_deact_urc_handler()` treats `+PDP DEACT` and `+PDP:DEACT` as global deactivation, marks all PDP cache entries inactive, clears all IP fields, and posts `MODEM_EVENT_PDP_DEACTIVATED` for affected contexts.

CID parsing rule:

- For `+CGEV` lines, parse the decimal cid immediately following `PDN ACT` or `PDN DEACT` and use it only when it is in `1..4`.
- If no valid cid is found, log and drop the malformed `+CGEV` line.

- [ ] **Step 3: Run build after URC implementation**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build succeeds or exposes concrete compile errors in the new Modem files. Fix only errors introduced by this implementation.

## Task 9: Documentation Sync and Final Verification

**Files:**
- Modify: `docs/agents/at_cmd_air780ep.md`
- Verify: `src/include/modem.h`
- Verify: `src/include/modem_air780ep.h`
- Verify: `src/modem/modem_priv.h`
- Verify: `src/modem/modem.c`
- Verify: `src/modem/modem_air780ep.c`
- Verify: `src/CMakeLists.txt`

- [ ] **Step 1: Update stale special-response wording**

In `docs/agents/at_cmd_air780ep.md`, update the `AT+CIFSR` and `AT+CIPSHUT` notes so they explicitly reference current AT Engine options:

```markdown
`AT+CIFSR` 使用 `at_engine_send_cmd_with_options()` 和 `AT_CMD_SUCCESS_MATCH_ANY_LINE` 处理纯 IP 成功行。
```

```markdown
`AT+CIPSHUT` 使用 `at_engine_send_cmd_with_options()` 和 exact match `SHUT OK` 处理非标准成功终止行。
```

- [ ] **Step 2: Run whitespace check**

Run: `git diff --check`

Expected: no output and exit status 0.

- [ ] **Step 3: Run ESP-IDF build**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build succeeds.

- [ ] **Step 4: Search for stale or inconsistent names**

Run: `rg -n "MODEM_EVENT_PDP_ACTIVE\\b|MODEM_EVENT_PDP_DEACTIVE\\b|modem_info_t\\.(firmware|revision)\\b|send_cmd_ex|modem_set_function_mode|modem_check_sim" src docs/agents docs/superpowers/specs/2026-05-23-modem-module-implementation-design.md`

Expected: no matches.

- [ ] **Step 5: Verify CMake source registration**

Run: `rg -n "modem/modem\\.c|modem/modem_air780ep\\.c|esp_driver_gpio" src/CMakeLists.txt`

Expected: matches for both new source files and `esp_driver_gpio`.

- [ ] **Step 6: Review final diff**

Run: `git diff -- src/include/modem.h src/include/modem_air780ep.h src/modem/modem_priv.h src/modem/modem.c src/modem/modem_air780ep.c src/CMakeLists.txt docs/agents/at_cmd_air780ep.md docs/superpowers/specs/2026-05-23-modem-module-implementation-design.md docs/superpowers/plans/2026-05-23-modem-module-implementation-plan.md`

Expected:

- Public headers expose only intended layer APIs.
- `modem.c` contains no Air780EP AT command strings.
- `modem_air780ep.c` contains all Air780EP AT command strings and URC translation.
- `src/CMakeLists.txt` includes new sources and GPIO dependency.
- Documentation changes only remove stale special-response wording and align corrected `+CGEV` CID/global deactivation behavior.

## Self-Review Notes

- Spec coverage: Tasks 1-4 cover file boundaries, public/private APIs, base lifecycle, event task, factory, and build integration. Tasks 5-8 cover Air780EP initialization, command helpers, query methods, PDP methods, special responses, and URC translation. Task 9 covers docs sync and verification.
- Placeholder scan: This plan intentionally avoids unresolved markers and unspecified implementation steps.
- Type consistency: Event names use `MODEM_EVENT_PDP_ACTIVATED` and `MODEM_EVENT_PDP_DEACTIVATED`; firmware field uses `fw_revision`; special responses use `at_engine_send_cmd_with_options()`.
- Commit policy: No commit steps are included because commits require explicit user approval.
