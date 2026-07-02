# Code Review: Core main + FSM（#5）

**日期**: 2026-07-02
**文件**: `src/core/core.c`（1277 行）、`src/core/core_fsm.c`（1020 行）、`src/core/core.h`、`src/core/core_priv.h`

**审查范围**: Core 生命周期（create/destroy/start/stop）、FSM 任务与信号分发、`core_submit_cmd` 命令克隆/执行/回收、协议数据回调路由、Modem 事件桥接。
**审查聚焦**: 并发与销毁安全、命令 ownership 与内存泄漏、FSM 状态一致性、协议回调注册/注销同步。

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

- **`src/core/core.c:1147` + `src/core/core_fsm.c:812-836`** — `CORE_CMD_HTTP_REQUEST` 未要求 `done_cb`，成功时 heap body 泄漏。
  - `core_cmd_valid()`（`:1142`）仅对 `CORE_CMD_SOCKET_RECV` 检查 `done_cb != NULL`（`:1147`），`CORE_CMD_HTTP_REQUEST` 不检查。
  - `handle_service_cmd` 的 HTTP 分支（`core_fsm.c:812-836`）：`modem_http_request` 成功时 `result.body = modem_resp.body`（heap 指针），随后 `finish_service_cmd` 调用 `done_cb`。若 `done_cb == NULL`，`finish_service_cmd`（`:964-975`）跳过回调直接 `core_free_cmd(cmd)`——**body 永不释放**。
  - `SOCKET_RECV` 已在验证层拦截无 callback 的情况（同理：payload ownership 转移给回调），HTTP body 同样是 heap ownership 转移，却缺少对称保护。
  - **触发条件**：调用方提交 `CORE_CMD_HTTP_REQUEST` 且 `done_cb == NULL`，网络 online 且 modem 成功返回 body。虽属调用方编程错误，但 SOCKET_RECV 已设先例——HTTP 应对称要求 `done_cb`。
  - **建议修复**：在 `core_cmd_valid()` 中将 HTTP_REQUEST 纳入 `done_cb` 必填检查，与 SOCKET_RECV 一致。

---

## 🟢 低严重度

- **`src/core/core.c:1265` + `src/core/core_fsm.c:977`** — `release_modem_protocol_payload` 在两个 `.c` 文件中各自 `static` 定义，实现完全相同（~15 行）。
  - core.c 版本由 `core_modem_event_cb` 使用（发送失败时释放克隆 payload）；core_fsm.c 版本由 `handle_signal`/`release_fsm_signal_payload` 使用（信号处理后释放）。
  - 两者正确且各自 file-local 合法，但若未来修改其一忘记同步另一份，会产生隐蔽的行为分歧。建议提取到 `core_priv.h` 声明为非 static 共享函数，或做 `static inline`。

- **`src/core/core.c:419-424`** — `core_get_net_state` 未检查 `me->lock`，与 `core_get_state`（`:411` 检查 `me && state && me->lock`）不对称。
  - `core_get_net_state` 仅检查 `me && state`（`:421`），然后委托 `net_mgr_get_state`。若在 Core 部分初始化时调用（lock=NULL），行为与 `core_get_state` 不一致。实践中 Core 句柄传给上层时已完成初始化（lock 必非 NULL），无实际风险，仅一致性瑕疵。

- **`src/core/core_fsm.c:404-426`** — `handle_start` 在阻塞 `modem_start()` 返回后未复查 `destroying`。
  - `modem_start` 可阻塞数秒至数十秒（EN 硬复位 + AT 轮询）。若 destroy 在此期间发起（`me->modem` 已被 `core_unregister_modem_callback` 置 NULL），`modem_start` 仍会用调用时捕获的有效句柄运行完毕。返回后 `handle_start` 继续执行 `handle_ready` + `net_mgr_start_activation`，这些操作均能安全降级（`core_set_state` 拒绝写入、`core_post_event` 拒绝发布、`net_mgr_start_activation` 应在销毁期间中止——待 #6 确认）。
  - 无 crash/泄漏，但产生少量无效工作。可在 `modem_start` 返回后、进入 `handle_ready` 前加一次 `core_is_destroying` 检查提前退出。

---

## 无问题维度

- **A 资源账本**: `calloc(sizeof(struct core_t))` 单实例；无乘法型分配。`core_cmd_t` 深拷贝按命令类型精确分配（字符串/payload），`free_core_cmd` 对称释放。FSM 队列 `sizeof(core_fsm_sig_t)` × queue_size（默认 16），`core_fsm_sig_t` 含内嵌 `modem_event_t`（最大 union 成员），栈/队列占用可控。
- **B 并发与同步**:
  - `me->lock` 仅保护 `state`/`destroying`/`stop_pending`/callback 槽位等短字段；FSM 调用 `modem_*` 时不持锁——符合设计约束。
  - 协议回调注册/注销的"NULL → wait_idle → set"序列正确：registration lock 序列化注册方，`protocol_callback_active[]` 计数 + 单一 done_sema 让注册方等待在飞回调完成。单注册方 + FSM 单线程回调，无多等待者竞争。
  - FSM 停止协议（stop_requested → SIG_STOP → task_done_sema）无死锁：`core_fsm_send` 在 `stop_requested` 后拒绝新信号，FSM 任务 drain 后 give 信号量，`core_fsm_deinit` 二次 drain 兜底。
  - `core_destroy` 的 retry_destroy 机制（destroy_in_progress + destroying 标记）允许部分失败后重试，状态机自洽。
- **D 状态机一致性**: STOPPED → STARTING → READY → NET_ACTIVATING → ONLINE ↔ ERROR 回路正确。`api_state_allows` 按 sig_type 限制入口状态；`handle_start` 接受 STOPPED 和 STARTING（API 已提前标记 STARTING）；`handle_stop` 拒绝 STOPPED/DESTROYING。
- **E 内存安全与生命周期**:
  - 命令 clone/free 对称：`clone_core_cmd` 每类型深拷贝必填字段，失败时 `free_core_cmd` 回滚已分配部分；`clone_payload(NULL,0)` 返回 NULL，与 `free(NULL)` 安全配对。
  - 协议 payload ownership 链清晰：modem 借用 → `core_modem_event_cb` 深拷贝 → FSM 队列 → `handle_modem_event` 传给 protocol callback（借用） → `release_modem_protocol_payload` 释放克隆。发送失败时由 `core_modem_event_cb` 释放，成功时由 FSM 释放——无 double-free。
  - `_Static_assert`（`core.c:27-34`）锁定 `core_net_state_t` 与 `lwlte_net_state_t` 枚举值一致，保证 `(lwlte_net_state_t)CORE_NET_STATE_ERROR` 强转安全。
- **F ESP-IDF API**: `xSemaphoreCreateMutex/Binary`、`xQueueCreate`、`xTaskCreate`（检查 pdPASS）、`xQueueSend/Receive`（timeout=0 非阻塞）、`vTaskDelete(NULL)` 自删除、`esp_event_post/post_to`（timeout=0）——均正确使用。
- **G 日志与诊断**: 销毁/启动/停止/错误路径均有 ESP_LOGW/E；`post_event_checked` 记录 post 失败；命令执行失败传播 esp_err_t 和 modem_error_code。

---

## 备注

- `core_destroy` → `core_deinit` → `core_unregister_modem_callback` 调用两次（destroy 入口 + deinit 内部），第二次因 `me->modem == NULL` 立即返回 OK——冗余但无害，属防御性设计。
- `handle_service_cmd` 的 invalid-state 拒绝路径（`:614-631`）和底部通用完成路径（`:842-852`）对 result_data 类型做了命令族区分（socket → `core_socket_result_t`、http → `core_http_result_t`、ssl → `esp_err_t*`、其他 → NULL）。类型不安全（void*），但各上层 service 的 done_cb 按 `cmd->type` 解读——当前可接受，后续若新增返回结构化结果的命令族可考虑统一为 tagged union。
- Core 借用 modem 句柄不拥有生命周期；destroy 期间 FSM 可能仍在阻塞 modem 调用中，destroy 需等待其返回——这是协作式取消模型的固有延迟（非 bug），`net_mgr_start_activation` 应在销毁期间中止以缩短延迟（待 #6 确认）。
