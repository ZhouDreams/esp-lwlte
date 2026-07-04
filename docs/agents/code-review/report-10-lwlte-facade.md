# Code Review: LWLTE Facade (general)

**日期**: 2026-07-04
**文件**: `src/lwlte/lwlte.c`, `lwlte_priv.h`

## 🔴 高严重度

（无）

## 🟡 中严重度

- **lwlte.c:1254-1271** — `end_api_call` 中 `api_done_sema` 在 `me->lock` 临界区外 give，与 `wait_api_calls_idle` / `lwlte_destroy` 存在竞态。
  - **竞态路径**（与 Module #9 ping client 相同模式）：
    1. Thread A（最后一条 API 调用结束）在 `end_api_call` 中：持 `me->lock` → `active_api_calls` 减到 0 → 保存 `done_sema` → **give `me->lock`**（line 1266）。
    2. Thread B（destroy）：在 `wait_api_calls_idle` 中阻塞在 `xSemaphoreTake(me->lock)`（line 1284）。Thread A give mutex 后被唤醒（若优先级更高则立即抢占）。
    3. Thread B 取 `me->lock`，读 `active_api_calls == 0`，give `me->lock`，返回。
    4. Thread B 回到 `lwlte_destroy`，执行 `destroy_owned_resources` → 注销事件处理器 → 删除 `api_done_sema`（line 324）。
    5. Thread A 恢复执行 `xSemaphoreGive(done_sema)`（line 1269）→ **use-after-free**。
  - **影响范围更严重**：facade destroy 不仅删 sema，还可能 free `me` 本身（line 335）。虽然 `end_api_call` 在 give sema 后不再访问 `me`，但 `done_sema` 局部变量指向已删除的 sema → UB。
  - 建议修复：将 `xSemaphoreGive(me->api_done_sema)` 移入 `me->lock` 临界区内。

## 🟢 低严重度

（无显著问题）

## 无问题维度

- **维度 A（资源账本）**：`lwlte_t` ~100B + 1 mutex + 1 counting sem (32767) + 1 binary sem。`core_replies` 在 ping 中 `calloc(count, sizeof(core_ping_reply_t))`（max 100×~40B = 4KB），用完即释放。HTTP body 由 core 分配、ownership 转移给调用方。无乘法型常驻分配。
- **维度 B（内存安全）**：同步命令 ctx（`lwlte_sync_cmd_ctx_t`/`lwlte_http_cmd_ctx_t`）在调用方栈上，callback give sema 后调用方 take sema 读取——semaphore 提供内存屏障。HTTP body ownership 在 callback 中明确转移（成功→ctx→response→caller；失败→callback 内 `free(hr->body)`）。MQTT init 使用 check-create-check 防止并发双重初始化。
- **维度 C（并发）**：除 `end_api_call` 竞态外，锁使用正确。`lwlte_mqtt_destroy` 在锁外调用 `mqtt_client_destroy`（不持锁阻塞）。`lwlte_tcp_destroy` 在锁内调用 `tcp_client_destroy`（可接受，destroy 是生命周期操作）。`lwlte_destroy` 注销事件处理器后取 `me->lock` 确保在途 handler 已完成（line 317-321 注释清晰）。
- **维度 D（失败路径）**：`core_submit_cmd` 失败 → 跳过 take、删 sema、返回错误。`lwlte_destroy` 失败 → `restore_after_destroy_failure` 回滚 `destroying` 标志并 drain `ready_sema`。`lwlte_http_request` 响应 zero-init 确保任何返回路径都可安全 `release()`。
- **维度 E（AT/Modem）**：Facade 不直接操作 modem，全部委托给 core。无特殊风险。
- **维度 F（跨模块契约）**：HTTP body 从 core → callback → ctx → response → caller 的 ownership 链条正确。MQTT/TCP client handle 在 facade 中存储并正确销毁。`facade_ready_handler` 监听 READY/ERROR 事件驱动 `lwlte_wait_ready` 同步。
- **维度 G（类型与边界）**：超时使用合理默认值（SSL provision 60s、SSL status 30s、HTTP 120s）。无整数溢出。
- **维度 H（代码质量）**：函数组织清晰，双语注释完整。`non_negative_int` helper 简洁安全。状态映射函数覆盖所有枚举值含 default。
