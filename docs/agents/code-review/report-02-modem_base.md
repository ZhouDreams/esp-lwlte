# Code Review: Modem Base / Wrapper（#2）

**日期**: 2026-07-01
**文件**: `src/modem/modem.c`, `src/modem/modem.h`, `src/modem/modem_priv.h`
**审查范围**: Modem 基类（`struct modem_t`）生命周期、事件队列/event_task 解耦、回调注销同步、`modem_post_event` PROTOCOL_DATA ownership，以及全部 `modem_*` 包装 API 的参数/状态校验与 ops 多态分发。
**审查维度**: A 资源账本 / B 内存安全 / C 并发死锁 / D 失败路径 / E AT-Modem / F 跨模块契约 / G 类型边界 / H 代码质量

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

- **`src/modem/modem.c:326-337`** — destroy 回滚路径在子类 `ops->destroy` 失败后把状态恢复为 `READY` 并清零 `destroying`，但此时 **event_task 已被 `modem_base_stop_event_task` 停止**（`me->event_task == NULL`、`me->event_task_done_sema` 仍存在但无任务可唤醒）。
  - 后果：回滚后 `modem_get_state()` 返回 `READY`，`check_ready()` 通过，调用方若继续使用 modem，命令路径（ops 直调）仍可用，但 `modem_post_event()` 因 `!me->event_task`（`modem.c:263`）对所有事件返回 `ESP_ERR_INVALID_STATE`，URC/协议数据永远无法上报 Core；event_task 无法重启。modem 处于"看起来可用、实则事件哑火"的降级态。
  - 可达性确认：`air780ep_destroy`（`modem_air780ep.c:3553-3555`）在 `air780ep_unregister_urcs` 失败时直接 `return ret`，触发该回滚分支。
  - 建议修复：回滚时不要恢复到原状态，应将 `state` 设为 `MODEM_STATE_ERROR`（`destroying` 可清零以便重试），让 `check_ready()` 拒绝大多数命令、明确告知调用方"对象已不可用，只能重试 destroy"。或在注释中明确"回滚后仅供重试 destroy，禁止继续使用"。

---

## 🟢 低严重度

- **`src/modem/modem.c:666-667`** — `modem_mqtt_publish` 强制要求 `publish->payload && publish->payload_len > 0`，拒绝零长度 payload。MQTT 协议本身允许空 payload（retain-only、遗嘱、心跳类消息）。确认这是有意的策略还是疏漏；若需支持空 payload 发布应放宽校验。
- **`src/modem/modem.c:304-310`** — `modem_destroy` 的允许状态白名单排除 `MODEM_STATE_INITIALIZING`。当前非 bug：两个子类 `start()` 在所有路径上都退出 INITIALIZING（成功→READY at `modem_air780ep.c:3633`/`modem_ml307r.c:3347`；失败→ERROR at `modem_air780ep.c:3646`/`modem_ml307r.c:3637`）。但这是**隐式跨模块契约**：任何未来子类若在 `start()` 失败时停留在 INITIALIZING，将导致 modem 无法 destroy（资源泄漏）。建议在 `modem_destroy` 或 `modem_state_t` 文档中显式声明该约束，或允许 INITIALIZING 作为防御。
- **`src/modem/modem.c:351` / `:363` / `:378` 等校验风格不一致**（H 代码质量）—— 部分包装函数用 `ESP_RETURN_ON_FALSE(me && me->lock, ...)`，部分（`modem_start`/`modem_reset`/`modem_activate_pdp` 等）只用 `ESP_RETURN_ON_FALSE(me, ...)` 再交给 `check_ready()` 校验 `me->lock`。结果一致但写法不统一，建议统一为其中一种。

---

## 无问题维度

- **A 资源账本（最高优先级）**：本层唯一的乘法型分配是 `xQueueCreate(event_queue_size, sizeof(modem_event_t))`（`modem.c:154`）。`sizeof(modem_event_t)` ≈ 128B（union 最大成员 `modem_pdp_context_t` ≈ 122B 主导），默认 `event_queue_size=8` → 队列 ≈ 1KB；event_task 栈默认 4096B；4 个同步原语（lock/2×sema）≈ 320B FreeRTOS 开销。**整体基类 footprint ≈ 5.5KB**，配置项（`event_queue_size`/`event_task_stack`）只在本层使用，不被跨模块乘以更大上限，无 AT Engine 式池爆裂风险。
- **B 内存安全与生命周期**：PROTOCOL_DATA 的 heap ownership 契约清晰（`modem.h:319-327`：post 成功→Modem 拥有并回调后释放；失败→调用方拥有）。`release_event_payload`（`:936`）与 `drain_event_queue_payloads`（`:924`）对每条出队事件恰好释放一次；event_task 退出前 drain（`:849`）+ base_deinit 再 drain（`:201/:205`）幂等不重复释放，无 double-free。指针池无二维常驻数组。
- **C 并发 / 死锁 / 实时性**：
  - 回调注销同步（`modem_register_event_callback` deregister 分支 `:412-426` + event_task `:816-843`）实现正确：`event_cb=NULL` 与读取 `event_cb_active` 在同一锁段内原子完成；event_task 单线程使 `event_cb_active` 至多为 1；注销方在 `while(active>0)` 中以 done_sema 唤醒并锁内重读 active，无 missed-wakeup（give 先于 take 的 token 语义）。
  - event_task 回调执行时**不持 `me->lock`**（`:827` give 后才 `cb()`），回调可重入 modem API 而不死锁。
  - 从 event_task 自身调用 `modem_destroy`（`:298`）/deregister（`:395`）被显式 `xTaskGetCurrentTaskHandle()==me->event_task` 阻断，避免自毁。
  - stop 流程顺序正确：`event_task_done_sema` 在 event_task 给出后（`:851`）任务不再访问任何 `me` 资源即 `vTaskDelete`，stopper 随后才删除 lock/queue，无 use-after-free。
- **D 失败路径**：`modem_base_init` 任一 `ESP_GOTO_ON_FALSE` 失败均跳 `err:` 调 `modem_base_deinit` 反序清理（创建顺序 lock→queue→2×sema→task，deinit 全程带 NULL 防御 `:196/:210/:214/:218`）。半初始化失败可正确回收。
- **G 类型与边界**：各包装函数参数校验一致（NULL 指针、port>0、qos≤2、ping count 1..100 / data_len≤1024 / ttl≥1 / max_replies≥count 等），`ssl_credentials_valid`（`:865`）对 ptr/len 配对一致性 + 认证模式所需证书都做了校验；输出结构体（`result`/`status`/`response`）在 ops 前置 `memset` 清零（`:570/:724/:788`）。无 uint 下溢、VLA、整数溢出。

---

## 备注（待验证阶段确认）

- `modem_base_stop_event_task`（`:249`）在 `me->event_task` 非空时取 `me->event_task_done_sema`，依赖"event_task 存在 ⇒ sema 存在"的不变量（init 中 sema 先于 task 创建）。变量成立，非 bug，但属隐式依赖。
