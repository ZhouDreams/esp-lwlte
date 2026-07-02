# Verify: Core main + FSM（#5）

**日期**: 2026-07-02
**对应报告**: `report-05-core-fsm.md`

---

## 🟡-1: HTTP_REQUEST body 泄漏（done_cb == NULL）

### 验证步骤

1. **`core_cmd_valid` 缺少 HTTP_REQUEST 的 done_cb 检查** — 已确认。
   - `core.c:1147-1149`：仅 `CORE_CMD_SOCKET_RECV` 要求 `done_cb != NULL`。
   - HTTP_REQUEST 分支（`core.c:1230-1239`）只验证 url/method/transport/body 合法性，不检查 `done_cb`。

2. **HTTP 成功路径 body 来自 modem heap** — 已确认。
   - `core_fsm.c:828-831`：`modem_http_request` 成功后 `result.body = modem_resp.body`。`modem_resp.body` 是 modem 层分配的 heap 指针（ownership 转移给 Core）。
   - `result` 是栈变量（`core_fsm.c:813`），`result.body` 是裸 heap 指针。

3. **finish_service_cmd 在 done_cb==NULL 时跳过回调** — 已确认。
   - `core_fsm.c:971-973`：`if (cmd->done_cb) { cb(...); }`——done_cb 为 NULL 时直接跳过。
   - `core_fsm.c:974`：`core_free_cmd(cmd)` 只释放命令结构体及其深拷贝字段（request 的 url/content_type/body），**不释放** `result.body`（response body）。
   - `result` 出栈后 `result.body` 指针丢失 → **泄漏确认**。

### 触发路径

```
core_submit_cmd(HTTP_REQUEST, done_cb=NULL)
  → core_cmd_valid: PASS (无 done_cb 检查)
  → clone_core_cmd: OK
  → FSM handle_service_cmd
    → ensure_net_online: OK
    → modem_http_request: OK, modem_resp.body = malloc(...)
    → result.body = modem_resp.body (heap)
    → finish_service_cmd(done_cb=NULL)
      → 跳过 done_cb
      → core_free_cmd(cmd)  // 释放 cmd，不释放 result.body
      → return
    → result 出栈，body 指针丢失 → LEAK
```

### 修复方案

在 `core_cmd_valid` 中将 `CORE_CMD_HTTP_REQUEST` 纳入 `done_cb` 必填检查：

```c
if ((cmd->type == CORE_CMD_SOCKET_RECV ||
     cmd->type == CORE_CMD_HTTP_REQUEST) && !cmd->done_cb) {
    return false;
}
```

与 SOCKET_RECV 对称——两者都通过 result_data 向回调转移 heap ownership。

### 结论: **确认（CONFIRMED）** — 修复。

---

## 🟢-1: release_modem_protocol_payload 重复定义

### 验证

- `core.c:1265-1277` 和 `core_fsm.c:977-989`：两处 `static void release_modem_protocol_payload(modem_event_t *event)` 实现完全相同（free topic + payload + 置零长度）。
- 均为 file-local static，各自编译单元独立——技术上合法。
- core.c 版由 `core_modem_event_cb`（modem event 回调入队失败时释放）使用；core_fsm.c 版由 `handle_signal`/`release_fsm_signal_payload`（信号处理后释放）使用。

### 结论: **确认** — DRY 瑕疵，不影响正确性。🟢 交用户决定是否提取共享。

---

## 🟢-2: core_get_net_state 缺少 me->lock 检查

### 验证

- `core.c:419-424`：`ESP_RETURN_ON_FALSE(me && state, ...)`——不检查 `me->lock`。
- `core.c:409-417`：`core_get_state` 检查 `me && state && me->lock`——不对称。
- 实践中 Core 句柄传给上层时已完成初始化（lock 必非 NULL），无实际风险。

### 结论: **确认** — 一致性瑕疵。🟢 交用户决定。

---

## 🟢-3: handle_start 阻塞后未复查 destroying

### 验证

- `core_fsm.c:404`：`modem_start(me->modem)` 阻塞调用，可达数秒~数十秒。
- 返回后（`:411-426`）直接执行 `handle_ready` + `net_mgr_start_activation`，中间无 `core_is_destroying` 检查。
- 若 destroy 在 modem_start 期间发起：`me->modem` 被置 NULL，但 modem_start 用调用时捕获的值已正常完成。
  - `handle_ready` → `core_set_state(READY)`：destroying=true → 拒绝写入 → 返回 ESP_ERR_INVALID_STATE（安全）。
  - `core_post_event`：destroying=true → 拒绝发布（安全）。
  - `net_mgr_start_activation`：应检查 destroying 并中止（待 #6 确认）；即使未中止，`modem_*(NULL)` 返回 ESP_ERR_INVALID_ARG（安全降级）。
- 无 crash/泄漏/事件泄漏，仅少量无效工作。

### 结论: **确认** — 防御性优化建议。🟢 交用户决定。

---

## 汇总

| # | 严重度 | 发现 | 结论 | 处置 |
|---|--------|------|------|------|
| 1 | 🟡 | HTTP_REQUEST body 泄漏（done_cb=NULL） | 确认 | **已修复** core_cmd_valid 加 HTTP_REQUEST done_cb 检查 |
| 2 | 🟢 | release_modem_protocol_payload 重复 | 确认 | **已修复** 提取到 core_priv.h 共享，core_fsm.c 删除重复定义 |
| 3 | 🟢 | core_get_net_state 缺 lock 检查 | 确认 | **已修复** 加 `me->lock` 到 ESP_RETURN_ON_FALSE |
| 4 | 🟢 | handle_start 阻塞后未复查 destroying | 确认 | **已修复** modem_start 返回后加 `core_is_destroying` 检查 |
