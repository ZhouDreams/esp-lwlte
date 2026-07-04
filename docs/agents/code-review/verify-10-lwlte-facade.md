# Verification: LWLTE Facade (general)

## ✅ 确认的问题

- **原报告: end_api_call api_done_sema 竞态**
  - 验证结论: 与 Module #9 ping client 完全相同的竞态模式。
  - `end_api_call`（lwlte.c:1260-1271）：`me->lock` take → `active_api_calls--` → 保存 `done_sema` → `me->lock` give（line 1266）→ **锁外** `xSemaphoreGive(done_sema)`（line 1269）。
  - `wait_api_calls_idle`（lwlte.c:1283-1296）：循环中 `me->lock` take → 读 `active` → `me->lock` give → 若 `active == 0` 直接 return（line 1289-1290）。
  - `lwlte_destroy`（lwlte.c:288-337）：`wait_api_calls_idle` 返回后 → `destroy_owned_resources` → 注销事件处理器 → 删除 `api_done_sema`（line 324）→ 删除 `lock`（line 332）→ `free(me)`（line 335）。
  - 竞态确认：Thread A give `me->lock`（mutex 带优先级继承）后，FreeRTOS 可切换到阻塞在 `me->lock` 上的 Thread B。Thread B 读 `active == 0` 返回，继续 destroy 删除 sema。Thread A 恢复后 give 已删除的 sema。
  - `end_api_call` 被 22 处 API 函数调用（`lwlte_start`/`stop`/`get_state`/`ping`/`ssl_provision`/`http_request`/`mqtt_*`/`tcp_*` 等），影响面广。
  - 结论：竞态真实存在。

## ❌ 误报

（无）

## ⚠️ 部分正确

（无）

## 修复方案

将 `xSemaphoreGive(me->api_done_sema)` 移入 `me->lock` 临界区内，删除局部变量：

```c
static void end_api_call(lwlte_handle_t me)
{
    if (!me || !me->lock) {
        return;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->active_api_calls > 0) {
        me->active_api_calls--;
    }
    if (me->active_api_calls == 0 && me->api_done_sema) {
        xSemaphoreGive(me->api_done_sema);
    }
    xSemaphoreGive(me->lock);
}
```

**安全性分析**：与 ping client 修复相同。`wait_api_calls_idle` 取 `done_sema` 在锁外（line 1295），无嵌套锁死锁风险。give sema 在锁内时，若 destroy thread 阻塞在 `done_sema` 上，它被唤醒后取 `me->lock` 阻塞（API thread 仍持有），API thread give lock → destroy thread 取 lock → 读 `active == 0` → 返回 → 删除 sema。此时 API thread 已完成 give 并即将返回 → 安全。

## 修复记录

- **lwlte.c:1260-1268** — 将 `xSemaphoreGive(me->api_done_sema)` 移入 `me->lock` 临界区内
  - 改动: 删除局部变量 `api_idle`/`done_sema` 和锁外的 `if (api_idle && done_sema)` 块，改为在锁内直接检查 `active_api_calls == 0 && me->api_done_sema` 后 give。
  - 构建验证: ✅

## 模块交付清单

- **Change summary**: 修复 `end_api_call` 中 `api_done_sema` give 移入锁内的竞态条件（1 处改动）。
- **Resource budget**: `lwlte_t` ~100B + 1 mutex + 1 counting sem (max 32767) + 1 binary sem。无队列、无 task（service 层各自管理）。`core_replies` 临时 calloc max 4KB。
- **Lifecycle / ownership notes**: 同步 cmd ctx 在调用方栈上（callback give sema → caller take sema 读结果）。HTTP body: core alloc → callback ownership transfer → ctx → response → caller free。MQTT/TCP handle 在 facade 中 stored + destroyed。
- **Failure-path review**: `core_submit_cmd` 失败 → 跳过 take、删 sema、返回错误。`lwlte_destroy` 失败 → `restore_after_destroy_failure` 回滚。HTTP 响应 zero-init 确保任何路径可 release。
- **Cross-module contract review**: 未破坏 core/service 契约。HTTP body ownership 链完整。MQTT init check-create-check 防并发双重初始化。
- **Residual risks**: 同步 API（`portMAX_DELAY` take）若 core callback 不触发则永久阻塞 + 阻塞 destroy——与所有同步 service 共享的固有风险。`lwlte_tcp_destroy` 在锁内调用 `tcp_client_destroy`（可阻塞 ~100ms 等待 FSM 退出）——可接受但与 MQTT 的锁外模式不一致。
