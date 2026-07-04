# Code Review: TCP Client Service

**日期**: 2026-07-04
**文件**: `src/tcp_client/tcp_client.c`, `tcp_client.h`, `tcp_client_priv.h`

## 🔴 高严重度

（无）

## 🟡 中严重度

（无）

## 🟢 低严重度

- **tcp_client_priv.h:72-78** — `tcp_protocol_data_owned_t.payload` 和 `.payload_len` 是死字段。
  - `tcp_protocol_data_cb`（tcp_client.c:924-938）创建 `owned` 后只设 `conn_id`、`reason`、`modem_error_code`，从不设 `payload`/`payload_len`（calloc 归零）。
  - `handle_protocol_data`（tcp_client.c:1364-1383）只读 `owned->conn_id`/`reason`/`modem_error_code`，不访问 `payload`/`payload_len`。
  - `free_fsm_sig_payload`（tcp_client.c:1771-1778）对 `owned->payload` 调 `free(NULL)`，是 no-op。
  - 建议修复：从 `tcp_protocol_data_owned_t` 删除 `payload` 和 `payload_len` 字段；同步删除 `free_fsm_sig_payload` 中 `TCP_SIG_PROTOCOL_DATA` 分支的 `free(owned->payload)` 行。

- **tcp_client_priv.h:130** — `tcp_client_conn_t.remote_closed_event_pending` 是死字段。
  - 在 5 处赋值（tcp_client.c:749, 774, 783, 1707, 1717），但在整个代码库中从未被读取——没有 `if (conn->remote_closed_event_pending)` 或任何条件引用。
  - 其意图的状态跟踪已被 `terminal_event_pending` + `remote_closed_event_posted` 覆盖。
  - 建议修复：从结构体删除字段；删除所有 5 处赋值。

- **tcp_client.c:1235-1362** — `handle_core_cmd_done` 圈复杂度偏高（~15-20）。
  - SOCKET_RECV 分支（1301-1339）尤其密集，包含 payload ownership 转移、remaining_len 链式 recv、错误处理三条路径。
  - 建议修复（可选）：将各 case 提取为 `handle_recv_done`/`handle_close_done` 等独立 helper，降低主函数复杂度。

## 无问题维度

- **维度 A（资源账本）**：单连接设计（`max_conns ≤ 1`），所有分配适中。FSM 队列 16×sizeof(tcp_fsm_sig_t) ≈ 1KB；send 队列 4×sizeof(tcp_send_item_t) = 64B + 每项最大 1460B payload。FSM task 栈 4096B。总量对 ESP32-C3 heap 无压力。
- **维度 B（内存安全）**：ownership 跟踪严谨。`core_submit_cmd` 对 `SOCKET_SEND.data` 做深拷贝（core.c:1021），`handle_send_ready` 在 `submit_socket_send` 返回后释放 `item.data` 是安全的。RECV 结果在 callback 中深拷贝 payload，FSM 处理后释放。事件 payload ownership 通过 `owns_payload` 标志在事件系统和 TCP client 之间正确转移。
- **维度 C（并发）**：锁顺序一致：`me->lock → conn->send_queue_lock → conn->lock`（`tcp_client_send`）；`me->lock → conn->lock`（`tcp_client_close`、`tcp_client_conn_destroy`、`acquire_current_conn`）。`send_queue_lock` 仅在 `tcp_client_send` 中与其他锁嵌套，其他路径（`handle_send_ready`、`clear_send_queue`）单独持有。无死锁路径。generation 编号正确过滤过期信号。
- **维度 D（失败路径）**：`malloc` 失败返回 `ESP_ERR_NO_MEM` 并清理。队列满返回 `ESP_ERR_TIMEOUT` 并释放 payload。事件 post 失败由 FSM task 每 100ms 重试（`handle_deferred_work` → `post_pending_terminal_event`）。`core_submit_cmd` 失败时 active_refs 回滚。
- **维度 E（AT/Modem）**：protocol callback 正确注册/注销。`conn_id` 匹配过滤。`conn_can_submit` 在持锁状态下原子检查状态并提交。remote-closed 与 pending-cmd 的交错通过 `latch_remote_closed` + deferred handling 正确处理。
- **维度 F（跨模块契约）**：与 core 层接口清晰——`core_submit_cmd` 深拷贝所有数据，callback 通过 `user_ctx` 传递 conn 指针，active_refs 引用计数确保 conn 在 callback 期间存活。与 facade 层通过 event loop 解耦。
- **维度 G（类型与边界）**：`len > max_tx_len` 在分配前检查。`conn_id` 为 `uint8_t`。无整数溢出风险。
- **维度 H（代码质量）**：命名一致（`tcp_` 前缀），双语注释，section 组织规范。除 `handle_core_cmd_done` 外函数复杂度合理。
