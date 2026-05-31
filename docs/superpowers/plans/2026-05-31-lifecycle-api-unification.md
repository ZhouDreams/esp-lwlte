# Lifecycle API Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unify lifecycle semantics so user APIs keep `esp_err_t init(config, out)`, heap objects use `create/destroy`, and embedded/base/composed objects use `init/deinit` with `esp_err_t` cleanup results.

**Architecture:** Keep public API compatibility and make the smallest code change that enforces the lifecycle boundary. `core_create()` will allocate a heap object and delegate initialization to private `core_init()`, while `core_destroy()` delegates cleanup to private `core_deinit()` before freeing memory.

**Tech Stack:** C, ESP-IDF, FreeRTOS, ESP Event, existing `esp_err_t` error handling macros.

---

## File Structure

- Modify: `src/core/core.c` - split Core allocation from initialization by adding private `core_init()` and `core_deinit()` helpers and updating `core_create()` / `core_destroy()` to use them.
- Modify: `src/core/core.h` - update `core_create()` return docs to mention initialization failure, not only invalid argument or no memory.
- Modify: `docs/agents/oop-design.md` - document this repository's lifecycle naming rule and keep examples aligned with `esp_err_t destroy/deinit`.
- Verify: `src/lwlte/lwlte_air780ep.c` - no user API shape change; call chain should remain `lwlte_air780ep_init()` -> `*_create()` / `modem_start()`.
- Verify: `src/lwlte/lwlte.c` - existing `destroy_owned_resources()` should continue calling `*_destroy()` in reverse dependency order.

Do not commit during execution unless the user explicitly asks for a commit.

---

### Task 1: Add Core Init/Deinit Boundary

**Files:**
- Modify: `src/core/core.c:135-145`
- Modify: `src/core/core.c:169-331`
- Modify: `src/core/core.c:877-907`

- [ ] **Step 1: Add private prototypes**

Replace the existing `cleanup_core()` prototype block with explicit lifecycle helpers:

```c
/**
 * @brief 初始化 Core 对象
 * @details Initialize Core object
 * @param[in] me LTE 核心服务对象
 * @param[in] config LTE 核心服务配置
 * @param[in] modem 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - other: ESP Event 错误码
 */
static esp_err_t core_init(core_t *me, const core_config_t *config,
                           modem_t *modem);

/**
 * @brief 反初始化 Core 对象
 * @details Deinitialize Core object
 * @param[in] me LTE 核心服务对象
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - other: ESP Event 错误码
 */
static esp_err_t core_deinit(core_t *me);
```

- [ ] **Step 2: Refactor `core_create()` to allocate only and call `core_init()`**

Replace `core_create()` with:

```c
core_t *core_create(const core_config_t *config, modem_t *modem)
{
    core_t *me = calloc(1, sizeof(core_t));
    if (!me) {
        ESP_LOGE(TAG, "calloc core failed");
        return NULL;
    }

    esp_err_t ret = core_init(me, config, modem);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init core failed: %s", esp_err_to_name(ret));
        free(me);
        return NULL;
    }

    return me;
}
```

- [ ] **Step 3: Refactor `core_destroy()` to call `core_deinit()`**

In `core_destroy()`, replace:

```c
    ret = cleanup_core(me);
```

with:

```c
    ret = core_deinit(me);
```

Keep the rest of the destroy state guards unchanged.

- [ ] **Step 4: Implement private `core_init()` from old `core_create()` body**

Insert this static function before `core_deinit()`:

```c
static esp_err_t core_init(core_t *me, const core_config_t *config,
                           modem_t *modem)
{
    esp_err_t ret = ESP_OK;
    esp_err_t cleanup_ret = ESP_OK;

    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(config_valid(config, modem), ESP_ERR_INVALID_ARG, TAG,
                        "invalid core config");

    ret = normalize_config(config, &me->config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "normalize core config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    me->modem = modem;
    me->state = CORE_STATE_STOPPED;
    me->destroying = false;
    me->destroy_in_progress = false;
    me->event_loop_task = NULL;
    me->event_callback_done_sema = NULL;
    me->event_callback_task = NULL;
    me->event_callback_active = 0;
    me->event_callback_waiting = false;

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        ESP_LOGE(TAG, "create lock failed");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    me->event_callback_done_sema = xSemaphoreCreateBinary();
    if (!me->event_callback_done_sema) {
        ESP_LOGE(TAG, "create event_callback_done_sema failed");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    ret = create_event_loop(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create event loop failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = pdp_mgr_init(&me->pdp_mgr, me->config.primary_cid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init PDP manager failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = net_mgr_init(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init net manager failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = core_fsm_init(me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init core FSM failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = modem_register_event_callback(modem, core_modem_event_cb, me);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register modem callback failed: %s", esp_err_to_name(ret));
        goto err;
    }

    return ESP_OK;

err:
    cleanup_ret = core_deinit(me);
    if (cleanup_ret != ESP_OK) {
        ESP_LOGE(TAG, "cleanup after init failure failed: %s",
                 esp_err_to_name(cleanup_ret));
    }
    return ret;
}
```

- [ ] **Step 5: Rename `cleanup_core()` to `core_deinit()`**

Replace:

```c
static esp_err_t cleanup_core(core_t *me)
```

with:

```c
static esp_err_t core_deinit(core_t *me)
```

Do not change the body in this step.

- [ ] **Step 6: Verify no `cleanup_core` references remain**

Run: `rg "cleanup_core" src/core/core.c`

Expected: no output.

---

### Task 2: Update Lifecycle Documentation

**Files:**
- Modify: `src/core/core.h:252-272`
- Modify: `docs/agents/oop-design.md` near section `1.2 句柄模式（opaque pointer）`

- [ ] **Step 1: Clarify `core_create()` docs**

In `src/core/core.h`, replace the `core_create()` return section with:

```c
 * @return
 *         - 非 NULL: 创建并初始化成功，返回 LTE 核心服务句柄
 *         - NULL: 参数无效、内存不足或初始化失败
 */
```

- [ ] **Step 2: Add lifecycle naming rules to OOP design docs**

In `docs/agents/oop-design.md`, after the opaque pointer example around lines 48-53, add:

```markdown
### 1.2.1 生命周期命名规则

本项目统一使用以下生命周期语义：

- 用户门面 API 使用 `esp_err_t xxx_init(const xxx_config_t *config, xxx_t **out)`，例如 `lwlte_air780ep_init()`。
- 独立 opaque 堆对象使用 `xxx_create()` / `xxx_destroy()`；`create` 返回对象指针，失败返回 `NULL`，`destroy` 返回 `esp_err_t`。
- 内嵌对象、基类对象、组合成员使用 `xxx_init(me, ...)` / `xxx_deinit(me)`；二者都返回 `esp_err_t`，不负责分配或释放 `me` 自身。
- `create` 内部可以调用私有或层内 `init`；初始化失败时必须按已成功初始化的反序调用对应 `deinit`，再释放堆内存。
- `destroy` 内部必须调用对应 `deinit`，再释放堆内存；如果清理阶段出现错误，返回第一个错误码。
```

- [ ] **Step 3: Verify docs do not claim destroy/deinit are void**

Run: `rg "destroy.*void|deinit.*void|void .*destroy|void .*deinit" docs/agents src`

Expected: no lifecycle rule claiming project-wide `destroy/deinit` should be `void`. Existing unrelated static helper functions with `void` return are acceptable only when they are not object lifecycle pairs.

---

### Task 3: Build and Lifecycle Verification

**Files:**
- Verify: `src/core/core.c`
- Verify: `src/include/lwlte_air780ep.h`
- Verify: `examples/basic_connect/main/main.c`
- Verify: `examples/mqtt_client/main/main.c`

- [ ] **Step 1: Search lifecycle symbols**

Run: `rg "\\b[a-zA-Z0-9_]+_(create|destroy|init|deinit)\\b" src examples main`

Expected: `lwlte_air780ep_init()` remains the user API; heap services still expose `*_create()` / `*_destroy()`; embedded helpers use `*_init()` / `*_deinit()`.

- [ ] **Step 2: Inspect Core pairing manually**

Confirm these pairs in `src/core/core.c`:

```c
core_create() -> core_init() -> core_deinit() on failure -> free(me)
core_destroy() -> core_deinit() -> free(me)
core_init() resource failure -> core_deinit(me)
```

- [ ] **Step 3: Build the ESP-IDF project**

Run: `idf.py build`

Expected: build succeeds with no compile errors from the refactor.

- [ ] **Step 4: If ESP-IDF command is unavailable, use component build tool**

Run through the available project build integration instead of shell-only guessing.

Expected: build succeeds, or the failure is an environment/toolchain issue unrelated to lifecycle code.

---

## Self-Review Notes

- Spec coverage: user API shape, heap object lifecycle, embedded `init/deinit`, `esp_err_t` cleanup returns, and strict pairing are covered by Tasks 1-3.
- Placeholder scan: no incomplete markers or unspecified implementation steps remain.
- Type consistency: plan uses existing `core_t`, `core_config_t`, `modem_t`, `esp_err_t`, and existing Core helper names.
