# lwlte MQTT Independent Init Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple MQTT client configuration from the LTE init config by adding dedicated `lwlte_mqtt_init` / `lwlte_mqtt_destroy` facade APIs and a unified `lwlte_mqtt_config_t`.

**Architecture:** A hard break refactor. The two duplicate `..._config_mqtt_client_t` typedefs and the `.mqtt_client` fields on both `lwlte_*_config_t` structs are deleted. The modem factory functions stop creating the MQTT client. New facade APIs `lwlte_mqtt_init` / `lwlte_mqtt_destroy` create and destroy `me->mqtt`, mirroring the existing init↔destroy (object) / start↔stop (FSM) pattern. `lwlte_destroy` remains a fallback that cleans up any leftover MQTT client. Examples and docs are updated to the new call sequence.

**Tech Stack:** ESP-IDF v6.0, C99, FreeRTOS, existing `mqtt_client_*` service (unchanged), existing facade gate (`begin_api_call` / `end_api_call`) in `src/lwlte/lwlte.c`.

**Verification note:** This project has no unit test suite (`test/` is empty). Verification is by build + example migration. The build is performed via the `esp-idf-eim_build_project` MCP tool. Commits are frequent; **the user must explicitly say "commit" before any `git commit` is run** (project policy, see `/Users/jovisdreams/.config/opencode/AGENTS.md`).

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/include/lwlte.h` | Public facade header | Delete two `..._config_mqtt_client_t` typedefs; delete two `.mqtt_client` fields; add `lwlte_mqtt_config_t`; add `lwlte_mqtt_init` / `lwlte_mqtt_destroy` prototypes with doxygen |
| `src/lwlte/lwlte.c` | Facade implementation | Add `lwlte_mqtt_init` and `lwlte_mqtt_destroy` next to existing `lwlte_mqtt_start`; add static `non_negative_int` helper (currently duplicated in both modem factory files — make this the canonical copy for the facade) |
| `src/lwlte/lwlte_air780ep.c` | Air780EP factory | Delete MQTT creation step (step 6); delete MQTT validation block in `validate_config` |
| `src/lwlte/lwlte_ml307r.c` | ML307R factory | Delete MQTT creation step; delete MQTT validation block in `validate_config` |
| `example/air780ep_mqtt_client.c` | Air780EP MQTT example | Migrate to `lwlte_mqtt_init` call sequence |
| `example/ml307r_mqtt_client.c` | ML307R MQTT example | Migrate to `lwlte_mqtt_init` call sequence |
| `docs/agents/classes.md` | Class design doc | Update MQTT client creation section |
| `docs/agents/architecture.md` | Architecture doc | Update App-layer call sequence |
| `docs/agents/feature-roadmap.md` | Feature roadmap | Update MQTT wording if needed |

`lwlte_priv.h` and the lower layers (`mqtt_client_*`, `modem_*_mqtt_*`, `modem_ops_t`) are **not modified**.

---

## Task 1: Add `lwlte_mqtt_config_t` to public header

**Files:**
- Modify: `src/include/lwlte.h` (replace `lwlte_air780ep_config_mqtt_client_t` typedef at lines 178-196 with the unified typedef; it will be referenced by the new init API added later)

**Why first:** The new config type is the foundation everything else references. Defining it before deleting the old typedefs keeps the header self-consistent at every commit.

- [ ] **Step 1: Read the existing typedef block to confirm exact text**

Read `src/include/lwlte.h:178-196` and confirm the doxygen note + field list matches what's in the spec. The current block is:

```c
/**
 * @brief Air780EP MQTT 客户端配置
 * @details Air780EP MQTT client configuration
 * @note enabled 为 false 时 MQTT 服务禁用，其余字段被忽略。
 * @note enabled 为 true 时 host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    bool enabled;                         /**< 是否启用 MQTT 服务； Whether to enable MQTT service */
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_air780ep_config_mqtt_client_t;
```

- [ ] **Step 2: Replace it with the unified `lwlte_mqtt_config_t` typedef**

Use the `edit` tool with `oldString` set to the full block from Step 1 and `newString`:

```c
/**
 * @brief MQTT 客户端配置
 * @details MQTT client configuration
 * @note host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 * @note config 及其字符串指针由调用方拥有，在 lwlte_mqtt_init() 返回前必须保持有效。
 */
typedef struct {
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_mqtt_config_t;
```

Key changes: `enabled` field removed (calling `lwlte_mqtt_init` is the enable action); field list otherwise identical; name unified to `lwlte_mqtt_config_t`.

- [ ] **Step 3: Leave the duplicate ML307R typedef in place for now**

Do **not** delete `lwlte_ml307r_config_mqtt_client_t` (lines 239-257) yet — it's still referenced by `lwlte_ml307r_config_t.mqtt_client` until Task 3. The Air780EP struct still references the old Air780EP typedef name until Task 3 as well. We removed only the **Air780EP typedef** in this task, but `lwlte_air780ep_config_t.mqtt_client` (line 236) still names it. **This is intentional breakage**: the build will fail after this step, which Task 3 fixes. Do not run the build yet.

Actually — to avoid leaving the header in a non-compiling state between tasks, reorder: Task 1 adds `lwlte_mqtt_config_t` **alongside** the existing typedefs (do not delete yet), Task 3 deletes the old typedefs and fields together with the factory changes. Revised Step 2 below supersedes the above.

- [ ] **Step 2 (revised): Insert `lwlte_mqtt_config_t` above the Air780EP typedef**

Use the `edit` tool. `oldString` is the doxygen opener of the Air780EP typedef:

```c
/**
 * @brief Air780EP MQTT 客户端配置
 * @details Air780EP MQTT client configuration
```

`newString` is the full new `lwlte_mqtt_config_t` block followed by a blank line, then the same Air780EP opener (so the old typedef stays put):

```c
/**
 * @brief MQTT 客户端配置
 * @details MQTT client configuration
 * @note host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 * @note config 及其字符串指针由调用方拥有，在 lwlte_mqtt_init() 返回前必须保持有效。
 */
typedef struct {
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_mqtt_config_t;

/**
 * @brief Air780EP MQTT 客户端配置
 * @details Air780EP MQTT client configuration
```

After this step the header still compiles (the old typedefs remain). Do not build yet — the new type is unreferenced and will produce no warning.

- [ ] **Step 4: Do NOT commit yet**

Per project policy, do not run `git commit`. Stop and await user instruction.

---

## Task 2: Add `lwlte_mqtt_init` / `lwlte_mqtt_destroy` prototypes to public header

**Files:**
- Modify: `src/include/lwlte.h` (insert prototypes next to existing `lwlte_mqtt_start` / `lwlte_mqtt_stop` around line 447-471)

**Why before implementation:** The prototypes + doxygen document the contract; placing them before the existing `lwlte_mqtt_start` groups all four lifecycle APIs together.

- [ ] **Step 1: Add the static `non_negative_int` helper to `lwlte.c`**

First add the helper that `lwlte_mqtt_init` will use. The factory files each have their own static copy (`src/lwlte/lwlte_air780ep.c:288`, `src/lwlte/lwlte_ml307r.c:208`); the facade gets its own copy too to keep the change minimal and avoid touching `lwlte_priv.h`. Read `src/lwlte/lwlte.c` near line 935 (after `end_api_call`) and use `edit` to insert before `static esp_err_t wait_api_calls_idle`:

Actually place it with the other small statics. Read around `src/lwlte/lwlte.c:935` (the `end_api_call` body) and after its closing brace, insert:

```c

static bool non_negative_int(int value)
{
    return value >= 0;
}
```

Also add its forward declaration in the STATIC PROTOTYPES section of `lwlte.c` (search for `static void end_api_call` declaration around line 111 and add `static bool non_negative_int(int value);` near it).

- [ ] **Step 2: Locate the insertion point in `lwlte.h`**

The existing MQTT lifecycle APIs are at `src/include/lwlte.h:447-484` (`lwlte_mqtt_start`, `lwlte_mqtt_stop`, `lwlte_mqtt_get_state`). Read that range to confirm exact text.

- [ ] **Step 3: Insert `lwlte_mqtt_init` and `lwlte_mqtt_destroy` prototypes before `lwlte_mqtt_start`**

Use `edit`. `oldString`:

```c
/**
 * @brief 启动 MQTT 客户端
 * @details Start MQTT client
```

`newString` (the two new prototypes followed by the existing start doxygen opener):

```c
/**
 * @brief 初始化 MQTT 客户端
 * @details Initialize MQTT client
 * @note 该函数只创建 MQTT 客户端对象及内部事件桥，不启动连接；连接由 lwlte_mqtt_start() 触发。
 * @note 须在 lwlte_air780ep_init()/lwlte_ml307r_init() 返回句柄之后、lwlte_destroy() 之前调用；与 lwlte_start() 的先后顺序无要求。
 * @note 同一句柄只能初始化一次，重复调用返回 ESP_ERR_INVALID_STATE；要更换配置须先 lwlte_mqtt_destroy()。
 * @note 内部自动注册事件桥，应用层无需手动调用事件注册 API。
 * @note config 及其字符串字段由调用方拥有，仅在该函数执行期间被借用；函数返回后调用方可释放或复用。
 * @note ESP_OK 返回时 MQTT 客户端可用，最终须通过 lwlte_mqtt_destroy() 或 lwlte_destroy() 释放。
 * @param[in] me LTE 用户门面句柄
 * @param[in] config MQTT 客户端配置
 * @return
 *         - ESP_OK: 初始化成功
 *         - ESP_ERR_INVALID_ARG: 参数无效或必填字段缺失
 *         - ESP_ERR_INVALID_STATE: 已初始化或门面正在销毁
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 下层创建失败
 */
esp_err_t lwlte_mqtt_init(lwlte_handle_t *me, const lwlte_mqtt_config_t *config);

/**
 * @brief 销毁 MQTT 客户端
 * @details Destroy MQTT client
 * @note 该函数从任何 FSM 状态安全调用：若 MQTT 仍在运行（CONNECTED/CONNECTING/...），下层会先自动停止。
 * @note 重复调用或未初始化时返回 ESP_ERR_INVALID_STATE。
 * @note 若应用层未手动调用本函数，lwlte_destroy() 会作为兜底清理 MQTT 客户端。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 销毁成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 未初始化或门面正在销毁
 *         - 其他 esp_err_t: 下层销毁错误（已记录日志）
 */
esp_err_t lwlte_mqtt_destroy(lwlte_handle_t *me);

/**
 * @brief 启动 MQTT 客户端
 * @details Start MQTT client
```

- [ ] **Step 4: Do NOT commit yet**

---

## Task 3: Implement `lwlte_mqtt_init` and `lwlte_mqtt_destroy` in `lwlte.c`

**Files:**
- Modify: `src/lwlte/lwlte.c` (add the two functions just before the existing `lwlte_mqtt_start` at line 497)

- [ ] **Step 1: Read the exact context around `lwlte_mqtt_start`**

Read `src/lwlte/lwlte.c:490-510` to confirm the exact text immediately before `esp_err_t lwlte_mqtt_start`.

- [ ] **Step 2: Insert the two implementations**

Use `edit`. `oldString` is the function signature line `esp_err_t lwlte_mqtt_start(lwlte_handle_t *me)\n{` (verify exact whitespace from Step 1). `newString` is the two new functions followed by the same `lwlte_mqtt_start` signature and `{`:

```c
esp_err_t lwlte_mqtt_init(lwlte_handle_t *me, const lwlte_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(config->host && config->host[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT host is required");
    ESP_RETURN_ON_FALSE(config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "MQTT port is required");
    ESP_RETURN_ON_FALSE(config->client_id && config->client_id[0],
                        ESP_ERR_INVALID_ARG, TAG, "MQTT client_id is required");
    ESP_RETURN_ON_FALSE(non_negative_int(config->fsm_queue_size) &&
                        non_negative_int(config->fsm_task_stack) &&
                        non_negative_int(config->fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "MQTT task fields must be non-negative");

    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool already_initialized = (me->mqtt != NULL);
    xSemaphoreGive(me->lock);
    if (already_initialized) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    const mqtt_client_config_t mqtt_config = {
        .transport         = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
        .host              = config->host,
        .port              = config->port,
        .client_id         = config->client_id,
        .username          = config->username,
        .password          = config->password,
        .keepalive_s       = config->keepalive_s,
        .clean_session     = config->clean_session,
        .fsm_queue_size    = config->fsm_queue_size,
        .fsm_task_stack    = config->fsm_task_stack,
        .fsm_task_priority = config->fsm_task_priority,
    };
    mqtt_client_handle_t *mqtt = mqtt_client_create(&mqtt_config, core);
    if (!mqtt) {
        end_api_call(me);
        ESP_LOGE(TAG, "create MQTT client failed");
        return ESP_FAIL;
    }

    ret = mqtt_client_register_event_callback(mqtt, lwlte_handle_mqtt_event, me);
    if (ret != ESP_OK) {
        mqtt_client_destroy(mqtt);
        end_api_call(me);
        ESP_LOGE(TAG, "register MQTT event bridge failed: %s", esp_err_to_name(ret));
        return ret;
    }

    me->mqtt = mqtt;
    end_api_call(me);
    return ESP_OK;
}

esp_err_t lwlte_mqtt_destroy(lwlte_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t *mqtt = me->mqtt;
    me->mqtt = NULL;
    xSemaphoreGive(me->lock);

    if (!mqtt) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    ret = mqtt_client_destroy(mqtt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "destroy MQTT client failed: %s", esp_err_to_name(ret));
    }
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_mqtt_start(lwlte_handle_t *me)
{
```

Notes on the implementation (verified during spec self-review):
- `begin_api_call(me, true, &core)` for init — needs `core` to pass to `mqtt_client_create`.
- `begin_api_call(me, false, NULL)` for destroy — does not need core.
- `me->mqtt = NULL` happens under the facade lock before calling `mqtt_client_destroy`, so concurrent `lwlte_mqtt_*` runtime calls observe NULL via `begin_mqtt_api_call` and bail with `ESP_ERR_INVALID_STATE`.
- `mqtt_client_destroy` is called **inside** the facade gate (before `end_api_call`). This keeps `active_api_calls > 0` so `lwlte_destroy` blocks at `wait_api_calls_idle` until MQTT teardown finishes. There is no self-deadlock because the facade lock is not held during the call and `lwlte_handle_mqtt_event` does not touch the api-call counter.

- [ ] **Step 3: Build to confirm the facade compiles**

Run: `esp-idf-eim_build_project`
Expected: **build fails** — `lwlte_air780ep_config_mqtt_client_t` is still referenced by `lwlte_air780ep_config_t.mqtt_client` (we haven't deleted the field yet) **but** we just deleted the typedef in Task 1's original ordering. 

Wait — Task 1 was revised to **keep** the old typedefs. So at this point the old typedefs still exist, the new `lwlte_mqtt_config_t` exists, and the new functions are implemented but unused. The build should **succeed** with no warnings (the new functions are non-static globals, so no unused-function warning; the new type is declared in a header and is fine even if unreferenced).

Expected: **build succeeds**. If it fails, fix before continuing.

- [ ] **Step 4: Do NOT commit yet**

---

## Task 4: Migrate Air780EP factory — drop MQTT creation

**Files:**
- Modify: `src/lwlte/lwlte_air780ep.c:190-218` (delete step 6, the entire `if (config->mqtt_client.enabled)` block)
- Modify: `src/lwlte/lwlte_air780ep.c:258-273` (delete the MQTT validation block in `validate_config`)
- Modify: `src/include/lwlte.h` (delete the `.mqtt_client` field of `lwlte_air780ep_config_t` at line 236, and delete the old `lwlte_air780ep_config_mqtt_client_t` typedef)

- [ ] **Step 1: Delete the MQTT creation step in `lwlte_air780ep_init`**

Read `src/lwlte/lwlte_air780ep.c:181-222` to confirm exact text. Use `edit` to replace the entire step 6 block + the trailing `*out_lte = me; return ESP_OK;` with just the trailing lines (so step 5 flows directly into the return). `oldString` (verify against actual file):

```c
    /*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * 步骤 6：按需创建 MQTT Client 并注册事件桥接
     *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/
    if (config->mqtt_client.enabled) {
        const mqtt_client_config_t mqtt_config = {
            .transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
            .host = config->mqtt_client.host,
            .port = config->mqtt_client.port,
            .client_id = config->mqtt_client.client_id,
            .username = config->mqtt_client.username,
            .password = config->mqtt_client.password,
            .keepalive_s = config->mqtt_client.keepalive_s,
            .clean_session = config->mqtt_client.clean_session,
            .fsm_queue_size = config->mqtt_client.fsm_queue_size,
            .fsm_task_stack = config->mqtt_client.fsm_task_stack,
            .fsm_task_priority = config->mqtt_client.fsm_task_priority,
        };
        me->mqtt = mqtt_client_create(&mqtt_config, me->core);
        if (!me->mqtt) {
            ESP_LOGE(TAG, "create MQTT client failed");
            return cleanup_after_failure(me, ESP_OK);
        }
        /* MQTT 事件桥接：MQTT event → Facade → lwlte 用户回调 */
        ret = mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register MQTT event bridge failed: %s", esp_err_to_name(ret));
            return cleanup_after_failure(me, ret);
        }
    }

    *out_lte = me;
    return ESP_OK;
}
```

`newString`:

```c
    *out_lte = me;
    return ESP_OK;
}
```

- [ ] **Step 2: Delete the MQTT validation block in `validate_config`**

Read `src/lwlte/lwlte_air780ep.c:244-276`. Use `edit`. `oldString` (the block from the `non_negative_int` chain's closing through the MQTT block through `return ESP_OK;`):

```c
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");
    if (config->mqtt_client.enabled) {
        ESP_RETURN_ON_FALSE(config->mqtt_client.host && config->mqtt_client.host[0],
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT host is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.port > 0,
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT port is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.client_id && config->mqtt_client.client_id[0],
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT client_id is required");
        ESP_RETURN_ON_FALSE(non_negative_int(config->mqtt_client.fsm_queue_size) &&
                            non_negative_int(config->mqtt_client.fsm_task_stack) &&
                            non_negative_int(config->mqtt_client.fsm_task_priority),
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT task fields must be non-negative");
    }

    return ESP_OK;
}
```

`newString`:

```c
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");

    return ESP_OK;
}
```

- [ ] **Step 3: Delete the `.mqtt_client` field from `lwlte_air780ep_config_t` and the related doxygen note**

Read `src/include/lwlte.h:198-237`. Use `edit`. `oldString` (the two doxygen notes that mention mqtt + the field + the closing brace):

```c
 * @note mqtt_client.enabled 为 false 时 MQTT 服务禁用；为 true 时 host、port 和 client_id 为必填字段。
 * @note UART 端口必须满足 UART_NUM_0 <= uart_num < UART_NUM_MAX；UART TX/RX 必须是有效 GPIO 且不能为 GPIO_NUM_NC。
 * @note uart_baud_rate 必须大于 0；Air780EP 门面当前仅支持 primary_cid 为 1。
 * @note 有符号的队列、任务和缓冲区字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
```

`newString`:

```c
 * @note UART 端口必须满足 UART_NUM_0 <= uart_num < UART_NUM_MAX；UART TX/RX 必须是有效 GPIO 且不能为 GPIO_NUM_NC。
 * @note uart_baud_rate 必须大于 0；Air780EP 门面当前仅支持 primary_cid 为 1。
 * @note 有符号的队列、任务和缓冲区字段允许 0 表示默认值，非 0 值必须大于 0。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_air780ep_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
```

Then separately delete the field itself. `oldString`:

```c
    int core_fsm_task_priority;               /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
    lwlte_air780ep_config_mqtt_client_t mqtt_client; /**< MQTT 客户端配置； MQTT client configuration */
} lwlte_air780ep_config_t;
```

`newString`:

```c
    int core_fsm_task_priority;               /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
} lwlte_air780ep_config_t;
```

- [ ] **Step 4: Delete the old `lwlte_air780ep_config_mqtt_client_t` typedef**

Read `src/include/lwlte.h` around the now-obsolete typedef (still present from before Task 1's insertion point). Use `edit`. `oldString` is the full typedef block:

```c
/**
 * @brief Air780EP MQTT 客户端配置
 * @details Air780EP MQTT client configuration
 * @note enabled 为 false 时 MQTT 服务禁用，其余字段被忽略。
 * @note enabled 为 true 时 host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    bool enabled;                         /**< 是否启用 MQTT 服务； Whether to enable MQTT service */
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_air780ep_config_mqtt_client_t;

```

`newString`: empty string (deletes the block + trailing blank line). The `lwlte_mqtt_config_t` typedef added in Task 1 remains as the only MQTT config typedef in the header.

- [ ] **Step 5: Build to confirm Air780EP factory + header compile**

Run: `esp-idf-eim_build_project`
Expected: **build fails** because `example/air780ep_mqtt_client.c` still references the deleted `.mqtt_client` field (fixed in Task 6). Confirm the failure is only in the example file, not in `lwlte.c` / `lwlte_air780ep.c` / `lwlte.h`. If the failure is elsewhere, fix before continuing.

- [ ] **Step 6: Do NOT commit yet**

---

## Task 5: Migrate ML307R factory — drop MQTT creation

**Files:**
- Modify: `src/lwlte/lwlte_ml307r.c:118-142` (delete the MQTT creation block)
- Modify: `src/lwlte/lwlte_ml307r.c:180-193` (delete the MQTT validation block in `validate_config`)
- Modify: `src/include/lwlte.h` (delete the `.mqtt_client` field of `lwlte_ml307r_config_t` at line 293, and delete the old `lwlte_ml307r_config_mqtt_client_t` typedef at lines 239-257)

- [ ] **Step 1: Delete the MQTT creation block in `lwlte_ml307r_init`**

Read `src/lwlte/lwlte_ml307r.c:112-146`. Use `edit`. `oldString` (from after ping creation through the return):

```c
    me->ping = ping_client_create(me->core);
    if (!me->ping) {
        ESP_LOGE(TAG, "create Ping client failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    if (config->mqtt_client.enabled) {
        const mqtt_client_config_t mqtt_config = {
            .transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
            .host = config->mqtt_client.host,
            .port = config->mqtt_client.port,
            .client_id = config->mqtt_client.client_id,
            .username = config->mqtt_client.username,
            .password = config->mqtt_client.password,
            .keepalive_s = config->mqtt_client.keepalive_s,
            .clean_session = config->mqtt_client.clean_session,
            .fsm_queue_size = config->mqtt_client.fsm_queue_size,
            .fsm_task_stack = config->mqtt_client.fsm_task_stack,
            .fsm_task_priority = config->mqtt_client.fsm_task_priority,
        };
        me->mqtt = mqtt_client_create(&mqtt_config, me->core);
        if (!me->mqtt) {
            ESP_LOGE(TAG, "create MQTT client failed");
            return cleanup_after_failure(me, ESP_OK);
        }
        ret = mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register MQTT event bridge failed: %s", esp_err_to_name(ret));
            return cleanup_after_failure(me, ret);
        }
    }

    *out_lte = me;
    return ESP_OK;
}
```

`newString`:

```c
    me->ping = ping_client_create(me->core);
    if (!me->ping) {
        ESP_LOGE(TAG, "create Ping client failed");
        return cleanup_after_failure(me, ESP_OK);
    }

    *out_lte = me;
    return ESP_OK;
}
```

- [ ] **Step 2: Delete the MQTT validation block in ML307R `validate_config`**

Read `src/lwlte/lwlte_ml307r.c:166-196`. Use `edit`. `oldString` (from the `non_negative_int` chain's closing through the MQTT block through `return ESP_OK;`):

```c
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");
    if (config->mqtt_client.enabled) {
        ESP_RETURN_ON_FALSE(config->mqtt_client.host && config->mqtt_client.host[0],
                            ESP_ERR_INVALID_ARG, TAG, "MQTT host is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.port > 0,
                            ESP_ERR_INVALID_ARG, TAG, "MQTT port is required");
        ESP_RETURN_ON_FALSE(config->mqtt_client.client_id &&
                            config->mqtt_client.client_id[0],
                            ESP_ERR_INVALID_ARG, TAG, "MQTT client_id is required");
        ESP_RETURN_ON_FALSE(non_negative_int(config->mqtt_client.fsm_queue_size) &&
                            non_negative_int(config->mqtt_client.fsm_task_stack) &&
                            non_negative_int(config->mqtt_client.fsm_task_priority),
                            ESP_ERR_INVALID_ARG, TAG,
                            "MQTT task fields must be non-negative");
    }

    return ESP_OK;
}
```

`newString`:

```c
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");

    return ESP_OK;
}
```

- [ ] **Step 3: Delete the `.mqtt_client` field from `lwlte_ml307r_config_t` and add a migration note**

Read `src/include/lwlte.h` around the ML307R config struct (after Air780EP edits, line numbers will have shifted — search for `lwlte_ml307r_config_t`). Use `edit`. `oldString` (the closing doxygen notes + field + brace):

```c
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note ML307R 门面当前仅支持 primary_cid 为 1。
 */
typedef struct {
```

`newString`:

```c
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note ML307R 门面当前仅支持 primary_cid 为 1。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_ml307r_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
```

Then delete the field itself. `oldString`:

```c
    int core_fsm_task_priority;               /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
    lwlte_ml307r_config_mqtt_client_t mqtt_client; /**< MQTT 客户端配置； MQTT client configuration */
} lwlte_ml307r_config_t;
```

`newString`:

```c
    int core_fsm_task_priority;               /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
} lwlte_ml307r_config_t;
```

- [ ] **Step 4: Delete the old `lwlte_ml307r_config_mqtt_client_t` typedef**

Read `src/include/lwlte.h` around the obsolete ML307R typedef. Use `edit`. `oldString` is the full typedef block (verify exact text in the file):

```c
/**
 * @brief ML307R MQTT 客户端配置
 * @details ML307R MQTT client configuration
 * @note enabled 为 false 时 MQTT 服务禁用，其余字段被忽略。
 * @note enabled 为 true 时 host、port 和 client_id 为必填字段；任务字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    bool enabled;                         /**< 是否启用 MQTT 服务； Whether to enable MQTT service */
    const char *host;                     /**< 必填 MQTT 服务器地址； Required MQTT broker host */
    uint16_t port;                        /**< 必填 MQTT 服务器端口； Required MQTT broker port */
    const char *client_id;                /**< 必填 MQTT 客户端 ID； Required MQTT client ID */
    const char *username;                 /**< 可选用户名； Optional username */
    const char *password;                 /**< 可选密码； Optional password */
    uint16_t keepalive_s;                 /**< keepalive 秒数，0 使用下层默认值； Keepalive seconds, 0 uses default */
    bool clean_session;                   /**< clean session 标志； Clean session flag */
    int fsm_queue_size;                   /**< MQTT FSM 队列长度，0 使用默认值； MQTT FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< MQTT FSM 任务栈大小，0 使用默认值； MQTT FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< MQTT FSM 任务优先级，0 使用默认值； MQTT FSM task priority, 0 uses default */
} lwlte_ml307r_config_mqtt_client_t;

```

`newString`: empty string (deletes the block + trailing blank line).

- [ ] **Step 5: Build to confirm factory + header compile**

Run: `esp-idf-eim_build_project`
Expected: **build fails** because both example files still reference the deleted `.mqtt_client` field (fixed in Task 6). Confirm the only failures are in `example/*.c`. If failures are elsewhere, fix before continuing.

- [ ] **Step 6: Do NOT commit yet**

---

## Task 6: Migrate Air780EP MQTT example

**Files:**
- Modify: `example/air780ep_mqtt_client.c:116-156` (drop the `.mqtt_client` initializer from the config literal; add `lwlte_mqtt_init` call after callback registration)

- [ ] **Step 1: Edit the config literal to drop the `.mqtt_client` field**

Read `example/air780ep_mqtt_client.c:110-160`. Use `edit`. `oldString`:

```c
    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
        .mqtt_client = {
            .enabled = true,
            .host = CONFIG_EXAMPLE_MQTT_HOST,
            .port = CONFIG_EXAMPLE_MQTT_PORT,
            .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
            .username = mqtt_username,
            .password = NULL,
            .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
            .clean_session = true,
        },
    };
```

`newString`:

```c
    const lwlte_mqtt_config_t mqtt_config = {
        .host = CONFIG_EXAMPLE_MQTT_HOST,
        .port = CONFIG_EXAMPLE_MQTT_PORT,
        .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
        .username = mqtt_username,
        .password = NULL,
        .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
        .clean_session = true,
    };

    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
    };
```

Note: `mqtt_config` is declared before `config` so it remains in scope for the `lwlte_mqtt_init` call below.

- [ ] **Step 2: Add `lwlte_mqtt_init` after the callback registration block**

Read `example/air780ep_mqtt_client.c:148-165` (after the edits above, line numbers will have shifted slightly — search for `lwlte_register_event_callback`). Use `edit`. `oldString` (the callback registration block + the start block):

```c
    /* 注册事件回调：网络、MQTT 连接和下行数据都会从这里返回。 */
    ret = lwlte_register_event_callback(lte, lte_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register callback failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 先启动 LTE 联网，MQTT 会在网络 online 后再启动。 */
    ret = lwlte_start(lte);
```

`newString`:

```c
    /* 注册事件回调：网络、MQTT 连接和下行数据都会从这里返回。 */
    ret = lwlte_register_event_callback(lte, lte_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register callback failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 创建 MQTT 客户端对象（不启动连接；连接在网络 online 后由 lwlte_mqtt_start 触发）。 */
    ret = lwlte_mqtt_init(lte, &mqtt_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 先启动 LTE 联网，MQTT 会在网络 online 后再启动。 */
    ret = lwlte_start(lte);
```

- [ ] **Step 3: Build to confirm the Air780EP example compiles**

Run: `esp-idf-eim_build_project`
Expected: **build fails** only because of `example/ml307r_mqtt_client.c` (still using old field; fixed in Task 7). The Air780EP example itself must compile cleanly.

- [ ] **Step 4: Do NOT commit yet**

---

## Task 7: Migrate ML307R MQTT example

**Files:**
- Modify: `example/ml307r_mqtt_client.c` (same shape as Task 6; the file is structurally identical to the Air780EP example)

- [ ] **Step 1: Read the ML307R example's config literal and init sequence**

Read `example/ml307r_mqtt_client.c` from the top through `lwlte_start(lte)`. The structure mirrors `example/air780ep_mqtt_client.c`. Confirm exact field order and surrounding text before editing.

- [ ] **Step 2: Drop the `.mqtt_client` initializer and declare `mqtt_config` separately**

Apply the same edit shape as Task 6 Step 1. The config struct name is `lwlte_ml307r_config_t` (ML307R-specific fields like absence of `init_ready_timeout_ms` default behavior may differ slightly — read the file first). The new `mqtt_config` literal uses the same `lwlte_mqtt_config_t` type and the same fields.

- [ ] **Step 3: Add `lwlte_mqtt_init` after callback registration**

Apply the same edit shape as Task 6 Step 2: insert the `lwlte_mqtt_init(lte, &mqtt_config)` call between `lwlte_register_event_callback` and `lwlte_start`.

- [ ] **Step 4: Build the full project**

Run: `esp-idf-eim_build_project`
Expected: **build succeeds**. All sources now compile: header has only `lwlte_mqtt_config_t`; both factories no longer touch MQTT; both examples use the new API; `lwlte.c` provides the new functions.

- [ ] **Step 5: Do NOT commit yet**

---

## Task 8: Verify no stale references remain

**Files:** None modified (verification only)

- [ ] **Step 1: Grep for the deleted type names**

Run via the `grep` tool:
- Pattern: `lwlte_air780ep_config_mqtt_client_t|lwlte_ml307r_config_mqtt_client_t`
- Include: `*.c`, `*.h`

Expected: **zero matches**.

- [ ] **Step 2: Grep for any `.mqtt_client` field access**

Run via the `grep` tool:
- Pattern: `\.mqtt_client`
- Include: `*.c`, `*.h`

Expected: **zero matches**.

- [ ] **Step 3: Grep for the new APIs**

Run via the `grep` tool:
- Pattern: `lwlte_mqtt_init|lwlte_mqtt_destroy`
- Include: `*.h`, `*.c`

Expected: declaration in `lwlte.h`; definition in `lwlte.c`; call sites in both example files.

- [ ] **Step 4: Final clean build**

Run: `esp-idf-eim_build_project`
Expected: success.

- [ ] **Step 5: Do NOT commit yet**

---

## Task 9: Update documentation

**Files:**
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/feature-roadmap.md` (only if it currently describes MQTT as configured inside the LTE init config)

- [ ] **Step 1: Update `docs/agents/classes.md`**

Use `grep` to find any mention of `mqtt_client` inside `lwlte_*_config_t` or of `lwlte_air780ep_config_mqtt_client_t`. For each hit, replace the description with the new pattern: MQTT client is created via `lwlte_mqtt_init(lte, &lwlte_mqtt_config_t)` after `lwlte_*_init()`, has its own init↔destroy (object) and start↔stop (FSM) lifecycle, and `lwlte_destroy()` remains the fallback. Add or update a subsection describing both lifecycle pairs.

- [ ] **Step 2: Update `docs/agents/architecture.md`**

Find the App-layer call sequence diagram or list. Update it to:

```
lwlte_air780ep_init / lwlte_ml307r_init
lwlte_register_event_callback
lwlte_mqtt_init
lwlte_start
... LWLTE_EVENT_NET_ONLINE ...
lwlte_mqtt_start
```

Note explicitly that `lwlte_*_config_t` now carries only Core/Modem/AT fields; MQTT configuration is supplied independently via `lwlte_mqtt_config_t`.

- [ ] **Step 3: Update `docs/agents/feature-roadmap.md` if needed**

Use `grep` to check whether this file currently describes MQTT as nested inside the init config. If yes, update the wording. If no mentions, skip this step.

- [ ] **Step 4: Do NOT commit yet**

---

## Task 10: Final verification and await user commit instruction

**Files:** None

- [ ] **Step 1: Final build**

Run: `esp-idf-eim_build_project`
Expected: success.

- [ ] **Step 2: Show `git status` and `git diff --stat` to the user**

Run:
```bash
git status
git diff --stat
```

Report the modified files. **Do not commit.**

- [ ] **Step 3: Wait for user to say "commit"**

Per project policy (`/Users/jovisdreams/.config/opencode/AGENTS.md` and explicit user instruction), the implementation is complete only when the user explicitly approves a commit. Once approved, stage all changes and create a single commit with a message following the repo style:

```bash
git add -A
git commit -m "refactor: decouple MQTT client init from LTE init config"
```

(Suggested message — confirm with user before running.)

---

## Self-Review Notes

**Spec coverage check (against `docs/superpowers/specs/2026-06-13-lwlte-mqtt-independent-init-design.md`):**

- ✅ §"Public API / `lwlte_mqtt_config_t`" → Task 1
- ✅ §"Public API / New APIs" prototypes → Task 2
- ✅ §"Public API / `lwlte_mqtt_init`" implementation → Task 3
- ✅ §"Public API / `lwlte_mqtt_destroy`" implementation → Task 3
- ✅ §"Internal Changes / `lwlte_air780ep_init`" → Task 4
- ✅ §"Internal Changes / `lwlte_ml307r_init`" → Task 5
- ✅ §"Concurrency invariants" → encoded in Task 3 implementation + notes
- ✅ §"`lwlte_destroy` fallback" → no change needed (existing `destroy_owned_resources` already correct); verified in Task 8 grep that no stale references break it
- ✅ §"Documentation / Examples" → Tasks 6, 7
- ✅ §"Documentation / `docs/agents/`" → Task 9
- ✅ §"Verification" → Tasks 8, 10

**Placeholder scan:** No "TBD"/"TODO"/"implement later". Every edit step contains the literal `oldString`/`newString` content. Where a file must be read first to confirm exact text (line numbers shift between tasks), the step says "Read ... to confirm exact text" before the edit — this is a verification step, not a placeholder.

**Type/name consistency:**
- `lwlte_mqtt_config_t` used consistently in Tasks 1, 2, 3, 6, 7.
- `lwlte_mqtt_init` / `lwlte_mqtt_destroy` consistent across Tasks 2, 3, 6, 7, 8.
- `begin_api_call(me, true, &core)` for init; `begin_api_call(me, false, NULL)` for destroy — matches the spec's corrected invariant.
- `MQTT_CLIENT_TRANSPORT_PLAIN_TCP`, `mqtt_client_config_t`, `mqtt_client_handle_t`, `mqtt_client_create`, `mqtt_client_register_event_callback`, `mqtt_client_destroy`, `lwlte_handle_mqtt_event` — all verified against actual headers (`src/mqtt_client/mqtt_client.h`, `src/lwlte/lwlte_priv.h`).

**Scope check:** Single focused refactor, no sub-project decomposition needed.
