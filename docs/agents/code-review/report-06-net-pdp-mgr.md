# Code Review: Core Net/PDP mgr（#6）

**日期**: 2026-07-02
**文件**: `src/core/net_mgr.c`（997 行）、`src/core/pdp_mgr.c`（117 行）

**审查范围**: 网络激活子状态机（SIM → 信号 → 注册 → 附着 → APN → PDP → IP）、重连定时器与回调同步、PDP context 缓存、deinit 定时器服务屏障。
**审查聚焦**: 激活流程的 destroying/stop 中止能力（#5 遗留线索）、定时器回调并发安全、状态迁移与事件发布完整性、deinit 清理顺序。

---

## #5 遗留线索确认

**`net_mgr_start_activation` 在销毁期间能及时中止** — 已确认。

- `check_activation_continue()`（`:567-581`）在每个 `run_activation_loop` 迭代顶部、每个 `run_activation_step` 的阻塞 `modem_*` 调用之后、以及 `wait_next_poll` 的 `vTaskDelay` 前后均被调用，检查 `core_is_destroying` + `core_stop_pending` + 总超时。
- `net_mgr_start_activation`（`:409-412`）对 `ESP_ERR_INVALID_STATE`（destroying/stop 引起）跳过 `fail_activation`，直接返回——**不发布网络错误事件**，符合文档约定。
- 销毁期间激活延迟上界 ≈ 一个轮询间隔（`NET_MGR_WAIT_POLL_INTERVAL_MS` = 1 秒）。

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

无。

---

## 🟢 低严重度

- **`src/core/net_mgr.c:351-358`** — `net_mgr_set_reconnect_enabled` 写 `reconnect_enabled` 不持锁，与定时器回调的加锁读取不对称。
  - 写方（FSM task，无锁）：`net_mgr_set_reconnect_enabled`（`:357`）、`net_mgr_start_activation`（`:396`）、`net_mgr_deactivate`（`:422`）直接赋值 `me->net_mgr.reconnect_enabled`。
  - 读方（timer daemon task，持锁）：`reconnect_timer_cb`（`:590-595`）在 `core->lock` 保护下读取 `reconnect_enabled`。
  - 定时器回调持锁读取，但写方不持锁——锁保护不完整，构成 data race。
  - **平台影响**：ESP32-C3（RISC-V 32-bit）上 `bool` 写读原子，最坏情况为 stale read：
    - 写 false 时读到 true → 发送冗余 RECONNECT 信号 → FSM 检查 destroying/stop 后忽略。安全。
    - 写 true 时读到 false → 不发送 RECONNECT → 激活流程已在运行，无影响。安全。
  - **建议**：在 `net_mgr_set_reconnect_enabled` 中持锁写入，与读方对称。

---

## 无问题维度

- **A 资源账本**: `net_mgr_t` 是 `core_handle_t` 的内嵌组合成员（非独立分配）；`reconnect_timer`（1 个 one-shot xTimer）+ `reconnect_cb_done_sema`（1 个 binary semaphore）是 net_mgr 唯一动态资源。`pdp_mgr_t` 含 `modem_pdp_context_t contexts[4]`（约 4×122B = 488B），纯栈/堆内嵌，无乘法型分配。
- **B 并发与同步**:
  - `reconnect_timer_cb` 在 timer daemon task 上运行，持 `core->lock` 递增/递减 `reconnect_cb_active`、读取 destroying/state/reconnect_enabled，不持锁调用 `core_fsm_send`（非阻塞 send）。回调极短，不阻塞 daemon。
  - `net_mgr_deinit` 的清理序列完备：cancel_reconnect → wait_timer_service_idle（屏障确保 stop 命令已处理）→ wait `reconnect_cb_active==0`（确保在飞回调完成）→ xTimerDelete(portMAX_DELAY) → wait_timer_service_idle → vSemaphoreDelete。无死锁（deinit 不持锁跨 wait）。
  - `wait_timer_service_idle` 拒绝从 timer daemon task 调用（`:537-539`），防自死锁。
  - `net_mgr.state` 读写均持 `core->lock`；`current_step`/`retry_count` 仅 FSM task 访问（单线程，无需锁）。
  - `pdp_mgr` 全部接口仅从 FSM task 调用（init → core_init；update/set_active → activation/handle_pdp_*），单线程无并发。
- **C 错误处理与传播**:
  - 激活失败路径完整：`fail_activation` → net_state=ERROR + core_state=ERROR + post NET_ERROR。
  - PDP activation INVALID_STATE 经 `classify_pdp_activation_invalid_state` 重查 SIM/reg/attach 并回退到对应步骤，而非直接失败。
  - PDP activation 其他错误先 `modem_deactivate_pdp` 清理再返回。
  - QUERY_IP 发现 PDP not active → 回退 WAIT_PACKET_ATTACH（处理激活后模块自行去激活的边界）。
- **D 状态机一致性**: NET_STEP_IDLE→CHECK_SIM→CHECK_SIGNAL→WAIT_REGISTRATION→WAIT_PACKET_ATTACH→SET_APN→ACTIVATE_PDP→QUERY_IP→DONE 正向链正确；QUERY_IP→WAIT_PACKET_ATTACH 回退合理；ERROR 为终止态。事件序列 STARTED→READY→CONNECTING→ONLINE/ERROR 与 `lwlte_event_id_t` 枚举顺序一致。
- **E 内存安全与生命周期**: `pdp_mgr` 用 `cid_valid(1..4)` + `cid_index(cid-1)` 做边界保护，无越界。`now_ms()` 在 uint32_t 空间回绕但差值运算（`now_ms() - activation_start_ms`）在无符号语义下正确，激活超时（≤120s）远小于回绕周期（~50 天）。
- **F ESP-IDF API**: `xTimerCreate`（one-shot, timerID=core handle）、`xTimerStop`/`xTimerStart`/`xTimerDelete`（portMAX_DELAY 或 0）、`xTimerPendFunctionCall`（屏障）、`vTaskDelay`（poll 间隔）、`xTaskGetTickCount`——均正确使用。

---

## 备注

- `pdp_mgr_get` 声明在 `core_priv.h` 但当前未被 net_mgr.c 或 core_fsm.c 调用——可能预留给未来查询接口，非 dead code（跨模块可见）。
- `pdp_mgr_set_active` 中 `me->contexts[index].cid = cid` 赋值与 init 的初始化重复（永远 no-op），无害冗余。
- 重连仅由 `MODEM_EVENT_PDP_DEACTIVATED` 触发（固定延迟 `reconnect_delay_ms`），初始激活失败不自动重连——设计如此，用户需显式重试。
