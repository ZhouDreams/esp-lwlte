# Modem Ops Function Typedef Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `modem_ops_t` easier to read by replacing inline function-pointer fields with named function-pointer typedefs.

**Architecture:** This is an internal Modem Adapter refactor only. `modem_ops_t` remains the virtual function table stored by `struct modem`, `s_air780ep_ops` keeps the same field names, and all public APIs remain unchanged.

**Tech Stack:** C99, ESP-IDF, FreeRTOS, project C OOP conventions in `docs/agents/oop-design.md` and coding style in `docs/agents/coding-style.md`.

---

## File Structure

- Modify: `src/modem/modem_priv.h`
- Responsibility: Own the internal modem base type, ops table type, and internal base helper declarations.
- Change: Add named function-pointer typedefs before `struct modem_ops`, then rewrite `struct modem_ops` fields to use those names.

- Modify: `src/modem/modem.c`
- Responsibility: Own common modem wrapper functions and internal call helpers.
- Change: Update `call_no_arg()` prototype and definition to use the new `modem_no_arg_fn` typedef.

- Verify only: `src/modem/modem_air780ep.c`
- Responsibility: Own the Air780EP `static const modem_ops_t s_air780ep_ops` instance.
- Change: No expected source change. It should compile unchanged because field names and compatible function signatures remain the same.

- Optional docs: `docs/agents/oop-design.md`
- Responsibility: Document the project's C OOP ops-table pattern.
- Change: Update the example if we want documentation to match the cleaner typedef style immediately.

---

### Task 1: Refactor `modem_ops_t` Type Definitions

**Files:**
- Modify: `src/modem/modem_priv.h:42-77`

- [ ] **Step 1: Write the failing compile check command**

Run after the edit, not before the edit:

```bash
idf.py build
```

Expected before implementation: not applicable, because this is a compile-only refactor. The useful failure condition is any compiler diagnostic about incompatible function-pointer assignments or unknown typedef names after the header edit.

- [ ] **Step 2: Replace inline function-pointer fields with named typedefs**

In `src/modem/modem_priv.h`, replace the current `modem_ops_t` block with this block:

```c
/**
 * @brief 无额外参数的调制解调器操作函数
 * @details Modem operation function without extra arguments
 */
typedef esp_err_t (*modem_no_arg_fn)(modem_t *me);

/**
 * @brief 获取模块信息操作函数
 * @details Get modem information operation function
 */
typedef esp_err_t (*modem_get_info_fn)(modem_t *me, modem_info_t *info);

/**
 * @brief 获取 SIM 状态操作函数
 * @details Get SIM status operation function
 */
typedef esp_err_t (*modem_get_sim_status_fn)(modem_t *me,
                                             modem_sim_status_t *status);

/**
 * @brief 获取信号质量操作函数
 * @details Get signal quality operation function
 */
typedef esp_err_t (*modem_get_signal_fn)(modem_t *me,
                                         modem_signal_t *signal);

/**
 * @brief 获取注册状态操作函数
 * @details Get registration status operation function
 */
typedef esp_err_t (*modem_get_registration_fn)(modem_t *me,
                                               modem_reg_status_t *status);

/**
 * @brief 获取分组域附着状态操作函数
 * @details Get packet attach status operation function
 */
typedef esp_err_t (*modem_get_packet_attach_status_fn)(modem_t *me,
                                                       bool *attached);

/**
 * @brief 设置 APN 操作函数
 * @details Set APN operation function
 */
typedef esp_err_t (*modem_set_apn_fn)(modem_t *me, uint8_t cid,
                                      const char *apn);

/**
 * @brief PDP 上下文 ID 操作函数
 * @details PDP context ID operation function
 */
typedef esp_err_t (*modem_pdp_cid_fn)(modem_t *me, uint8_t cid);

/**
 * @brief 获取 PDP 上下文操作函数
 * @details Get PDP context operation function
 */
typedef esp_err_t (*modem_get_pdp_context_fn)(modem_t *me, uint8_t cid,
                                              modem_pdp_context_t *pdp);

/**
 * @brief 配置 MQTT 操作函数
 * @details Configure MQTT operation function
 */
typedef esp_err_t (*modem_mqtt_configure_fn)(modem_t *me,
                                             const modem_mqtt_config_t *config);

/**
 * @brief MQTT 主题操作函数
 * @details MQTT topic operation function
 */
typedef esp_err_t (*modem_mqtt_topic_fn)(modem_t *me,
                                         const modem_mqtt_topic_t *topic);

/**
 * @brief MQTT 发布操作函数
 * @details MQTT publish operation function
 */
typedef esp_err_t (*modem_mqtt_publish_fn)(modem_t *me,
                                           const modem_mqtt_publish_t *publish);

/**
 * @brief Ping 诊断操作函数
 * @details Ping diagnostic operation function
 */
typedef esp_err_t (*modem_ping_fn)(modem_t *me,
                                   const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t max_replies,
                                   modem_ping_summary_t *summary);

/**
 * @brief 调制解调器虚函数表
 * @details Modem virtual function table
 */
typedef struct modem_ops {
    modem_no_arg_fn destroy;                         /**< 销毁子类资源； Destroy subclass resources */
    modem_no_arg_fn init;                            /**< 初始化模块； Initialize modem */
    modem_no_arg_fn reset;                           /**< 复位模块； Reset modem */
    modem_get_info_fn get_info;                      /**< 获取模块信息； Get modem information */
    modem_get_sim_status_fn get_sim_status;          /**< 获取 SIM 状态； Get SIM status */
    modem_get_signal_fn get_signal;                  /**< 获取信号质量； Get signal quality */
    modem_get_registration_fn get_registration;      /**< 获取注册状态； Get registration status */
    modem_get_packet_attach_status_fn get_packet_attach_status; /**< 获取分组域附着状态； Get packet attach status */
    modem_set_apn_fn set_apn;                        /**< 设置 APN； Set APN */
    modem_pdp_cid_fn activate_pdp;                   /**< 激活 PDP； Activate PDP */
    modem_pdp_cid_fn deactivate_pdp;                 /**< 去激活 PDP； Deactivate PDP */
    modem_get_pdp_context_fn get_pdp_context;        /**< 获取 PDP 上下文； Get PDP context */
    modem_mqtt_configure_fn mqtt_configure;          /**< 配置 MQTT； Configure MQTT */
    modem_no_arg_fn mqtt_tcp_connect;                /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    modem_no_arg_fn mqtt_connect;                    /**< 连接 MQTT； Connect MQTT */
    modem_no_arg_fn mqtt_disconnect;                 /**< 断开 MQTT； Disconnect MQTT */
    modem_no_arg_fn mqtt_tcp_disconnect;             /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
    modem_mqtt_topic_fn mqtt_subscribe;              /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    modem_mqtt_topic_fn mqtt_unsubscribe;            /**< 取消订阅 MQTT 主题； Unsubscribe MQTT topic */
    modem_mqtt_publish_fn mqtt_publish;              /**< 发布 MQTT 消息； Publish MQTT message */
    modem_ping_fn ping;                              /**< 执行 Ping 诊断； Execute ping diagnostic */
} modem_ops_t;
```

- [ ] **Step 3: Keep `struct modem` unchanged**

Confirm this line remains unchanged in `src/modem/modem_priv.h`:

```c
const modem_ops_t *ops;                       /**< 虚函数表； Virtual function table */
```

- [ ] **Step 4: Commit header-only ops type cleanup**

```bash
git add src/modem/modem_priv.h
git commit -m "refactor(modem): name ops function pointer types"
```

---

### Task 2: Update Internal Helper Signature

**Files:**
- Modify: `src/modem/modem.c:66-76`
- Modify: `src/modem/modem.c` function definition for `call_no_arg`

- [ ] **Step 1: Update the static prototype**

Replace the existing prototype:

```c
static esp_err_t call_no_arg(modem_t *me, esp_err_t (*fn)(modem_t *me));
```

with:

```c
static esp_err_t call_no_arg(modem_t *me, modem_no_arg_fn fn);
```

- [ ] **Step 2: Update the function definition**

Find the `call_no_arg` definition near the bottom of `src/modem/modem.c` and replace its signature with:

```c
static esp_err_t call_no_arg(modem_t *me, modem_no_arg_fn fn)
```

Do not change the function body.

- [ ] **Step 3: Verify all callers still pass named ops fields**

Confirm these calls remain unchanged:

```c
return call_no_arg(me, me->ops->init);
return call_no_arg(me, me->ops->reset);
```

If other no-argument ops wrappers exist, they may also continue passing `me->ops->...` fields without casts.

- [ ] **Step 4: Build after helper signature update**

```bash
idf.py build
```

Expected: build succeeds. If it fails with an unknown type error for `modem_no_arg_fn`, ensure `modem.c` includes `modem_priv.h` before using the typedef. It already includes `modem_priv.h` at the top.

- [ ] **Step 5: Commit helper signature cleanup**

```bash
git add src/modem/modem.c
git commit -m "refactor(modem): reuse no-arg ops typedef"
```

---

### Task 3: Verify Air780EP Ops Initializer Compatibility

**Files:**
- Verify only: `src/modem/modem_air780ep.c:809-831`

- [ ] **Step 1: Inspect `s_air780ep_ops`**

Confirm the initializer remains field-based and does not need casts:

```c
static const modem_ops_t s_air780ep_ops = {
    .destroy = air780ep_destroy,
    .init = air780ep_init,
    .reset = air780ep_reset,
    .get_info = air780ep_get_info,
    .get_sim_status = air780ep_get_sim_status,
    .get_signal = air780ep_get_signal,
    .get_registration = air780ep_get_registration,
    .get_packet_attach_status = air780ep_get_packet_attach_status,
    .set_apn = air780ep_set_apn,
    .activate_pdp = air780ep_activate_pdp,
    .deactivate_pdp = air780ep_deactivate_pdp,
    .get_pdp_context = air780ep_get_pdp_context,
    .mqtt_configure = air780ep_mqtt_configure,
    .mqtt_tcp_connect = air780ep_mqtt_tcp_connect,
    .mqtt_connect = air780ep_mqtt_connect,
    .mqtt_disconnect = air780ep_mqtt_disconnect,
    .mqtt_tcp_disconnect = air780ep_mqtt_tcp_disconnect,
    .mqtt_subscribe = air780ep_mqtt_subscribe,
    .mqtt_unsubscribe = air780ep_mqtt_unsubscribe,
    .mqtt_publish = air780ep_mqtt_publish,
    .ping = air780ep_ping,
};
```

- [ ] **Step 2: Build to validate function-pointer type compatibility**

```bash
idf.py build
```

Expected: build succeeds with no incompatible pointer type warnings or errors for `s_air780ep_ops`.

- [ ] **Step 3: Commit only if `modem_air780ep.c` changed**

If no change was needed, skip this commit. If formatting or compatibility edits were needed, commit only that file:

```bash
git add src/modem/modem_air780ep.c
git commit -m "refactor(air780ep): align ops initializer with typed ops"
```

---

### Task 4: Optional Documentation Alignment

**Files:**
- Modify: `docs/agents/oop-design.md:315-330`
- Modify: `docs/agents/oop-design.md:451-454`

- [ ] **Step 1: Update the ops table example**

Replace the example in section `3.2 ops 操作表定义` with:

```c
/* src/modem/modem_priv.h — Modem 内部多态接口 */
typedef esp_err_t (*modem_no_arg_fn)(modem_t *me);
typedef esp_err_t (*modem_get_signal_fn)(modem_t *me,
                                         modem_signal_t *signal);
typedef esp_err_t (*modem_set_apn_fn)(modem_t *me, uint8_t cid,
                                      const char *apn);
typedef esp_err_t (*modem_pdp_cid_fn)(modem_t *me, uint8_t cid);

typedef struct modem_ops {
    modem_no_arg_fn destroy;
    modem_no_arg_fn init;
    modem_no_arg_fn reset;
    modem_get_signal_fn get_signal;
    modem_set_apn_fn set_apn;
    modem_pdp_cid_fn activate_pdp;
    modem_pdp_cid_fn deactivate_pdp;
} modem_ops_t;
```

- [ ] **Step 2: Update the summary rule**

Replace the `ops 表定义` rule row with:

```markdown
| ops 表定义 | 复杂签名先定义 `xxx_fn` 函数指针类型，`struct xxx_ops` 中只放字段名和类型名 |
```

- [ ] **Step 3: Build after docs change if source changed in same branch**

```bash
idf.py build
```

Expected: build succeeds. Documentation changes alone do not affect the build, but this confirms the source refactor still compiles.

- [ ] **Step 4: Commit docs alignment**

```bash
git add docs/agents/oop-design.md
git commit -m "docs: document named ops function pointer types"
```

---

### Task 5: Final Verification

**Files:**
- Verify: entire project

- [ ] **Step 1: Check worktree status**

```bash
git status --short
```

Expected: either clean, or only intentional uncommitted files if commits were intentionally skipped.

- [ ] **Step 2: Run full build**

```bash
idf.py build
```

Expected: build succeeds.

- [ ] **Step 3: Review final diff if commits were skipped**

```bash
git diff -- src/modem/modem_priv.h src/modem/modem.c src/modem/modem_air780ep.c docs/agents/oop-design.md
```

Expected: diff only contains named ops function-pointer typedefs, `call_no_arg()` signature cleanup, and optional docs alignment.

---

## Self-Review

- Spec coverage: The plan covers the requested style change for `modem_ops_t`, keeps behavior unchanged, and includes optional documentation alignment.
- Placeholder scan: No deferred implementation steps are present; each source edit includes exact replacement code or exact signatures.
- Type consistency: `modem_no_arg_fn`, `modem_get_info_fn`, `modem_get_sim_status_fn`, `modem_get_signal_fn`, `modem_get_registration_fn`, `modem_get_packet_attach_status_fn`, `modem_set_apn_fn`, `modem_pdp_cid_fn`, `modem_get_pdp_context_fn`, `modem_mqtt_configure_fn`, `modem_mqtt_topic_fn`, `modem_mqtt_publish_fn`, and `modem_ping_fn` are defined before `modem_ops_t` uses them.
