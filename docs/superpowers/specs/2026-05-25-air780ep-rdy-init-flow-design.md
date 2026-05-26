# Air780EP RDY 初始化流程设计

**日期**: 2026-05-25
**状态**: 已批准

## 背景

Air780EP 当前初始化流程在硬复位后先等待固定 `boot_wait_ms`，再注册 URC 并发送 AT 初始化命令。这个顺序存在两个问题：

- `RDY` 可能在 URC 注册前出现，导致软件错过模块真正启动完成的信号。
- `boot_wait_ms` 是静态等待，无法准确表达模块实际 ready 状态。

同时，当前 APN 处理与 public header 中“`NULL` 或空字符串表示门面不配置 APN 字符串”的语义不一致：联网流程仍会发送空 APN 的 `AT+CGDCONT`。

## 目标

- MCU 侧资源先初始化完成，包括 AT Engine、Modem 对象和 Air780EP URC handler。
- Air780EP 硬复位后通过 `RDY` URC 判定模块启动完成。
- 移除 `boot_wait_ms` / `modem_boot_wait_ms` 配置，不再使用固定启动等待替代 `RDY`。
- `config->apn == NULL` 或 `config->apn[0] == '\0'` 时不发送 APN 配置命令。
- 保持用户 API 最小化，RDY 等待超时复用 `init_ready_timeout_ms`。

## 非目标

- 不新增独立 `rdy_timeout_ms` public 字段。
- 不改变 Core 的 ready / online 事件语义。
- 不改变 Air780EP 当前只支持 `primary_cid == 1` 的限制。
- 不增加对其他模块型号的抽象。

## 设计

### Public Config

`lwlte_air780ep_config_t` 删除 `modem_boot_wait_ms`。`init_ready_timeout_ms` 的语义扩展为初始化 ready 总超时，覆盖 Air780EP `RDY` 等待和后续 Core ready 等待；字段为 0 时继续使用 facade 默认值。Facade 在初始化开始时计算一个 deadline，传给 modem 的 RDY 等待使用，Core ready 等待使用同一 deadline 的剩余时间，避免两个阶段各自消耗一整个超时窗口。

APN 注释改为明确语义：`apn` 为 `NULL` 或空字符串时，facade 不配置 APN 字符串；需要显式 APN 时调用方传入非空字符串。

### Private Modem Config

`modem_air780ep_config_t` 删除 `boot_wait_ms`，新增 `ready_timeout_ms` 作为内部配置。Facade 使用同一初始化 deadline 的剩余时间填充它。

### Air780EP Init Flow

`air780ep_init()` 和 `air780ep_reset()` 使用同一初始化序列：

1. 设置 modem 状态为 `MODEM_STATE_INITIALIZING`。
2. 注册 Air780EP URC handler，包括 `RDY`。
3. 清除旧 RDY 标志。
4. 调用 `hardware_reset()`。
5. 等待 `RDY` URC，超时返回 `ESP_ERR_TIMEOUT`。
6. 依次发送 AT 初始化命令：`ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`。
7. 设置 `initialized = true` 和 `MODEM_STATE_READY`。
8. 投递 `MODEM_EVENT_READY`。

`hardware_reset()` 只负责 EN GPIO 电平动作：如果配置了 `en_pin`，先 reset/configure 输出，拉低 EN，等待 `reset_pulse_ms`，再拉高 EN。它不等待 `boot_wait_ms`，也不负责 AT 命令或 ready 判定。

如果 `en_pin == GPIO_NUM_NC`，`hardware_reset()` 不操作 GPIO；这种配置下仍进入 RDY 等待。若模块不会产生新的 `RDY`，初始化会按 `ready_timeout_ms` 超时，调用方应配置 EN GPIO 或保证模块复位路径能产生 RDY。

### RDY Synchronization

Air780EP 对象增加内部 RDY 同步状态：

- `ready_sema`：由 `rdy_urc_handler()` 释放，供 init/reset 阻塞等待。
- `rdy_seen`：记录 RDY 是否已收到，避免 handler 先于等待点执行时丢信号。
- `waiting_rdy`：表示当前处于受控 init/reset RDY 等待，避免 RDY handler 提前对上层声明 modem ready。

`rdy_urc_handler()` 收到 `RDY` 后在 lock 内设置 `rdy_seen = true`，再释放 semaphore。受控 init/reset 期间不在 handler 中投递 `MODEM_EVENT_READY`；ready 事件仍只在 AT 初始化命令全部成功后由 `air780ep_init()` / `air780ep_reset()` 投递。等待函数先检查 `rdy_seen`，未见 RDY 时按 `ready_timeout_ms` 等待 semaphore，醒来后再次检查标志。

### APN Behavior

Core 继续保存 APN 字符串，但 net manager 在设置 APN 阶段判断 `me->config.apn[0]`：

- 非空：调用 `modem_set_apn()`，发送 `AT+CGDCONT=1,"IP","<apn>"`。
- 空字符串：跳过 `modem_set_apn()`，不发送 `AT+CGDCONT`。

Air780EP PDP 激活阶段继续读取缓存 APN。APN 为空时发送无参 `AT+CSTT`，交给模块或运营商默认配置处理。

## Error Handling

- URC 注册失败：保持现有 rollback 逻辑。
- 硬复位 GPIO 失败：进入现有 `err` 分支，必要时注销本次注册的 URC，并将 modem 状态置为 `MODEM_STATE_ERROR`。
- RDY 等待超时：返回 `ESP_ERR_TIMEOUT`，facade 初始化失败并清理已创建资源。
- APN 为空：不是错误，不发送 APN 配置命令。

## Documentation Updates

- 更新 `src/include/lwlte_air780ep.h` 字段与注释。
- 更新 `src/modem/modem_air780ep.h` 字段与注释。
- 更新 `docs/agents/classes.md` 和相关架构文档中关于 EN、RDY、APN 的描述。
- 更新 `examples/basic_connect/README.md`，说明初始化等待 RDY，不再描述固定 boot wait。
- 将旧的 Air780EP pin simplify spec 中关于 `boot_wait_ms` 的内容标记为被本设计取代，避免文档冲突。

## Verification

- 搜索确认 `boot_wait_ms`、`modem_boot_wait_ms` 不再出现在 active code/public headers 中。
- 搜索确认 APN 为空时不会调用 `modem_set_apn()`。
- 构建 `examples/basic_connect`。
- 如连接硬件，开启 AT IO log 验证顺序为：注册 URC 后复位，收到 `RDY` 后才发送 `ATE0` 等初始化命令；空 APN 时不出现 `AT+CGDCONT=1,"IP",""`。
