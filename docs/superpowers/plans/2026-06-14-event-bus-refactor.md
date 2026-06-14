# Event Bus Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace core/mqtt_client private event loops and the facade callback slot with a single shared `esp_event_loop_handle_t` (default loop by default), expose two public event bases (`LWLTE_EVENT`, `LWLTE_MQTT_EVENT`), and move protocol data onto a private synchronous callback.

**Architecture:** A single shared event bus (passed via config, NULL = `esp_event_loop_get_default()`). Core FSM and MQTT FSM post directly to the bus using public event IDs defined in `lwlte.h`. Protocol data (PROTOCOL_DATA / PROTOCOL_CLOSED) flows over a private synchronous callback (`core_register_protocol_callback`), never touching the bus. The facade's callback slot, translator bridges, and entire sync machinery are deleted. Users observe events via standard `esp_event_handler_register`.

**Tech Stack:** ESP-IDF v6.0, C, FreeRTOS, `esp_event` library, Python host contract tests (unittest).

**Spec:** `docs/superpowers/specs/2026-06-14-event-bus-refactor-design.md`

**Reference doc — read before starting:**
- `docs/agents/coding-style.md` (naming, file layout, Doxygen conventions)
- `docs/agents/oop-design.md` (handle / create/destroy patterns)
- `docs/agents/build-and-debug.md` (build + host-test commands)

**Commit policy:** Per `AGENTS.md`, every commit step below requires explicit user authorization before running. Stage with `git add` freely; run `git commit` only when the user approves.

**Build commands:**
```bash
source ~/.espressif/v6.0/esp-idf/export.sh   # one-time per shell
idf.py build                                  # compile check
python3 tests/host/test_<name>.py             # run a single host contract test
python3 -m unittest discover -s tests/host -v # run all host contract tests
```

---

## Task 1: Define new event contract in `lwlte.h`

Add the new public event bases, enums, and data structs alongside the old ones. The old API stays in place so the project keeps compiling. Nothing is deleted yet.

**Files:**
- Modify: `src/include/lwlte.h`

- [ ] **Step 1: Add `#include "esp_event.h"` to lwlte.h**

The current includes block is at the top of the file (`src/include/lwlte.h:17-23`). Add `"esp_event.h"` after `"esp_err.h"`:

```c
#include "esp_err.h"
#include "esp_event.h"
```

- [ ] **Step 2: Add new event bases + enums + structs + release decl**

Insert the following block **immediately before** the existing `lwlte_event_callback_t` typedef (currently around `src/include/lwlte.h:164`). This places all new contract types together, above the soon-to-be-deleted old types.

```c
/**
 * @brief LTE 用户事件 base
 * @details LTE user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_EVENT);

/**
 * @brief LTE MQTT 用户事件 base
 * @details LTE MQTT user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT);

/**
 * @brief LTE 用户事件 ID（新）
 * @details LTE user event ID (new)
 * @note 投递到共享事件总线 LWLTE_EVENT。
 */
typedef enum {
    LWLTE_EVENT_STARTED_NEW = 0,        /**< 已启动； Started */
    LWLTE_EVENT_READY_NEW,              /**< 已就绪； Ready */
    LWLTE_EVENT_NET_CONNECTING_NEW,     /**< 网络连接中； Network connecting */
    LWLTE_EVENT_NET_ONLINE_NEW,         /**< 网络在线； Network online */
    LWLTE_EVENT_NET_OFFLINE_NEW,        /**< 网络离线； Network offline */
    LWLTE_EVENT_NET_ERROR_NEW,          /**< 网络错误； Network error */
    LWLTE_EVENT_STOPPED_NEW,            /**< 已停止； Stopped */
    LWLTE_EVENT_ERROR_NEW,              /**< 错误； Error */
} lwlte_event_id_new_t;

/**
 * @brief LTE MQTT 用户事件 ID（新）
 * @details LTE MQTT user event ID (new)
 * @note 投递到共享事件总线 LWLTE_MQTT_EVENT。
 */
typedef enum {
    LWLTE_MQTT_EVENT_STARTED_NEW = 0,   /**< MQTT 已启动； MQTT started */
    LWLTE_MQTT_EVENT_STOPPED_NEW,       /**< MQTT 已停止； MQTT stopped */
    LWLTE_MQTT_EVENT_CONNECTING_NEW,    /**< MQTT 连接中； MQTT connecting */
    LWLTE_MQTT_EVENT_CONNECTED_NEW,     /**< MQTT 已连接； MQTT connected */
    LWLTE_MQTT_EVENT_DISCONNECTED_NEW,  /**< MQTT 已断开； MQTT disconnected */
    LWLTE_MQTT_EVENT_SUBSCRIBED_NEW,    /**< MQTT 已订阅； MQTT subscribed */
    LWLTE_MQTT_EVENT_UNSUBSCRIBED_NEW,  /**< MQTT 已取消订阅； MQTT unsubscribed */
    LWLTE_MQTT_EVENT_PUBLISHED_NEW,     /**< MQTT 已发布； MQTT published */
    LWLTE_MQTT_EVENT_DATA_NEW,          /**< MQTT 数据； MQTT data */
    LWLTE_MQTT_EVENT_ERROR_NEW,         /**< MQTT 错误； MQTT error */
} lwlte_mqtt_event_id_new_t;

/**
 * @brief LTE 用户事件数据（新）
 * @details LTE user event data (new)
 */
typedef struct {
    lwlte_net_state_t net_state;    /**< 网络状态； Network state */
    int error_code;                 /**< 诊断错误码； Diagnostic error code */
} lwlte_event_data_new_t;

/**
 * @brief LTE MQTT 用户事件数据（新）
 * @details LTE MQTT user event data (new)
 */
typedef struct {
    lwlte_mqtt_state_t mqtt_state;  /**< MQTT 状态； MQTT state */
    int error_code;                 /**< 诊断错误码； Diagnostic error code */
    lwlte_mqtt_msg_t msg;           /**< MQTT 消息，仅 LWLTE_MQTT_EVENT_DATA 有效 */
    bool owns_payload;              /**< DATA 事件为 true，其余为 false */
} lwlte_mqtt_event_data_new_t;

/**
 * @brief 释放 MQTT_DATA 事件的堆缓冲
 * @details Release heap buffers carried by LWLTE_MQTT_EVENT_DATA
 * @note 处理 LWLTE_MQTT_EVENT_DATA 的 handler 必须在返回前调用。
 * @param[in] data 事件数据指针，可为 NULL
 */
void lwlte_mqtt_event_data_release_new(lwlte_mqtt_event_data_new_t *data);
```

**Naming rationale:** The `_NEW` suffix is temporary scaffolding so the new types coexist with the old `lwlte_event_id_t` / `lwlte_event_data_t` / `lwlte_event_callback_t` during Phases 1–4. Task 5 renames them (strips `_NEW`) and deletes the old types. This avoids a flag-day where everything breaks at once.

- [ ] **Step 3: Verify build**

Run: `idf.py build`
Expected: PASS (no references to the new types yet; they are just declarations)

- [ ] **Step 4: Commit**

```bash
git add src/include/lwlte.h
git commit -m "feat(event): add new event bus contract types to lwlte.h"
```

---

## Task 2: Core refactor — borrow event loop, add protocol callbacks, post to LWLTE_EVENT

Core stops owning its event loop. It borrows one via `core_config_t.event_loop`, posts `LWLTE_EVENT_*` directly, and exposes a private synchronous protocol-callback API.

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core_priv.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`

- [ ] **Step 1: Add `#include "lwlte.h"` and protocol callback API to core.h**

In `src/core/core.h`, after the existing `#include "esp_event.h"` (line 22), add:

```c
#include "lwlte.h"
```

Then, after the `core_protocol_data_t` struct definition (currently `src/core/core.h:116-122`), add the new callback types and registration API:

```c
/**
 * @brief Core 协议数据回调（私有，service-service 内部）
 * @details Core protocol data callback (private, service-service internal)
 * @note core FSM 同步调用；callback 必须只做轻量操作（入队、memcpy），不得阻塞。
 * @param[in] core LTE 核心服务句柄
 * @param[in] data 协议数据，指针仅在 callback 期间有效
 * @param[in] user_ctx 用户上下文
 */
typedef void (*core_protocol_callback_t)(core_handle_t *core,
                                         const core_protocol_data_t *data,
                                         void *user_ctx);

/**
 * @brief Core 协议通道关闭回调（私有）
 * @details Core protocol closed callback (private)
 * @param[in] core LTE 核心服务句柄
 * @param[in] protocol 协议类型
 * @param[in] user_ctx 用户上下文
 */
typedef void (*core_protocol_closed_callback_t)(core_handle_t *core,
                                                core_protocol_t protocol,
                                                void *user_ctx);

/**
 * @brief 注册协议数据回调
 * @details Register protocol data callback
 * @note callback 为 NULL 时注销。
 * @param[in] core LTE 核心服务句柄
 * @param[in] callback 回调函数
 * @param[in] user_ctx 用户上下文
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE
 */
esp_err_t core_register_protocol_callback(core_handle_t *core,
                                          core_protocol_callback_t callback,
                                          void *user_ctx);

/**
 * @brief 注册协议通道关闭回调
 * @details Register protocol closed callback
 */
esp_err_t core_register_protocol_closed_callback(core_handle_t *core,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx);
```

- [ ] **Step 2: Add `event_loop` field to `core_config_t`**

Find `core_config_t` in `src/core/core.h` (search for `typedef struct {` near the `core_config_t` definition). Add the field:

```c
    esp_event_loop_handle_t event_loop;   /**< 共享事件总线（借用）； Shared event bus (borrowed) */
```

Place it after the existing `primary_cid` / config fields, before the closing `}`. (Read the struct first to find the exact insertion point — the struct currently holds `apn`, `primary_cid`, fsm sizing, timeouts, etc.)

- [ ] **Step 3: Update `struct core_handle` in core_priv.h**

In `src/core/core_priv.h`, modify the `struct core_handle` block (currently lines 106-124).

**Remove** these fields:
```c
    esp_event_loop_handle_t event_loop;          /* keep — but now borrowed via config */
    TaskHandle_t event_loop_task;
    SemaphoreHandle_t event_callback_done_sema;
    TaskHandle_t event_callback_task;
    int event_callback_active;
    bool event_callback_waiting;
    core_event_callback_t event_callback;
    void *event_user_ctx;
```

**Replace** the whole `struct core_handle` block with:

```c
struct core_handle {
    core_config_t config;
    modem_handle_t *modem;
    core_fsm_t fsm;
    net_mgr_t net_mgr;
    pdp_mgr_t pdp_mgr;
    core_state_t state;
    bool destroying;
    bool destroy_in_progress;
    SemaphoreHandle_t lock;
    core_protocol_callback_t protocol_callback;
    void *protocol_user_ctx;
    core_protocol_closed_callback_t protocol_closed_callback;
    void *protocol_closed_user_ctx;
};
```

Note: `me->config.event_loop` is now the borrowed loop reference (config is stored by value in the handle).

- [ ] **Step 4: Update prototypes in core_priv.h**

In `src/core/core_priv.h`, the GLOBAL PROTOTYPES section (lines ~164-168) currently declares:

```c
esp_err_t core_post_event(core_handle_t *me, core_event_id_t event_id,
                          const core_event_data_t *data);
void core_free_cmd(core_cmd_t *cmd);
esp_err_t core_post_protocol_data(core_handle_t *me,
                                  const core_protocol_data_t *protocol_data);
```

Replace `core_post_event` and `core_post_protocol_data` declarations with:

```c
esp_err_t core_post_event(core_handle_t *me, lwlte_event_id_new_t event_id,
                          const lwlte_event_data_new_t *data);
void core_free_cmd(core_cmd_t *cmd);
```

(`core_post_protocol_data` is deleted — it is replaced by direct synchronous callback invocation in the FSM.)

- [ ] **Step 5: Update `core_post_event` implementation in core.c**

Find `core_post_event` in `src/core/core.c` (currently around line 490). It currently calls `esp_event_post_to(me->event_loop, CORE_EVENT, event_id, ...)`. Rewrite it to post to the borrowed loop under the `LWLTE_EVENT` base:

```c
esp_err_t core_post_event(core_handle_t *me, lwlte_event_id_new_t event_id,
                          const lwlte_event_data_new_t *data)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    lwlte_event_data_new_t empty_data = {0};
    if (!data) {
        data = &empty_data;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_event_post_to(me->config.event_loop, LWLTE_EVENT,
                                      event_id, data, sizeof(*data), 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
    }
    xSemaphoreGive(me->lock);

    return ret;
}
```

- [ ] **Step 6: Implement `core_register_protocol_callback` / `_closed_callback` in core.c**

Add these two functions to `src/core/core.c` (place them near the existing `core_register_event_callback`, which will be deleted in Step 8):

```c
esp_err_t core_register_protocol_callback(core_handle_t *me,
                                          core_protocol_callback_t callback,
                                          void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->protocol_callback = callback;
    me->protocol_user_ctx = callback ? user_ctx : NULL;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}

esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state == CORE_STATE_DESTROYING) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->protocol_closed_callback = callback;
    me->protocol_closed_user_ctx = callback ? user_ctx : NULL;
    xSemaphoreGive(me->lock);
    return ESP_OK;
}
```

- [ ] **Step 7: Change core_fsm.c PROTOCOL_DATA / CLOSED to synchronous callback**

In `src/core/core_fsm.c`, find the `MODEM_EVENT_PROTOCOL_DATA` and `MODEM_EVENT_PROTOCOL_CLOSED` cases (currently around lines 467-479).

Replace them with:

```c
    case MODEM_EVENT_PROTOCOL_DATA: {
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

(The modem-event payload is released by the existing FSM signal cleanup after the switch — no change there.)

- [ ] **Step 8: Delete core's private event loop machinery**

Delete these functions and references from `src/core/core.c`:
- `create_event_loop()` (currently lines ~593-625)
- `destroy_event_loop()` (currently lines ~627-650)
- `core_event_adapter()` (currently lines ~736-795)
- `core_register_event_callback()` (currently lines ~326-362)
- `core_get_event_loop()` (currently lines ~364-375)
- `core_post_protocol_data()` (currently lines ~527-543)
- `clone_protocol_data()` (currently lines ~1189-1207)
- `release_core_event_payload()` / `release_core_event_payload` static helper (currently lines ~1209-1254)
- All references to `me->event_loop`, `me->event_callback_*`, `me->event_loop_task` throughout `core.c`
- **Keep** the `clone_modem_protocol_payload` call block in the modem event callback (currently `core.c:720-727`). That clone is necessary — the modem→core FSM queue transit still crosses a task boundary. Only the core-loop transit clone (`clone_protocol_data`) is eliminated; the modem-queue clone stays.

Also delete from `src/core/core.c`:
- `wait_event_callbacks_idle()` static function and its forward declaration (search for the name)
- The `core_register_event_callback` call inside `core_init` / `core_create` flow if present
- `CORE_EVENT_QUEUE_SIZE`, `CORE_EVENT_TASK_STACK`, `CORE_EVENT_TASK_PRIORITY` defines in `src/core/core_priv.h` (lines 39-41) — no longer used

In `src/core/core.h`:
- Delete the `core_event_callback_t` typedef (currently lines ~242-245)
- Delete the `core_register_event_callback` declaration (currently lines ~304-315)
- Delete the `core_get_event_loop` declaration (currently line ~325)
- Delete the `ESP_EVENT_DECLARE_BASE(CORE_EVENT)` and the `core_event_id_t` enum (currently lines ~83-100) — core now uses `lwlte_event_id_new_t` from `lwlte.h`
- Delete `core_event_data_t` (currently lines ~228-232) — replaced by `lwlte_event_data_new_t`. **But check first:** `core_event_data_t` is used in `core_fsm_sig_t` and elsewhere. Search for all uses before deleting; if the FSM signal carries event data, switch its type to `lwlte_event_data_new_t` (core events only carry `net_state` + `error_code`, which fits the new struct).

In `src/core/core_priv.h`:
- Delete the forward prototype for `core_post_protocol_data` (already done in Step 4)

- [ ] **Step 9: Update all CORE_EVENT_* references to LWLTE_EVENT_*_NEW**

Search the entire `src/core/` directory for `CORE_EVENT_STARTED`, `CORE_EVENT_READY`, `CORE_EVENT_NET_CONNECTING`, `CORE_EVENT_NET_ONLINE`, `CORE_EVENT_NET_OFFLINE`, `CORE_EVENT_NET_ERROR`, `CORE_EVENT_STOPPED`, `CORE_EVENT_ERROR`. Replace each with the corresponding `LWLTE_EVENT_*_NEW` constant.

These appear in:
- `src/core/core_fsm.c` — the FSM posts events on state transitions
- `src/core/net_mgr.c` — net manager posts `NET_*` events via `core_post_event`
- Anywhere else `core_post_event(me, CORE_EVENT_*, ...)` is called

The mapping:
```
CORE_EVENT_STARTED        → LWLTE_EVENT_STARTED_NEW
CORE_EVENT_READY          → LWLTE_EVENT_READY_NEW
CORE_EVENT_NET_CONNECTING → LWLTE_EVENT_NET_CONNECTING_NEW
CORE_EVENT_NET_ONLINE     → LWLTE_EVENT_NET_ONLINE_NEW
CORE_EVENT_NET_OFFLINE    → LWLTE_EVENT_NET_OFFLINE_NEW
CORE_EVENT_NET_ERROR      → LWLTE_EVENT_NET_ERROR_NEW
CORE_EVENT_STOPPED        → LWLTE_EVENT_STOPPED_NEW
CORE_EVENT_ERROR          → LWLTE_EVENT_ERROR_NEW
```

For every `core_post_event(me, OLD_ID, &data)` call, the `data` struct type changes from `core_event_data_t` to `lwlte_event_data_new_t`. Update the local variable declarations accordingly:

```c
/* Old */
core_event_data_t data = { .net_state = ..., .error_code = ... };

/* New */
lwlte_event_data_new_t data = { .net_state = ..., .error_code = ... };
```

The field names (`net_state`, `error_code`) are the same in both structs, so only the type name changes.

**Delete** `CORE_EVENT_PROTOCOL_DATA` and `CORE_EVENT_PROTOCOL_CLOSED` — they no longer exist as event IDs. They are handled by the synchronous callback in Step 7, not posted to the bus.

- [ ] **Step 10: Update `core_init` / `core_create` to remove loop creation**

In `src/core/core.c`, find `core_init` (the internal init function called by `core_create`). It currently calls `create_event_loop(me)` and `esp_event_handler_register_with(...)`. Remove the `create_event_loop` call. The borrowed loop is already in `me->config.event_loop` (set from the caller's config).

Also remove the `core_register_event_callback(modem, core_modem_event_cb, me)` call if it registers on the now-deleted core loop — **read carefully**: `modem_register_event_callback` is a different mechanism (modem→core callback), keep it. Only remove references to `core`'s own event loop creation and the `core_event_adapter` registration.

In `core_destroy` (`src/core/core.c:227+`): remove the `destroy_event_loop(me)` call, the `event_callback_task` / `event_loop_task` guards, and any `wait_event_callbacks_idle` call. The protocol callback slots don't need explicit cleanup (they're just function pointers that become unreachable when the handle is freed).

- [ ] **Step 11: Verify build**

Run: `idf.py build`
Expected: PASS. Core now posts `LWLTE_EVENT_*_NEW` to the borrowed loop. The borrowed loop comes from config — but no caller passes it yet (facade still passes the old way), so `me->config.event_loop` is currently NULL/zero-initialized from `calloc`. This will be wired in Task 4.

**Note:** If the build fails because mqtt_client.c still references `core_get_event_loop` / `CORE_EVENT` / `core_event_callback_t` — that is expected. Those are fixed in Task 3. To get a clean build for this checkpoint, temporarily stub or comment the broken mqtt_client references; do NOT delete them yet. The cleanest approach: proceed directly to Task 3 without an intermediate build, then build after Task 3.

- [ ] **Step 12: Commit**

```bash
git add src/core/
git commit -m "refactor(core): borrow shared event loop, add protocol callbacks, post LWLTE_EVENT"
```

---

## Task 3: MQTT client refactor — borrow loop, protocol callbacks, post LWLTE_MQTT_EVENT

MQTT client stops owning a loop, registers protocol callbacks with core, listens on the shared bus for net state, and posts `LWLTE_MQTT_EVENT_*_NEW`.

**Files:**
- Modify: `src/mqtt_client/mqtt_client.h`
- Modify: `src/mqtt_client/mqtt_client_priv.h`
- Modify: `src/mqtt_client/mqtt_client.c`

- [ ] **Step 1: Add `event_loop` to `mqtt_client_config_t` and include lwlte.h**

In `src/mqtt_client/mqtt_client.h`:
- Add `#include "lwlte.h"` after the existing `#include "esp_event.h"` (line 23)
- Add `esp_event_loop_handle_t event_loop;` to the `mqtt_client_config_t` struct (currently lines 39-51), after `fsm_task_priority`

- [ ] **Step 2: Delete old callback API and event base from mqtt_client.h**

In `src/mqtt_client/mqtt_client.h`, delete:
- `ESP_EVENT_DECLARE_BASE(MQTT_CLIENT_EVENT)` (line 63)
- The `mqtt_client_event_id_t` enum (lines 65-76)
- The `mqtt_client_event_callback_t` typedef (lines 110-113)
- `mqtt_client_register_event_callback` declaration (lines 123-125)
- `mqtt_client_get_event_loop` declaration (line 126)

Keep `mqtt_client_state_t`, `mqtt_client_operation_t`, `mqtt_client_publish_t`, `mqtt_client_msg_t`. The `mqtt_client_event_data_t` struct (lines 101-108) will be renamed in Step 3 to an internal payload type.

- [ ] **Step 3: Update `struct mqtt_client_handle` in mqtt_client_priv.h**

In `src/mqtt_client/mqtt_client_priv.h`:

**Delete** these fields from the struct (currently lines 93, 111-114):
```c
    esp_event_loop_handle_t event_loop;
    mqtt_client_event_callback_t event_callback;
    void *event_user_ctx;
    SemaphoreHandle_t event_callback_done_sema;
    TaskHandle_t event_callback_task;
    int event_callback_active;
    bool event_callback_waiting;
```

The resulting struct keeps: `config`, `core`, `fsm_task`, `fsm_queue`, `fsm_task_done_sema`, `stop_done_sema`, `lock`, `state`, `connect_step`, `stop_step`, `pending_cmd`, `destroying`, `started`, `net_online`, `stop_requested`, `transport_open`, `session_connected`.

**Add** the borrowed loop field:
```c
    /* esp_event_loop_handle_t event_loop; — now borrowed via me->config.event_loop */
```
No new field needed — it lives in `me->config.event_loop`.

**Rename** `mqtt_client_event_data_t` → keep as internal `mqtt_client_event_payload_t` (it's the struct assembled before posting to the bus). Update its definition to align with the public `lwlte_mqtt_event_data_new_t`:

```c
typedef struct {
    lwlte_mqtt_state_t mqtt_state;
    int error_code;
    lwlte_mqtt_msg_t msg;
    bool owns_payload;
} mqtt_client_event_payload_t;
```

- [ ] **Step 4: Implement protocol data + closed callbacks in mqtt_client.c**

Add these two static functions to `src/mqtt_client/mqtt_client.c` (place near `handle_core_event`, before the GLOBAL FUNCTIONS section):

```c
static void mqtt_protocol_data_cb(core_handle_t *core,
                                  const core_protocol_data_t *data,
                                  void *user_ctx)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)user_ctx;
    if (!me || !data || data->protocol != CORE_PROTOCOL_MQTT) {
        return;
    }

    mqtt_protocol_data_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) {
        ESP_LOGW(TAG, "protocol data cb: alloc owned failed");
        return;
    }
    owned->topic = clone_string(data->topic);
    owned->topic_len = data->topic_len;
    owned->payload = clone_payload(data->payload, data->payload_len);
    owned->payload_len = data->payload_len;
    if ((data->topic && !owned->topic) ||
        (data->payload && data->payload_len > 0 && !owned->payload)) {
        free(owned->topic);
        free(owned->payload);
        free(owned);
        ESP_LOGW(TAG, "protocol data cb: clone failed");
        return;
    }

    mqtt_fsm_sig_t sig = {
        .type     = MQTT_SIG_PROTOCOL_DATA,
        .data     = owned,
        .data_size = sizeof(*owned),
    };
    if (send_fsm_sig(me, &sig) != ESP_OK) {
        free(owned->topic);
        free(owned->payload);
        free(owned);
    }
}

static void mqtt_protocol_closed_cb(core_handle_t *core,
                                    core_protocol_t protocol,
                                    void *user_ctx)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)user_ctx;
    if (!me || protocol != CORE_PROTOCOL_MQTT) {
        return;
    }
    mqtt_fsm_sig_t sig = { .type = MQTT_SIG_PROTOCOL_CLOSED };
    (void)send_fsm_sig(me, &sig);
}
```

(`clone_string` and `clone_payload` already exist in `mqtt_client.c` — verify by searching for their definitions.)

- [ ] **Step 5: Rename `handle_core_event` → `handle_lwlte_event`, listen on LWLTE_EVENT**

In `src/mqtt_client/mqtt_client.c`, find `handle_core_event` (currently line 254). Rewrite it to only handle `LWLTE_EVENT` net state changes (PROTOCOL_DATA/CLOSED are now handled by the callbacks in Step 4):

```c
static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    mqtt_client_handle_t *me = (mqtt_client_handle_t *)handler_arg;
    if (!me || event_base != LWLTE_EVENT) {
        return;
    }
    if (me->destroying) {
        return;
    }

    mqtt_fsm_sig_t sig = {0};
    switch ((lwlte_event_id_new_t)event_id) {
    case LWLTE_EVENT_NET_ONLINE_NEW:
        sig.type = MQTT_SIG_NET_ONLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    case LWLTE_EVENT_NET_OFFLINE_NEW:
        sig.type = MQTT_SIG_NET_OFFLINE;
        (void)send_fsm_sig(me, &sig);
        break;
    default:
        break;
    }
}
```

Delete the old `CORE_EVENT_PROTOCOL_DATA` / `CORE_EVENT_PROTOCOL_CLOSED` cases that were in `handle_core_event` — they are now in the protocol callbacks.

- [ ] **Step 6: Simplify `post_mqtt_event` to pure bus post**

Find `post_mqtt_event` in `src/mqtt_client/mqtt_client.c` (currently line 488). Rewrite it:

```c
static esp_err_t post_mqtt_event(mqtt_client_handle_t *me,
                                 lwlte_mqtt_event_id_new_t event_id,
                                 const mqtt_client_event_payload_t *payload)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    mqtt_client_event_payload_t empty_payload = {0};
    if (!payload) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        empty_payload.mqtt_state = me->state;
        xSemaphoreGive(me->lock);
        payload = &empty_payload;
    }

    esp_err_t ret = esp_event_post_to(me->config.event_loop, LWLTE_MQTT_EVENT,
                                      event_id, payload, sizeof(*payload), 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post mqtt event %d failed: %s", (int)event_id,
                 esp_err_to_name(ret));
    }
    return ret;
}
```

Delete the entire synchronous-callback invocation block (the old lines 508-536 that read `me->event_callback`, incremented `event_callback_active`, called the callback, etc.).

**Note on MQTT_DATA payload ownership:** When posting `LWLTE_MQTT_EVENT_DATA_NEW`, the caller (`handle_protocol_data`, currently line 903) must clone topic/payload into heap buffers owned by the payload before posting, because the bus is async. Update `handle_protocol_data`:

```c
static void handle_protocol_data(mqtt_client_handle_t *me, mqtt_fsm_sig_t *sig)
{
    mqtt_protocol_data_owned_t *owned = (mqtt_protocol_data_owned_t *)sig->data;
    if (!owned) {
        return;
    }

    /* Clone topic/payload for the async bus — the bus dispatch may happen
     * long after this FSM step returns and frees `owned`. */
    char *topic_copy = clone_string(owned->topic);
    uint8_t *payload_copy = clone_payload(owned->payload, owned->payload_len);
    if ((owned->topic && !topic_copy) ||
        (owned->payload && owned->payload_len > 0 && !payload_copy)) {
        free(topic_copy);
        free(payload_copy);
        ESP_LOGW(TAG, "handle_protocol_data: clone for bus failed, dropping");
        return;
    }

    mqtt_client_event_payload_t payload = {
        .mqtt_state    = MQTT_CLIENT_STATE_CONNECTED,
        .error_code    = 0,
        .msg = {
            .topic       = topic_copy,
            .topic_len   = owned->topic_len,
            .payload     = payload_copy,
            .payload_len = owned->payload_len,
        },
        .owns_payload  = true,
    };
    (void)post_mqtt_event(me, LWLTE_MQTT_EVENT_DATA_NEW, &payload);
}
```

The user releases `topic_copy` / `payload_copy` via `lwlte_mqtt_event_data_release_new`.

- [ ] **Step 7: Implement `lwlte_mqtt_event_data_release_new`**

This is a public function declared in `lwlte.h`. Implement it in `src/mqtt_client/mqtt_client.c` (or `src/lwlte/lwlte.c` — pick the layer that owns MQTT semantics; `lwlte.c` is the facade and is appropriate since this is a public API). Add to `src/lwlte/lwlte.c`:

```c
void lwlte_mqtt_event_data_release_new(lwlte_mqtt_event_data_new_t *data)
{
    if (!data || !data->owns_payload) {
        return;
    }
    free((void *)data->msg.topic);
    free((void *)data->msg.payload);
    data->msg.topic = NULL;
    data->msg.payload = NULL;
    data->msg.topic_len = 0;
    data->msg.payload_len = 0;
    data->owns_payload = false;
}
```

- [ ] **Step 8: Update `mqtt_client_create` — register callbacks instead of loop handler**

In `src/mqtt_client/mqtt_client.c`, find `mqtt_client_create` (currently line 923). Replace the `create_event_loop` + `esp_event_handler_register_with(core_get_event_loop(...), CORE_EVENT, ...)` block (lines ~959-970) with:

```c
    /* Register protocol callbacks with core (synchronous, private channel) */
    ret = core_register_protocol_callback(core, mqtt_protocol_data_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
    ret = core_register_protocol_closed_callback(core, mqtt_protocol_closed_cb, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }

    /* Register on the shared bus for net-state changes (async channel) */
    ret = esp_event_handler_register_with(me->config.event_loop, LWLTE_EVENT,
                                          LWLTE_EVENT_NET_ONLINE_NEW,
                                          handle_lwlte_event, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
    ret = esp_event_handler_register_with(me->config.event_loop, LWLTE_EVENT,
                                          LWLTE_EVENT_NET_OFFLINE_NEW,
                                          handle_lwlte_event, me);
    if (ret != ESP_OK) {
        cleanup_partial_client(me);
        return NULL;
    }
```

Also remove the `me->event_callback_done_sema = xSemaphoreCreateBinary()` line and its error check (lines ~952-954) from `mqtt_client_create`.

**Update `normalize_config`** (called in `mqtt_client_create`): it copies config fields into `me->config`. Make sure `event_loop` is copied: add `.event_loop = config->event_loop` to the normalized config, or if `normalize_config` does a struct copy, verify the field is included.

- [ ] **Step 9: Update `mqtt_client_destroy` — unregister bus handlers, clear callbacks**

In `src/mqtt_client/mqtt_client.c`, find `mqtt_client_destroy` (currently line 984). Replace the old `esp_event_handler_unregister_with(core_loop, CORE_EVENT, ...)` block (lines ~1005-1008) with:

```c
    /* Unregister shared-bus handlers (after FSM task stopped, before free) */
    (void)esp_event_handler_unregister_with(me->config.event_loop, LWLTE_EVENT,
                                            LWLTE_EVENT_NET_ONLINE_NEW,
                                            handle_lwlte_event);
    (void)esp_event_handler_unregister_with(me->config.event_loop, LWLTE_EVENT,
                                            LWLTE_EVENT_NET_OFFLINE_NEW,
                                            handle_lwlte_event);

    /* Clear protocol callbacks so core stops invoking us */
    if (me->core) {
        (void)core_register_protocol_callback(me->core, NULL, NULL);
        (void)core_register_protocol_closed_callback(me->core, NULL, NULL);
    }
```

Also remove the `me->event_callback_task` guard at the top of `mqtt_client_destroy` (lines ~991-993) and the `event_callback_done_sema` cleanup (lines ~1032-1034).

**Verify destroy ordering:** The `wait_stop_before_destroy(me)` call at the top must still run first (stops the FSM task), THEN unregister handlers. This matches the spec's ordering invariant: "stop FSM task THEN unregister handler".

- [ ] **Step 10: Delete `create_event_loop`, `destroy_event_loop`, `wait_event_callbacks_idle`, old `post_error_event` from mqtt_client.c**

Delete these static functions from `src/mqtt_client/mqtt_client.c`:
- `create_event_loop` (line 132)
- `destroy_event_loop` (line 147)
- `wait_event_callbacks_idle` (line 425)
- Their forward declarations in the STATIC PROTOTYPES section (lines 35, 36, 51)

Update `post_error_event` (line 541) to use the new event ID type: change `MQTT_CLIENT_EVENT_ERROR` → `LWLTE_MQTT_EVENT_ERROR_NEW` and the data struct type from `mqtt_client_event_data_t` → `mqtt_client_event_payload_t`.

- [ ] **Step 11: Replace all MQTT_CLIENT_EVENT_* with LWLTE_MQTT_EVENT_*_NEW**

Search `src/mqtt_client/mqtt_client.c` for every `MQTT_CLIENT_EVENT_STARTED`, `_STOPPED`, `_CONNECTING`, `_CONNECTED`, `_DISCONNECTED`, `_SUBSCRIBED`, `_UNSUBSCRIBED`, `_PUBLISHED`, `_DATA`, `_ERROR`. Replace with:

```
MQTT_CLIENT_EVENT_STARTED     → LWLTE_MQTT_EVENT_STARTED_NEW
MQTT_CLIENT_EVENT_STOPPED     → LWLTE_MQTT_EVENT_STOPPED_NEW
MQTT_CLIENT_EVENT_CONNECTING  → LWLTE_MQTT_EVENT_CONNECTING_NEW
MQTT_CLIENT_EVENT_CONNECTED   → LWLTE_MQTT_EVENT_CONNECTED_NEW
MQTT_CLIENT_EVENT_DISCONNECTED → LWLTE_MQTT_EVENT_DISCONNECTED_NEW
MQTT_CLIENT_EVENT_SUBSCRIBED  → LWLTE_MQTT_EVENT_SUBSCRIBED_NEW
MQTT_CLIENT_EVENT_UNSUBSCRIBED → LWLTE_MQTT_EVENT_UNSUBSCRIBED_NEW
MQTT_CLIENT_EVENT_PUBLISHED   → LWLTE_MQTT_EVENT_PUBLISHED_NEW
MQTT_CLIENT_EVENT_DATA        → LWLTE_MQTT_EVENT_DATA_NEW
MQTT_CLIENT_EVENT_ERROR       → LWLTE_MQTT_EVENT_ERROR_NEW
```

- [ ] **Step 12: Add `core_get_net_state` probe on MQTT_SIG_START**

In `src/mqtt_client/mqtt_client.c`, find the `MQTT_SIG_START` handling in the FSM (search for `case MQTT_SIG_START`). Add a net-state probe before transitioning to WAITING_NET:

```c
    case MQTT_SIG_START: {
        /* Late-subscriber probe: catch NET_ONLINE that fired before we existed */
        core_net_state_t ns = CORE_NET_STATE_OFFLINE;
        if (me->core) {
            (void)core_get_net_state(me->core, &ns);
        }
        if (ns == CORE_NET_STATE_ONLINE) {
            (void)begin_connect(me);
        } else {
            set_state(me, MQTT_CLIENT_STATE_WAITING_NET);
        }
        break;
    }
```

(Read the existing `MQTT_SIG_START` handler first — it may already set `WAITING_NET` or do other setup. Merge the probe into the existing logic rather than duplicating.)

- [ ] **Step 13: Verify build**

Run: `idf.py build`
Expected: PASS. Both core and mqtt_client now use the shared bus. The facade still uses the old callback API (which still exists), so there will be **linker errors** because `lwlte_handle_core_event` / `lwlte_handle_mqtt_event` reference deleted types. To get past this checkpoint, proceed directly to Task 4.

- [ ] **Step 14: Commit**

```bash
git add src/mqtt_client/ src/lwlte/lwlte.c
git commit -m "refactor(mqtt): borrow shared bus, protocol callbacks, post LWLTE_MQTT_EVENT"
```

---

## Task 4: Facade refactor — borrow loop, delete bridges, register internal ready handler

The facade stops translating events. It borrows the loop via config, passes it down to core/mqtt, registers one internal handler for `lwlte_wait_ready`, and deletes all callback-sync machinery.

**Files:**
- Modify: `src/include/lwlte.h`
- Modify: `src/lwlte/lwlte_priv.h`
- Modify: `src/lwlte/lwlte.c`
- Modify: `src/lwlte/lwlte_air780ep.c`
- Modify: `src/lwlte/lwlte_ml307r.c`

- [ ] **Step 1: Add `event_loop` field to both lwlte config structs**

In `src/include/lwlte.h`, add to `lwlte_air780ep_config_t` (after the last field before closing brace, currently around line 234):

```c
    esp_event_loop_handle_t event_loop;   /**< 可选事件总线，NULL 使用 default loop； Optional event loop, NULL uses default */
```

Add the same field to `lwlte_ml307r_config_t` (currently around line 272).

- [ ] **Step 2: Update `struct lwlte_handle` in lwlte_priv.h**

In `src/lwlte/lwlte_priv.h`, **delete** these fields from `struct lwlte_handle` (currently lines 49-62):

```c
    SemaphoreHandle_t callback_done_sema;
    lwlte_event_callback_t event_callback;
    void *event_user_ctx;
    int callback_active;
    int callback_task_overflow;
    TaskHandle_t callback_tasks[LWLTE_CALLBACK_TASKS_MAX];
    int callback_task_counts[LWLTE_CALLBACK_TASKS_MAX];
    bool callback_waiting;
```

**Add** the borrowed loop field:

```c
    esp_event_loop_handle_t event_loop;
```

Also delete `#define LWLTE_CALLBACK_TASKS_MAX 4` (line 34) — no longer used.

The resulting struct:
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
    esp_event_loop_handle_t event_loop;
    int init_error_code;
    int active_api_calls;
    int ready_waiter_count;
    bool ready;
    bool init_failed;
    bool destroying;
};
```

- [ ] **Step 3: Delete translator bridge declarations from lwlte_priv.h**

In `src/lwlte/lwlte_priv.h`, delete the declarations of `lwlte_handle_core_event` and `lwlte_handle_mqtt_event` (currently lines 71-76). They no longer exist.

- [ ] **Step 4: Resolve loop + pass to core in factory**

In `src/lwlte/lwlte_air780ep.c`, find the core_config construction (currently lines 159-167) and the `core_register_event_callback` call (line 175).

Replace the core_config block + callback registration with:

```c
    /* Resolve shared event bus: NULL → default loop (must already exist) */
    esp_event_loop_handle_t loop = config->event_loop;
    if (loop == NULL) {
        loop = esp_event_loop_get_default();
    }
    ESP_RETURN_ON_FALSE(loop, ESP_ERR_INVALID_STATE, TAG,
                        "no event loop: pass config->event_loop or create default loop");
    me->event_loop = loop;

    const core_config_t core_config = {
        .apn = config->apn ? config->apn : "",
        .primary_cid = config->primary_cid,
        .event_loop = me->event_loop,
        .net_activate_timeout_ms = config->net_activate_timeout_ms,
        .reconnect_delay_ms = config->reconnect_delay_ms,
        .fsm_queue_size = config->core_fsm_queue_size,
        .fsm_task_stack = config->core_fsm_task_stack,
        .fsm_task_priority = config->core_fsm_task_priority,
    };
    me->core = core_create(&core_config, me->modem);
    if (!me->core) {
        ESP_LOGE(TAG, "create core failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    /* No core_register_event_callback — core posts directly to the bus */
```

Delete the `core_register_event_callback(me->core, lwlte_handle_core_event, me)` block entirely.

Apply the **same change** to `src/lwlte/lwlte_ml307r.c` (find its core_config + callback registration, apply the same pattern).

- [ ] **Step 5: Update `lwlte_mqtt_init` to pass loop + remove callback registration**

In `src/lwlte/lwlte.c`, find `lwlte_mqtt_init` (currently line 507). In the `mqtt_client_config_t` literal (lines 534-546), add:

```c
        .event_loop = me->event_loop,
```

Delete the `mqtt_client_register_event_callback(mqtt, lwlte_handle_mqtt_event, me)` block (lines 554-560) — mqtt_client now posts directly to the bus.

- [ ] **Step 6: Add facade internal ready handler**

In `src/lwlte/lwlte.c`, add this static function (place near the top, after the STATIC PROTOTYPES section):

```c
static void facade_ready_handler(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    lwlte_handle_t *me = (lwlte_handle_t *)arg;
    if (!me || !me->lock || base != LWLTE_EVENT) {
        return;
    }
    const lwlte_event_data_new_t *ev = data;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return;
    }
    if ((lwlte_event_id_new_t)id == LWLTE_EVENT_READY_NEW) {
        me->ready = true;
        wake_ready_waiters_locked(me);
    } else if ((lwlte_event_id_new_t)id == LWLTE_EVENT_ERROR_NEW && !me->ready) {
        me->init_failed = true;
        me->init_error_code = ev ? ev->error_code : ESP_FAIL;
        wake_ready_waiters_locked(me);
    }
    xSemaphoreGive(me->lock);
}
```

Register it in the factory (both `lwlte_air780ep.c` and `lwlte_ml307r.c`), after `me->core` is created:

```c
    ret = esp_event_handler_register_with(me->event_loop, LWLTE_EVENT,
                                          LWLTE_EVENT_READY_NEW,
                                          facade_ready_handler, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register ready handler failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }
    ret = esp_event_handler_register_with(me->event_loop, LWLTE_EVENT,
                                          LWLTE_EVENT_ERROR_NEW,
                                          facade_ready_handler, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register error handler failed: %s", esp_err_to_name(ret));
        return cleanup_after_failure(me, ret);
    }
```

Add a forward declaration for `facade_ready_handler` in `src/lwlte/lwlte_priv.h` (internal, so the factory files can reference it):

```c
void facade_ready_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data);
```

- [ ] **Step 7: Delete translator bridges and all callback-sync machinery from lwlte.c**

In `src/lwlte/lwlte.c`, delete:
- `lwlte_handle_core_event` (currently line 742)
- `lwlte_handle_mqtt_event` (currently line 798)
- `map_core_event` (line 909)
- `map_mqtt_event` (line 953)
- `map_core_event_data` (line 980)
- `wait_callbacks_idle` (line 1088)
- `callback_task_active_locked` (line 1132)
- `add_callback_task_locked` (line 1147)
- `remove_callback_task_locked` (line 1172)
- All their forward declarations in the STATIC PROTOTYPES section (lines ~134-173)
- `lwlte_register_event_callback` (line 311) and its forward declaration

- [ ] **Step 8: Simplify `lwlte_destroy`**

In `src/lwlte/lwlte.c`, find `lwlte_destroy` (currently line 253). Remove:
- The `callback_active` guard at the top (lines ~263-267)
- The `wait_callbacks_idle(me, false)` call (line 278)
- The `callback_done_sema` cleanup (lines ~290-293)

Add, after `destroy_owned_resources(me)` succeeds and before freeing semaphores, the unregister of the internal handler:

```c
    /* Unregister internal bus handler (after resources destroyed) */
    if (me->event_loop) {
        (void)esp_event_handler_unregister_with(me->event_loop, LWLTE_EVENT,
                                                LWLTE_EVENT_READY_NEW,
                                                facade_ready_handler);
        (void)esp_event_handler_unregister_with(me->event_loop, LWLTE_EVENT,
                                                LWLTE_EVENT_ERROR_NEW,
                                                facade_ready_handler);
    }
```

Remove the `callback_done_sema` field references in `lwlte_create_empty` (lines ~240-247): delete the `xSemaphoreCreateBinary` for `callback_done_sema` and its error-cleanup chain.

- [ ] **Step 9: Verify build**

Run: `idf.py build`
Expected: PASS. The full stack now compiles. The old `lwlte_register_event_callback` / `lwlte_event_id_t` / `lwlte_event_data_t` are still declared in `lwlte.h` but no longer used internally — they will be deleted in Task 5.

- [ ] **Step 10: Commit**

```bash
git add src/include/lwlte.h src/lwlte/
git commit -m "refactor(lwlte): borrow shared bus, delete callback machinery, add ready handler"
```

---

## Task 5: Delete old public API and rename `_NEW` types to final names

This is the breaking-change cutover. The `_NEW` suffix is stripped, and the old types/APIs are deleted.

**Files:**
- Modify: `src/include/lwlte.h`

- [ ] **Step 1: Delete old types from lwlte.h**

In `src/include/lwlte.h`, delete:
- The old `lwlte_event_id_t` enum (currently lines 130-149, the one containing `LWLTE_EVENT_MQTT_*`)
- The old `lwlte_event_data_t` struct (currently lines 155-162, with `net_state` + `mqtt_state` + `mqtt_msg` union)
- The old `lwlte_event_callback_t` typedef (currently lines 173-176)
- The old `lwlte_register_event_callback` declaration (currently lines 343-345)

- [ ] **Step 2: Rename all `_NEW` types and constants to final names**

In `src/include/lwlte.h`, do a global rename (use replaceAll or equivalent):

```
lwlte_event_id_new_t           → lwlte_event_id_t
lwlte_mqtt_event_id_new_t      → lwlte_mqtt_event_id_t
lwlte_event_data_new_t         → lwlte_event_data_t
lwlte_mqtt_event_data_new_t    → lwlte_mqtt_event_data_t
lwlte_mqtt_event_data_release_new → lwlte_mqtt_event_data_release
LWLTE_EVENT_STARTED_NEW        → LWLTE_EVENT_STARTED
LWLTE_EVENT_READY_NEW          → LWLTE_EVENT_READY
LWLTE_EVENT_NET_CONNECTING_NEW → LWLTE_EVENT_NET_CONNECTING
LWLTE_EVENT_NET_ONLINE_NEW     → LWLTE_EVENT_NET_ONLINE
LWLTE_EVENT_NET_OFFLINE_NEW    → LWLTE_EVENT_NET_OFFLINE
LWLTE_EVENT_NET_ERROR_NEW      → LWLTE_EVENT_NET_ERROR
LWLTE_EVENT_STOPPED_NEW        → LWLTE_EVENT_STOPPED
LWLTE_EVENT_ERROR_NEW          → LWLTE_EVENT_ERROR
LWLTE_MQTT_EVENT_STARTED_NEW   → LWLTE_MQTT_EVENT_STARTED
LWLTE_MQTT_EVENT_STOPPED_NEW   → LWLTE_MQTT_EVENT_STOPPED
LWLTE_MQTT_EVENT_CONNECTING_NEW → LWLTE_MQTT_EVENT_CONNECTING
LWLTE_MQTT_EVENT_CONNECTED_NEW → LWLTE_MQTT_EVENT_CONNECTED
LWLTE_MQTT_EVENT_DISCONNECTED_NEW → LWLTE_MQTT_EVENT_DISCONNECTED
LWLTE_MQTT_EVENT_SUBSCRIBED_NEW → LWLTE_MQTT_EVENT_SUBSCRIBED
LWLTE_MQTT_EVENT_UNSUBSCRIBED_NEW → LWLTE_MQTT_EVENT_UNSUBSCRIBED
LWLTE_MQTT_EVENT_PUBLISHED_NEW → LWLTE_MQTT_EVENT_PUBLISHED
LWLTE_MQTT_EVENT_DATA_NEW      → LWLTE_MQTT_EVENT_DATA
LWLTE_MQTT_EVENT_ERROR_NEW     → LWLTE_MQTT_EVENT_ERROR
```

Apply the same rename across the **entire** `src/` tree:
- `src/core/core.h`, `src/core/core_priv.h`, `src/core/core.c`, `src/core/core_fsm.c`, `src/core/net_mgr.c`
- `src/mqtt_client/mqtt_client.h`, `src/mqtt_client/mqtt_client_priv.h`, `src/mqtt_client/mqtt_client.c`
- `src/lwlte/lwlte_priv.h`, `src/lwlte/lwlte.c`, `src/lwlte/lwlte_air780ep.c`, `src/lwlte/lwlte_ml307r.c`

- [ ] **Step 3: Verify build**

Run: `idf.py build`
Expected: PASS for `src/`. Examples (`example/*.c`) and host tests will FAIL — they still reference the old API. That is fixed in Task 6.

- [ ] **Step 4: Commit**

```bash
git add src/
git commit -m "refactor(event): delete old callback API, finalize event bus contract names"
```

---

## Task 6: Update examples, host tests, and docs

Bring all consumers in line with the new API.

**Files:**
- Modify: `example/air780ep_mqtt_client.c`
- Modify: `example/ml307r_mqtt_client.c`
- Modify: `example/air780ep_basic_connect.c`
- Modify: `example/ml307r_basic_connect.c`
- Modify: `tests/host/test_mqtt_end_to_end_contract.py`
- Modify: `tests/host/test_lwlte_start_lifecycle.py`
- Modify: `tests/host/test_ml307r_contract.py`
- Modify: `tests/host/test_ml307r_examples_contract.py`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/err.md`
- Modify: `docs/agents/oop-design.md`
- Modify: `AGENTS.md` and `AGENTS_ZH.md` (sync if needed)

- [ ] **Step 1: Update the four examples — replace callback registration**

For each example (`example/*_basic_connect.c` and `example/*_mqtt_client.c`):

**1a. Add `esp_event_loop_create_default()` to `app_main`** (near the top, before `lwlte_*_init`):

```c
    ESP_ERROR_CHECK(esp_event_loop_create_default());
```

**1b. Add `.event_loop = NULL` to the config literal** (e.g. `lwlte_air780ep_config_t`):

```c
    lwlte_air780ep_config_t config = {
        /* ... existing fields ... */
        .event_loop = NULL,
    };
```

**1c. Replace `lwlte_register_event_callback(lte, lte_event_cb, NULL)`** with:

```c
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                               lwlte_event_cb, NULL));
```

For mqtt_client examples, also add:
```c
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_MQTT_EVENT, ESP_EVENT_ANY_ID,
                                               mqtt_event_cb, NULL));
```

**1d. Change callback signatures** from the old `lwlte_event_callback_t` shape to the standard `esp_event_handler_t`:

```c
/* Old */
static void lte_event_cb(lwlte_handle_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *ctx);

/* New */
static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    const lwlte_event_data_t *data = event_data;
    switch ((lwlte_event_id_t)event_id) {
    case LWLTE_EVENT_NET_ONLINE:
        /* ... */
        break;
    /* ... */
    default:
        break;
    }
}

/* mqtt_client examples also get: */
static void mqtt_event_cb(void *arg, esp_event_base_t base,
                          int32_t event_id, void *event_data)
{
    lwlte_mqtt_event_data_t *data = event_data;
    switch ((lwlte_mqtt_event_id_t)event_id) {
    case LWLTE_MQTT_EVENT_DATA:
        /* consume data->msg.topic / data->msg.payload */
        lwlte_mqtt_event_data_release(data);
        break;
    case LWLTE_MQTT_EVENT_CONNECTED:
        /* ... */
        break;
    /* ... */
    default:
        break;
    }
}
```

For each example, read its current callback body and migrate the switch cases. The event ID names changed (e.g. `LWLTE_EVENT_MQTT_CONNECTED` → `LWLTE_MQTT_EVENT_CONNECTED`).

- [ ] **Step 2: Verify examples build**

Run: `idf.py build`
Expected: PASS (all four examples compile)

- [ ] **Step 3: Update host contract tests**

In `tests/host/test_mqtt_end_to_end_contract.py`:

- Delete or rewrite `test_facade_tracks_active_callback_tasks_individually` (currently lines 825-853) — the callback-task tracking no longer exists. Replace with assertions that the callback machinery is **absent**:

```python
    def test_facade_has_no_callback_machinery(self):
        """The old callback slot and sync machinery must be deleted."""
        self.assertNotIn("callback_done_sema", self.lwlte_priv)
        self.assertNotIn("event_callback", self.lwlte_priv)
        self.assertNotIn("callback_active", self.lwlte_priv)
        self.assertNotIn("lwlte_register_event_callback", self.lwlte_c)
        self.assertNotIn("lwlte_handle_core_event", self.lwlte_c)
        self.assertNotIn("lwlte_handle_mqtt_event", self.lwlte_c)
        self.assertNotIn("wait_callbacks_idle", self.lwlte_c)
```

- Delete the API-ordering assertions that reference `lwlte_register_event_callback` (currently lines 831-833).
- Add contracts asserting the new API exists:

```python
    def test_new_event_bus_contract_exists(self):
        self.assertIn("ESP_EVENT_DECLARE_BASE(LWLTE_EVENT)", self.lwlte_h)
        self.assertIn("ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT)", self.lwlte_h)
        self.assertIn("lwlte_mqtt_event_data_release", self.lwlte_h)

    def test_core_has_protocol_callback_api(self):
        self.assertIn("core_register_protocol_callback", self.core_h)
        self.assertIn("core_register_protocol_closed_callback", self.core_h)

    def test_core_has_no_private_event_loop(self):
        self.assertNotIn("create_event_loop", self.core_c)
        self.assertNotIn("core_event_adapter", self.core_c)
        self.assertNotIn("CORE_EVENT_QUEUE_SIZE", self.core_priv)

    def test_mqtt_client_has_no_private_event_loop(self):
        self.assertNotIn("create_event_loop", self.mqtt_c)
        self.assertNotIn("mqtt_client_register_event_callback", self.mqtt_h)
        self.assertNotIn("mqtt_client_get_event_loop", self.mqtt_h)

    def test_event_loop_field_in_configs(self):
        self.assertIn("event_loop", self.lwlte_h)  # appears in both config structs
```

Update the file-path constants at the top of the test file to read all the source files needed (most are already defined).

Apply analogous changes to `tests/host/test_lwlte_start_lifecycle.py` and `tests/host/test_ml307r_*` — any assertion referencing `lwlte_register_event_callback` or the callback-sync fields must be updated or deleted.

- [ ] **Step 4: Run all host tests**

Run: `python3 -m unittest discover -s tests/host -v`
Expected: all PASS

- [ ] **Step 5: Update architecture.md**

In `docs/agents/architecture.md`:

- §6.1 init chain: remove the `core_register_event_callback(core, facade_core_event_handler, lte)` step. Add `event_loop` to the config literal. Change the sequence to: `lwlte_*_init` → `esp_event_handler_register(LWLTE_EVENT, ...)` → (optional) `lwlte_mqtt_init` → `lwlte_start`.
- Add a new subsection "## Event Bus" with the architecture diagram from the spec (`docs/superpowers/specs/2026-06-14-event-bus-refactor-design.md` — copy the ASCII diagram).
- Add a new subsection "## Protocol Private Callback" explaining that PROTOCOL_DATA/CLOSED flow over `core_register_protocol_callback`, not the bus.

- [ ] **Step 6: Update classes.md, err.md, oop-design.md**

- `docs/agents/classes.md`: delete `lwlte_event_callback_t`, `lwlte_register_event_callback`. Add entries for `LWLTE_EVENT`, `LWLTE_MQTT_EVENT`, `lwlte_event_id_t`, `lwlte_mqtt_event_id_t`, `lwlte_mqtt_event_data_release`.
- `docs/agents/err.md`: add a note that `error_code` is a diagnostic number (may be CME/CMS or esp_err_t), callers must not branch on it.
- `docs/agents/oop-design.md`: replace any `lwlte_register_event_callback(g_lte, app_event_handler, NULL)` sample with the `esp_event_handler_register` equivalent.

- [ ] **Step 7: Verify full build + host tests**

Run: `idf.py build && python3 -m unittest discover -s tests/host -v`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add example/ tests/ docs/
git commit -m "docs+examples+tests: migrate to shared event bus API"
```

---

## Task 7: Optional cleanup

- [ ] **Step 1: Check `esp_event_post_to` return values**

Search all call sites of `esp_event_post_to` in `src/`. Each should log a warning on failure (the implementations in Tasks 2-3 already do this for core and mqtt_client). Verify no call site silently swallows the return value.

- [ ] **Step 2: Remove any dead includes / defines left from the refactor**

Search `src/` for now-unused references: `core_event_data_t`, `core_event_callback_t`, `mqtt_client_event_data_t`, `MQTT_CLIENT_EVENT`, `CORE_EVENT`, `core_get_event_loop`, `mqtt_client_get_event_loop`. Delete any lingering include or forward declaration.

- [ ] **Step 3: Final build + test**

Run: `idf.py build && python3 -m unittest discover -s tests/host -v`
Expected: PASS

- [ ] **Step 4: Commit (if any changes)**

```bash
git add -A
git commit -m "chore: cleanup dead references after event bus refactor"
```

---

## Device Verification Checklist (after all tasks)

These require hardware (Air780EP or ML307R). Run after the host-side work is green:

- [ ] **Flash + run `air780ep_basic_connect`**: verify `LWLTE_EVENT_NET_ONLINE` reaches the user handler, log shows the expected state transitions.
- [ ] **Flash + run `air780ep_mqtt_client`**: verify `LWLTE_MQTT_EVENT_CONNECTED` then `LWLTE_MQTT_EVENT_DATA` on a publish/subscribe round-trip; call `lwlte_mqtt_event_data_release` in the handler; verify no heap leak over many messages (enable heap tracing in menuconfig if needed).
- [ ] **Flash + run `ml307r_mqtt_client`**: same as above.
- [ ] **Late subscriber test**: start LTE, wait for NET_ONLINE, **then** call `lwlte_mqtt_init` + `lwlte_mqtt_start`. Verify MQTT reaches CONNECTED (the `core_get_net_state` probe path).
- [ ] **Destroy test**: while MQTT is connected, call `lwlte_destroy`. Verify no crash, no use-after-free in serial log. Verify internal handlers are not dispatched after destroy.
