# Verification: TCP Client Service

## ✅ 确认的问题

- **原报告: tcp_protocol_data_owned_t 死字段**
  - 验证结论: `rg "owned->payload" src/tcp_client/` 在 `tcp_protocol_data_cb`（创建 `tcp_protocol_data_owned_t` 的唯一位置）中无匹配。`tcp_protocol_data_cb`（tcp_client.c:924-938）只设 `conn_id`、`reason`、`modem_error_code`。`handle_protocol_data`（tcp_client.c:1364-1383）只读这三个字段。`free_fsm_sig_payload` 的 `free(owned->payload)` 对 NULL 是 no-op。确认 `payload` 和 `payload_len` 从不写入、从不读取。

- **原报告: remote_closed_event_pending 死字段**
  - 验证结论: `rg "remote_closed_event_pending" src/tcp_client/` 返回 5 处，全部是赋值（`= true` 或 `= false`）。`rg "if.*remote_closed_event_pending|== .*remote_closed_event_pending|remote_closed_event_pending.*==" src/tcp_client/` 无匹配。确认字段写而不读。

- **原报告: handle_core_cmd_done 圈复杂度偏高**
  - 验证结论: 函数跨 1235-1362 行（128 行），含 4 个 case 分支，RECV case 内嵌 payload ownership 转移、remaining_len 链式 recv、错误分支共 3 条路径。估算圈复杂度 ~18。超过建议上限 15。

## ❌ 误报

（无）

## ⚠️ 部分正确

（无）

## 模块交付清单

- **Change summary**: 本轮审查未修改代码。3 项 🟢 发现待用户决定是否修复。
- **Resource budget**:
  - `tcp_client_t`: ~80B（calloc）
  - `tcp_client_conn_t`: ~120B（calloc）
  - FSM 队列: `16 × sizeof(tcp_fsm_sig_t)` ≈ 16 × 60B = 960B
  - send 队列: `4 × sizeof(tcp_send_item_t)` = 4 × 16B = 64B（槽位），每项 payload ≤ 1460B
  - FSM task 栈: 4096B
  - 事件 payload: `sizeof(lwlte_tcp_event_data_t)` ≈ 80B per event post
  - 总量 < 7KB，对 ESP32-C3 (~300KB heap) 无压力。
- **Lifecycle / ownership notes**:
  - `tcp_open_owned_t.host`: owned（clone_string），随 OPEN 信号传递，FSM 处理后由 `free_fsm_sig_payload` 释放。
  - `tcp_send_item_t.data`: owned（clone_payload），入 send_queue，`handle_send_ready` 出队后传给 `submit_socket_send`，core 深拷贝后立即 `free(item.data)`。
  - `core_socket_recv_result_t.payload`: callback 中深拷贝（`tcp_core_cmd_done_cb`），通过 FSM 信号传递，事件 post 时 ownership 转移到事件系统（`owns_payload`），或由 `free_fsm_sig_payload` 释放。
  - conn 本身: `active_refs` 引用计数控制生命周期。`tcp_client_conn_destroy` 标记 `destroyed`，`active_refs == 0` 时 `cleanup_conn` 释放。
- **Failure-path review**:
  - `calloc` 失败 → 返回 `ESP_ERR_NO_MEM`，已分配资源由 `cleanup_conn`/`cleanup_partial_client` 清理。
  - `xQueueSend` 失败（FSM 队列满） → 返回 `ESP_ERR_TIMEOUT`，payload 释放，conn 状态不变。
  - `xQueueSend` 失败（send 队列满） → 返回 `ESP_ERR_TIMEOUT`，`item.data` 释放，FSM 信号已入队但无数据（spurious wakeup，harmless）。
  - `esp_event_post` 失败 → terminal event 保持 pending，FSM task 每 100ms 重试。
  - `core_submit_cmd` 失败 → `active_refs` 回滚，`pending_cmd` 不设置。
- **Cross-module contract review**: 未破坏门面/Core/Modem/AT Engine 契约。与 core 层通过 `core_submit_cmd`（深拷贝）+ callback（user_ctx = conn）交互，与 facade 层通过 event loop 解耦。
- **Residual risks**:
  - `tcp_protocol_closed_cb` 和 `handle_lwlte_event` 对 `send_fsm_sig_wait` 返回值 `(void)` 忽略——`portMAX_DELAY` 保证只在 destroying 时失败，可接受。
  - terminal event 若事件循环持续满则 destroy 被阻塞——FSM 重试机制可缓解，极端场景需上机验证。
  - `max_conns` 当前硬限为 1；未来扩展为多连接时需重新审查 `deferred_destroy_conn` 单指针设计。
