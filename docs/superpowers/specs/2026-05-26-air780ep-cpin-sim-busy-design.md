# Air780EP CPIN SIM Busy 轮询设计

**日期**: 2026-05-26
**状态**: 已批准

## 背景

`examples/basic_connect` 实机串口日志显示 Air780EP 在上报 `RDY` 后，基础 AT 初始化命令成功，但随后的 `AT+CPIN?` 连续返回 `+CME ERROR: 14`。Air780EP CME ERROR 文档定义错误码 `14` 为 `SIM busy` / SIM 卡遇忙。

合宙 Air780EP TCP 快速入门文档建议每隔 1 秒发送 `AT+CPIN?` 查询 SIM 状态，直到收到 `+CPIN: READY`；如果 10 秒内仍未识别 SIM，再建议重启模块或检查 SIM/硬件。当前 Core 激活流程会在短时间内执行 3 次完整激活尝试，导致 `AT+CPIN?` 在 SIM 初始化窗口内快速连续失败，并过早发布 `NET_ERROR`。

## 目标

- 将 `+CME ERROR: 14` 视为 SIM 初始化期间的暂态 busy，而不是立即失败。
- `AT+CPIN?` 按 1 秒间隔轮询，直到 SIM ready、出现明确不可恢复 SIM 状态，或达到总等待预算。
- 保持改动集中在 Air780EP modem adapter 层，不重构 Core 网络激活流程。
- 保持 public API 不变，不新增用户配置字段。

## 非目标

- 不重写 Core `net_mgr_t` 激活 FSM。
- 不改变网络注册、信号、`CGATT`、PDP 激活等步骤的重试策略。
- 不新增项目自定义错误码体系。
- 不改变 Air780EP 只支持 `primary_cid == 1` TCPIP 激活路径的限制。

## 设计

### 轮询位置

在 `src/modem/modem_air780ep.c` 的 `air780ep_get_sim_status()` 内实现 SIM ready 轮询。这样 Core 仍然只调用 `modem_get_sim_status()`，不需要理解 Air780EP 的 CME 错误码细节。

`air780ep_activate_pdp()` 内部当前也会调用 `air780ep_get_sim_status()` 做防御性检查。该调用保留；新轮询逻辑同时保护 Core `CHECK_SIM` 阶段和 PDP 激活阶段的 SIM 检查。

### 轮询参数

新增 Air780EP 私有常量：

- `AIR780EP_SIM_READY_TIMEOUT_MS = 10000`
- `AIR780EP_SIM_READY_POLL_INTERVAL_MS = 1000`

10 秒总预算来自合宙快速入门建议；1 秒间隔来自同一文档。两个常量为 Air780EP 私有实现细节，不暴露到 public config。

每次 `AT+CPIN?` 命令本身继续使用现有默认命令超时。总预算只约束整个 SIM ready 等待窗口，避免无限等待。

### 响应处理

`air780ep_get_sim_status()` 每轮发送 `AT+CPIN?` 后按响应分类处理：

- `AT_RESP_OK` 且包含 `+CPIN:`：使用现有 `parse_sim_status_line()` 解析并缓存 `last_sim_status`。
- 解析结果为 `MODEM_SIM_READY`：立即返回 `ESP_OK`。
- 解析结果为 `MODEM_SIM_PIN_REQUIRED`、`MODEM_SIM_PUK_REQUIRED`、`MODEM_SIM_NOT_INSERTED` 或 `MODEM_SIM_ERROR`：立即返回 `ESP_OK`，让 Core 根据非 ready 状态走现有失败路径。
- `AT_RESP_CME_ERROR` 且 `error_code == 14`：记录一次日志，等待 1 秒后重试，直到总预算耗尽。
- `AT_RESP_CME_ERROR` 且 `error_code == 10`：缓存并返回 `MODEM_SIM_NOT_INSERTED`，不等待。
- `AT_RESP_CME_ERROR` 且 `error_code == 11`：缓存并返回 `MODEM_SIM_PIN_REQUIRED`，不等待。
- `AT_RESP_CME_ERROR` 且 `error_code == 12`：缓存并返回 `MODEM_SIM_PUK_REQUIRED`，不等待。
- 其他 `ERROR`、`+CME ERROR`、`+CMS ERROR`、超时或异常响应：保持现有失败语义，返回 `ESP_FAIL` 或底层错误。

如果 10 秒内持续收到 `+CME ERROR: 14`，返回 `ESP_ERR_TIMEOUT`，并将 `last_sim_status` 保持为 `MODEM_SIM_UNKNOWN`。Core 会按现有逻辑发布网络错误，但不会在 `RDY` 后数百毫秒内误判。

### 时间与取消

轮询使用 `xTaskGetTickCount()` 和 `vTaskDelay()`，与当前 Core/Modem 代码风格一致。等待每次不超过剩余总预算和 1 秒间隔中的较小值。

本次最小修复不新增跨层取消机制。若 Core 销毁发生在等待期间，现有销毁流程最多等待当前 `AT+CPIN?` 命令和 1 秒轮询间隔结束。

## Error Handling

- `send_cmd()` 返回非 `ESP_OK`：立即返回该错误。
- `+CPIN:` 响应缺失：返回 `ESP_ERR_INVALID_RESPONSE`。
- `+CME ERROR: 14` 超过 10 秒：返回 `ESP_ERR_TIMEOUT`。
- 明确 SIM 状态不是 READY：返回 `ESP_OK` 并通过 `status` 输出具体状态，不把 PIN/PUK/未插卡误报为命令执行失败。
- 不新增自定义错误码，继续使用 ESP-IDF `esp_err_t`。

## Documentation Updates

- 更新 `docs/agents/at_cmd_air780ep.md` 中 `AT+CPIN?` 说明，明确 `+CME ERROR: 14` 应按 SIM busy 暂态轮询处理。
- 如实现中新增常量，保持注释说明其来源是 Air780EP TCP 快速入门建议。

## Verification

- 构建 `examples/basic_connect`。
- 实机串口监测 `examples/basic_connect`，确认 `AT+CPIN?` 遇到 `+CME ERROR: 14` 时不再 20ms 级连续重试，而是按约 1 秒间隔重试。
- 确认早期 `SIM busy` 不再立即触发 `NET_ERROR`。
- 确认最终仍能进入 `LTE event: NET_ONLINE` 和 `periodic: lte=ONLINE net=ONLINE`。
- 若 SIM 未插入，确认 `+CME ERROR: 10` 不被误当作 busy 等待。
