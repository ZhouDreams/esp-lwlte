# Modem Adapter 类设计文档更新

日期：2026-05-23

## 背景

本设计用于补全 `docs/agents/classes.md` 的第二部分：Modem Adapter（模块适配层）。当前 `classes.md` 已完整描述 AT Engine，第 2 节仍是占位。AT Engine 已实现并通过 `src/include/at_engine.h` 暴露层间 API，Modem 层应在此基础上定义面向 Core 的语义接口，并封装 Air780EP 的 AT 指令和 URC 差异。

本轮范围只更新类设计文档，不新增 `src/include/modem.h`、`src/include/modem_air780ep.h` 或 `src/modem/*.c` 源码。

## 目标

- 在 `docs/agents/classes.md` 中补全 Modem Adapter 的类定义。
- 采用“通用基类 + ops 多态 + Air780EP 子类 + 语义值对象 + 内部事件上下文”的完整类设计。
- 明确 Core 只通过 `modem_*` 层间包装 API 使用 `modem_t`，不直接操作 AT Engine，不写 AT 指令字符串。
- 明确 `modem_ops_t` 是 Modem 层内部多态机制，包装 API 内部调用 `me->ops->method(me, ...)`。
- 明确 Air780EP 工厂函数只给 Board Init 装配使用，换模块时只替换具体工厂。
- 明确 URC 上行必须通过 Modem event queue 和 event task 解耦，禁止在 AT Engine URC 回调上下文中直接调用 Core。

## 非目标

- 不实现 Modem Adapter 源码。
- 不重新设计 AT Engine。
- 不修改 Core Service 的类设计。
- 不引入自定义错误码体系。
- 不新增 host/mock 单元测试工程。

## 文档结构

`classes.md` 的 `## 2. Modem Adapter（模块适配层）` 将替换占位内容，并按以下结构组织：

- `2.1 类总览`：列出层间 API、具体子类 API、内部类型。
- `2.2 modem_t`：通用 Modem 句柄和基类。
- `2.3 modem_ops_t`：虚函数表。
- `2.4 modem_state_t`：Modem 层本地状态。
- `2.5 modem_reg_status_t`：网络注册状态枚举。
- `2.6 modem_info_t`：模块/卡静态信息值对象。
- `2.7 modem_signal_t`：信号质量值对象。
- `2.8 modem_pdp_context_t`：PDP 上下文值对象。
- `2.9 modem_event_t` 和事件回调：URC 翻译后的上行事件。
- `2.10 modem_air780ep_config_t`：Air780EP 配置。
- `2.11 modem_air780ep_t`：Air780EP 子类。
- `2.12 air780ep_cmd_ctx_t`：Air780EP 内部命令解析上下文。
- `2.13 Modem 线程模型`：命令下行与 URC 上行的运行时边界。

## 架构选择

### 头文件和可见性

文档会把 Modem 层可见性拆清楚：

- `src/include/modem.h`：通用层间 API，暴露 opaque `modem_t`、语义值对象、事件回调类型和 `modem_*` 包装 API，供 Core 和 Board Init 使用。
- `src/include/modem_air780ep.h`：Air780EP 具体工厂和配置，供 Board Init 使用；Core 不 include 该头文件。
- `src/modem/modem_priv.h` 或对应 `.c` 文件内部：定义 `struct modem`、`modem_ops_t` 和通用基类辅助函数，只给 Modem 层实现文件使用。
- `src/modem/modem_air780ep.c` 内部：定义 `modem_air780ep_t` 和 Air780EP 解析辅助类型。

这会细化 `architecture.md` 中“Core 通过 modem_ops 表操作模块”的表述：Core 在语义上使用 Modem 多态能力，但实际代码只调用 `modem_*` 包装 API；`modem_ops_t` 不直接暴露给 Core。

### Opaque 句柄和包装 API

`modem_t` 对 Core 保持 opaque。Core 使用 `modem_init()`、`modem_get_signal()`、`modem_set_apn()`、`modem_activate_pdp()` 等包装 API，而不是直接解引用 `me->ops`。这样符合项目 OOP 规范中的信息隐藏要求，也把参数检查、状态检查、必填方法检查集中在通用入口。

`modem_ops_t` 仍然是 Modem 层多态核心。包装 API 在内部执行 `me->ops->method(me, ...)`，不同模块通过不同的 `static const modem_ops_t` 实现同一语义接口。

### Air780EP 具体子类

Air780EP 子类以 `modem_t base` 作为第一个字段，实现单继承。Board Init 调用 `modem_air780ep_create(at, config)` 获得 `modem_t *`，并把该句柄传给 Core。后续支持其他模块时新增 `modem_xxx_create()` 和对应 ops 表，不要求 Core 修改。

### 语义值对象

Modem 层不向 Core 暴露 `at_response_t` 或原始 AT 响应行，而是返回语义值对象：

- `modem_info_t`：IMEI、IMSI、ICCID、模块型号、固件版本等模块/卡静态信息。
- `modem_signal_t`：RSSI、BER，后续可扩展 RSRP、RSRQ、SINR。
- `modem_pdp_context_t`：CID、APN、激活状态、IP 地址等 PDP 上下文信息。

## 数据流

### 命令下行

Core 调用 `modem_*` 包装 API，包装 API 校验参数和状态后调用 `modem_ops_t`。Air780EP 子类把语义操作翻译为具体 AT 命令，通过 `at_engine_send_cmd()` 发送，解析 `at_response_t` 并填充 `modem_info_t`、`modem_signal_t` 或 `modem_pdp_context_t`。

Core 不知道具体 AT 指令，不包含模块型号判断，不直接调用 AT Engine。

### URC 上行

Air780EP 子类向 AT Engine 注册 `+CEREG:`、`+CGREG:`、`+CGEV:` 等 URC 前缀。AT Engine RX task 调用 Modem 的 URC handler 时，Modem 只做短小解析并投递 `modem_event_t` 到自身事件队列。

硬约束：Modem URC handler 禁止直接调用 Core 回调。Core 注册的 `modem_event_callback_t` 必须由 Modem event task 执行，而不是由 AT Engine RX task 执行。

该约束避免运行时调用栈变成 `AT RX task -> Modem URC handler -> Core callback`，防止 Core 逻辑阻塞 UART RX、延迟 AT 行解析，或在 AT Engine 内部锁未释放时反向进入下层造成死锁风险。

## 线程模型

Modem 层新增一个事件任务和一个事件队列，属于 `modem_t` 基类资源：

- AT Engine RX task：只负责调用相邻层的 Modem URC handler。
- Modem URC handler：解析 URC 并以 0 timeout 投递 `modem_event_t`。
- Modem event task：从队列取事件，并调用 Core 注册的 `modem_event_callback_t`。
- Core 自身线程或 FSM task：由 Core 设计决定，不属于本轮文档范围。

事件队列满时，URC handler 只记录警告并丢弃事件，不得阻塞 AT Engine RX task。

## 错误处理

Modem 层遵循 `docs/agents/err.md`：公开 API 和 ops 方法统一返回 `esp_err_t`，不新增自定义错误码。

- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 状态错误返回 `ESP_ERR_INVALID_STATE`。
- 不支持的选填能力返回 `ESP_ERR_NOT_SUPPORTED`，或在文档明确的场景下安静跳过。
- AT 命令超时传播 `ESP_ERR_TIMEOUT`。
- AT Engine 返回 `ESP_OK` 但 `at_response_t.status` 为 `AT_RESP_ERROR`、`AT_RESP_CME_ERROR` 或 `AT_RESP_CMS_ERROR` 时，Air780EP 适配层映射为标准 ESP-IDF 错误码，并在内部保留/记录原始错误码。
- 响应格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。

## 验证计划

本轮只做文档更新，验证范围为：

- 自检 spec 和目标文档无占位、无自相矛盾、无范围漂移。
- 检查 `classes.md` 的 Modem 设计与 `architecture.md`、`oop-design.md`、`err.md`、已实现的 AT Engine API 一致。
- 运行 `git diff --check` 检查 Markdown 空白问题。
- 审阅 `git diff` 确认只修改预期文档。

不运行 ESP-IDF build，因为本轮不新增或修改 C 源码。

## 已批准的设计选择

- 范围只写 `classes.md` 的 Modem 类设计，不新增源码。
- 采用完整类设计，而不是最小草案或 Air780EP 实现优先写法。
- 保留 `modem_info_t`、`modem_signal_t`、`modem_pdp_context_t` 三个独立语义值对象。
- Core 使用 opaque `modem_t` 和 `modem_*` 包装 API；`modem_ops_t` 作为 Modem 层内部多态机制。
- URC 上行必须通过 Modem `event_queue + event_task` 解耦。
- “Modem URC handler 禁止直接调用 Core，必须投递到 Modem event task”写成硬约束。
- Modem 层错误处理使用 ESP-IDF 标准 `esp_err_t`，不新增自定义错误码。
- 本轮验证只覆盖文档自检和 diff 检查，不运行 ESP-IDF build。

## Git 约束

当前会话不创建 git commit，除非用户明确要求提交。这遵守项目协作约定，并覆盖 superpowers 默认“写 spec 后提交”的流程项。
