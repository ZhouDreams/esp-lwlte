# Verification: Ping Service

## ✅ 确认的问题

- **原报告: end_ping_call active_done_sema 竞态**
  - 验证结论: 逐行追踪确认竞态路径存在。
  - `end_ping_call`（ping_client.c:280-291）：`me->lock` take → `active_calls--` → 保存 `active_done_sema` 到局部变量 → `me->lock` give（line 287）→ **锁外** `xSemaphoreGive(active_done_sema)`（line 290）。
  - `wait_active_calls_idle`（ping_client.c:299-309）：循环中 `me->lock` take（line 300）→ 读 `active_calls`（line 301）→ `me->lock` give（line 303）→ 若 `active_calls == 0` 直接 return（line 305-306）。
  - `ping_client_destroy`（ping_client.c:159-166）：`wait_active_calls_idle` 返回后立即 `vSemaphoreDelete(active_done_sema)`（line 164）。
  - 竞态确认：Thread A give `me->lock`（mutex，带优先级继承）后若 Thread B 阻塞在该 mutex 上，FreeRTOS 立即切换到 Thread B。Thread B 读 `active_calls == 0` 返回，delete sema。Thread A 恢复后 give 已删除的 sema。
  - FreeRTOS 互斥量 give 时确有 yield 语义：`xQueueGenericSend` → `prvUnlockQueue` → 若 `xYieldRequired != pdFALSE` 则 `taskYIELD_WITHIN_API()`（在 return 之前）。
  - `core_submit_cmd` 的 `done_cb` 在 core FSM task 中同步调用（core_fsm.c:974-975），callback give `done_sema` 唤醒 ping 调用方 task，ping 调用方 task 在 `ping_client_ping`（line 221 take done_sema → line 225 end_ping_call）中调用 `end_ping_call`——确认 ping 调用方 task 和 destroy task 是不同 task。
  - 结论：竞态真实存在，窗口窄但非零。

## ❌ 误报

（无）

## ⚠️ 部分正确

（无）

## 修复方案

将 `xSemaphoreGive(active_done_sema)` 移入 `me->lock` 临界区内：

```c
static void end_ping_call(ping_client_handle_t me)
{
    if (!me || !me->lock) {
        return;
    }

    SemaphoreHandle_t active_done_sema = NULL;
    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_calls > 0) {
        me->active_calls--;
        if (me->active_calls == 0) {
            active_done_sema = me->active_done_sema;
            xSemaphoreGive(active_done_sema);  /* 修复：在锁内 give */
        }
    }
    xSemaphoreGive(me->lock);
}
```

**安全性分析**：
- `xSemaphoreGive(active_done_sema)` 在 `me->lock` 内调用，若 destroy thread 阻塞在 `active_done_sema` 上，它被唤醒后尝试取 `me->lock`（在 `wait_active_calls_idle` 的循环中），但 `me->lock` 仍被 ping thread 持有 → destroy thread 再次阻塞。
- ping thread give `me->lock` → destroy thread 取 `me->lock` → 读 `active_calls == 0` → 返回 → 删除 sema。
- 此时 ping thread 已完成 `xSemaphoreGive(active_done_sema)` 并即将返回 → 无 use-after-free。
- 互斥量优先级继承保证 ping thread 在持锁期间继承 destroy thread 的优先级，加速临界区。

## 修复记录

- **ping_client.c:279-285** — 将 `xSemaphoreGive(me->active_done_sema)` 移入 `me->lock` 临界区内
  - 改动: 删除局部变量 `active_done_sema` 和锁外的 `if (active_done_sema) { xSemaphoreGive(...) }`，改为在锁内直接 `xSemaphoreGive(me->active_done_sema)`。
  - 构建验证: ✅

## 模块交付清单

- **Change summary**: 修复 `end_ping_call` 中 `active_done_sema` give 移入锁内的竞态条件（1 处改动）。
- **Resource budget**: `ping_client_t` ~32B + 1 mutex + 2 binary semaphore。无队列、无 task。`replies`/`summary` 由调用方提供（borrowed）。无乘法型分配。
- **Lifecycle / ownership notes**: `wait_ctx` 在调用方栈上（callback 同步写入后 give sema，调用方 take 后读取）。`replies`/`summary` borrowed 给 core（写入后不持有）。`host` 被 core 深拷贝。
- **Failure-path review**: `done_sema` 创建失败 → `end_ping_call` + `ESP_ERR_NO_MEM`。`core_submit_cmd` 失败 → 删 sema + `end_ping_call` + 返回错误。core drain → callback 以 `result_data=NULL` 触发 → `esp_result=ESP_FAIL`。
- **Cross-module contract review**: 未破坏 core 契约。`result_data` 对 PING 始终是 `esp_err_t *`，callback 转型正确。
- **Residual risks**: `ping_client_ping` 是阻塞调用（`portMAX_DELAY`），若 core FSM 异常导致 callback 不触发则永久阻塞——与所有 service 共享的固有风险。
