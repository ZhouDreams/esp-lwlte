# Code Review: MQTT Client Service（#7）

**日期**: 2026-07-02
**文件**: `src/mqtt_client/mqtt_client.c`（1177 行）、`src/mqtt_client/mqtt_client.h`（126 行）、`src/mqtt_client/mqtt_client_priv.h`（120 行）

**审查范围**: MQTT 生命周期（create/destroy/start/stop）、连接子状态机（CONFIGURE→TCP_CONNECT→CONNECT→CONNECTED）、运行时操作（subscribe/unsubscribe/publish）、Core command 提交与完成回调、协议数据回调与事件总线发布、FSM 任务停止协议。
**审查聚焦**: 命令/信号 ownership 与内存泄漏、FSM 状态一致性、停止流程完备性、并发边界。

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

- **`src/mqtt_client/mqtt_client.c:843-848`** — `handle_runtime_operation` 重入队列失败时信号 heap data 泄漏。
  - 当 `pending_cmd.active`（一个 Core command 正在执行）时，新到的 subscribe/unsubscribe/publish 信号被重排到队列尾部：
    ```c
    mqtt_fsm_sig_t requeue = *sig;
    sig->data = NULL;           // ownership 转移给 requeue
    sig->data_size = 0;
    (void)send_fsm_sig(me, &requeue);  // 返回值被忽略
    return;
    ```
  - 若 `send_fsm_sig` 失败（队列满，返回 `ESP_ERR_TIMEOUT`），`requeue.data`（已分配的 topic/publish 结构）指针丢失——栈上 `requeue` 出栈后无人释放。
  - `handle_signal` 末尾的 `free_mqtt_fsm_sig_payload(sig)` 因 `sig->data` 已被置 NULL 而跳过，无法补救。
  - **触发条件**：Core command pending + FSM 队列满（默认 16 槽）+ 新 subscribe/unsubscribe/publish 到达。在高频 publish 场景下可达。
  - **建议修复**：检查 `send_fsm_sig` 返回值，失败时释放 `requeue` 的 data：
    ```c
    if (send_fsm_sig(me, &requeue) != ESP_OK) {
        free_mqtt_fsm_sig_payload(&requeue);
    }
    ```

---

## 🟢 低严重度

- **`src/mqtt_client/mqtt_client_priv.h:103`** — `bool started` 字段声明但从未读写（dead field）。
  - `calloc` 初始化为 false，全文件无 `me->started` 赋值或读取。可删除或留待未来 start/stop 幂等校验使用。

- **`src/mqtt_client/mqtt_client_priv.h:70-71` + `mqtt_client.c:573-574`** — `pending_cmd.started_ms` / `timeout_ms` 被设置但从未读取。
  - `submit_core_cmd` 设置 `started_ms = 0` 和 `timeout_ms = MQTT_CLIENT_CMD_TIMEOUT_MS`，但无任何代码读取它们做超时检查。实际超时由 Core command 的 `timeout_ms` 字段 + Core FSM 执行兜底。这两个字段是 dead state。

- **`src/mqtt_client/mqtt_client.c:1110, 549`** — subscribe qos 通过 `sig->error_code` 字段传递。
  - `mqtt_client_subscribe` 把 qos 存入 `sig->error_code`（`:1110`），`submit_core_cmd` 读回 `(uint8_t)sig->error_code`（`:549`）。类型不安全的字段复用，正确但脆弱——若 `mqtt_fsm_sig_t` 字段语义变化易引入隐蔽 bug。

- **`src/mqtt_client/mqtt_client.c:868-900`** — `handle_protocol_data` 对 topic/payload 二次克隆。
  - `mqtt_protocol_data_cb`（Core FSM task）已深拷贝 topic/payload 到 `mqtt_protocol_data_owned_t`（第一次克隆）；`handle_protocol_data`（MQTT FSM task）再次克隆到 event bus payload（第二次克隆）。所有权链正确（两次克隆各有独立 free 路径），但可优化：将 `owned` 直接转移给 event payload，设 `sig->data = NULL` 跳过 `free_mqtt_fsm_sig_payload`。

---

## 无问题维度

- **A 资源账本**: `calloc(sizeof(struct mqtt_client_t))` 单实例 + 4 个配置字符串 + 1 mutex + 1 queue + 2 binary semaphores。FSM 信号 `sizeof(mqtt_fsm_sig_t)` × queue_size（默认 16），每信号最多携带一个 owned struct。无乘法型分配。
- **B 并发与同步**:
  - `me->lock` 保护 `state`/`destroying`/`pending_cmd`；FSM 调用 `core_submit_cmd()` 时不持锁——符合设计。
  - FSM 停止协议（destroying 标记 → fsm_task_done_sema）正确：`send_fsm_sig` 在 destroying 后拒绝新信号，FSM drain 后 give 信号量，destroy 二次 drain 兜底。
  - `wait_stop_before_destroy` 先检查 STOPPED 短路返回，再 drain stale tokens，再发 STOP + 超时等待（`MQTT_CLIENT_STOP_WAIT_MS` = 120 秒）。
  - Protocol data callback（Core FSM task）和 event handler（esp_event task）只投递信号，不直接修改 MQTT 状态。
  - Core command done callback 只投递 `MQTT_SIG_CORE_CMD_DONE`，不直接修改状态——符合硬约束。
- **C 错误处理**: 连接失败 → ERROR + post ERROR_EVENT；stop 期间 command done 按 stop_requested 路径处理（断开 → TCP 断开 → complete_stop）；net offline/protocol closed → 清除连接状态 + WAITING_NET 或 complete_stop。
- **D 状态机一致性**: STOPPED → WAITING_NET/CONNECTING → CONNECTED ↔ DISCONNECTING → STOPPED 回路正确。CONNECT 子步骤 CONFIGURE → TCP_CONNECT → CONNECT → DONE 正向链正确。stop_requested 与 in-flight command 的交叉处理（CONNECT 成功后立即 DISCONNECT）逻辑完备。
- **E 内存安全与生命周期**:
  - 配置字符串 clone/free 对称（normalize_config → free in destroy/cleanup_partial_client）。
  - 信号 data clone/free 对称：API 层 clone → 信号入队 → FSM 处理后 `free_mqtt_fsm_sig_payload` 释放；send 失败时 API 层释放。
  - `mqtt_protocol_data_owned_t` ownership：callback 分配 → send 成功转移给队列 → FSM 处理后释放；send 失败 callback 自释放。无 double-free。
  - event bus `LWLTE_MQTT_EVENT_DATA` 的 topic/payload heap-owned + `owns_payload=true`，post 失败时立即 free，成功时由 handler 通过 `lwlte_mqtt_event_data_release` 释放。
- **F ESP-IDF API**: `xTaskCreate`（检查 pdPASS）、`xQueueCreate/Send/Receive`（0 timeout 非阻塞）、`xSemaphoreCreateBinary`、`esp_event_handler_register/unregister/_with`、`esp_event_post/post_to`（0 timeout）——均正确使用。
- **G 日志**: create/destroy/connect/stop/error 路径有 ESP_LOGW；protocol data clone 失败有 warning。

---

## 备注

- `cleanup_partial_client`（`:380`）和 `mqtt_client_destroy`（`:987`）是两条独立清理路径——前者用于 create 中途失败（无 FSM task），后者用于正常销毁（含 wait_stop_before_destroy）。资源释放项相同但顺序略异，属常见 C 模式。
- `mqtt_client_create` 的 task 创建失败路径调用 `mqtt_client_destroy(me)`——此时 state 为 STOPPED，`wait_stop_before_destroy` 短路返回 OK，后续清理正常执行。验证无泄漏。
- 停止流程的 stop_step 状态机（IDLE → DISCONNECT → TCP_DISCONNECT）确保连接已建立时按 MQTT 会话断开 → TCP 通道断开顺序清理。
