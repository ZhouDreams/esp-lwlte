# Core Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Core Service MVP described in `docs/superpowers/specs/2026-05-24-core-module-design.md` and `docs/agents/classes.md` section 3.

**Architecture:** Add a focused Core layer split across public API, private shared definitions, FSM, network manager, and PDP manager files. Core owns the event loop and FSM resources, borrows `modem_t *`, calls only `modem_*` APIs, and publishes App-facing events through `esp_event`.

**Tech Stack:** C, ESP-IDF, FreeRTOS task/queue/timer/semaphore APIs, `esp_event`, existing Modem public API.

---

## Commit Policy

Do not run `git commit` during execution unless the user explicitly authorizes commits. After each task, use `git status --short` to inspect the working tree.

## File Structure

- Create `src/include/lwlte_core.h`: public Core API, opaque handle, config, state enums, event declarations, callback signature.
- Create `src/core/core_priv.h`: internal Core object, FSM signal types, submodule structs, and internal function declarations.
- Create `src/core/pdp_mgr.c`: fixed-size PDP context cache helpers.
- Create `src/core/net_mgr.c`: synchronous network activation flow and reconnect timer.
- Create `src/core/core_fsm.c`: Core FSM task, signal queue, and signal handlers.
- Create `src/core/core.c`: public API, event loop, Core lifecycle, state helpers, callback adapters.
- Modify `src/CMakeLists.txt`: add Core source files and `esp_event` dependency.

No unit-test harness exists in this repository and the approved spec excludes introducing one. Verification is compile-driven plus boundary checks.

---

### Task 1: Public Core Header

**Files:**
- Create: `src/include/lwlte_core.h`
- Reference: `src/include/modem.h`
- Reference: `docs/agents/coding-style.md`

- [ ] **Step 1: Create the public header**

Create `src/include/lwlte_core.h` with this structure and API. Keep the Doxygen style consistent with `modem.h`.

```c
/**
 * @file lwlte_core.h
 * @brief LTE 核心服务公共接口
 * @details LTE core service public interface
 * @author JovisDreams
 * @date 2026-05-24
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

typedef struct lwlte_core lwlte_core_t;
typedef struct modem modem_t;

typedef struct {
    const char *apn;                     /**< APN； APN */
    uint8_t primary_cid;                 /**< 主 PDP 上下文 ID； Primary PDP context ID */
    uint32_t net_activate_timeout_ms;    /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;         /**< 重连延迟； Reconnect delay */
    bool auto_connect;                   /**< 是否自动联网； Whether to connect automatically */
    int fsm_queue_size;                  /**< FSM 队列长度； FSM queue size */
    int fsm_task_stack;                  /**< FSM 任务栈大小； FSM task stack size */
    int fsm_task_priority;               /**< FSM 任务优先级； FSM task priority */
} lwlte_core_config_t;

typedef enum {
    LWLTE_CORE_STATE_STOPPED = 0,        /**< 已停止； Stopped */
    LWLTE_CORE_STATE_STARTING,           /**< 启动中； Starting */
    LWLTE_CORE_STATE_READY,              /**< 已就绪； Ready */
    LWLTE_CORE_STATE_NET_ACTIVATING,     /**< 网络激活中； Network activating */
    LWLTE_CORE_STATE_ONLINE,             /**< 网络在线； Online */
    LWLTE_CORE_STATE_ERROR,              /**< 错误； Error */
    LWLTE_CORE_STATE_DESTROYING,         /**< 销毁中； Destroying */
} lwlte_core_state_t;

typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,         /**< 离线； Offline */
    LWLTE_NET_STATE_ACTIVATING,          /**< 激活中； Activating */
    LWLTE_NET_STATE_ONLINE,              /**< 在线； Online */
    LWLTE_NET_STATE_ERROR,               /**< 错误； Error */
} lwlte_net_state_t;

ESP_EVENT_DECLARE_BASE(LWLTE_CORE_EVENT);

typedef enum {
    LWLTE_CORE_EVENT_STARTED = 0,        /**< Core 已启动； Core started */
    LWLTE_CORE_EVENT_READY,              /**< Core 已就绪； Core ready */
    LWLTE_CORE_EVENT_NET_CONNECTING,     /**< 网络连接中； Network connecting */
    LWLTE_CORE_EVENT_NET_ONLINE,         /**< 网络在线； Network online */
    LWLTE_CORE_EVENT_NET_OFFLINE,        /**< 网络离线； Network offline */
    LWLTE_CORE_EVENT_NET_ERROR,          /**< 网络错误； Network error */
    LWLTE_CORE_EVENT_STOPPED,            /**< Core 已停止； Core stopped */
    LWLTE_CORE_EVENT_ERROR,              /**< Core 错误； Core error */
} lwlte_core_event_id_t;

typedef struct {
    lwlte_net_state_t net_state;         /**< 网络状态； Network state */
    int error_code;                      /**< 错误码； Error code */
} lwlte_core_event_data_t;

typedef void (*lwlte_core_event_callback_t)(lwlte_core_t *core,
                                             lwlte_core_event_id_t event_id,
                                             const lwlte_core_event_data_t *data,
                                             void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

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

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 2: Verify the header has no forbidden dependency**

Run: `rg '#include "(at_engine|modem_air780ep|modem)\.h"' src/include/lwlte_core.h`

Expected: no matches.

- [ ] **Step 3: Inspect status**

Run: `git status --short`

Expected: `?? src/include/lwlte_core.h` appears.

---

### Task 2: Internal Core Header

**Files:**
- Create: `src/core/core_priv.h`
- Reference: `src/modem/modem_priv.h`
- Reference: `docs/agents/classes.md:932-1033`

- [ ] **Step 1: Create private definitions**

Create `src/core/core_priv.h` with the internal structs and function declarations below.

```c
/**
 * @file core_priv.h
 * @brief LTE 核心服务内部接口
 * @details LTE core service internal interface
 * @author JovisDreams
 * @date 2026-05-24
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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "lwlte_core.h"
#include "modem.h"

/*********************
 *      DEFINES
 *********************/
#define CORE_MAX_PDP_CONTEXTS                 4
#define CORE_DEFAULT_PRIMARY_CID              1
#define CORE_DEFAULT_NET_ACTIVATE_TIMEOUT_MS  120000
#define CORE_DEFAULT_RECONNECT_DELAY_MS       5000
#define CORE_DEFAULT_FSM_QUEUE_SIZE           16
#define CORE_DEFAULT_FSM_TASK_STACK           4096
#define CORE_DEFAULT_FSM_TASK_PRIORITY        8
#define CORE_EVENT_QUEUE_SIZE                 16
#define CORE_EVENT_TASK_STACK                 4096
#define CORE_EVENT_TASK_PRIORITY              8
#define CORE_FSM_WAIT_MS                      100
#define CORE_NET_MAX_RETRY                    3

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    CORE_SIG_MODEM_EVENT = 0,
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
    CORE_SIG_NET_DEACTIVATE,
    CORE_SIG_NET_STEP_DONE,
    CORE_SIG_NET_STEP_TIMEOUT,
    CORE_SIG_RECONNECT,
} core_fsm_sig_type_t;

typedef struct {
    core_fsm_sig_type_t type;
    modem_event_t modem_event;
    int error_code;
} core_fsm_sig_t;

typedef struct {
    TaskHandle_t task;
    QueueHandle_t queue;
    SemaphoreHandle_t task_done_sema;
    bool running;
    bool stop_requested;
} core_fsm_t;

typedef enum {
    NET_STEP_IDLE = 0,
    NET_STEP_CHECK_SIM,
    NET_STEP_CHECK_SIGNAL,
    NET_STEP_CHECK_REGISTRATION,
    NET_STEP_SET_APN,
    NET_STEP_ACTIVATE_PDP,
    NET_STEP_DONE,
    NET_STEP_ERROR,
} net_mgr_step_t;

typedef struct {
    net_mgr_step_t current_step;
    uint32_t step_start_time_ms;
    uint32_t step_timeout_ms;
    int retry_count;
    int max_retry;
    TimerHandle_t reconnect_timer;
    lwlte_net_state_t state;
    bool reconnect_enabled;
} net_mgr_t;

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t primary_cid;
} pdp_mgr_t;

struct lwlte_core {
    lwlte_core_config_t config;
    modem_t *modem;
    esp_event_loop_handle_t event_loop;
    core_fsm_t fsm;
    net_mgr_t net_mgr;
    pdp_mgr_t pdp_mgr;
    lwlte_core_state_t state;
    bool destroying;
    SemaphoreHandle_t lock;
    lwlte_core_event_callback_t event_callback;
    void *event_user_ctx;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/
esp_err_t core_fsm_init(lwlte_core_t *me);
void core_fsm_deinit(lwlte_core_t *me);
esp_err_t core_fsm_send(lwlte_core_t *me, const core_fsm_sig_t *sig);
bool core_fsm_is_task(lwlte_core_t *me);

esp_err_t net_mgr_init(lwlte_core_t *me);
void net_mgr_deinit(lwlte_core_t *me);
void net_mgr_cancel_reconnect(lwlte_core_t *me);
esp_err_t net_mgr_start_activation(lwlte_core_t *me);
esp_err_t net_mgr_deactivate(lwlte_core_t *me);
esp_err_t net_mgr_handle_pdp_activated(lwlte_core_t *me,
                                       const modem_pdp_context_t *pdp);
esp_err_t net_mgr_handle_pdp_deactivated(lwlte_core_t *me,
                                         const modem_pdp_context_t *pdp);
esp_err_t net_mgr_get_state(lwlte_core_t *me, lwlte_net_state_t *state);
esp_err_t net_mgr_set_state(lwlte_core_t *me, lwlte_net_state_t state);
void net_mgr_set_reconnect_enabled(lwlte_core_t *me, bool enabled);

esp_err_t pdp_mgr_init(pdp_mgr_t *me, uint8_t primary_cid);
esp_err_t pdp_mgr_get(const pdp_mgr_t *me, uint8_t cid,
                      modem_pdp_context_t *pdp);
esp_err_t pdp_mgr_update(pdp_mgr_t *me, const modem_pdp_context_t *pdp);
esp_err_t pdp_mgr_set_active(pdp_mgr_t *me, uint8_t cid, bool active);

esp_err_t core_set_state(lwlte_core_t *me, lwlte_core_state_t state);
lwlte_core_state_t core_get_state_value(lwlte_core_t *me);
bool core_is_destroying(lwlte_core_t *me);
esp_err_t core_post_event(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                          const lwlte_core_event_data_t *data);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 2: Verify private header only depends on public Modem API**

Run: `rg '#include "(at_engine|modem_air780ep|modem_priv)\.h"' src/core/core_priv.h`

Expected: no matches.

- [ ] **Step 3: Inspect status**

Run: `git status --short`

Expected: `?? src/core/core_priv.h` appears.

---

### Task 3: PDP Manager

**Files:**
- Create: `src/core/pdp_mgr.c`
- Depends on: `src/core/core_priv.h`

- [ ] **Step 1: Create the PDP cache implementation**

Create `src/core/pdp_mgr.c`. Implement only cache behavior; do not call Modem APIs from this file.

```c
/**
 * @file pdp_mgr.c
 * @brief PDP 上下文缓存管理
 * @details PDP context cache management
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include <string.h>

#include "esp_check.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "pdp_mgr"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool cid_valid(uint8_t cid);
static int cid_index(uint8_t cid);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
esp_err_t pdp_mgr_init(pdp_mgr_t *me, uint8_t primary_cid)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(primary_cid), ESP_ERR_INVALID_ARG, TAG,
                        "invalid primary cid");

    memset(me, 0, sizeof(*me));
    me->primary_cid = primary_cid;
    for (uint8_t cid = 1; cid <= CORE_MAX_PDP_CONTEXTS; cid++) {
        me->contexts[cid - 1].cid = cid;
    }
    return ESP_OK;
}

esp_err_t pdp_mgr_get(const pdp_mgr_t *me, uint8_t cid,
                      modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    *pdp = me->contexts[cid_index(cid)];
    return ESP_OK;
}

esp_err_t pdp_mgr_update(pdp_mgr_t *me, const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(cid_valid(pdp->cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    me->contexts[cid_index(pdp->cid)] = *pdp;
    return ESP_OK;
}

esp_err_t pdp_mgr_set_active(pdp_mgr_t *me, uint8_t cid, bool active)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cid_valid(cid), ESP_ERR_INVALID_ARG, TAG, "invalid cid");

    me->contexts[cid_index(cid)].cid = cid;
    me->contexts[cid_index(cid)].active = active;
    return ESP_OK;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool cid_valid(uint8_t cid)
{
    return cid >= 1 && cid <= CORE_MAX_PDP_CONTEXTS;
}

static int cid_index(uint8_t cid)
{
    return (int)cid - 1;
}
```

- [ ] **Step 2: Verify there are no lower-layer calls**

Run: `rg 'modem_|at_engine|air780ep' src/core/pdp_mgr.c`

Expected: matches only type names such as `modem_pdp_context_t`, and no `modem_*(` function calls.

- [ ] **Step 3: Inspect status**

Run: `git status --short`

Expected: `?? src/core/pdp_mgr.c` appears.

---

### Task 4: Network Manager

**Files:**
- Create: `src/core/net_mgr.c`
- Depends on: `src/core/core_priv.h`
- Uses: `modem_get_sim_status`, `modem_get_signal`, `modem_get_registration`, `modem_set_apn`, `modem_activate_pdp`, `modem_deactivate_pdp`, `modem_get_pdp_context`

- [ ] **Step 1: Create network manager source skeleton**

Create `src/core/net_mgr.c` with these includes, constants, and static prototypes.

```c
/**
 * @file net_mgr.c
 * @brief LTE 网络状态管理
 * @details LTE network state management
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/timers.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "net_mgr"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void reconnect_timer_cb(TimerHandle_t timer);
static esp_err_t run_activation_once(lwlte_core_t *me);
static esp_err_t fail_activation(lwlte_core_t *me, esp_err_t err);
static bool registration_ready(modem_reg_status_t status);
static bool is_primary_pdp(lwlte_core_t *me, const modem_pdp_context_t *pdp);
static void post_net_state(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                           lwlte_net_state_t net_state, int error_code);
```

- [ ] **Step 2: Implement initialization and teardown**

Add these global functions after the static prototypes.

```c
esp_err_t net_mgr_init(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->net_mgr.current_step = NET_STEP_IDLE;
    me->net_mgr.step_start_time_ms = 0;
    me->net_mgr.step_timeout_ms = me->config.net_activate_timeout_ms;
    me->net_mgr.retry_count = 0;
    me->net_mgr.max_retry = CORE_NET_MAX_RETRY;
    me->net_mgr.state = LWLTE_NET_STATE_OFFLINE;
    me->net_mgr.reconnect_enabled = false;
    me->net_mgr.reconnect_timer = xTimerCreate("core_reconn",
                                               pdMS_TO_TICKS(me->config.reconnect_delay_ms),
                                               pdFALSE, me, reconnect_timer_cb);
    ESP_RETURN_ON_FALSE(me->net_mgr.reconnect_timer, ESP_ERR_NO_MEM, TAG,
                        "create reconnect timer failed");

    return ESP_OK;
}

void net_mgr_deinit(lwlte_core_t *me)
{
    if (!me) {
        return;
    }
    net_mgr_cancel_reconnect(me);
    if (me->net_mgr.reconnect_timer) {
        xTimerDelete(me->net_mgr.reconnect_timer, portMAX_DELAY);
        me->net_mgr.reconnect_timer = NULL;
    }
}

void net_mgr_cancel_reconnect(lwlte_core_t *me)
{
    if (!me || !me->net_mgr.reconnect_timer) {
        return;
    }
    xTimerStop(me->net_mgr.reconnect_timer, 0);
}

void net_mgr_set_reconnect_enabled(lwlte_core_t *me, bool enabled)
{
    if (!me) {
        return;
    }
    me->net_mgr.reconnect_enabled = enabled;
}
```

- [ ] **Step 3: Implement network state accessors**

Use `core->lock` for App/FSM shared network state access.

```c
esp_err_t net_mgr_get_state(lwlte_core_t *me, lwlte_net_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    *state = me->net_mgr.state;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t net_mgr_set_state(lwlte_core_t *me, lwlte_net_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(state >= LWLTE_NET_STATE_OFFLINE &&
                        state <= LWLTE_NET_STATE_ERROR,
                        ESP_ERR_INVALID_ARG, TAG, "invalid net state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->net_mgr.state = state;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}
```

- [ ] **Step 4: Implement activation**

Add the public activation function and the single-attempt helper. This code must run only from the FSM task.

```c
esp_err_t net_mgr_start_activation(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = true;
    me->net_mgr.retry_count = 0;

    esp_err_t ret = ESP_FAIL;
    while (me->net_mgr.retry_count < me->net_mgr.max_retry && !core_is_destroying(me)) {
        ret = run_activation_once(me);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        me->net_mgr.retry_count++;
        ESP_LOGW(TAG, "activation attempt %d failed: %s",
                 me->net_mgr.retry_count, esp_err_to_name(ret));
    }

    return fail_activation(me, ret);
}

static esp_err_t run_activation_once(lwlte_core_t *me)
{
    modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
    modem_signal_t signal = {0};
    modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
    modem_pdp_context_t pdp = {0};
    esp_err_t ret = ESP_OK;

    me->net_mgr.current_step = NET_STEP_CHECK_SIM;
    core_set_state(me, LWLTE_CORE_STATE_NET_ACTIVATING);
    net_mgr_set_state(me, LWLTE_NET_STATE_ACTIVATING);
    post_net_state(me, LWLTE_CORE_EVENT_NET_CONNECTING,
                   LWLTE_NET_STATE_ACTIVATING, 0);

    ret = modem_get_sim_status(me->modem, &sim_status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (sim_status != MODEM_SIM_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    me->net_mgr.current_step = NET_STEP_CHECK_SIGNAL;
    ret = modem_get_signal(me->modem, &signal);
    if (ret != ESP_OK) {
        return ret;
    }

    me->net_mgr.current_step = NET_STEP_CHECK_REGISTRATION;
    ret = modem_get_registration(me->modem, &reg_status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!registration_ready(reg_status)) {
        return ESP_ERR_INVALID_STATE;
    }

    me->net_mgr.current_step = NET_STEP_SET_APN;
    ret = modem_set_apn(me->modem, me->config.primary_cid, me->config.apn);
    if (ret != ESP_OK) {
        return ret;
    }

    me->net_mgr.current_step = NET_STEP_ACTIVATE_PDP;
    ret = modem_activate_pdp(me->modem, me->config.primary_cid);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = modem_get_pdp_context(me->modem, me->config.primary_cid, &pdp);
    if (ret != ESP_OK) {
        return ret;
    }

    me->net_mgr.current_step = NET_STEP_DONE;
    pdp_mgr_update(&me->pdp_mgr, &pdp);
    net_mgr_set_state(me, LWLTE_NET_STATE_ONLINE);
    core_set_state(me, LWLTE_CORE_STATE_ONLINE);
    post_net_state(me, LWLTE_CORE_EVENT_NET_ONLINE, LWLTE_NET_STATE_ONLINE, 0);
    return ESP_OK;
}
```

- [ ] **Step 5: Implement deactivation and modem PDP event handlers**

Add explicit manual disconnect behavior and automatic reconnect behavior for dropped PDP.

```c
esp_err_t net_mgr_deactivate(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = false;

    lwlte_net_state_t old_state = LWLTE_NET_STATE_OFFLINE;
    net_mgr_get_state(me, &old_state);
    if (old_state == LWLTE_NET_STATE_ONLINE || old_state == LWLTE_NET_STATE_ACTIVATING) {
        esp_err_t ret = modem_deactivate_pdp(me->modem, me->config.primary_cid);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "deactivate PDP failed: %s", esp_err_to_name(ret));
        }
    }

    pdp_mgr_set_active(&me->pdp_mgr, me->config.primary_cid, false);
    net_mgr_set_state(me, LWLTE_NET_STATE_OFFLINE);
    if (old_state != LWLTE_NET_STATE_OFFLINE) {
        post_net_state(me, LWLTE_CORE_EVENT_NET_OFFLINE, LWLTE_NET_STATE_OFFLINE, 0);
    }
    return ESP_OK;
}

esp_err_t net_mgr_handle_pdp_activated(lwlte_core_t *me,
                                       const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    pdp_mgr_update(&me->pdp_mgr, pdp);
    net_mgr_set_state(me, LWLTE_NET_STATE_ONLINE);
    core_set_state(me, LWLTE_CORE_STATE_ONLINE);
    post_net_state(me, LWLTE_CORE_EVENT_NET_ONLINE, LWLTE_NET_STATE_ONLINE, 0);
    return ESP_OK;
}

esp_err_t net_mgr_handle_pdp_deactivated(lwlte_core_t *me,
                                         const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    pdp_mgr_update(&me->pdp_mgr, pdp);
    pdp_mgr_set_active(&me->pdp_mgr, me->config.primary_cid, false);
    net_mgr_set_state(me, LWLTE_NET_STATE_OFFLINE);
    core_set_state(me, LWLTE_CORE_STATE_READY);
    post_net_state(me, LWLTE_CORE_EVENT_NET_OFFLINE, LWLTE_NET_STATE_OFFLINE, 0);

    if (me->net_mgr.reconnect_enabled && !core_is_destroying(me) &&
        me->net_mgr.reconnect_timer) {
        xTimerStart(me->net_mgr.reconnect_timer, 0);
    }
    return ESP_OK;
}
```

- [ ] **Step 6: Implement static helpers**

Add the timer callback, error helper, registration check, primary CID check, and event posting helper.

```c
static void reconnect_timer_cb(TimerHandle_t timer)
{
    lwlte_core_t *core = (lwlte_core_t *)pvTimerGetTimerID(timer);
    core_fsm_sig_t sig = {
        .type = CORE_SIG_RECONNECT,
    };

    esp_err_t ret = core_fsm_send(core, &sig);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "send reconnect signal failed: %s", esp_err_to_name(ret));
    }
}

static esp_err_t fail_activation(lwlte_core_t *me, esp_err_t err)
{
    me->net_mgr.current_step = NET_STEP_ERROR;
    net_mgr_set_state(me, LWLTE_NET_STATE_ERROR);
    core_set_state(me, LWLTE_CORE_STATE_ERROR);
    post_net_state(me, LWLTE_CORE_EVENT_NET_ERROR, LWLTE_NET_STATE_ERROR, err);
    return err;
}

static bool registration_ready(modem_reg_status_t status)
{
    return status == MODEM_REG_REGISTERED_HOME ||
           status == MODEM_REG_REGISTERED_ROAMING;
}

static bool is_primary_pdp(lwlte_core_t *me, const modem_pdp_context_t *pdp)
{
    return me && pdp && pdp->cid == me->config.primary_cid;
}

static void post_net_state(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                           lwlte_net_state_t net_state, int error_code)
{
    lwlte_core_event_data_t data = {
        .net_state = net_state,
        .error_code = error_code,
    };
    esp_err_t ret = core_post_event(me, event_id, &data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post net event failed: %s", esp_err_to_name(ret));
    }
}
```

- [ ] **Step 7: Verify layer boundaries in net manager**

Run: `rg '#include "(at_engine|modem_air780ep|modem_priv)\.h"|at_engine_' src/core/net_mgr.c`

Expected: no matches.

---

### Task 5: Core FSM

**Files:**
- Create: `src/core/core_fsm.c`
- Depends on: `src/core/core_priv.h`

- [ ] **Step 1: Create FSM source skeleton**

Create `src/core/core_fsm.c` with task setup, queue setup, and static handlers.

```c
/**
 * @file core_fsm.c
 * @brief LTE Core FSM 任务
 * @details LTE Core FSM task
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "core_fsm"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void fsm_task(void *arg);
static bool fsm_should_stop(lwlte_core_t *me);
static void handle_signal(lwlte_core_t *me, const core_fsm_sig_t *sig);
static void handle_start(lwlte_core_t *me);
static void handle_stop(lwlte_core_t *me);
static void handle_modem_event(lwlte_core_t *me, const modem_event_t *event);
static void handle_ready(lwlte_core_t *me);
static void handle_core_error(lwlte_core_t *me, int error_code);
static bool modem_state_ready(modem_state_t state);
```

- [ ] **Step 2: Implement FSM lifecycle and queue send**

Add the global functions below. `core_fsm_deinit()` must not delete the task from inside the FSM task.

```c
esp_err_t core_fsm_init(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    me->fsm.queue = xQueueCreate(me->config.fsm_queue_size, sizeof(core_fsm_sig_t));
    ESP_RETURN_ON_FALSE(me->fsm.queue, ESP_ERR_NO_MEM, TAG, "create fsm queue failed");

    me->fsm.task_done_sema = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(me->fsm.task_done_sema, ESP_ERR_NO_MEM, TAG,
                        "create task_done_sema failed");

    BaseType_t task_ret = xTaskCreate(fsm_task, "lwlte_fsm",
                                      me->config.fsm_task_stack, me,
                                      me->config.fsm_task_priority,
                                      &me->fsm.task);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "create fsm task failed");

    me->fsm.running = true;
    me->fsm.stop_requested = false;
    return ESP_OK;
}

void core_fsm_deinit(lwlte_core_t *me)
{
    if (!me) {
        return;
    }
    if (me->fsm.task && xTaskGetCurrentTaskHandle() == me->fsm.task) {
        return;
    }

    if (me->lock) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->fsm.stop_requested = true;
        xSemaphoreGive(me->lock);
    }

    if (me->fsm.queue) {
        core_fsm_sig_t sig = {.type = CORE_SIG_STOP};
        xQueueSend(me->fsm.queue, &sig, 0);
    }

    if (me->fsm.task && me->fsm.task_done_sema) {
        xSemaphoreTake(me->fsm.task_done_sema, portMAX_DELAY);
        me->fsm.task = NULL;
    }
    if (me->fsm.queue) {
        vQueueDelete(me->fsm.queue);
        me->fsm.queue = NULL;
    }
    if (me->fsm.task_done_sema) {
        vSemaphoreDelete(me->fsm.task_done_sema);
        me->fsm.task_done_sema = NULL;
    }
    me->fsm.running = false;
}

esp_err_t core_fsm_send(lwlte_core_t *me, const core_fsm_sig_t *sig)
{
    ESP_RETURN_ON_FALSE(me && sig && me->fsm.queue, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    if (xQueueSend(me->fsm.queue, sig, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool core_fsm_is_task(lwlte_core_t *me)
{
    return me && me->fsm.task && xTaskGetCurrentTaskHandle() == me->fsm.task;
}
```

- [ ] **Step 3: Implement task loop and signal dispatch**

Add the static task loop and dispatcher.

```c
static void fsm_task(void *arg)
{
    lwlte_core_t *me = (lwlte_core_t *)arg;

    while (!fsm_should_stop(me)) {
        core_fsm_sig_t sig = {0};
        if (xQueueReceive(me->fsm.queue, &sig,
                          pdMS_TO_TICKS(CORE_FSM_WAIT_MS)) != pdTRUE) {
            continue;
        }
        if (fsm_should_stop(me)) {
            break;
        }
        handle_signal(me, &sig);
    }

    if (me->fsm.task_done_sema) {
        xSemaphoreGive(me->fsm.task_done_sema);
    }
    vTaskDelete(NULL);
}

static bool fsm_should_stop(lwlte_core_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool stop = me->fsm.stop_requested || me->destroying;
    xSemaphoreGive(me->lock);
    return stop;
}

static void handle_signal(lwlte_core_t *me, const core_fsm_sig_t *sig)
{
    if (!me || !sig) {
        return;
    }

    switch (sig->type) {
    case CORE_SIG_START:
        handle_start(me);
        break;
    case CORE_SIG_STOP:
        handle_stop(me);
        break;
    case CORE_SIG_NET_ACTIVATE:
        net_mgr_start_activation(me);
        break;
    case CORE_SIG_NET_DEACTIVATE:
        net_mgr_deactivate(me);
        core_set_state(me, LWLTE_CORE_STATE_READY);
        break;
    case CORE_SIG_MODEM_EVENT:
        handle_modem_event(me, &sig->modem_event);
        break;
    case CORE_SIG_RECONNECT:
        if (!core_is_destroying(me)) {
            net_mgr_start_activation(me);
        }
        break;
    case CORE_SIG_NET_STEP_DONE:
    case CORE_SIG_NET_STEP_TIMEOUT:
        break;
    default:
        ESP_LOGW(TAG, "unknown signal %d", sig->type);
        break;
    }
}
```

- [ ] **Step 4: Implement lifecycle handlers**

Add the start/stop/ready/error handlers.

```c
static void handle_start(lwlte_core_t *me)
{
    modem_state_t modem_state = MODEM_STATE_CREATED;

    core_set_state(me, LWLTE_CORE_STATE_STARTING);
    core_post_event(me, LWLTE_CORE_EVENT_STARTED, NULL);

    esp_err_t ret = modem_get_state(me->modem, &modem_state);
    if (ret == ESP_OK && modem_state_ready(modem_state)) {
        handle_ready(me);
    }
}

static void handle_stop(lwlte_core_t *me)
{
    net_mgr_set_reconnect_enabled(me, false);
    net_mgr_cancel_reconnect(me);
    net_mgr_deactivate(me);
    core_set_state(me, LWLTE_CORE_STATE_STOPPED);
    core_post_event(me, LWLTE_CORE_EVENT_STOPPED, NULL);
}

static void handle_ready(lwlte_core_t *me)
{
    lwlte_core_state_t state = core_get_state_value(me);
    if (state != LWLTE_CORE_STATE_ONLINE &&
        state != LWLTE_CORE_STATE_NET_ACTIVATING) {
        core_set_state(me, LWLTE_CORE_STATE_READY);
        core_post_event(me, LWLTE_CORE_EVENT_READY, NULL);
    }
    if (me->config.auto_connect) {
        net_mgr_start_activation(me);
    }
}

static void handle_core_error(lwlte_core_t *me, int error_code)
{
    lwlte_core_event_data_t data = {
        .net_state = LWLTE_NET_STATE_ERROR,
        .error_code = error_code,
    };
    core_set_state(me, LWLTE_CORE_STATE_ERROR);
    core_post_event(me, LWLTE_CORE_EVENT_ERROR, &data);
}
```

- [ ] **Step 5: Implement Modem event mapping**

Add the Modem event handler and Modem state predicate.

```c
static void handle_modem_event(lwlte_core_t *me, const modem_event_t *event)
{
    if (!me || !event) {
        return;
    }
    if (core_get_state_value(me) == LWLTE_CORE_STATE_STOPPED) {
        return;
    }

    switch (event->id) {
    case MODEM_EVENT_READY:
        handle_ready(me);
        break;
    case MODEM_EVENT_PDP_ACTIVATED:
        net_mgr_handle_pdp_activated(me, &event->data.pdp);
        break;
    case MODEM_EVENT_PDP_DEACTIVATED:
        net_mgr_handle_pdp_deactivated(me, &event->data.pdp);
        break;
    case MODEM_EVENT_ERROR:
        handle_core_error(me, event->data.error_code);
        break;
    case MODEM_EVENT_SIM_CHANGED:
    case MODEM_EVENT_REG_CHANGED:
    case MODEM_EVENT_SIGNAL_CHANGED:
        break;
    default:
        ESP_LOGW(TAG, "unknown modem event %d", event->id);
        break;
    }
}

static bool modem_state_ready(modem_state_t state)
{
    return state == MODEM_STATE_READY ||
           state == MODEM_STATE_REGISTERING ||
           state == MODEM_STATE_REGISTERED ||
           state == MODEM_STATE_PDP_ACTIVE;
}
```

- [ ] **Step 6: Verify FSM does not include lower-layer headers**

Run: `rg '#include "(at_engine|modem_air780ep|modem_priv)\.h"|at_engine_' src/core/core_fsm.c`

Expected: no matches.

---

### Task 6: Core Facade and Public API

**Files:**
- Create: `src/core/core.c`
- Depends on: `src/core/core_priv.h`

- [ ] **Step 1: Create Core source skeleton and static prototypes**

Create `src/core/core.c` with event base definition and prototypes.

```c
/**
 * @file core.c
 * @brief LTE 核心服务实现
 * @details LTE core service implementation
 * @author JovisDreams
 * @date 2026-05-24
 */

/*********************
 *      INCLUDES
 *********************/
#include "core_priv.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "lwlte_core"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool config_valid(const lwlte_core_config_t *config, modem_t *modem);
static void normalize_config(lwlte_core_config_t *dst,
                             const lwlte_core_config_t *src);
static esp_err_t create_event_loop(lwlte_core_t *me);
static void destroy_event_loop(lwlte_core_t *me);
static esp_err_t send_simple_signal(lwlte_core_t *me, core_fsm_sig_type_t type);
static bool api_state_allows(lwlte_core_t *me, core_fsm_sig_type_t type);
static void core_modem_event_cb(modem_t *modem, const modem_event_t *event,
                                void *user_ctx);
static void core_event_adapter(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void cleanup_core(lwlte_core_t *me);

/**********************
 *  STATIC VARIABLES
 **********************/
ESP_EVENT_DEFINE_BASE(LWLTE_CORE_EVENT);
```

- [ ] **Step 2: Implement create and destroy**

Add the constructor/destructor. On any constructor failure, clean up in reverse creation order and return `NULL`.

```c
lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 modem_t *modem)
{
    if (!config_valid(config, modem)) {
        ESP_LOGE(TAG, "invalid core config");
        return NULL;
    }

    lwlte_core_t *me = calloc(1, sizeof(lwlte_core_t));
    if (!me) {
        ESP_LOGE(TAG, "calloc core failed");
        return NULL;
    }

    normalize_config(&me->config, config);
    me->modem = modem;
    me->state = LWLTE_CORE_STATE_STOPPED;
    me->destroying = false;

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        cleanup_core(me);
        return NULL;
    }
    if (create_event_loop(me) != ESP_OK) {
        cleanup_core(me);
        return NULL;
    }
    if (pdp_mgr_init(&me->pdp_mgr, me->config.primary_cid) != ESP_OK) {
        cleanup_core(me);
        return NULL;
    }
    if (net_mgr_init(me) != ESP_OK) {
        cleanup_core(me);
        return NULL;
    }
    if (core_fsm_init(me) != ESP_OK) {
        cleanup_core(me);
        return NULL;
    }
    if (modem_register_event_callback(modem, core_modem_event_cb, me) != ESP_OK) {
        cleanup_core(me);
        return NULL;
    }

    return me;
}

esp_err_t lwlte_core_destroy(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(!core_fsm_is_task(me), ESP_ERR_INVALID_STATE, TAG,
                        "destroy from fsm task");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    me->state = LWLTE_CORE_STATE_DESTROYING;
    xSemaphoreGive(me->lock);

    modem_register_event_callback(me->modem, NULL, NULL);
    cleanup_core(me);
    free(me);
    return ESP_OK;
}
```

- [ ] **Step 3: Implement public control and query APIs**

Add the App-facing APIs. They validate state and enqueue signals; they must not call blocking `modem_*` operations directly.

```c
esp_err_t lwlte_core_start(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_START),
                        ESP_ERR_INVALID_STATE, TAG, "state does not allow start");
    return send_simple_signal(me, CORE_SIG_START);
}

esp_err_t lwlte_core_stop(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_STOP),
                        ESP_ERR_INVALID_STATE, TAG, "state does not allow stop");
    return send_simple_signal(me, CORE_SIG_STOP);
}

esp_err_t lwlte_core_connect(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_ACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "state does not allow connect");
    return send_simple_signal(me, CORE_SIG_NET_ACTIVATE);
}

esp_err_t lwlte_core_disconnect(lwlte_core_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_DEACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "state does not allow disconnect");
    return send_simple_signal(me, CORE_SIG_NET_DEACTIVATE);
}

esp_err_t lwlte_core_register_event_callback(lwlte_core_t *me,
                                              lwlte_core_event_callback_t callback,
                                              void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

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

esp_event_loop_handle_t lwlte_core_get_event_loop(lwlte_core_t *me)
{
    return me ? me->event_loop : NULL;
}

esp_err_t lwlte_core_get_state(lwlte_core_t *me, lwlte_core_state_t *state)
{
    ESP_RETURN_ON_FALSE(me && state, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    *state = core_get_state_value(me);
    return ESP_OK;
}

esp_err_t lwlte_core_get_net_state(lwlte_core_t *me, lwlte_net_state_t *state)
{
    return net_mgr_get_state(me, state);
}
```

- [ ] **Step 4: Implement shared Core helpers**

Add the internal functions used by FSM and net manager.

```c
esp_err_t core_set_state(lwlte_core_t *me, lwlte_core_state_t state)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(state >= LWLTE_CORE_STATE_STOPPED &&
                        state <= LWLTE_CORE_STATE_DESTROYING,
                        ESP_ERR_INVALID_ARG, TAG, "invalid state");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->state = state;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

lwlte_core_state_t core_get_state_value(lwlte_core_t *me)
{
    if (!me || !me->lock) {
        return LWLTE_CORE_STATE_ERROR;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    lwlte_core_state_t state = me->state;
    xSemaphoreGive(me->lock);
    return state;
}

bool core_is_destroying(lwlte_core_t *me)
{
    if (!me || !me->lock) {
        return true;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    xSemaphoreGive(me->lock);
    return destroying;
}

esp_err_t core_post_event(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                          const lwlte_core_event_data_t *data)
{
    ESP_RETURN_ON_FALSE(me && me->event_loop, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    lwlte_core_event_data_t empty = {0};
    const lwlte_core_event_data_t *event_data = data ? data : &empty;
    return esp_event_post_to(me->event_loop, LWLTE_CORE_EVENT, event_id,
                             event_data, sizeof(*event_data), 0);
}
```

- [ ] **Step 5: Implement static helpers and adapters**

Add validation, config normalization, event loop, signal send, API state checks, and callbacks.

```c
static bool config_valid(const lwlte_core_config_t *config, modem_t *modem)
{
    if (!config || !modem || !config->apn || config->apn[0] == '\0') {
        return false;
    }
    if (config->primary_cid > CORE_MAX_PDP_CONTEXTS) {
        return false;
    }
    return true;
}

static void normalize_config(lwlte_core_config_t *dst,
                             const lwlte_core_config_t *src)
{
    *dst = *src;
    if (dst->primary_cid == 0) {
        dst->primary_cid = CORE_DEFAULT_PRIMARY_CID;
    }
    if (dst->net_activate_timeout_ms == 0) {
        dst->net_activate_timeout_ms = CORE_DEFAULT_NET_ACTIVATE_TIMEOUT_MS;
    }
    if (dst->reconnect_delay_ms == 0) {
        dst->reconnect_delay_ms = CORE_DEFAULT_RECONNECT_DELAY_MS;
    }
    if (dst->fsm_queue_size <= 0) {
        dst->fsm_queue_size = CORE_DEFAULT_FSM_QUEUE_SIZE;
    }
    if (dst->fsm_task_stack <= 0) {
        dst->fsm_task_stack = CORE_DEFAULT_FSM_TASK_STACK;
    }
    if (dst->fsm_task_priority <= 0) {
        dst->fsm_task_priority = CORE_DEFAULT_FSM_TASK_PRIORITY;
    }
}

static esp_err_t create_event_loop(lwlte_core_t *me)
{
    esp_event_loop_args_t args = {
        .queue_size = CORE_EVENT_QUEUE_SIZE,
        .task_name = "lwlte_evt",
        .task_priority = CORE_EVENT_TASK_PRIORITY,
        .task_stack_size = CORE_EVENT_TASK_STACK,
        .task_core_id = tskNO_AFFINITY,
    };

    esp_err_t ret = esp_event_loop_create(&args, &me->event_loop);
    ESP_RETURN_ON_ERROR(ret, TAG, "create event loop failed");
    return esp_event_handler_register_with(me->event_loop, LWLTE_CORE_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           core_event_adapter, me);
}

static void destroy_event_loop(lwlte_core_t *me)
{
    if (!me || !me->event_loop) {
        return;
    }
    esp_event_handler_unregister_with(me->event_loop, LWLTE_CORE_EVENT,
                                      ESP_EVENT_ANY_ID, core_event_adapter);
    esp_event_loop_delete(me->event_loop);
    me->event_loop = NULL;
}

static esp_err_t send_simple_signal(lwlte_core_t *me, core_fsm_sig_type_t type)
{
    core_fsm_sig_t sig = {
        .type = type,
    };
    return core_fsm_send(me, &sig);
}

static bool api_state_allows(lwlte_core_t *me, core_fsm_sig_type_t type)
{
    if (!me || core_is_destroying(me)) {
        return false;
    }

    lwlte_core_state_t state = core_get_state_value(me);
    switch (type) {
    case CORE_SIG_START:
        return state == LWLTE_CORE_STATE_STOPPED;
    case CORE_SIG_STOP:
        return state != LWLTE_CORE_STATE_STOPPED &&
               state != LWLTE_CORE_STATE_DESTROYING;
    case CORE_SIG_NET_ACTIVATE:
        return state == LWLTE_CORE_STATE_READY ||
               state == LWLTE_CORE_STATE_ERROR;
    case CORE_SIG_NET_DEACTIVATE:
        return state == LWLTE_CORE_STATE_NET_ACTIVATING ||
               state == LWLTE_CORE_STATE_ONLINE ||
               state == LWLTE_CORE_STATE_ERROR;
    default:
        return false;
    }
}

static void core_modem_event_cb(modem_t *modem, const modem_event_t *event,
                                void *user_ctx)
{
    (void)modem;
    lwlte_core_t *me = (lwlte_core_t *)user_ctx;
    if (!me || !event) {
        return;
    }

    core_fsm_sig_t sig = {
        .type = CORE_SIG_MODEM_EVENT,
        .modem_event = *event,
    };
    esp_err_t ret = core_fsm_send(me, &sig);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "drop modem event %d: %s", event->id, esp_err_to_name(ret));
    }
}

static void core_event_adapter(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)event_base;
    lwlte_core_t *me = (lwlte_core_t *)handler_arg;
    if (!me || !me->lock) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    lwlte_core_event_callback_t cb = me->event_callback;
    void *user_ctx = me->event_user_ctx;
    xSemaphoreGive(me->lock);

    if (cb) {
        lwlte_core_event_data_t empty = {0};
        const lwlte_core_event_data_t *data = event_data ? event_data : &empty;
        cb(me, (lwlte_core_event_id_t)event_id, data, user_ctx);
    }
}

static void cleanup_core(lwlte_core_t *me)
{
    if (!me) {
        return;
    }
    net_mgr_deinit(me);
    core_fsm_deinit(me);
    destroy_event_loop(me);
    if (me->lock) {
        vSemaphoreDelete(me->lock);
        me->lock = NULL;
    }
}
```

- [ ] **Step 6: Verify public APIs do not directly perform blocking Modem operations**

Run: `rg 'lwlte_core_(start|stop|connect|disconnect).*modem_|modem_(get|set|activate|deactivate)' src/core/core.c`

Expected: no matches that show `lwlte_core_start`, `lwlte_core_stop`, `lwlte_core_connect`, or `lwlte_core_disconnect` calling blocking Modem operations.

---

### Task 7: CMake Integration and Build Fixes

**Files:**
- Modify: `src/CMakeLists.txt`
- Potential fixes: `src/core/*.c`, `src/core/core_priv.h`, `src/include/lwlte_core.h`

- [ ] **Step 1: Update component source list and requirements**

Modify `src/CMakeLists.txt` to include Core source files and `esp_event`.

```cmake
idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "core/core.c"
         "core/core_fsm.c"
         "core/net_mgr.c"
         "core/pdp_mgr.c"
    INCLUDE_DIRS include
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
```

- [ ] **Step 2: Run the ESP-IDF build**

Run the MCP build tool: `esp-idf-eim_build_project`

Expected: build succeeds with exit status 0.

- [ ] **Step 3: If the build reports C compile errors, fix only the reported Core errors**

Use the compiler output to fix concrete issues such as missing includes, unavailable constants, type mismatches, or unused static declarations. Keep fixes within the Core files unless the compiler error identifies `src/CMakeLists.txt`.

Examples of acceptable fixes:

```c
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
```

```c
static void unused_helper(void);  /* Remove the declaration if the helper is not used. */
```

- [ ] **Step 4: Re-run the ESP-IDF build after fixes**

Run the MCP build tool again: `esp-idf-eim_build_project`

Expected: build succeeds with exit status 0.

---

### Task 8: Boundary and Acceptance Verification

**Files:**
- Inspect: `src/core/*.c`
- Inspect: `src/core/core_priv.h`
- Inspect: `src/include/lwlte_core.h`

- [ ] **Step 1: Verify Core does not include forbidden headers**

Run: `rg '#include "(at_engine|modem_air780ep|modem_priv)\.h"' src/core src/include/lwlte_core.h`

Expected: no matches.

- [ ] **Step 2: Verify Core implementation does not call AT Engine APIs**

Run: `rg 'at_engine_' src/core src/include/lwlte_core.h`

Expected: no matches.

- [ ] **Step 3: Verify public API signatures exist**

Run: `rg 'lwlte_core_(create|destroy|start|stop|register_event_callback|get_event_loop|get_state|get_net_state|connect|disconnect)' src/include/lwlte_core.h src/core/core.c`

Expected: matches for all ten public API functions in the header, and definitions in `src/core/core.c`.

- [ ] **Step 4: Verify the Modem callback only queues FSM signals**

Run: `rg -n 'core_modem_event_cb|core_fsm_send|core_set_state|net_mgr_' src/core/core.c`

Expected: within `core_modem_event_cb`, the only Core action after copying `modem_event_t` is `core_fsm_send()` plus logging on failure.

- [ ] **Step 5: Verify the build after boundary checks**

Run the MCP build tool: `esp-idf-eim_build_project`

Expected: build succeeds with exit status 0.

- [ ] **Step 6: Inspect final working tree**

Run: `git status --short`

Expected: only intended files are modified or untracked:

```text
?? docs/superpowers/specs/2026-05-24-core-module-design.md
?? docs/superpowers/plans/2026-05-24-core-module-implementation-plan.md
?? src/core/core.c
?? src/core/core_fsm.c
?? src/core/core_priv.h
?? src/core/net_mgr.c
?? src/core/pdp_mgr.c
?? src/include/lwlte_core.h
 M src/CMakeLists.txt
```

The exact ordering may differ.

---

## Self-Review Notes

- Spec coverage: tasks cover public API, private structs, FSM, net manager activation and reconnect, PDP cache, event loop, CMake integration, and boundary verification.
- Placeholder scan: no unresolved placeholder markers are intentionally present.
- Type consistency: public types match `docs/superpowers/specs/2026-05-24-core-module-design.md`; internal names match `docs/agents/classes.md` section 3.
- Known verification limit: this plan requires compile verification only. It does not claim hardware behavior without flashing or serial logs.
