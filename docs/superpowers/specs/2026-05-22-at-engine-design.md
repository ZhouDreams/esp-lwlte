# AT Engine 模块设计

日期：2026-05-22

## 背景

本设计实现 `docs/agents/classes.md` 中的第一个模块：AT Engine。AT Engine 是四层架构最底层，直接使用 ESP-IDF UART 和 FreeRTOS API，负责通用 AT 协议收发、响应解析、超时处理和 URC 前缀分发。它只暴露层间 API，最终 App 开发者不可见。

当前主源码基本为空，`src/CMakeLists.txt` 尚未登记实际源文件。旧项目中的 UART RX 和 AT 管理逻辑只作为思路参考，新实现按四层架构、`esp_err_t` 错误约定和 C OOP 句柄模式重写。

## 目标

- 新增 `src/include/at_engine.h`，暴露层间 API 类型和函数。
- 新增 `src/at_engine/at_engine.c`，完整实现 AT Engine。
- 更新 `src/CMakeLists.txt`，使组件编译包含 AT Engine 源文件。
- 支持 UART 初始化和销毁、RX task、阻塞命令发送、响应行保存、echo 过滤、最终响应识别、命令超时、URC 注册和分发。
- 完成 ESP-IDF 静态构建验证，不默认执行烧录或串口实机验证。

## 非目标

- 不实现 Modem Adapter、Core Service 或用户 API。
- 不新增平台抽象层，AT Engine 直接调用 ESP-IDF/FreeRTOS API。
- 不新增 host/mock 单元测试工程。
- 不处理带 payload prompt 的命令模式；后续 MQTT 等模块需要时再扩展。

## 文件布局

- `src/include/at_engine.h`：层间 API 头文件，供 Board Init 和 Modem 层使用。
- `src/at_engine/at_engine.c`：内部实现，包含 opaque `struct at_engine`、状态枚举、命令上下文、UART RX task 和静态辅助函数。
- `src/CMakeLists.txt`：登记 `at_engine/at_engine.c`，保留 `INCLUDE_DIRS include`，并按 ESP-IDF 6.0 UART 组件要求使用 `REQUIRES esp_driver_uart`。

## 公开 API

`at_engine.h` 暴露以下类型和函数：

- `at_engine_config_t`：UART 端口、TX/RX GPIO、波特率、UART RX 缓冲区、RX task 栈大小和优先级、单行缓冲大小、默认命令超时、最大响应行数。
- `at_engine_t`：opaque 句柄，定义只存在于 `.c` 文件。
- `at_response_status_t`：`AT_RESP_OK`、`AT_RESP_ERROR`、`AT_RESP_CME_ERROR`、`AT_RESP_CMS_ERROR`、`AT_RESP_TIMEOUT`、`AT_RESP_ABORTED`。
- `at_response_t`：调用方提供 `lines` 数组和 `max_lines`，AT Engine 填写 `status/error_code/line_count/lines[i]`。
- `at_urc_callback_t` 和 `at_urc_handler_t`：URC 前缀、回调、用户上下文、链表 next。
- `at_engine_create()` / `at_engine_destroy()`。
- `at_engine_send_cmd()`。
- `at_engine_register_urc()` / `at_engine_unregister_urc()`。

## 内部结构

`struct at_engine` 保存：

- 配置快照和 UART 端口。
- UART event queue 和 RX task 句柄。
- `cmd_mutex`，串行化多个调用方线程的 `send_cmd()`。
- `cmd_done_sema`，RX task 通知当前阻塞命令完成。
- `lock`，保护 URC 链表、当前命令上下文和响应文本池的并发访问。
- 当前 `at_state_t`、实例内 `at_cmd_ctx_t cmd_ctx_storage` 和活动指针 `at_cmd_ctx_t *cmd_ctx`。
- URC handler 单向链表及计数。
- 行组装缓冲 `line_buf`。
- 响应文本池，容量按 `config.max_response_lines` 和调用方 `response->max_lines` 的较小值使用，保证 `response->lines[i]` 在 `send_cmd()` 返回后到下一次 `send_cmd()` 之前有效。
- UART driver 安装标志 `uart_driver_installed`，用于 create 失败回滚和 destroy 清理。
- 停止标志 `rx_task_stop_requested`。

`at_cmd_ctx_t` 保存当前命令字符串、调用方 `at_response_t *`、命令 echo 消费状态、响应行索引和最终响应状态。

## 生命周期

`at_engine_create()`：

1. 校验配置中的 UART 端口、TX/RX 引脚和波特率。
2. 对可选尺寸和超时应用默认值。
3. `calloc` 分配对象并复制配置。
4. 创建 mutex、lock、binary semaphore 和 line/response 缓冲。
5. 调用 `uart_driver_install()` 创建 UART driver 和 event queue。
6. 调用 `uart_param_config()` 设置 `UART_DATA_8_BITS`、无校验、1 stop bit、无硬件流控。
7. 调用 `uart_set_pin()` 设置 TX/RX 引脚。
8. 创建 RX task。
9. 任一步失败都走 `goto err`，按资源创建逆序释放并返回 `NULL`。

`at_engine_destroy()`：

1. 校验 `me`。
2. 如果当前有命令执行，返回 `ESP_ERR_INVALID_STATE`，由调用方先停止上层业务。
3. 设置停止标志并删除 RX task。
4. 调用 `uart_driver_delete()`。
5. 删除 semaphore/mutex，释放 line/response 缓冲和对象。
6. 返回 `ESP_OK`，释放过程中非关键失败优先记录日志并继续清理。

## 命令发送流程

`at_engine_send_cmd()` 在调用方线程执行：

1. 校验 `me/cmd/response`，校验 `response->lines` 和 `max_lines`。
2. 获取 `cmd_mutex`，保证同一实例同时只有一个命令。
3. 清空响应文本池并初始化实例内 `cmd_ctx_storage`。
4. 自动发送 `cmd`，若调用方未包含 CR/LF，则补 `\r\n`。
5. 等待 `cmd_done_sema`，等待时间为入参 `timeout_ms` 或配置默认值。
6. RX task 收到最终响应时释放信号量，调用方返回 `ESP_OK`。
7. 等待超时时设置 `AT_RESP_TIMEOUT`，清除 `cmd_ctx`，刷新 UART 输入和事件队列，重置行缓冲，返回 `ESP_ERR_TIMEOUT`。
8. 返回前释放 `cmd_mutex`。

函数返回值表达调用流程是否完成，AT 业务结果放在 `response->status`。收到 `ERROR`、`+CME ERROR:` 或 `+CMS ERROR:` 时，`send_cmd()` 返回 `ESP_OK`，同时设置对应 `response->status` 和 `error_code`。

## UART RX 和行解析

RX task 使用 ESP-IDF UART event queue 等待 `UART_DATA`，调用 `uart_read_bytes()` 读取数据。输入按字节组装成行：

- 忽略 `\r`。
- 遇到 `\n` 时，如果当前行非空则处理一行。
- 超过 `rx_line_buf_size` 的行丢弃并重置行缓冲，防止溢出。

处理一行时：

1. 如果有当前命令且该行等于命令 echo，标记 echo 已消费并忽略。
2. 如果匹配 `OK`，设置 `AT_RESP_OK` 并通知命令完成。
3. 如果匹配 `ERROR`，设置 `AT_RESP_ERROR` 并通知命令完成。
4. 如果匹配 `+CME ERROR:` 或 `+CMS ERROR:`，解析错误码，设置对应状态并通知命令完成。
5. 如果有当前命令且不是最终响应，将行复制到响应文本池并填入 `response->lines`，超过响应文本池容量时截断。
6. 如果无当前命令，则按已注册 URC 前缀匹配并分发 URC。

URC 优先级：有当前命令时，先按命令响应处理最终响应和普通数据行；无当前命令时才按 URC 分发。这样避免把 `AT+CEREG?` 这类查询响应误分发为同前缀 URC。第一版不保证命令等待期间的自发 URC 独立分发；后续若 Modem 需要该能力，可在 Modem 层约束 URC 前缀并扩展匹配策略。

## URC 管理

`at_engine_register_urc()` 校验 `me/prefix/handler/callback`，写入 handler 字段并插入链表头。handler 的生命周期由调用方管理，通常是 static 或 Modem 对象字段。

`at_engine_unregister_urc()` 按 prefix 查找并移除第一个匹配节点。未找到返回 `ESP_ERR_NOT_FOUND`。

URC 回调在 RX task 中同步调用，并在持有内部锁时执行，以保护调用方拥有的 handler/prefix 生命周期。callback 必须短小且非阻塞，复杂处理应由 Modem 层投递到自己的队列；callback 不得在同一引擎实例上调用会获取内部锁的 AT Engine API，包括 `at_engine_send_cmd()`、`at_engine_register_urc()` 和 `at_engine_unregister_urc()`。

## 并发模型

- `cmd_mutex` 只串行化 `send_cmd()`。
- `lock` 保护 URC 链表、`cmd_ctx`、状态和响应文本池。
- RX task 与调用方线程通过 `cmd_done_sema` 同步当前命令完成。
- 命令超时由 `send_cmd()` 的 `xSemaphoreTake()` 控制，不由 RX task 轮询。这比文档中“RX task 检查 timeout_ticks”的实现更简单，且避免额外轮询逻辑。
- 超时返回前会刷新 UART 输入、重置 UART 事件队列和当前行缓冲，降低迟到响应污染下一条命令的风险；刷新之后才释放 `cmd_mutex`。

## 错误处理

- 所有公开函数返回 `esp_err_t` 或对象指针。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 资源不足返回 `ESP_ERR_NO_MEM`。
- 命令等待超时返回 `ESP_ERR_TIMEOUT`，并设置 `response->status = AT_RESP_TIMEOUT`。
- 状态不合法返回 `ESP_ERR_INVALID_STATE`。
- UART driver API 错误直接传播或映射为标准 ESP-IDF 错误码。
- 使用 `ESP_RETURN_ON_FALSE`、`ESP_GOTO_ON_ERROR`、`ESP_GOTO_ON_FALSE`，带清理函数中的局部变量命名为 `ret`，清理标签命名为 `err`。

## 验证计划

- 静态检查新增头文件和源文件是否符合项目文件模板和 Doxygen 注释规则。
- 构建验证使用 ESP-IDF MCP build 工具，确认组件和示例 main 可编译。
- 本轮不默认执行烧录或串口日志验证。

## 已批准的设计选择

- 选择“单文件完整实现”：一个公开头文件加一个实现文件，完整实现 AT Engine。
- 响应行使用实例内响应文本池，不要求调用方释放每行字符串。
- URC handler 节点由调用方拥有，AT Engine 链接和解除链接。
- 命令超时由 `send_cmd()` 等待信号量处理。
- 验证范围为静态构建，不默认做实机烧录。
