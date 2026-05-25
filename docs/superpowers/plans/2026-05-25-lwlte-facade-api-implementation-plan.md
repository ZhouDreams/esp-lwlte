# LWLTE Facade API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Do not commit unless the user explicitly authorizes it; use status checkpoints instead.

**Goal:** Replace the current Core-facing user surface with a real `lwlte_t` facade API and an Air780EP factory so applications only include `lwlte.h` and `lwlte_air780ep.h`.

**Architecture:** Add `src/lwlte/` as the public facade implementation. Move AT Engine, Modem, Air780EP modem factory, and Core headers out of `src/include` into their module directories and expose them only through `PRIV_INCLUDE_DIRS`. Demote Core from user API to service-level inter-layer API by renaming `lwlte_core_*` symbols to `core_*` symbols.

**Tech Stack:** ESP-IDF C component, FreeRTOS semaphores, ESP logging/check macros, UART/GPIO drivers, ESP event inside Core, `idf_component_register()` public/private include directories.

---

## File Structure

- Create `src/include/lwlte.h`: true public generic user API; declares opaque `lwlte_t`, LWLTE states/events, callback, connect/disconnect/state/destroy functions.
- Create `src/include/lwlte_air780ep.h`: true public Air780EP factory config and `lwlte_air780ep_init()`; includes ESP-IDF UART/GPIO public types but no lower-layer project headers except `lwlte.h`.
- Create `src/lwlte/lwlte_priv.h`: facade-private runtime definition for `struct lwlte`, plus internal helper prototypes shared by `lwlte.c` and `lwlte_air780ep.c`.
- Create `src/lwlte/lwlte.c`: generic facade operations, Core event translation, ready wait helper, and owned-resource destroy sequence.
- Create `src/lwlte/lwlte_air780ep.c`: Air780EP composition root; creates AT Engine, Air780EP Modem, Core, waits for ready, optionally submits connect.
- Move `src/include/at_engine.h` to `src/at_engine/at_engine.h`: inter-layer API for Modem and facade composition roots.
- Move `src/include/modem.h` to `src/modem/modem.h`: inter-layer API for Core and facade composition roots.
- Move `src/include/modem_air780ep.h` to `src/modem/modem_air780ep.h`: inter-layer Air780EP modem factory API for facade composition roots.
- Move `src/include/lwlte_core.h` to `src/core/core.h`: inter-layer Core service API, renamed from `lwlte_core_*` to `core_*`.
- Modify `src/core/core_priv.h`: include `core.h`; rename Core public-looking types and internal fields to `core_*` names.
- Modify `src/core/core.c`, `src/core/core_fsm.c`, `src/core/net_mgr.c`, `src/core/pdp_mgr.c`: rename `lwlte_core_*` symbols to `core_*` symbols and keep `net_mgr`, `pdp_mgr`, `core_fsm` as composition members of `core_t`, not subclasses.
- Modify `src/modem/modem_priv.h`: keep as modem-private header; it may be found through `PRIV_INCLUDE_DIRS`, but only files in `src/modem/` should include it.
- Modify `src/CMakeLists.txt`: add facade sources, public include dir, and private include dirs.
- Modify `examples/basic_connect/main/main.c`: convert to user-layer example using only `lwlte.h` and `lwlte_air780ep.h` from this component.
- Modify `examples/basic_connect/main/CMakeLists.txt`: remove direct lower-layer requirements not needed by the example.
- Modify `docs/agents/directory-structure.md`: document `src/lwlte/`, public-only `src/include/`, and private module headers.
- Modify `docs/agents/architecture.md`: document App + internal Facade/Service/Modem/AT Engine layers and the facade composition-root exception.
- Modify `docs/agents/classes.md`: update visibility table; add `lwlte_t` and `lwlte_air780ep_config_t`; demote Core classes to service inter-layer API; clarify `net_mgr_t`, `pdp_mgr_t`, and `core_fsm_t` are owned components, not Core subclasses.
- Modify older example/spec docs only when they directly contradict the new public boundary.

---

## Task 1: Lock Header Visibility And CMake Shape

**Files:**
- Move: `src/include/at_engine.h` -> `src/at_engine/at_engine.h`
- Move: `src/include/modem.h` -> `src/modem/modem.h`
- Move: `src/include/modem_air780ep.h` -> `src/modem/modem_air780ep.h`
- Move: `src/include/lwlte_core.h` -> `src/core/core.h`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Run the current public-header check and observe the red state**

Run from the repository root:

```bash
rg '#include "(at_engine|modem|modem_air780ep|lwlte_core)\.h"' examples src/main main || true
rg '^#include "(at_engine|modem|modem_air780ep|lwlte_core)\.h"' src/include || true
```

Expected result before changes: matches are printed for `examples/basic_connect/main/main.c` and lower-layer public headers under `src/include`. This confirms the current boundary violation.

- [ ] **Step 2: Move layer headers out of `src/include`**

Use `apply_patch` with move operations:

```diff
*** Begin Patch
*** Update File: src/include/at_engine.h
*** Move to: src/at_engine/at_engine.h
*** Update File: src/include/modem.h
*** Move to: src/modem/modem.h
*** Update File: src/include/modem_air780ep.h
*** Move to: src/modem/modem_air780ep.h
*** Update File: src/include/lwlte_core.h
*** Move to: src/core/core.h
*** End Patch
```

Expected result: only the files move. Do not change header contents in this step.

- [ ] **Step 3: Update component CMake include visibility**

Replace `src/CMakeLists.txt` with:

```cmake
idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "core/core.c"
         "core/core_fsm.c"
         "core/net_mgr.c"
         "core/pdp_mgr.c"
         "lwlte/lwlte.c"
         "lwlte/lwlte_air780ep.c"
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS lwlte core modem at_engine
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
```

Expected result: the component exports only `src/include` and keeps `src/lwlte`, `src/core`, `src/modem`, and `src/at_engine` private to the component.

- [ ] **Step 4: Run the expected intermediate build failure**

Run:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build fails because the moved `core.h` still contains `lwlte_core_*` names and the new facade sources/headers do not exist yet. This confirms CMake is now looking for the new facade source files.

---

## Task 2: Demote Core API To Service-Level Names

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core_priv.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`
- Modify: `src/core/net_mgr.c`
- Modify: `src/core/pdp_mgr.c`

- [ ] **Step 1: Apply the Core rename map**

Use project-wide editor refactor or `apply_patch` to apply this exact mapping inside `src/core/*.c` and `src/core/*.h` only:

```text
lwlte_core_t                    -> core_t
struct lwlte_core               -> struct core
lwlte_core_config_t             -> core_config_t
lwlte_core_state_t              -> core_state_t
lwlte_net_state_t               -> core_net_state_t
lwlte_core_event_id_t           -> core_event_id_t
lwlte_core_event_data_t         -> core_event_data_t
lwlte_core_event_callback_t     -> core_event_callback_t
lwlte_core_create               -> core_create
lwlte_core_destroy              -> core_destroy
lwlte_core_start                -> core_start
lwlte_core_stop                 -> core_stop
lwlte_core_register_event_callback -> core_register_event_callback
lwlte_core_get_event_loop       -> core_get_event_loop
lwlte_core_get_state            -> core_get_state
lwlte_core_get_net_state        -> core_get_net_state
lwlte_core_connect              -> core_connect
lwlte_core_disconnect           -> core_disconnect
LWLTE_CORE_EVENT                -> CORE_EVENT
LWLTE_CORE_STATE_               -> CORE_STATE_
LWLTE_NET_STATE_                -> CORE_NET_STATE_
LWLTE_CORE_EVENT_               -> CORE_EVENT_
```

Expected result: Core no longer exposes user-looking `lwlte_core_*` names in the Core source tree.

- [ ] **Step 2: Rewrite the top of `src/core/core.h` after the mechanical rename**

Ensure the file starts with this exact public-facing service description and type declarations:

```c
/**
 * @file core.h
 * @brief LTE 核心服务层间接口
 * @details LTE core service inter-layer interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief 调制解调器句柄前置声明
 * @details Modem handle forward declaration
 */
typedef struct modem modem_t;

/**
 * @brief LTE 核心服务句柄
 * @details LTE core service handle
 */
typedef struct core core_t;
```

Keep the remaining config, enum, event, and function documentation from the current file, but use the `core_*`, `CORE_STATE_*`, `CORE_NET_STATE_*`, and `CORE_EVENT_*` names from Step 1.

- [ ] **Step 3: Fix `src/core/core_priv.h` include and struct declaration**

Ensure `src/core/core_priv.h` includes `core.h` instead of `lwlte_core.h`:

```c
#include "core.h"
#include "modem.h"
```

Ensure the Core object definition is:

```c
struct core {
    core_config_t config;
    modem_t *modem;
    esp_event_loop_handle_t event_loop;
    core_fsm_t fsm;
    net_mgr_t net_mgr;
    pdp_mgr_t pdp_mgr;
    core_state_t state;
    bool destroying;
    bool destroy_in_progress;
    SemaphoreHandle_t lock;
    TaskHandle_t event_loop_task;
    SemaphoreHandle_t event_callback_done_sema;
    TaskHandle_t event_callback_task;
    int event_callback_active;
    bool event_callback_waiting;
    core_event_callback_t event_callback;
    void *event_user_ctx;
};
```

Expected result: `core_fsm_t`, `net_mgr_t`, and `pdp_mgr_t` remain composition fields owned by `core_t`.

- [ ] **Step 4: Verify the Core rename**

Run:

```bash
rg 'lwlte_core|LWLTE_CORE|LWLTE_NET_STATE|LWLTE_CORE_STATE' src/core || true
```

Expected result: no matches in `src/core`. If matches remain, rename them to the corresponding `core_*` or `CORE_*` names.

- [ ] **Step 5: Run the expected intermediate build failure**

Run:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build still fails because facade headers and sources are not implemented yet, but failures should no longer mention missing `lwlte_core.h` from Core source files.

---

## Task 3: Add Public Facade Headers

**Files:**
- Create: `src/include/lwlte.h`
- Create: `src/include/lwlte_air780ep.h`

- [ ] **Step 1: Write `src/include/lwlte.h`**

Create `src/include/lwlte.h` with exactly:

```c
/**
 * @file lwlte.h
 * @brief LTE 用户门面公共接口
 * @details LTE user facade public interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>

#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief LTE 用户门面句柄
 * @details LTE user facade handle
 */
typedef struct lwlte lwlte_t;

/**
 * @brief LTE 门面状态
 * @details LTE facade state
 */
typedef enum {
    LWLTE_STATE_STOPPED = 0,        /**< 已停止； Stopped */
    LWLTE_STATE_STARTING,           /**< 启动中； Starting */
    LWLTE_STATE_READY,              /**< 已就绪； Ready */
    LWLTE_STATE_NET_ACTIVATING,     /**< 网络激活中； Network activating */
    LWLTE_STATE_ONLINE,             /**< 网络在线； Online */
    LWLTE_STATE_ERROR,              /**< 错误； Error */
    LWLTE_STATE_DESTROYING,         /**< 销毁中； Destroying */
} lwlte_state_t;

/**
 * @brief LTE 网络状态
 * @details LTE network state
 */
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,    /**< 离线； Offline */
    LWLTE_NET_STATE_ACTIVATING,     /**< 激活中； Activating */
    LWLTE_NET_STATE_ONLINE,         /**< 在线； Online */
    LWLTE_NET_STATE_ERROR,          /**< 错误； Error */
} lwlte_net_state_t;

/**
 * @brief LTE 用户事件 ID
 * @details LTE user event ID
 */
typedef enum {
    LWLTE_EVENT_STARTED = 0,        /**< 已启动； Started */
    LWLTE_EVENT_READY,              /**< 已就绪； Ready */
    LWLTE_EVENT_NET_CONNECTING,     /**< 网络连接中； Network connecting */
    LWLTE_EVENT_NET_ONLINE,         /**< 网络在线； Network online */
    LWLTE_EVENT_NET_OFFLINE,        /**< 网络离线； Network offline */
    LWLTE_EVENT_NET_ERROR,          /**< 网络错误； Network error */
    LWLTE_EVENT_STOPPED,            /**< 已停止； Stopped */
    LWLTE_EVENT_ERROR,              /**< 错误； Error */
} lwlte_event_id_t;

/**
 * @brief LTE 用户事件数据
 * @details LTE user event data
 */
typedef struct {
    lwlte_net_state_t net_state;    /**< 网络状态； Network state */
    int error_code;                 /**< 错误码； Error code */
} lwlte_event_data_t;

/**
 * @brief LTE 用户事件回调
 * @details LTE user event callback
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据，可能为 NULL
 * @param[in] user_ctx 用户上下文
 */
typedef void (*lwlte_event_callback_t)(lwlte_t *lte,
                                       lwlte_event_id_t event_id,
                                       const lwlte_event_data_t *data,
                                       void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_destroy(lwlte_t *me);
esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx);
esp_err_t lwlte_connect(lwlte_t *me);
esp_err_t lwlte_disconnect(lwlte_t *me);
esp_err_t lwlte_get_state(lwlte_t *me, lwlte_state_t *state);
esp_err_t lwlte_get_net_state(lwlte_t *me, lwlte_net_state_t *state);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 2: Write `src/include/lwlte_air780ep.h`**

Create `src/include/lwlte_air780ep.h` with exactly:

```c
/**
 * @file lwlte_air780ep.h
 * @brief Air780EP LTE 用户门面公共接口
 * @details Air780EP LTE user facade public interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief Air780EP LTE 初始化配置
 * @details Air780EP LTE initialization configuration
 */
typedef struct {
    uart_port_t uart_num;                 /**< UART 端口号； UART port number */
    gpio_num_t uart_tx_pin;               /**< UART TX GPIO； UART TX GPIO */
    gpio_num_t uart_rx_pin;               /**< UART RX GPIO； UART RX GPIO */
    int uart_baud_rate;                   /**< UART 波特率； UART baud rate */
    gpio_num_t en_pin;                    /**< 模块 EN GPIO，GPIO_NUM_NC 表示不控制； Module EN GPIO, GPIO_NUM_NC disables control */
    gpio_num_t pwrkey_pin;                /**< PWRKEY GPIO； PWRKEY GPIO */
    gpio_num_t reset_pin;                 /**< RESET GPIO； RESET GPIO */
    gpio_num_t status_pin;                /**< STATUS GPIO； STATUS GPIO */
    const char *apn;                      /**< APN； APN */
    uint8_t primary_cid;                  /**< 主 PDP 上下文 ID； Primary PDP context ID */
    bool auto_connect;                    /**< ready 后是否自动提交联网请求； Whether to submit connect after ready */
    uint32_t init_ready_timeout_ms;        /**< 初始化等待 ready 超时； Ready wait timeout during init */
    uint32_t module_power_stable_ms;       /**< EN 拉高后电源稳定等待； Power stable wait after EN high */
    uint32_t net_activate_timeout_ms;      /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;           /**< 重连延迟； Reconnect delay */
    int at_rx_buf_size;                   /**< AT RX 缓冲大小； AT RX buffer size */
    int at_rx_task_stack;                 /**< AT RX 任务栈大小； AT RX task stack */
    int at_rx_task_priority;              /**< AT RX 任务优先级； AT RX task priority */
    int at_rx_line_buf_size;              /**< AT 单行缓冲大小； AT line buffer size */
    int at_cmd_default_timeout_ms;         /**< AT 默认命令超时； AT default command timeout */
    int at_max_response_lines;             /**< AT 最大响应行数； AT maximum response lines */
    uint32_t modem_power_on_pulse_ms;      /**< Modem 开机脉冲； Modem power-on pulse */
    uint32_t modem_reset_pulse_ms;         /**< Modem 复位脉冲； Modem reset pulse */
    uint32_t modem_boot_wait_ms;           /**< Modem 启动等待； Modem boot wait */
    uint32_t modem_default_cmd_timeout_ms; /**< Modem 默认命令超时； Modem default command timeout */
    int modem_event_queue_size;            /**< Modem 事件队列长度； Modem event queue size */
    int modem_event_task_stack;            /**< Modem 事件任务栈大小； Modem event task stack */
    int modem_event_task_priority;         /**< Modem 事件任务优先级； Modem event task priority */
    int core_fsm_queue_size;               /**< Core FSM 队列长度； Core FSM queue size */
    int core_fsm_task_stack;               /**< Core FSM 任务栈大小； Core FSM task stack */
    int core_fsm_task_priority;            /**< Core FSM 任务优先级； Core FSM task priority */
} lwlte_air780ep_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 3: Verify public headers are opaque**

Run:

```bash
rg 'struct lwlte \{' src/include || true
rg '#include "(core|modem|modem_air780ep|at_engine)\.h"' src/include || true
```

Expected result: no matches. Public headers forward-declare `lwlte_t` and do not include private project layer headers.

---

## Task 4: Implement Generic Facade Runtime

**Files:**
- Create: `src/lwlte/lwlte_priv.h`
- Create: `src/lwlte/lwlte.c`

- [ ] **Step 1: Write `src/lwlte/lwlte_priv.h`**

Create `src/lwlte/lwlte_priv.h` with exactly:

```c
/**
 * @file lwlte_priv.h
 * @brief LTE 用户门面内部接口
 * @details LTE user facade internal interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "at_engine.h"
#include "core.h"
#include "lwlte.h"
#include "modem.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

struct lwlte {
    at_engine_t *at;
    modem_t *modem;
    core_t *core;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t ready_sema;
    lwlte_event_callback_t event_callback;
    void *event_user_ctx;
    int init_error_code;
    bool ready;
    bool init_failed;
    bool destroying;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

esp_err_t lwlte_create_empty(lwlte_t **out_lte);
esp_err_t lwlte_wait_ready(lwlte_t *me, uint32_t timeout_ms);
void lwlte_core_event_bridge(core_t *core, core_event_id_t event_id,
                             const core_event_data_t *data, void *user_ctx);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 2: Write `src/lwlte/lwlte.c`**

Create `src/lwlte/lwlte.c` with exactly:

```c
/**
 * @file lwlte.c
 * @brief LTE 用户门面通用实现
 * @details LTE user facade common implementation
 * @author JovisDreams
 * @date 2026-05-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "lwlte_priv.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "lwlte"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lwlte_state_t map_core_state(core_state_t state);
static lwlte_net_state_t map_core_net_state(core_net_state_t state);
static lwlte_event_id_t map_core_event(core_event_id_t event_id);
static void map_core_event_data(const core_event_data_t *core_data,
                                lwlte_event_data_t *lwlte_data);
static esp_err_t destroy_owned_resources(lwlte_t *me);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_create_empty(lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(out_lte, ESP_ERR_INVALID_ARG, TAG, "out_lte is NULL");

    *out_lte = NULL;
    lwlte_t *me = calloc(1, sizeof(lwlte_t));
    ESP_RETURN_ON_FALSE(me, ESP_ERR_NO_MEM, TAG, "calloc lwlte failed");

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        free(me);
        return ESP_ERR_NO_MEM;
    }

    me->ready_sema = xSemaphoreCreateBinary();
    if (!me->ready_sema) {
        vSemaphoreDelete(me->lock);
        free(me);
        return ESP_ERR_NO_MEM;
    }

    *out_lte = me;
    return ESP_OK;
}

esp_err_t lwlte_destroy(lwlte_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    xSemaphoreGive(me->lock);

    esp_err_t ret = destroy_owned_resources(me);
    if (ret != ESP_OK) {
        return ret;
    }

    if (me->ready_sema) {
        vSemaphoreDelete(me->ready_sema);
        me->ready_sema = NULL;
    }
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
    free(me);

    return ESP_OK;
}

esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->event_callback = callback;
    me->event_user_ctx = callback ? user_ctx : NULL;
    xSemaphoreGive(me->lock);

    return ESP_OK;
}

esp_err_t lwlte_connect(lwlte_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->core, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    return core_connect(me->core);
}

esp_err_t lwlte_disconnect(lwlte_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->core, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    return core_disconnect(me->core);
}

esp_err_t lwlte_get_state(lwlte_t *me, lwlte_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->core && state,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    core_state_t core_state = CORE_STATE_STOPPED;
    ESP_RETURN_ON_ERROR(core_get_state(me->core, &core_state), TAG,
                        "get core state failed");
    *state = map_core_state(core_state);

    return ESP_OK;
}

esp_err_t lwlte_get_net_state(lwlte_t *me, lwlte_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->core && state,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    core_net_state_t core_state = CORE_NET_STATE_OFFLINE;
    ESP_RETURN_ON_ERROR(core_get_net_state(me->core, &core_state), TAG,
                        "get core net state failed");
    *state = map_core_net_state(core_state);

    return ESP_OK;
}

esp_err_t lwlte_wait_ready(lwlte_t *me, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(me && me->lock && me->ready_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    if (xSemaphoreTake(me->ready_sema, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool ready = me->ready;
    bool failed = me->init_failed;
    int error_code = me->init_error_code;
    xSemaphoreGive(me->lock);

    if (ready) {
        return ESP_OK;
    }
    if (failed) {
        return error_code ? (esp_err_t)error_code : ESP_FAIL;
    }

    return ESP_ERR_INVALID_STATE;
}

void lwlte_core_event_bridge(core_t *core, core_event_id_t event_id,
                             const core_event_data_t *data, void *user_ctx)
{
    (void)core;

    lwlte_t *me = (lwlte_t *)user_ctx;
    if (!me || !me->lock) {
        return;
    }

    lwlte_event_callback_t callback = NULL;
    void *callback_ctx = NULL;
    lwlte_event_data_t lwlte_data = {0};
    map_core_event_data(data, &lwlte_data);

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (event_id == CORE_EVENT_READY) {
        me->ready = true;
        xSemaphoreGive(me->ready_sema);
    } else if (event_id == CORE_EVENT_ERROR) {
        me->init_failed = true;
        me->init_error_code = data ? data->error_code : ESP_FAIL;
        xSemaphoreGive(me->ready_sema);
    }
    callback = me->event_callback;
    callback_ctx = me->event_user_ctx;
    xSemaphoreGive(me->lock);

    if (callback) {
        callback(me, map_core_event(event_id), &lwlte_data, callback_ctx);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static lwlte_state_t map_core_state(core_state_t state)
{
    switch (state) {
    case CORE_STATE_STOPPED:
        return LWLTE_STATE_STOPPED;
    case CORE_STATE_STARTING:
        return LWLTE_STATE_STARTING;
    case CORE_STATE_READY:
        return LWLTE_STATE_READY;
    case CORE_STATE_NET_ACTIVATING:
        return LWLTE_STATE_NET_ACTIVATING;
    case CORE_STATE_ONLINE:
        return LWLTE_STATE_ONLINE;
    case CORE_STATE_ERROR:
        return LWLTE_STATE_ERROR;
    case CORE_STATE_DESTROYING:
        return LWLTE_STATE_DESTROYING;
    default:
        return LWLTE_STATE_ERROR;
    }
}

static lwlte_net_state_t map_core_net_state(core_net_state_t state)
{
    switch (state) {
    case CORE_NET_STATE_OFFLINE:
        return LWLTE_NET_STATE_OFFLINE;
    case CORE_NET_STATE_ACTIVATING:
        return LWLTE_NET_STATE_ACTIVATING;
    case CORE_NET_STATE_ONLINE:
        return LWLTE_NET_STATE_ONLINE;
    case CORE_NET_STATE_ERROR:
        return LWLTE_NET_STATE_ERROR;
    default:
        return LWLTE_NET_STATE_ERROR;
    }
}

static lwlte_event_id_t map_core_event(core_event_id_t event_id)
{
    switch (event_id) {
    case CORE_EVENT_STARTED:
        return LWLTE_EVENT_STARTED;
    case CORE_EVENT_READY:
        return LWLTE_EVENT_READY;
    case CORE_EVENT_NET_CONNECTING:
        return LWLTE_EVENT_NET_CONNECTING;
    case CORE_EVENT_NET_ONLINE:
        return LWLTE_EVENT_NET_ONLINE;
    case CORE_EVENT_NET_OFFLINE:
        return LWLTE_EVENT_NET_OFFLINE;
    case CORE_EVENT_NET_ERROR:
        return LWLTE_EVENT_NET_ERROR;
    case CORE_EVENT_STOPPED:
        return LWLTE_EVENT_STOPPED;
    case CORE_EVENT_ERROR:
        return LWLTE_EVENT_ERROR;
    default:
        return LWLTE_EVENT_ERROR;
    }
}

static void map_core_event_data(const core_event_data_t *core_data,
                                lwlte_event_data_t *lwlte_data)
{
    if (!lwlte_data) {
        return;
    }
    if (!core_data) {
        lwlte_data->net_state = LWLTE_NET_STATE_OFFLINE;
        lwlte_data->error_code = 0;
        return;
    }

    lwlte_data->net_state = map_core_net_state(core_data->net_state);
    lwlte_data->error_code = core_data->error_code;
}

static esp_err_t destroy_owned_resources(lwlte_t *me)
{
    esp_err_t ret = ESP_OK;

    if (me->core) {
        ret = core_destroy(me->core);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy core failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->core = NULL;
    }

    if (me->modem) {
        ret = modem_destroy(me->modem);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy modem failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->modem = NULL;
    }

    if (me->at) {
        ret = at_engine_destroy(me->at);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy AT engine failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->at = NULL;
    }

    return ESP_OK;
}
```

- [ ] **Step 3: Build to expose factory gaps**

Run:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build fails because `src/lwlte/lwlte_air780ep.c` is not implemented yet and the example still uses old lower-layer APIs.

---

## Task 5: Implement Air780EP Facade Factory

**Files:**
- Create: `src/lwlte/lwlte_air780ep.c`

- [ ] **Step 1: Write `src/lwlte/lwlte_air780ep.c`**

Create `src/lwlte/lwlte_air780ep.c` with exactly:

```c
/**
 * @file lwlte_air780ep.c
 * @brief Air780EP LTE 用户门面工厂实现
 * @details Air780EP LTE user facade factory implementation
 * @author JovisDreams
 * @date 2026-05-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "lwlte_air780ep.h"
#include "lwlte_priv.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "at_engine.h"
#include "core.h"
#include "modem_air780ep.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "lwlte_air780ep"
#define LWLTE_AIR780EP_DEFAULT_READY_TIMEOUT_MS  30000

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static esp_err_t validate_config(const lwlte_air780ep_config_t *config,
                                 lwlte_t **out_lte);
static esp_err_t init_enable_pin(const lwlte_air780ep_config_t *config);
static at_engine_t *create_at_engine(const lwlte_air780ep_config_t *config);
static modem_t *create_modem(at_engine_t *at,
                             const lwlte_air780ep_config_t *config);
static core_t *create_core(modem_t *modem,
                           const lwlte_air780ep_config_t *config);
static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte)
{
    ESP_RETURN_ON_ERROR(validate_config(config, out_lte), TAG,
                        "invalid config");

    *out_lte = NULL;
    lwlte_t *me = NULL;
    esp_err_t ret = lwlte_create_empty(&me);
    ESP_RETURN_ON_ERROR(ret, TAG, "create lwlte runtime failed");

    ret = init_enable_pin(config);
    if (ret != ESP_OK) {
        goto err;
    }

    me->at = create_at_engine(config);
    if (!me->at) {
        ret = ESP_FAIL;
        goto err;
    }

    me->modem = create_modem(me->at, config);
    if (!me->modem) {
        ret = ESP_FAIL;
        goto err;
    }

    ret = modem_init(me->modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "modem init failed: %s", esp_err_to_name(ret));
        goto err;
    }

    me->core = create_core(me->modem, config);
    if (!me->core) {
        ret = ESP_FAIL;
        goto err;
    }

    ret = core_register_event_callback(me->core, lwlte_core_event_bridge, me);
    if (ret != ESP_OK) {
        goto err;
    }

    ret = core_start(me->core);
    if (ret != ESP_OK) {
        goto err;
    }

    ret = lwlte_wait_ready(me, ready_timeout_ms(config));
    if (ret != ESP_OK) {
        goto err;
    }

    if (config->auto_connect) {
        ret = lwlte_connect(me);
        if (ret != ESP_OK) {
            goto err;
        }
    }

    *out_lte = me;
    return ESP_OK;

err:
    if (me) {
        esp_err_t destroy_ret = lwlte_destroy(me);
        if (destroy_ret != ESP_OK) {
            ESP_LOGW(TAG, "cleanup after init failure failed: %s",
                     esp_err_to_name(destroy_ret));
        }
    }
    return ret;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static esp_err_t validate_config(const lwlte_air780ep_config_t *config,
                                 lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(config && out_lte, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(config->uart_num >= UART_NUM_0 &&
                        config->uart_num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART number");
    ESP_RETURN_ON_FALSE(config->uart_tx_pin >= 0 && config->uart_rx_pin >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(config->uart_baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART baud rate");
    ESP_RETURN_ON_FALSE(config->primary_cid > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid primary cid");

    return ESP_OK;
}

static esp_err_t init_enable_pin(const lwlte_air780ep_config_t *config)
{
    if (config->en_pin == GPIO_NUM_NC) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_reset_pin(config->en_pin), TAG,
                        "reset EN pin failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(config->en_pin, GPIO_MODE_OUTPUT), TAG,
                        "set EN direction failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(config->en_pin, 1), TAG,
                        "set EN high failed");

    if (config->module_power_stable_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(config->module_power_stable_ms));
    }

    return ESP_OK;
}

static at_engine_t *create_at_engine(const lwlte_air780ep_config_t *config)
{
    const at_engine_config_t at_config = {
        .uart_num = config->uart_num,
        .tx_pin = config->uart_tx_pin,
        .rx_pin = config->uart_rx_pin,
        .baud_rate = config->uart_baud_rate,
        .rx_buf_size = config->at_rx_buf_size,
        .rx_task_stack = config->at_rx_task_stack,
        .rx_task_priority = config->at_rx_task_priority,
        .rx_line_buf_size = config->at_rx_line_buf_size,
        .cmd_default_timeout_ms = config->at_cmd_default_timeout_ms,
        .max_response_lines = config->at_max_response_lines,
    };

    return at_engine_create(&at_config);
}

static modem_t *create_modem(at_engine_t *at,
                             const lwlte_air780ep_config_t *config)
{
    const modem_air780ep_config_t modem_config = {
        .pwrkey_pin = config->pwrkey_pin,
        .reset_pin = config->reset_pin,
        .status_pin = config->status_pin,
        .power_on_pulse_ms = config->modem_power_on_pulse_ms,
        .reset_pulse_ms = config->modem_reset_pulse_ms,
        .boot_wait_ms = config->modem_boot_wait_ms,
        .default_cmd_timeout_ms = config->modem_default_cmd_timeout_ms,
        .event_queue_size = config->modem_event_queue_size,
        .event_task_stack = config->modem_event_task_stack,
        .event_task_priority = config->modem_event_task_priority,
    };

    return modem_air780ep_create(at, &modem_config);
}

static core_t *create_core(modem_t *modem,
                           const lwlte_air780ep_config_t *config)
{
    const core_config_t core_config = {
        .apn = config->apn,
        .primary_cid = config->primary_cid,
        .net_activate_timeout_ms = config->net_activate_timeout_ms,
        .reconnect_delay_ms = config->reconnect_delay_ms,
        .auto_connect = false,
        .fsm_queue_size = config->core_fsm_queue_size,
        .fsm_task_stack = config->core_fsm_task_stack,
        .fsm_task_priority = config->core_fsm_task_priority,
    };

    return core_create(&core_config, modem);
}

static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config)
{
    if (config->init_ready_timeout_ms > 0) {
        return config->init_ready_timeout_ms;
    }
    return LWLTE_AIR780EP_DEFAULT_READY_TIMEOUT_MS;
}
```

- [ ] **Step 2: Build to expose example gaps**

Run:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build fails only because `examples/basic_connect/main/main.c` still includes old lower-layer headers and uses old APIs.

---

## Task 6: Convert Basic Connect To User-Layer Example

**Files:**
- Modify: `examples/basic_connect/main/main.c`
- Modify: `examples/basic_connect/main/CMakeLists.txt`

- [ ] **Step 1: Replace the example main component CMake**

Replace `examples/basic_connect/main/CMakeLists.txt` with:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
```

- [ ] **Step 2: Rewrite `examples/basic_connect/main/main.c` as a user-layer example**

Replace the entire file with:

```c
/**
 * @file main.c
 * @brief LTE 基础连接示例
 * @details LTE basic connection example
 * @author JovisDreams
 * @date 2026-05-25
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwlte.h"
#include "lwlte_air780ep.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "basic_connect"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1

#define EXAMPLE_MODULE_POWER_STABLE_MS       3000
#define EXAMPLE_INIT_READY_TIMEOUT_MS        30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS        120000
#define EXAMPLE_POLL_INTERVAL_MS             100
#define EXAMPLE_STATUS_LOG_INTERVAL_MS       5000

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    volatile bool started;
    volatile bool ready;
    volatile bool connecting;
    volatile bool online;
    volatile bool offline;
    volatile bool stopped;
    volatile bool error;
    volatile int error_code;
} example_flags_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx);
static void log_lte_status(lwlte_t *lte, const char *stage);
static void idle_forever(void);
static const char *lwlte_state_name(lwlte_state_t state);
static const char *lwlte_net_state_name(lwlte_net_state_t state);
static const char *lwlte_event_name(lwlte_event_id_t event_id);

/**********************
 *  STATIC VARIABLES
 **********************/
static example_flags_t s_flags;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    lwlte_t *lte = NULL;

    ESP_LOGI(TAG, "esp-lwlte basic connect example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);

    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .pwrkey_pin = GPIO_NUM_NC,
        .reset_pin = GPIO_NUM_NC,
        .status_pin = GPIO_NUM_NC,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .auto_connect = false,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .module_power_stable_ms = EXAMPLE_MODULE_POWER_STABLE_MS,
    };

    esp_err_t ret = lwlte_air780ep_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init LTE failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    ret = lwlte_register_event_callback(lte, lte_event_cb, &s_flags);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register LTE callback failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    ret = lwlte_connect(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "connect request failed: %s", esp_err_to_name(ret));
        log_lte_status(lte, "connect request failed");
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    uint32_t elapsed_ms = 0;
    while (!s_flags.online && !s_flags.error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (s_flags.online) {
        ESP_LOGI(TAG, "LTE network is online");
    } else {
        ESP_LOGW(TAG, "LTE network did not become online, error=%d",
                 s_flags.error_code);
        log_lte_status(lte, "network wait ended");
    }

    while (1) {
        log_lte_status(lte, "periodic");
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_LOG_INTERVAL_MS));
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx)
{
    (void)lte;

    example_flags_t *flags = (example_flags_t *)user_ctx;
    if (!flags) {
        return;
    }

    if (data) {
        ESP_LOGI(TAG, "LTE event: %s net=%s err=%d",
                 lwlte_event_name(event_id), lwlte_net_state_name(data->net_state),
                 data->error_code);
    } else {
        ESP_LOGI(TAG, "LTE event: %s", lwlte_event_name(event_id));
    }

    switch (event_id) {
    case LWLTE_EVENT_STARTED:
        flags->started = true;
        break;
    case LWLTE_EVENT_READY:
        flags->ready = true;
        break;
    case LWLTE_EVENT_NET_CONNECTING:
        flags->connecting = true;
        break;
    case LWLTE_EVENT_NET_ONLINE:
        flags->online = true;
        flags->error = false;
        flags->error_code = 0;
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        flags->offline = true;
        flags->online = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        flags->error_code = data ? data->error_code : ESP_FAIL;
        flags->error = true;
        break;
    case LWLTE_EVENT_STOPPED:
        flags->stopped = true;
        break;
    default:
        break;
    }
}

static void log_lte_status(lwlte_t *lte, const char *stage)
{
    if (!lte) {
        ESP_LOGW(TAG, "%s: LTE is NULL", stage);
        return;
    }

    lwlte_state_t state = LWLTE_STATE_STOPPED;
    lwlte_net_state_t net_state = LWLTE_NET_STATE_OFFLINE;
    esp_err_t state_ret = lwlte_get_state(lte, &state);
    esp_err_t net_ret = lwlte_get_net_state(lte, &net_state);

    if (state_ret != ESP_OK || net_ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: get state failed state_ret=%s net_ret=%s", stage,
                 esp_err_to_name(state_ret), esp_err_to_name(net_ret));
        return;
    }

    ESP_LOGI(TAG, "%s: state=%s net=%s", stage, lwlte_state_name(state),
             lwlte_net_state_name(net_state));
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_LOG_INTERVAL_MS));
    }
}

static const char *lwlte_state_name(lwlte_state_t state)
{
    switch (state) {
    case LWLTE_STATE_STOPPED:
        return "stopped";
    case LWLTE_STATE_STARTING:
        return "starting";
    case LWLTE_STATE_READY:
        return "ready";
    case LWLTE_STATE_NET_ACTIVATING:
        return "net_activating";
    case LWLTE_STATE_ONLINE:
        return "online";
    case LWLTE_STATE_ERROR:
        return "error";
    case LWLTE_STATE_DESTROYING:
        return "destroying";
    default:
        return "unknown";
    }
}

static const char *lwlte_net_state_name(lwlte_net_state_t state)
{
    switch (state) {
    case LWLTE_NET_STATE_OFFLINE:
        return "offline";
    case LWLTE_NET_STATE_ACTIVATING:
        return "activating";
    case LWLTE_NET_STATE_ONLINE:
        return "online";
    case LWLTE_NET_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static const char *lwlte_event_name(lwlte_event_id_t event_id)
{
    switch (event_id) {
    case LWLTE_EVENT_STARTED:
        return "started";
    case LWLTE_EVENT_READY:
        return "ready";
    case LWLTE_EVENT_NET_CONNECTING:
        return "net_connecting";
    case LWLTE_EVENT_NET_ONLINE:
        return "net_online";
    case LWLTE_EVENT_NET_OFFLINE:
        return "net_offline";
    case LWLTE_EVENT_NET_ERROR:
        return "net_error";
    case LWLTE_EVENT_STOPPED:
        return "stopped";
    case LWLTE_EVENT_ERROR:
        return "error";
    default:
        return "unknown";
    }
}
```

- [ ] **Step 3: Verify example includes only public esp-lwlte headers**

Run:

```bash
rg '#include "(at_engine|modem|modem_air780ep|core|lwlte_core)\.h"' examples/basic_connect || true
```

Expected result: no matches.

---

## Task 7: Update Agent Documentation To Match The New Boundary

**Files:**
- Modify: `docs/agents/directory-structure.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/classes.md`

- [ ] **Step 1: Update `docs/agents/directory-structure.md`**

Change the `src/` structure block to describe:

```text
src/
├── include/       # 用户公共头文件，仅导出 lwlte.h、lwlte_air780ep.h
├── lwlte/         # 用户门面层（lwlte_t、模块 factory、资源组合根）
├── core/          # Core Service 层（网络状态机、PDP 管理、连接/重连）
├── modem/         # 模块适配层（modem_t 抽象 + 具体模块实现）
└── at_engine/     # AT 引擎层（通用 AT 协议引擎 + UART 硬件操作）
```

Add this rule below the block:

```text
`src/include/` 只放真正给 App include 的用户 API。`core.h`、`modem.h`、`modem_air780ep.h`、`at_engine.h` 放在各自模块目录，通过组件 `PRIV_INCLUDE_DIRS` 给内部源码使用，不导出给用户 App。
```

- [ ] **Step 2: Update `docs/agents/architecture.md` layer model**

Replace the old four-layer App/Core/Modem/AT summary with this model:

```text
App
  ↓ 只依赖 src/include/lwlte*.h
LWLTE Facade
  ↓ 调用 Core/MQTT/TCP/HTTP 等 service API，并在模块 factory 中完成装配
Service Layer: Core, future MQTT, future TCP, future HTTP
  ↓
Modem Adapter
  ↓
AT Engine
```

Add these rules in the call-rule section:

```text
- App 只能 include `lwlte.h`、`lwlte_air780ep.h` 等用户公共头。
- Facade 的通用文件只应调用 service 层 API。
- Facade 的模块 factory 文件是 composition root，允许认识 AT Engine、Modem、具体 Modem factory 和 Core，用于创建并持有完整依赖树。
- Service 层仍只能向下调用紧邻的 Modem Adapter，不能直接调用 AT Engine。
```

- [ ] **Step 3: Update `docs/agents/classes.md` visibility table**

Replace the visibility table with:

```text
| 可见性 | 落入哪个头文件 | 谁能看到 | 命名前缀 |
|--------|-------------|---------|---------|
| 用户 API | `src/include/lwlte*.h` | App 开发者 | `lwlte_` |
| 层间 API | `src/core/core.h`、`src/modem/modem.h`、`src/modem/modem_air780ep.h`、`src/at_engine/at_engine.h` | 组件内部相邻层；Facade factory 作为 composition root 可见全部装配 API | `core_`、`modem_`、`modem_air780ep_`、`at_engine_` |
| 模块私有 API | `*_priv.h` | 当前模块自己的 `.c` 文件 | 模块内部命名 |
| 文件内部 | `.c` 中 static | 当前 `.c` 文件 | 无限制 |
```

Add this note:

```text
`*_priv.h` 虽然通过 `PRIV_INCLUDE_DIRS` 在编译上可见，但约束上只允许同模块源码 include。Core 不 include `modem_priv.h`，Modem 不 include `core_priv.h`，Facade 不 include 任意 `_priv.h`。
```

- [ ] **Step 4: Update Core class descriptions**

In `docs/agents/classes.md`, describe Core classes as:

```text
Core Service
├── core_t        层间 API opaque 句柄，Facade 持有并调用
├── core_fsm_t    Core 内部组件，属于 core_t，负责串行处理 Core 信号
├── net_mgr_t     Core 内部组件，属于 core_t，负责网络激活和重连策略
└── pdp_mgr_t     Core 内部组件，属于 core_t，负责 PDP 上下文状态缓存
```

State explicitly:

```text
`net_mgr_t`、`pdp_mgr_t`、`core_fsm_t` 不是 `core_t` 子类。它们不能向上转型为 `core_t *`，也不实现 `core_ops`；它们是 `core_t` 的组合成员。`modem_air780ep_t` 才是 `modem_t` 的子类，因为它以 `modem_t base` 为第一个成员并实现 `modem_ops`。
```

- [ ] **Step 5: Verify docs no longer call Core user API**

Run:

```bash
rg 'lwlte_core|LWLTE_CORE|用户 API.*core|include/lwlte_core\.h' docs/agents || true
```

Expected result: no matches that describe Core as user API. Historical references are allowed only if they explicitly say the old boundary was replaced.

---

## Task 8: Final Verification

**Files:**
- Verify: all touched files

- [ ] **Step 1: Build with ESP-IDF MCP**

Run the ESP-IDF build tool from the example project context:

```bash
idf.py -C examples/basic_connect build
```

Expected result: build succeeds.

- [ ] **Step 2: Verify public include directory contents**

Run:

```bash
ls src/include
```

Expected result includes only:

```text
lwlte.h
lwlte_air780ep.h
```

- [ ] **Step 3: Verify examples do not include private layer headers**

Run:

```bash
rg '#include "(at_engine|modem|modem_air780ep|core|lwlte_core)\.h"' examples main src/include || true
```

Expected result: no matches.

- [ ] **Step 4: Verify Core demotion is complete**

Run:

```bash
rg 'lwlte_core|LWLTE_CORE|LWLTE_NET_STATE|LWLTE_CORE_STATE' src examples main || true
```

Expected result: no matches.

- [ ] **Step 5: Verify private header include rules**

Run:

```bash
rg '#include "(core_priv|modem_priv)\.h"' src
```

Expected result:

```text
src/core/core.c:#include "core_priv.h"
src/core/core_fsm.c:#include "core_priv.h"
src/core/net_mgr.c:#include "core_priv.h"
src/core/pdp_mgr.c:#include "core_priv.h"
src/modem/modem.c:#include "modem_priv.h"
src/modem/modem_air780ep.c:#include "modem_priv.h"
```

- [ ] **Step 6: Check working tree**

Run:

```bash
git status --short --untracked-files=all
```

Expected result: only intentional source, example, and documentation files are changed or added. Do not commit unless the user explicitly authorizes a commit.

---

## Self-Review Notes

- Spec coverage: public `lwlte_t` facade, Air780EP factory, blocking ready init, generic destroy, private layer headers, Core demotion, basic example conversion, and documentation updates are all covered by tasks.
- Type consistency: public API uses `lwlte_t *`; Core service API uses `core_t *`; lower layers keep `modem_t *` and `at_engine_t *` as component-private handles.
- Boundary consistency: `src/include` exports only `lwlte.h` and `lwlte_air780ep.h`; all layer headers move to module directories and are included through `PRIV_INCLUDE_DIRS`.
- OOP consistency: `modem_air780ep_t` remains a `modem_t` subclass; `core_fsm_t`, `net_mgr_t`, and `pdp_mgr_t` are composition members of `core_t`.
