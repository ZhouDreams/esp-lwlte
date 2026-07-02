# Verify: Core Net/PDP mgr（#6）

**日期**: 2026-07-02
**对应报告**: `report-06-net-pdp-mgr.md`

---

## #5 遗留线索: net_mgr_start_activation 销毁中止能力

### 验证步骤

1. `check_activation_continue`（`:567-581`）检查 `core_is_destroying` + `core_stop_pending` + `activation_timed_out`。
2. `run_activation_loop`（`:643-644`）在 while 循环顶部调用。
3. `run_activation_step` 每个 case 在阻塞 `modem_*` 调用后调用（`:677`, `:700`, `:714`, `:734`, `:752`, `:765`, `:801`）。
4. `wait_next_poll`（`:840`, `:858`）在 `vTaskDelay` 前后均调用。
5. `net_mgr_start_activation`（`:409-412`）对 destroying/stop 引起的 INVALID_STATE 跳过 fail_activation。

### 结论: **确认安全** — 销毁延迟 ≤1 秒（一个 poll 间隔），不发布错误事件。

---

## 🟢-1: reconnect_enabled 写无锁

### 验证步骤

1. **写方无锁** — `net_mgr_set_reconnect_enabled`（`:357`）`me->net_mgr.reconnect_enabled = enabled`，不持 `me->lock`。
2. **读方持锁** — `reconnect_timer_cb`（`:590-595`）在 `core->lock` 下读取 `reconnect_enabled`。
3. **跨 task 访问** — 写方在 FSM task，读方在 timer daemon task。锁保护不完整（只有读方持锁）。
4. **平台安全性** — ESP32-C3 RISC-V 32-bit，`bool` 原子；stale read 后果已分析为安全（冗余信号被 FSM 丢弃，缺失信号无影响）。

### 结论: **确认** — 良性 data race，代码一致性瑕疵。🟢 交用户决定。

---

## 汇总

| # | 严重度 | 发现 | 结论 | 处置 |
|---|--------|------|------|------|
| - | #5 线索 | net_mgr_start_activation 销毁中止 | 确认安全 | 无需修复 |
| 1 | 🟢 | reconnect_enabled 写无锁 | 确认 | **已修复** net_mgr_set_reconnect_enabled 加锁 + 两处直接赋值改为调用 |
