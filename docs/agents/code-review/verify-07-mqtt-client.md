# Verify: MQTT Client Service（#7）

**日期**: 2026-07-02
**对应报告**: `report-07-mqtt-client.md`

---

## 🟡-1: handle_runtime_operation 重排失败时 heap data 泄漏

### 验证步骤

1. **重排路径** — `handle_runtime_operation`（`:843-848`）：
   - `requeue = *sig`：复制信号结构，包括 `data` 指针（SUBSCRIBE/UNSUBSCRIBE 为 topic 字符串，PUBLISH 为 `mqtt_client_publish_t *`）。
   - `sig->data = NULL`：原始信号的 data 被清除，ownership 转移给 `requeue`。
   - `(void)send_fsm_sig(me, &requeue)`：**返回值被显式忽略**。

2. **send_fsm_sig 失败时** — `send_fsm_sig`（`:192-218`）在 `xQueueSend` 返回 `pdFALSE`（队列满）时返回 `ESP_ERR_TIMEOUT`。信号未入队，`requeue` 留在栈上。

3. **handle_signal 补救无效** — `handle_signal`（`:683`）调用 `free_mqtt_fsm_sig_payload(sig)`，但 `sig->data` 已为 NULL（`:845`），`free_mqtt_fsm_sig_payload` 跳过释放。

4. **data 指针丢失** — `requeue` 是栈变量，`handle_runtime_operation` 返回后 `requeue.data`（指向 heap 分配的 topic/publish）无人持有 → **泄漏**。

5. **free_mqtt_fsm_sig_payload 能释放对应类型** — 确认 `:337-350`：SUBSCRIBE/UNSUBSCRIBE 释放 `sig->data`（topic 字符串），PUBLISH 释放 `publish->topic` + `publish->payload` + `publish`。若对 `requeue` 调用即可正确释放。

### 触发条件

- Core command pending（如 MQTT CONNECT 流程中的 CONFIGURE/TCP_CONNECT/CONNECT 之一正在执行）+ FSM 队列满（16 槵）+ 新 SUBSCRIBE/UNSUBSCRIBE/PUBLISH 到达。
- 高频 publish + 连接建立阶段可达。

### 修复方案

```c
if (send_fsm_sig(me, &requeue) != ESP_OK) {
    free_mqtt_fsm_sig_payload(&requeue);
}
```

### 结论: **确认（CONFIRMED）** — 修复。

---

## 🟢 汇总

| # | 发现 | 结论 | 处置 |
|---|------|------|------|
| 2 | `started` 字段 dead code | 确认 | **已修复** 删除字段 |
| 3 | `pending_cmd.started_ms/timeout_ms` 未使用 | 确认 | **已修复** 删除字段 + 赋值 |
| 4 | qos 经 error_code 字段传递 | 确认 | **已修复** 添加注释标注复用语义 |
| 5 | protocol data 二次克隆 | 确认 | 保持现状（正确性优先） |
