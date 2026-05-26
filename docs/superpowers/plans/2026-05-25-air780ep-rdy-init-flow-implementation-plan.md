# Air780EP RDY Init Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Air780EP initialization register URCs before EN reset, wait for `RDY` instead of fixed boot delay, and skip APN configuration when APN is empty.

**Architecture:** The public Air780EP facade remains the composition root and passes one normalized init timeout down to the modem. The Air780EP modem owns RDY synchronization and only reports modem ready after RDY plus AT initialization commands succeed. Core keeps APN as a copied string, while net manager treats an empty APN as “do not configure APN”.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS semaphores/tasks, ESP-IDF UART/GPIO drivers, C component build with `idf.py`.

---

## Repository Constraints

- Work directly in `/Users/jovisdreams/Projects/esp-lwlte`; do not create a worktree because the repository instructions say not to proactively create worktrees.
- The workspace already contains uncommitted changes. Do not revert user changes. Only edit the files listed in this plan.
- Do not commit unless the user explicitly asks. Commit steps are intentionally omitted to comply with repository policy.
- Use `apply_patch` for manual edits.

## File Structure

- `src/include/lwlte_air780ep.h`: public Air780EP facade config; remove `modem_boot_wait_ms`, tighten APN and timeout docs.
- `src/lwlte/lwlte_air780ep.c`: facade init orchestration; compute one init deadline, pass remaining RDY timeout to modem, then wait Core ready with remaining time.
- `src/modem/modem_air780ep.h`: private Air780EP modem config; replace `boot_wait_ms` with `ready_timeout_ms`.
- `src/modem/modem_air780ep.c`: Air780EP modem implementation; add RDY semaphore/state, reorder init/reset, simplify `hardware_reset()`.
- `src/core/net_mgr.c`: network activation; skip `modem_set_apn()` when APN is empty.
- `docs/agents/classes.md`: update Air780EP config and init-flow docs.
- `docs/agents/architecture.md`: update facade/modem snippet if it still references boot wait semantics.
- `examples/basic_connect/README.md`: document RDY-based init and empty APN behavior.
- `docs/superpowers/specs/2026-05-25-air780ep-pin-simplify-design.md`: mark boot-wait details as superseded by the RDY init-flow spec.

---

### Task 1: Public and Private Config Cleanup

**Files:**
- Modify: `src/include/lwlte_air780ep.h`
- Modify: `src/modem/modem_air780ep.h`
- Modify: `src/lwlte/lwlte_air780ep.c`

- [ ] **Step 1: Capture the current failing static check**

Run:

```bash
git grep -n "modem_boot_wait_ms\|boot_wait_ms" -- src/include/lwlte_air780ep.h src/modem/modem_air780ep.h src/lwlte/lwlte_air780ep.c
```

Expected before this task: output includes `modem_boot_wait_ms` and `boot_wait_ms`, proving the obsolete boot-wait API still exists.

- [ ] **Step 2: Update the public Air780EP facade config**

In `src/include/lwlte_air780ep.h`, replace the config notes and remove the `modem_boot_wait_ms` field so this part reads:

```c
 * @note 超时、任务和缓冲区字段为 0 时使用下层默认值；init_ready_timeout_ms 为 0 时使用门面默认值。
 * @note init_ready_timeout_ms 覆盖 Air780EP RDY 等待和 Core ready 等待的初始化总超时。
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
```

```c
    const char *apn;                      /**< 可选 APN，NULL/空表示门面不配置； Optional APN, NULL/empty means facade does not configure it */
    uint8_t primary_cid;                  /**< 必填主 PDP 上下文 ID，Air780EP 门面当前仅支持 1； Required primary PDP context ID, Air780EP facade currently supports 1 only */
    bool auto_connect;                    /**< ready 后是否自动提交联网请求，不等待网络上线； Whether to submit connect after ready, without waiting online */
    uint32_t init_ready_timeout_ms;        /**< 初始化 RDY+Core ready 总超时，0 使用门面默认值； Total init RDY+Core ready timeout, 0 uses facade default */
    uint32_t net_activate_timeout_ms;      /**< 网络激活总超时，0 使用 Core 默认值； Network activation timeout, 0 uses Core default */
```

Keep `modem_reset_pulse_ms` and delete only this line:

```c
    uint32_t modem_boot_wait_ms;           /**< Modem 复位后启动等待，0 表示不额外等待； Modem boot wait after reset, 0 skips extra wait */
```

- [ ] **Step 3: Update the private Air780EP modem config**

In `src/modem/modem_air780ep.h`, replace the struct fields with:

```c
typedef struct {
    gpio_num_t en_pin;                  /**< EN GPIO，GPIO_NUM_NC 表示不控制； EN GPIO, GPIO_NUM_NC disables control */
    uint32_t reset_pulse_ms;            /**< 复位脉冲时间(EN 拉低保持时长)； Reset pulse time (EN low hold duration) */
    uint32_t ready_timeout_ms;          /**< 等待 RDY URC 超时； RDY URC wait timeout */
    uint32_t default_cmd_timeout_ms;    /**< 默认命令超时； Default command timeout */
    int event_queue_size;               /**< 事件队列长度； Event queue size */
    int event_task_stack;               /**< 事件任务栈大小； Event task stack size */
    int event_task_priority;            /**< 事件任务优先级； Event task priority */
} modem_air780ep_config_t;
```

- [ ] **Step 4: Add facade timeout helpers**

In `src/lwlte/lwlte_air780ep.c`, add prototypes after `non_negative_int()`:

```c
static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config);
static esp_err_t remaining_timeout_ms(TickType_t start_tick,
                                      uint32_t total_timeout_ms,
                                      uint32_t *out_timeout_ms);
```

Add the implementations before `cleanup_after_failure()`:

```c
static uint32_t ready_timeout_ms(const lwlte_air780ep_config_t *config)
{
    if (config && config->init_ready_timeout_ms > 0) {
        return config->init_ready_timeout_ms;
    }

    return LWLTE_AIR780EP_DEFAULT_READY_MS;
}

static esp_err_t remaining_timeout_ms(TickType_t start_tick,
                                      uint32_t total_timeout_ms,
                                      uint32_t *out_timeout_ms)
{
    ESP_RETURN_ON_FALSE(out_timeout_ms, ESP_ERR_INVALID_ARG, TAG,
                        "out_timeout_ms is NULL");

    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    uint32_t elapsed_ms = (uint32_t)(elapsed_ticks * portTICK_PERIOD_MS);
    if (elapsed_ms >= total_timeout_ms) {
        *out_timeout_ms = 0;
        return ESP_ERR_TIMEOUT;
    }

    *out_timeout_ms = total_timeout_ms - elapsed_ms;
    if (*out_timeout_ms == 0) {
        *out_timeout_ms = 1;
    }

    return ESP_OK;
}
```

- [ ] **Step 5: Pass remaining RDY timeout to modem and remaining Core timeout to wait_ready**

In `lwlte_air780ep_init()`, immediately after config validation, add:

```c
    const uint32_t total_ready_timeout_ms = ready_timeout_ms(config);
    const TickType_t init_start_tick = xTaskGetTickCount();
```

Before constructing `modem_air780ep_config_t`, compute the current remaining timeout:

```c
    uint32_t stage_timeout_ms = 0;
    ret = remaining_timeout_ms(init_start_tick, total_ready_timeout_ms,
                               &stage_timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init timeout before modem create");
        return cleanup_after_failure(me, ret);
    }
```

Update the modem config mapping to use `ready_timeout_ms` and remove `boot_wait_ms`:

```c
    const modem_air780ep_config_t modem_config = {
        .en_pin = config->en_pin,
        .reset_pulse_ms = config->modem_reset_pulse_ms,
        .ready_timeout_ms = stage_timeout_ms,
        .default_cmd_timeout_ms = config->modem_default_cmd_timeout_ms,
        .event_queue_size = config->modem_event_queue_size,
        .event_task_stack = config->modem_event_task_stack,
        .event_task_priority = config->modem_event_task_priority,
    };
```

Replace the existing local `ready_timeout_ms` variable and `lwlte_wait_ready()` call with:

```c
    ret = remaining_timeout_ms(init_start_tick, total_ready_timeout_ms,
                               &stage_timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init timeout before waiting core ready");
        return cleanup_after_failure(me, ret);
    }

    ret = lwlte_wait_ready(me, stage_timeout_ms);
```

- [ ] **Step 6: Verify obsolete fields are removed from these files**

Run:

```bash
git grep -n "modem_boot_wait_ms\|boot_wait_ms" -- src/include/lwlte_air780ep.h src/modem/modem_air780ep.h src/lwlte/lwlte_air780ep.c
```

Expected after this task: no output.

---

### Task 2: RDY-Based Air780EP Modem Initialization

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Capture the current wrong-order check**

Run:

```bash
git grep -n "hardware_reset(self)\|register_urcs(self)\|boot_wait_ms" -- src/modem/modem_air780ep.c
```

Expected before this task: `hardware_reset(self)` appears before `register_urcs(self)` in `air780ep_init()` or `air780ep_reset()`, and `boot_wait_ms` appears in `hardware_reset()`.

- [ ] **Step 2: Add RDY synchronization fields and defaults**

In `src/modem/modem_air780ep.c`, add this define after `AIR780EP_DEFAULT_CMD_TIMEOUT_MS`:

```c
#define AIR780EP_DEFAULT_READY_TIMEOUT_MS 30000
```

Add these fields to `modem_air780ep_t` after `pdp`:

```c
    SemaphoreHandle_t rdy_sema;
    bool rdy_seen;
    bool waiting_rdy;
```

In `modem_air780ep_create()`, after normalizing `default_cmd_timeout_ms`, normalize `ready_timeout_ms`:

```c
    if (self->config.ready_timeout_ms == 0) {
        self->config.ready_timeout_ms = AIR780EP_DEFAULT_READY_TIMEOUT_MS;
    }
```

After `modem_base_init()` succeeds and before `return &self->base;`, create the semaphore:

```c
    self->rdy_sema = xSemaphoreCreateBinary();
    if (!self->rdy_sema) {
        ESP_LOGE(TAG, "create RDY semaphore failed");
        modem_base_deinit(&self->base);
        free(self);
        return NULL;
    }
```

- [ ] **Step 3: Add RDY helper prototypes**

Add these static prototypes near the other Air780EP private prototypes:

```c
static TickType_t timeout_ticks(uint32_t timeout_ms);
static esp_err_t begin_wait_rdy(modem_air780ep_t *self);
static void cancel_wait_rdy(modem_air780ep_t *self);
static esp_err_t wait_rdy(modem_air780ep_t *self);
static esp_err_t run_basic_init_cmds(modem_air780ep_t *self);
static esp_err_t finish_modem_ready(modem_t *me, modem_air780ep_t *self);
```

- [ ] **Step 4: Implement RDY helper functions**

Add these functions before `hardware_reset()`:

```c
static TickType_t timeout_ticks(uint32_t timeout_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        return 1;
    }

    return ticks;
}

static esp_err_t begin_wait_rdy(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.lock && self->rdy_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->rdy_seen = false;
    self->waiting_rdy = true;
    xSemaphoreGive(self->base.lock);

    while (xSemaphoreTake(self->rdy_sema, 0) == pdTRUE) {
    }

    return ESP_OK;
}

static void cancel_wait_rdy(modem_air780ep_t *self)
{
    if (!self || !self->base.lock) {
        return;
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->waiting_rdy = false;
    xSemaphoreGive(self->base.lock);
}

static esp_err_t wait_rdy(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self && self->base.lock && self->rdy_sema,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    bool seen = self->rdy_seen;
    xSemaphoreGive(self->base.lock);

    if (!seen) {
        TickType_t ticks = timeout_ticks(self->config.ready_timeout_ms);
        BaseType_t sema_ret = xSemaphoreTake(self->rdy_sema, ticks);
        if (sema_ret != pdTRUE) {
            cancel_wait_rdy(self);
            return ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    seen = self->rdy_seen;
    self->waiting_rdy = false;
    xSemaphoreGive(self->base.lock);

    return seen ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t run_basic_init_cmds(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    const char *cmds[] = {
        "ATE0",
        "AT+CMEE=1",
        "AT+CEREG=2",
        "AT+CGREG=2",
        "AT+CREG=2",
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        air780ep_cmd_ctx_t ctx;
        esp_err_t ret = send_cmd(self, cmds[i], &ctx, 0);
        ESP_RETURN_ON_ERROR(ret, TAG, "send %s failed", cmds[i]);
        ret = ensure_at_ok(&ctx.response, cmds[i]);
        ESP_RETURN_ON_ERROR(ret, TAG, "%s failed", cmds[i]);
    }

    return ESP_OK;
}

static esp_err_t finish_modem_ready(modem_t *me, modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(me && self, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = modem_set_state(me, MODEM_STATE_READY);
    ESP_RETURN_ON_ERROR(ret, TAG, "set ready state failed");

    self->initialized = true;

    const modem_event_t event = {
        .id = MODEM_EVENT_READY,
    };
    ret = modem_post_event(me, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "post ready event failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}
```

- [ ] **Step 5: Simplify hardware_reset**

Replace `hardware_reset()` with:

```c
static esp_err_t hardware_reset(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    if (self->config.en_pin == GPIO_NUM_NC) {
        return ESP_OK;
    }

    esp_err_t ret = gpio_reset_pin(self->config.en_pin);
    ESP_RETURN_ON_ERROR(ret, TAG, "reset EN GPIO failed");

    ret = gpio_set_direction(self->config.en_pin, GPIO_MODE_OUTPUT);
    ESP_RETURN_ON_ERROR(ret, TAG, "set EN GPIO direction failed");

    ret = gpio_set_level(self->config.en_pin, 0);
    ESP_RETURN_ON_ERROR(ret, TAG, "set EN GPIO low failed");

    if (self->config.reset_pulse_ms > 0) {
        vTaskDelay(timeout_ticks(self->config.reset_pulse_ms));
    }

    ret = gpio_set_level(self->config.en_pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "set EN GPIO high failed");

    return ESP_OK;
}
```

- [ ] **Step 6: Reorder air780ep_init around URC registration and RDY wait**

Replace the body of `air780ep_init()` after local variable declarations with:

```c
    ret = modem_set_state(me, MODEM_STATE_INITIALIZING);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set initializing state failed");

    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    ret = begin_wait_rdy(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "begin RDY wait failed");

    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    ret = wait_rdy(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait RDY failed");

    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");

    return ESP_OK;

err:
    cancel_wait_rdy(self);
    if (!urc_registered_before && self->urc_registered) {
        unregister_urcs(self);
    }
    self->initialized = false;
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
```

- [ ] **Step 7: Reorder air780ep_reset the same way**

Replace the body of `air780ep_reset()` after local variable declarations with:

```c
    self->initialized = false;

    ret = modem_set_state(me, MODEM_STATE_INITIALIZING);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set initializing state failed");

    ret = register_urcs(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URCs failed");

    ret = begin_wait_rdy(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "begin RDY wait failed");

    ret = hardware_reset(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "hardware reset failed");

    ret = wait_rdy(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "wait RDY failed");

    ret = run_basic_init_cmds(self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "run init commands failed");

    ret = finish_modem_ready(me, self);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "finish modem ready failed");

    return ESP_OK;

err:
    cancel_wait_rdy(self);
    if (!urc_registered_before && self->urc_registered) {
        unregister_urcs(self);
    }
    self->initialized = false;
    (void)modem_set_state(me, MODEM_STATE_ERROR);
    return ret;
```

- [ ] **Step 8: Update RDY handler so it synchronizes but does not announce ready during init**

Replace `rdy_urc_handler()` with:

```c
static void rdy_urc_handler(const char *prefix, const char *line, void *user_ctx)
{
    (void)prefix;
    (void)line;

    if (!user_ctx) {
        return;
    }

    modem_air780ep_t *self = (modem_air780ep_t *)user_ctx;
    bool should_post_ready = false;

    if (!self->base.lock) {
        return;
    }
    xSemaphoreTake(self->base.lock, portMAX_DELAY);
    self->rdy_seen = true;
    should_post_ready = self->initialized && !self->waiting_rdy;
    SemaphoreHandle_t rdy_sema = self->rdy_sema;
    xSemaphoreGive(self->base.lock);

    if (rdy_sema) {
        (void)xSemaphoreGive(rdy_sema);
    }

    if (should_post_ready) {
        const modem_event_t event = {
            .id = MODEM_EVENT_READY,
        };
        esp_err_t ret = modem_post_event(&self->base, &event);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "post ready event failed: %s", esp_err_to_name(ret));
        }
    }
}
```

- [ ] **Step 9: Destroy the RDY semaphore in subclass destroy**

In `air780ep_destroy()`, after URC unregister succeeds and before `self->initialized = false;`, add:

```c
    if (self->rdy_sema) {
        vSemaphoreDelete(self->rdy_sema);
        self->rdy_sema = NULL;
    }
    self->rdy_seen = false;
    self->waiting_rdy = false;
```

- [ ] **Step 10: Verify modem code no longer uses boot wait and orders URC before reset**

Run:

```bash
git grep -n "boot_wait_ms" -- src/modem/modem_air780ep.c src/modem/modem_air780ep.h
```

Expected: no output.

Run:

```bash
git grep -n "register_urcs(self)\|hardware_reset(self)\|wait_rdy(self)\|run_basic_init_cmds(self)" -- src/modem/modem_air780ep.c
```

Expected: in both `air780ep_init()` and `air780ep_reset()`, the order is `register_urcs(self)`, `begin_wait_rdy(self)`, `hardware_reset(self)`, `wait_rdy(self)`, `run_basic_init_cmds(self)`.

---

### Task 3: Skip APN Configuration When APN Is Empty

**Files:**
- Modify: `src/core/net_mgr.c`

- [ ] **Step 1: Capture the current APN behavior check**

Run:

```bash
git grep -n "modem_set_apn(me->modem, me->config.primary_cid, me->config.apn)" -- src/core/net_mgr.c
```

Expected before this task: one unconditional call in `run_activation_once()`.

- [ ] **Step 2: Guard APN configuration with non-empty APN check**

In `src/core/net_mgr.c`, replace the `NET_STEP_SET_APN` block with:

```c
    me->net_mgr.current_step = NET_STEP_SET_APN;
    me->net_mgr.step_start_time_ms = now_ms();
    if (me->config.apn[0] != '\0') {
        ret = modem_set_apn(me->modem, me->config.primary_cid, me->config.apn);
        continue_ret = check_activation_continue(me, activation_start_ms);
        if (continue_ret != ESP_OK) {
            return continue_ret;
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }
```

- [ ] **Step 3: Verify the APN call is conditional**

Run:

```bash
git grep -n "me->config.apn\[0\] != '\\0'\|modem_set_apn(me->modem" -- src/core/net_mgr.c
```

Expected: the `me->config.apn[0] != '\0'` check appears before the `modem_set_apn()` call in `run_activation_once()`.

---

### Task 4: Documentation and Example Cleanup

**Files:**
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/architecture.md`
- Modify: `examples/basic_connect/README.md`
- Modify: `docs/superpowers/specs/2026-05-25-air780ep-pin-simplify-design.md`

- [ ] **Step 1: Update the class design Air780EP config snippet**

In `docs/agents/classes.md`, replace the Air780EP config struct snippet with:

```c
typedef struct {
    gpio_num_t en_pin;                  // EN GPIO，未使用时为 GPIO_NUM_NC
    uint32_t   reset_pulse_ms;          // 复位脉冲(EN 拉低保持)时长
    uint32_t   ready_timeout_ms;        // 等待 RDY URC 超时
    uint32_t   default_cmd_timeout_ms;  // Air780EP 命令默认超时
    int        event_queue_size;        // Modem 事件队列长度
    int        event_task_stack;        // Modem event task 栈大小
    int        event_task_priority;     // Modem event task 优先级
} modem_air780ep_config_t;
```

Replace the hardware reset bullet with:

```md
- 硬件复位通过 EN 引脚实现：注册 URC 后，拉低 EN，等待 reset_pulse_ms，再拉高 EN；随后等待 RDY URC，收到 RDY 后才发送 AT 初始化命令。`air780ep_init()` 和 `air780ep_reset()` 都使用此方式。
```

If the modem ops table still says `AT+CGEREP=1`, replace that command list with:

```md
`ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`
```

- [ ] **Step 2: Update architecture docs that describe the facade flow**

In `docs/agents/architecture.md`, ensure the facade/modem sequence says:

```md
Air780EP modem 初始化时先完成 AT Engine 和 URC 注册，再通过 EN 执行硬复位，等待 RDY URC 后发送基础 AT 初始化命令。Facade 的 init_ready_timeout_ms 是 RDY 等待和 Core ready 等待的初始化总超时。
```

Do not reintroduce `pwrkey_pin`, `reset_pin`, `status_pin`, `boot_wait_ms`, or `module_power_stable_ms`.

- [ ] **Step 3: Update the basic connect README**

In `examples/basic_connect/README.md`, keep the wiring table and replace the EN paragraph with:

```md
The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. The modem adapter registers URC handlers first, toggles EN low then high during init, waits for the module `RDY` URC, and only then sends AT initialization commands.
```

Replace the APN row with:

```md
| APN | empty string; the facade does not send an APN configuration command and Air780EP uses its module/operator default path |
```

Replace the PDP/APN troubleshooting bullet with:

```md
- PDP/APN errors: this example uses an empty APN, so esp-lwlte does not send `AT+CGDCONT`. If your SIM requires an explicit APN, change `EXAMPLE_LTE_APN` in `main/main.c`.
```

- [ ] **Step 4: Mark the older pin simplify spec as superseded for boot-wait details**

At the top of `docs/superpowers/specs/2026-05-25-air780ep-pin-simplify-design.md`, after the status line, add:

```md
**后续修订**: RDY 等待流程以后续设计 `docs/superpowers/specs/2026-05-25-air780ep-rdy-init-flow-design.md` 为准；该后续设计移除了 `boot_wait_ms`，并要求先注册 URC、硬复位、等待 `RDY` 后再发送 AT 初始化命令。
```

Replace any remaining “等 boot_wait_ms” description in that spec with “等待 RDY URC”.

- [ ] **Step 5: Verify docs do not describe active boot-wait behavior**

Run:

```bash
git grep -n "boot_wait_ms\|modem_boot_wait_ms\|module_power_stable_ms\|power_on_pulse_ms\|pwrkey_pin\|reset_pin\|status_pin" -- docs/agents examples/basic_connect src/include src/modem
```

Expected: no matches in active headers/source/example docs. Matches in old superseded planning files outside the specified paths are acceptable.

---

### Task 5: Final Verification

**Files:**
- Verify all changed files.

- [ ] **Step 1: Check formatting and whitespace**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 2: Build the basic connect example**

Run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py -C examples/basic_connect build
```

Expected: build completes successfully.

- [ ] **Step 3: Verify obsolete boot-wait identifiers are gone from active code**

Run:

```bash
git grep -n "boot_wait_ms\|modem_boot_wait_ms" -- src examples/basic_connect docs/agents
```

Expected: no output.

- [ ] **Step 4: Verify RDY path exists in code**

Run:

```bash
git grep -n "ready_timeout_ms\|begin_wait_rdy\|wait_rdy\|rdy_seen\|waiting_rdy" -- src/modem src/lwlte
```

Expected: matches show facade passing `ready_timeout_ms`, Air780EP modem waiting for RDY, and RDY handler setting the synchronization flag.

- [ ] **Step 5: Verify empty APN skip is present**

Run:

```bash
git grep -n "me->config.apn\[0\] != '\\0'\|modem_set_apn(me->modem" -- src/core/net_mgr.c
```

Expected: the APN non-empty check appears before `modem_set_apn()`.

- [ ] **Step 6: Report hardware verification status**

If hardware is connected and the user requests flashing, use the project workflow:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py -C examples/basic_connect -p /dev/<PORT> flash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/<PORT>
```

Expected with `CONFIG_LWLTE_AT_ENGINE_LOG_IO` enabled: logs show `RDY` before `ATE0`, and no `AT+CGDCONT=1,"IP",""` when `EXAMPLE_LTE_APN` is empty. If hardware is not connected or flashing is not requested, report that build verification passed and real hardware RDY ordering was not verified.
