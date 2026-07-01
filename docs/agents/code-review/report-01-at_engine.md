# Code Review: AT Engine

**日期**: 2026-06-30
**文件**: src/at_engine/at_engine.c (1702 行), src/at_engine/at_engine.h (370 行)
**审查阶段**: Phase 2（仅报告，未改动源码）

---

## 🔴 高严重度

无。

> 说明：本项目最严重的已知坑——`response_pool = max_response_lines * rx_line_buf_size` 一次性撑爆 ESP32-C3 heap——在当前实现中**已修复**。`response_pool` 现在只常驻指针槽位（`calloc(max_response_lines, sizeof(char*))`），响应文本按需 `malloc`，见"无问题维度 / 维度 A"。本模块未发现会导致内存爆掉、崩溃或数据竞争级内存安全的高危问题。

---

## 🟡 中严重度

### M-1 超时路径与命令完成的竞态：迟到结果被误报为 TIMEOUT

- **文件:行号**: `src/at_engine/at_engine.c:925-939`（`send_cmd_internal` 步骤 5 超时分支）
- **问题描述**:
  调用方在 `cmd_done_sema` 上等待（`:925`）。当等待**恰好超时**（`xSemaphoreTake` 返回 `pdFALSE`）后，进入超时分支，无条件执行：
  ```c
  xSemaphoreTake(me->lock, portMAX_DELAY);
  response->status = AT_RESP_TIMEOUT;          // :928 无条件覆盖
  response->error_code = 0;
  if (me->cmd_ctx == ctx) { me->cmd_ctx = NULL; }   // :930
  flush_rx_input_locked(me);                    // :933
  me->state = AT_STATE_IDLE;
  ...
  return ESP_ERR_TIMEOUT;
  ```
  与此同时，RX task 在 `handle_line` 中检测到最终响应后会调用 `finish_cmd_locked`（`:1429`），在持有 `me->lock` 时写入真实 `response->status`（OK/ERROR/CME…）、清空 `cmd_ctx`、`give cmd_done_sema`。

  **竞态窗口**：调用方 sema 超时返回 → 调用方尝试获取 `me->lock`。若 RX task 在此窗口内先拿到 `lock`（epoch 仍匹配、`cmd_ctx==ctx`），它会 `finish_cmd_locked` 写入真实结果并 `give sema`，然后释放 lock。随后调用方拿到 lock，**无条件把 `response->status` 覆盖为 `AT_RESP_TIMEOUT`**（`:928`），并 `clear_done_signal`（`:936`）抽干 RX 刚 give 的信号量，最终返回 `ESP_ERR_TIMEOUT`——丢弃了 RX 已写入的真实结果。

- **为何在本硬件上 realistic**：ESP32-C3 单核；RX task 默认优先级 10（`AT_ENGINE_DEFAULT_RX_TASK_PRIORITY`），Core FSM task 优先级通常更低（~8）。一条最终响应行若恰在 deadline 附近到达 UART，高优先级 RX 会抢占调用方、跑完整个 `handle_line → finish_cmd_locked`，再让调用方继续。窗口虽窄但**在 deadline 边界处现实存在**。
- **后果**：实际成功（或带特定 CME 错误码）的命令被上层误判为超时。Core `net_mgr` / MQTT / TCP 可能把成功命令当失败重试，对非幂等命令（如重复 `AT+CSTT/CIICR` 激活、重复 PDP）可能造成状态紊乱或不必要的重试开销。
- **建议修复方向**（Phase 4 再定）：
  超时分支获取 `lock` 后，**先判断命令是否已被 RX 完成**——例如检查 `me->cmd_ctx != ctx`（已被 RX 清空）即说明 RX 已写入真实 `response->status`，此时应采用 RX 的结果（依据 `response->status` 映射返回值）而非强行 TIMEOUT；只有 `me->cmd_ctx == ctx`（命令确实未完成）才走 TIMEOUT + flush 路径。同理 `state`/`flush` 也应只在确实未完成时执行。

---

## 🟢 低严重度

### L-1 `finish_cmd_locked` 无条件 give `cmd_done_sema`

- **文件:行号**: `src/at_engine/at_engine.c:1429-1438`
- **问题**：`finish_cmd_locked` 末尾 `xSemaphoreGive(me->cmd_done_sema)` 不受 `cmd_ctx` 守卫保护。当前所有调用点都在确认 `cmd_ctx==ctx` 之后进入，未触发实际问题；binary semaphore 在已满时 `give` 仅返回 `pdFALSE` 不崩溃。但语义上不严谨——若无活动命令被误调用会多 give 一次。
- **建议**：将 `give` 纳入 `if (me->cmd_ctx ...)` 守卫，或在文档中明确"仅在完成活动命令时调用"。低成本即可收紧。

### L-2 `classes.md` 文档漂移：新增公开 API 未记录

- **文件:行号**: `src/at_engine/at_engine.h:618-687`（`at_engine_begin_exclusive` / `flush_rx_exclusive` / `end_exclusive` / `flush_rx`）、`:276-281`（`send_cmd_with_payload`）
- **问题**：`at_engine.h` 已暴露独占段 API（`begin/end_exclusive`、`flush_rx*`）与 payload prompt API，但 `docs/agents/classes.md` 的 "1. AT Engine" 章节只描述了 `send_cmd` / `send_cmd_with_options` / `register_urc`，未记录这批新接口及其线程模型约束。文档与实现不一致。
- **建议**：在 `classes.md` AT Engine 章节补充独占段 API、payload API 及其"不得从 URC 回调调用 / 必须配对"等契约。

### L-3 `register_urc` 对调用方 handler 结构体的隐式副作用

- **文件:行号**: `src/at_engine/at_engine.c:723`（`handler->prefix = prefix;`）
- **问题**：`at_engine_register_urc(me, prefix, handler)` 既接收独立 `prefix` 参数，又读写 `handler->prefix`，并用独立参数覆盖结构体字段。功能正确（注销时按 `node->prefix` 匹配，与注册写入一致），但属于对调用方持有结构的隐式写副作用，易让读者误以为两处 prefix 可以不同。
- **建议**：注释说明"以独立 prefix 参数为准并回填 handler->prefix"，或 API 仅取 `handler->prefix`（减少参数）。属风格层面。

### L-4 URC 回调在持锁状态下同步执行（实时性契约风险）

- **文件:行号**: `src/at_engine/at_engine.c:1448-1451`（`dispatch_urc`）
- **问题**：`dispatch_urc` 遍历链表命中后，在**持有 `me->lock`** 时同步调用 `it->callback(...)`。这是 `at_engine.h:150-155` 明确文档化的契约（回调必须短小、非阻塞、不得调用同实例加锁 API），当前 Modem 层 URC handler 也只做 `xQueueSend`（符合契约）。但该设计意味着：任何**违反契约的慢回调**会阻塞所有依赖 `lock` 的操作（`send_cmd`、`register/unregister`、`destroy`、`flush`），构成实时性风险。
- **建议**：保留当前设计（避免再开 task / 队列），但在 `dispatch_urc` 注释和 `classes.md` 强化"回调不得阻塞"的硬约束；后续若 URC 负载变重，可考虑 defer 到独立任务（架构改动，仅作 residual risk 记录）。

### L-5 超时路径在 RX 已完成时仍执行 flush

- **文件:行号**: `src/at_engine/at_engine.c:930-934`
- **问题**：与 M-1 相关。当 `me->cmd_ctx == ctx` 为假（RX 已完成并清空 ctx），超时分支仍调用 `flush_rx_input_locked`（递增 epoch、清 UART 输入）。基本无害（丢弃的是命令完成后的过时字节），但会不必要地丢弃命令完成后合法到达的、本应进入 URC 模式的字节。
- **建议**：随 M-1 一并调整——仅在"确实未完成"分支才 flush。

---

## 无问题维度 / 正面发现

- **维度 A（资源账本与乘法型分配）✅ 核心教训已修复**：
  `response_pool` 只常驻指针槽位（`init_resources` `:1630` `calloc(max_response_lines, sizeof(char*))`，默认 101×4 = **404 B**），响应文本按需 `malloc`（`append_response_line_locked` `:1378`），并在下一次 `send_cmd` 开始（`:879 clear_response_pool`）和 `destroy`（`cleanup_resources`）时统一释放。**没有** `count*size` 二维常驻数组。详见末尾 Resource budget。
- **维度 B（内存安全与生命周期）✅**：
  - `line_buf` / `line_work_buf` 双缓冲设计正确（`process_rx_char` `:1131-1148`）：完整行在锁内拷贝到 `work_buf` 再释放锁调用 `handle_line`，防止 `flush_rx_input_locked` 在释放/重获锁之间篡改 `line_buf`。
  - 行缓冲溢出防护（`:1152` `line_buf_pos + 1 >= rx_line_buf_size`）正确，保证最多写入 `size-1` 字符 + NUL，无越界。
  - `append_response_line_locked` / `append_final_response_line_locked` 的 `limit = min(response->max_lines, response_pool_lines)` 守卫完备，无越界写；final 行覆盖最后一个槽位时先 `free` 旧串，无泄漏/双释放。
- **维度 C（死锁）✅**：锁序一致（`cmd_mutex` → `lock`，无反向获取）；`cmd_mutex`/`lock` 均为 `xSemaphoreCreateMutex`（支持优先级继承），`cmd_done_sema` 为 binary sema（无需继承）。`end_exclusive` 先释放 `cmd_mutex` 再 `end_send_call`（取 `lock`），无嵌套。
- **维度 D（失败路径）✅**：`at_engine_create` 的 `err:` 回滚（UART driver / `cleanup_resources` / `free`）按反序、含 NULL 守卫；`write_cmd` malloc 失败回滚上下文并 flush；`append_response_line_locked` malloc 失败走 `abort_current_cmd_for_no_mem_locked`（置 `io_error=NO_MEM` + flush + finish）；UART 写失败在调用方与 RX 两侧都有处理。
- **维度 C/D（destroy 与并发）✅**：`active_callers` 在阻塞 `cmd_mutex` 前递增（`:837` 早于 `:855`），使 `destroy` 能拒绝并发销毁；RX task 用 100ms 超时轮询 `stop_requested`，`destroy` 通过 `rx_task_done_sema` join 后才 `uart_driver_delete`，无 UART use-after-free。
- **维度 G（类型与边界）✅**：乘法型 malloc（`calloc`/`malloc(len+3)`/`malloc(copy_len+1)`）均无现实溢出风险；`to_read -= len` 循环收敛；`(size_t)response_line_size - 1U` 依赖 `normalize_config` 保证 `rx_line_buf_size >= 1`（可加 assert 进一步加固，但非缺陷）。
- **维度 E（AT 特有）✅**：echo 单次消费（`echo_consumed`）、`"\r\n"` 行尾（吞 `\r`、`\n` 交付、空行忽略）、CME/CMS 解析（`atoi` 失败返 0，已文档化）均合理；命令期间非最终行优先归入响应、空闲期才 URC 分发，与设计契约一致。

---

## Resource budget（启动 footprint）

> 配置：默认值（`rx_line_buf_size=256`, `max_response_lines=101`, `rx_buf_size=2048`, `rx_task_stack=4096`）。

| 项目 | 计算 | 字节 | 备注 |
|------|------|------|------|
| `at_engine_t` 结构体 | sizeof | ~200 | 配置快照 + 句柄/指针字段 |
| 4 个同步对象 | mutex×2 + binary sema×2 | ~320 | FreeRTOS 每对象约 80 B |
| `line_buf` | `rx_line_buf_size` | 256 | 行组装缓冲 |
| `line_work_buf` | `rx_line_buf_size` | 256 | 完整行处理缓冲 |
| `response_pool`（指针槽） | `max_response_lines × sizeof(char*)` = `101 × 4` | **404** | **仅指针常驻，非 `×line_size`** |
| UART driver RX buffer | `rx_buf_size` | 2048 | `uart_driver_install` 内部 |
| RX task stack | `rx_task_stack` | 4096 | 片内 RAM |
| **启动常驻合计（不含 task stack）** | | **~3.5 KB** | |
| **峰值（命令期间，最坏 CIPPING 101 行 × 255B）** | `101 × 256` | **~25.8 KB** | 按需 malloc，命令结束/下次 send_cmd 释放 |

关键：旧的 `max_response_lines × rx_line_buf_size`（默认 101×256≈25.8 KB **常驻**）已改为**指针 404 B 常驻 + 内容按需 malloc**，启动 footprint 从 ~26 KB 降到 ~3.5 KB。25.8 KB 仅作为命令期峰值（CIPPING 场景），可接受。
