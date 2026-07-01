# Verification: AT Engine

> 对应报告：`report-01-at_engine.md`
> 验证原则（review-checklist.md 阶段 3）：读上下文 ≥30 行 + 工具交叉验证（`rg` 搜调用点、核对优先级/调用链），不靠单一推理路径下结论。

## ✅ 确认的问题

### M-1 超时路径与命令完成的竞态（迟到结果被误报为 TIMEOUT）— 确认

- **原报告条目**: 🟡 M-1（`at_engine.c:925-939`）
- **验证结论**: 真实问题，确认存在。

**证据 1 — 覆盖早于检测（代码上下文 `:925-939`）**：
```c
if (xSemaphoreTake(me->cmd_done_sema, remaining_ticks) != pdTRUE) {
    xSemaphoreTake(me->lock, portMAX_DELAY);
    response->status = AT_RESP_TIMEOUT;          // :928 无条件覆盖
    response->error_code = 0;
    if (me->cmd_ctx == ctx) {                    // :930 此处才检测命令是否仍活动
        me->cmd_ctx = NULL;
    }
    flush_rx_input_locked(me);                    // :933 无条件 flush
    ...
}
```
`response->status` 的覆盖在 `cmd_ctx==ctx` 检测**之前**，且无条件执行。

**证据 2 — RX 侧确实在锁内写入真实状态（`finish_cmd_locked :1429-1438`）**：
```c
if (me->cmd_ctx && me->cmd_ctx->response) {
    me->cmd_ctx->response->status = status;      // RX 写入真实 OK/ERROR/CME
    me->cmd_ctx->response->error_code = error_code;
}
me->cmd_ctx = NULL;
me->state = AT_STATE_IDLE;
xSemaphoreGive(me->cmd_done_sema);               // give（亦印证 L-1：不受 cmd_ctx 守卫）
```

**证据 3 — 竞态窗口在本硬件上现实（优先级交叉验证 `rg`）**：
| 任务 | 默认优先级 | 来源 |
|------|-----------|------|
| AT Engine RX task | **10** | `at_engine.c:38` `AT_ENGINE_DEFAULT_RX_TASK_PRIORITY` |
| Modem event task | 9 | `modem.c:26` `MODEM_DEFAULT_EVENT_TASK_PRIORITY` |
| Core FSM task | **8** | `core_priv.h:38` `CORE_DEFAULT_FSM_TASK_PRIORITY` |

`at_engine_send_cmd_with_options` / `at_engine_send_cmd_with_payload` 的调用方为 Modem ops（`modem_air780ep.c:1526`、`modem_ml307r.c:1602` 等 9 处），这些 ops 运行在 **Core FSM task（优先级 8）** 上。

**时序推演（单核 ESP32-C3，RX 优先级 10 > Core FSM 8）**：
1. Core FSM 在 `xSemaphoreTake(cmd_done_sema, remaining)` 上等待，至 deadline `T_d` 超时返回 `pdFALSE`（此时 sema 仍空）。
2. 最终响应字节恰在 `T_d` 附近到达 UART，UART 中断将 `UART_DATA` 事件入队 → RX task（优先级 10）就绪 → **抢占** Core FSM。
3. RX 读字节（`rx_epoch=E` 与当前一致），`handle_line` 取 `lock`（Core FSM 尚未持锁）→ epoch 匹配 → `finish_cmd_locked` 写入真实 `response->status` 并 `give sema`。
4. RX 阻塞回 `uart_queue`。Core FSM 恢复，取 `lock`，**把 RX 刚写的真实 status 覆盖为 `AT_RESP_TIMEOUT`**（`:928`），随后 `clear_done_signal`（`:936`）抽干 RX 刚 give 的 sema。

**epoch 守卫为何没能挡住**：步骤 3 中 `handle_line` 的 epoch 校验（`:1175`）此时仍匹配——因为 Core FSM 的 `flush_rx_input_locked`（会 `rx_epoch++`）尚未执行（它要等步骤 4 才拿到锁）。故 RX 正常完成命令并 give sema。

**后果**：实际成功（或带特定 CME/CMS 错误码）的命令被误报为 `ESP_ERR_TIMEOUT` / `AT_RESP_TIMEOUT`。Core `net_mgr` 激活序列（`AT+CIICR` 等）、MQTT/TCP 连接类命令可能因此误判失败而重试，对非幂等或慢命令造成状态紊乱/不必要的重试开销。

**修复方向（Phase 4 落实）**：超时分支取 `lock` 后**先判断命令是否已被 RX 完成**：若 `me->cmd_ctx != ctx`（已被 RX 清空），说明 RX 已写入真实 `response->status`，应采用 RX 的结果（按 `response->status` 映射返回值）；仅当 `me->cmd_ctx == ctx`（命令确实未完成）才置 `TIMEOUT` 并 `flush`。即把 `:928` 的覆盖与 `:933` 的 flush 移入"确实未完成"分支。

---

### L-1 `finish_cmd_locked` 无条件 give `cmd_done_sema` — 确认（latent）

- **原报告条目**: 🟢 L-1（`at_engine.c:1437`）
- **验证结论**: 确认。`xSemaphoreGive(me->cmd_done_sema)`（`:1437`）位于 `if (me->cmd_ctx ...)` 守卫**之外**。

**调用点核查（`rg` finish_cmd_locked 调用方，均在锁内且 cmd_ctx 非空）**：
- `handle_line:1199/1219/1231/1250`（`if(ctx)` 块内或 `me->cmd_ctx==ctx` 守卫内）
- `process_rx_char:1101`（`me->cmd_ctx==payload_ctx` 守卫内）
- `abort_current_cmd_for_no_mem_locked:1426`（由 `append_*_response_line_locked` 在 `if(ctx)` 内调用）

**结论**：当前所有调用点 `cmd_ctx` 均非 NULL，**无实际触发**；binary sema 满时 `give` 仅返回 `pdFALSE` 不崩溃。属**潜在隐患**而非现网缺陷。建议随 M-1 一并收紧（把 give 纳入"确有活动命令"守卫）。

---

### L-2 `classes.md` 文档漂移 — 确认

- **原报告条目**: 🟢 L-2
- **验证结论**: 确认。`rg` 在 `docs/agents/classes.md` 全文检索 `begin_exclusive|flush_rx_exclusive|end_exclusive|send_cmd_with_payload` → **零命中**（仅在本 verify/report 文件命中）。`at_engine.h:276-687` 暴露的独占段 API 与 payload prompt API 确未进入类设计文档。

---

### L-3 `register_urc` 对调用方 handler 结构体的隐式副作用 — 确认

- **原报告条目**: 🟢 L-3（`at_engine.c:723`）
- **验证结论**: 确认。`at_engine_register_urc(me, prefix, handler)` 在 `:723` 执行 `handler->prefix = prefix;`，用独立 `prefix` 参数覆盖调用方 handler 结构体的 `prefix` 字段。功能自洽（`:756` 注销按 `node->prefix` 匹配，与注册写入一致），属隐式写副作用，非缺陷。

---

### L-4 URC 回调在持锁状态下同步执行 — 确认（设计契约）

- **原报告条目**: 🟢 L-4（`at_engine.c:1448-1451`）
- **验证结论**: 确认。`dispatch_urc` 命中后 `it->callback(it->prefix, line, it->user_ctx)`（`:1450`）在持有 `me->lock` 时同步执行。与 `at_engine.h:150-155` 文档化契约一致（回调必须短小、非阻塞、不得调用同实例加锁 API）。当前 Modem 层 URC handler 仅 `xQueueSend`（符合契约）。属设计约束 + residual risk，非缺陷。

---

### L-5 超时路径在 RX 已完成时仍 flush — 确认

- **原报告条目**: 🟢 L-5（`at_engine.c:930-934`）
- **验证结论**: 确认，与 M-1 同源。`:933` `flush_rx_input_locked(me)` 在 `if (me->cmd_ctx == ctx)` 检测之外无条件执行；当 RX 已完成（`cmd_ctx != ctx`）时仍 flush，会丢弃命令完成后合法到达、本应进入 URC 模式的字节。基本无害但可优化。随 M-1 修复一并处理。

---

## ❌ 误报

无。本次报告 6 条发现经交叉验证全部成立。

## ⚠️ 部分正确（需调整修复方案）

无。

---

## 修复记录

### M-1 + L-5 — 超时路径区分"真超时"与"RX 迟到完成"

- **文件:行号**: `src/at_engine/at_engine.c:925-963`（`send_cmd_internal` 超时分支）
- **改动**：超时分支取 `me->lock` 后**先判断 `me->cmd_ctx == ctx`**：
  - `cmd_ctx == ctx`（命令确实未完成）→ 置 `AT_RESP_TIMEOUT`、清 `cmd_ctx`、`flush_rx_input_locked`、回 `IDLE`，返回 `ESP_ERR_TIMEOUT`。
  - `cmd_ctx != ctx`（RX 已在窗口内 `finish_cmd_locked` 完成命令）→ 采用 RX 写入的真实 `response->status`，读 `ctx->io_error`，不 flush（命令已结束），返回值与正常完成路径一致；`clear_done_signal` 抽干 RX 迟到 `give` 的 sema。
  - 安全性论证：本调用方仍持有 `cmd_mutex`，故不会有其他命令改写 `cmd_ctx`；`cmd_ctx != ctx` 只能是 RX 把它置 NULL，即命令确实由 RX 完成。
- **构建验证**: ✅ `idf.py build` 通过。

### L-2 — classes.md 补齐 AT Engine 新增 API

- **文件:行号**: `docs/agents/classes.md`（AT Engine 1.3 层间方法代码块）
- **改动**：补充 `at_engine_send_cmd_with_payload` 与独占段 API（`begin/end_exclusive`、`flush_rx`、`flush_rx_exclusive`）及其线程模型契约（"必须配对 / 不得从 URC 回调调用"）。
- **构建验证**: ✅（纯文档，无构建影响）。

### 未修复（验证阶段重新评估）

- **L-1（`finish_cmd_locked` 无条件 give sema）**：经验证判定为**防御性安全网**——若改为有条件 give，反而可能在边界情况下让等待方永久阻塞。当前所有调用点 `cmd_ctx` 均非 NULL，无实际触发。决定**不改代码**。
- **L-3（`register_urc` 覆盖 `handler->prefix` 副作用）**：功能自洽，仅风格层面，暂不改。
- **L-4（URC 回调持锁执行）**：文档化设计契约，非缺陷；已在 `at_engine.h` 与本 verify 记录其硬约束。

---

## 模块交付清单（Phase 5）

- **Change summary**：修复 AT Engine 超时与命令完成的竞态（M-1）——成功命令在 deadline 边界不再被误报 TIMEOUT；同步收紧超时路径的 flush 行为（L-5）；补齐 `classes.md` 对 payload/独占段 API 的文档（L-2）。共改动 1 个源文件（`at_engine.c`）+ 1 个文档（`classes.md`）。

- **Resource budget**（默认配置：`rx_line_buf_size=256`, `max_response_lines=101`, `rx_buf_size=2048`, `rx_task_stack=4096`）：
  - 启动常驻（不含 task stack）：~3.5 KB（`at_engine_t` ~200B + 4 同步对象 ~320B + `line_buf`/`line_work_buf` 各 256B + `response_pool` 指针槽 `101×4=404B` + UART RX buffer 2048B）。
  - RX task stack：4096 B（片内 RAM）。
  - 命令期峰值（按需 malloc）：最坏 CIPPING 101 行 × 255B ≈ **25.8 KB**，命令结束/下次 `send_cmd` 释放。
  - **本次改动对 footprint 零影响**（仅在超时分支增加 2 个栈局部变量）。
  - 旧坑已修复确认：`response_pool` 为"指针槽常驻 + 内容按需 malloc"，非 `count×size` 二维常驻。

- **Lifecycle / ownership notes**：
  - `at_response_t.lines[i]` → **borrowed**：指向引擎 `response_pool` 拥有的字符串，调用方不得释放/修改，在下次 `send_cmd` 前有效。
  - `at_cmd_ctx_t` 的 `cmd`/`payload`/`payload_prompt`/`response` → **borrowed** 调用方内存（仅命令执行期间）。
  - URC `handler` 节点与 `prefix` 字符串 → **调用方 owned**，注册期间须有效。
  - 本次改动未改变任何 ownership 契约；超时分支在 RX 已完成时正确复用 RX 写入的 borrowed `response`。

- **Failure-path review**：
  - `malloc` 失败（响应行）→ `abort_current_cmd_for_no_mem_locked`（`io_error=NO_MEM` + flush + finish），调用方收 `ESP_ERR_NO_MEM`。
  - `write_cmd` malloc/UART 失败 → 回滚上下文 + flush + 释放锁。
  - 命令超时 → 现在区分"真超时"（flush + TIMEOUT）与"迟到完成"（采用真实结果）。
  - `destroy` 与活动调用方/命令互斥（`active_callers` 在阻塞 `cmd_mutex` 前递增）。
  - 本次改动未削弱任何失败路径，反而修正了"迟到完成被当失败"的路径缺陷。

- **Cross-module contract review**：
  - 未破坏门面/Service/Modem/AT Engine 分层契约；改动局限于 AT Engine 内部超时语义，对上层 API 签名零影响。
  - 返回值契约保持不变（"返回值=本地流程，AT 业务结果见 `response->status`"）：迟到完成路径返回值与正常完成一致，`response->status` 为 RX 真实结果。
  - Modem 层 `send_cmd` 调用点（Air780EP/ML307R 共 9 处）无需改动。

- **Residual risks**：
  - **L-4**：URC 回调在持锁下同步执行——若未来有 Modem URC handler 违反"短小非阻塞"契约，会阻塞所有 `lock` 操作。当前 Modem 实现符合契约，仅作 residual 记录。
  - **M-1 修复的边界**：修复覆盖了"RX 在调用方取锁前完成"的窗口；理论上仍存在"响应字节在调用方取锁后、flush 前"的极窄窗口——此时 `cmd_ctx==ctx`、走真超时分支 flush，属于"结果确实未及处理"的正确超时，非缺陷。
  - 上机验证未做：本次仅静态分析 + 编译验证，未做烧录/串口验证（修复为纯逻辑分支重构，无硬件行为变化）。
