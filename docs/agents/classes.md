# 类定义

在编码前先确定每个模块有哪些类（struct + 配套函数），是理解项目架构最重要的文档。每个类的定义包含：**所属层、职责、可见性、关键字段、关键方法、OOP 角色**。

## 可见性定义

| 可见性 | 落入哪个头文件 | 谁能看到 | 命名前缀 |
|--------|-------------|---------|---------|
| 用户 API | `src/include/lwlte.h` | App 开发者 | `lwlte_` |
| 层间 API | `src/core/core.h`、`src/mqtt_client/mqtt_client.h`、`src/tcp_client/tcp_client.h`、`src/ping_client/ping_client.h`、`src/modem/modem.h`、`src/modem/modem_air780ep.h`、`src/at_engine/at_engine.h` | 组件内部相邻层；Facade factory 作为 composition root 可见全部装配 API | `core_`、`mqtt_client_`、`tcp_client_`、`ping_client_`、`modem_`、`modem_air780ep_`、`at_engine_` |
| 模块私有 API | `*_priv.h` | 当前模块自己的 `.c` 文件 | 模块内部命名 |
| 文件内部 | `.c` 中 static | 当前 `.c` 文件 | 无限制 |

**核心区别**：用户 API 是给 App 开发者用的，层间 API 是层与层之间、以及 Facade 模块 factory 装配时用的。AT Engine、Modem、Core、MQTT Client Service、TCP Client Service 和 Ping Service 都没有任何用户 API——它们被 LWLTE Facade 封装，最终用户看不到它们的存在。

兼容既有文档契约：AT Engine、Modem、Core、MQTT Client Service 和 Ping Service 都没有任何用户 API；TCP Client Service 同样遵循这一规则。

`*_priv.h` 虽然通过 `PRIV_INCLUDE_DIRS` 在编译上可见，但约束上只允许同模块源码 include。Core 不 include `modem_priv.h`，Modem 不 include `core_priv.h`，Facade 不 include 任意 `_priv.h`。

---

## 1. AT Engine（AT 引擎层）

AT Engine 是内部最底层，负责 AT 协议解析和 UART 硬件操作。该层有以下类：

### 1.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `at_engine_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | 分组保存 UART 硬件参数和运行参数 |
| `at_engine_handle_t` | 层间 API (opaque) | Modem 层 + Facade 模块 factory | 句柄 | AT Engine 实例句柄 |
| `at_response_t` | 层间 API | Modem 层 | 值对象 | 一次 AT 命令的响应结果 |
| `at_cmd_options_t` | 层间 API | Modem 层 | 值对象 | 单次命令的超时、成功终止匹配和 OK 处理选项 |
| `at_cmd_success_match_t` | 层间 API | Modem 层 | 值对象 | 自定义成功响应匹配规则 |
| `at_urc_handler_t` | 层间 API | Modem 层 | 回调注册项 | URC 前缀 + 回调 + 用户上下文 |
| `at_state_t` | 内部 | AT Engine 自身 | 状态枚举 | 命令处理状态机 |
| `at_cmd_ctx_t` | 内部 | AT Engine 自身 | 工作上下文 | 一次命令执行期间的临时数据 |

### 1.2 `at_engine_config_t` — 引擎配置

**所属层**：AT Engine  
**可见性**：层间 API — Facade 模块 factory 创建 AT Engine 时填充并传入
**OOP 角色**：配置结构体

```c
typedef struct {
    uart_port_t uart_num;        // UART 端口号（如 UART_NUM_1）
    int tx_pin;                  // TX GPIO
    int rx_pin;                  // RX GPIO
    int baud_rate;               // 波特率（如 115200）
    int rx_buf_size;             // UART RX 环形缓冲区大小（字节）
} at_engine_uart_config_t;

typedef struct {
    int rx_task_stack;           // 接收任务栈大小（字节）
    int rx_task_priority;        // 接收任务优先级
    int rx_line_buf_size;        // 单行最大长度（字节）
    int cmd_default_timeout_ms;  // 默认命令超时（毫秒）
    int max_response_lines;      // 单次响应最大行数
} at_engine_runtime_config_t;

typedef struct {
    at_engine_uart_config_t uart;        // UART 硬件参数
    at_engine_runtime_config_t runtime;  // AT 引擎运行参数
} at_engine_config_t;
```

### 1.3 `at_engine_handle_t` — 引擎句柄

**所属层**：AT Engine  
**可见性**：层间 API (opaque) — Facade 模块 factory 创建，Modem 层持有句柄；struct 定义在 `.c` 中
**OOP 角色**：顶层对象，持有该层所有资源

**层间方法**：

```c
at_engine_handle_t at_engine_create(const at_engine_config_t *config);
esp_err_t    at_engine_destroy(at_engine_handle_t me);

/* 发送普通 AT 命令（阻塞调用，直到 OK/ERROR/CME/CMS 或超时） */
esp_err_t    at_engine_send_cmd(at_engine_handle_t me, const char *cmd,
                                at_response_t *response, uint32_t timeout_ms);

/* 使用单次命令选项发送 AT 命令，支持自定义成功终止响应 */
esp_err_t    at_engine_send_cmd_with_options(at_engine_handle_t me, const char *cmd,
                                             at_response_t *response,
                                             const at_cmd_options_t *options);

/* 发送带 payload prompt 的 AT 命令：等待 prompt（如 ">"）后写入原始 payload，
 * 再继续按 options 等待最终响应（如 MQTT/HTTP/SSL 证书下载类命令） */
esp_err_t    at_engine_send_cmd_with_payload(at_engine_handle_t me, const char *cmd,
                                             const uint8_t *payload, size_t payload_len,
                                             const char *payload_prompt,
                                             at_response_t *response,
                                             const at_cmd_options_t *options);

/* URC 回调注册 / 注销 */
esp_err_t    at_engine_register_urc(at_engine_handle_t me, const char *prefix,
                                     at_urc_handler_t *handler);
esp_err_t    at_engine_unregister_urc(at_engine_handle_t me, const char *prefix);

/* 命令路径独占段（层间私有 API，用于非 AT 临界段保留命令路径）：
 *   begin/end_exclusive 必须配对；不得从同一 AT Engine 的 URC 回调中调用。
 *   flush_rx 内部自取独占段；当前有命令执行时返回 ESP_ERR_INVALID_STATE。
 *   flush_rx_exclusive 须在已持独占段时调用。 */
esp_err_t    at_engine_begin_exclusive(at_engine_handle_t me);
esp_err_t    at_engine_flush_rx_exclusive(at_engine_handle_t me);
void         at_engine_end_exclusive(at_engine_handle_t me);
esp_err_t    at_engine_flush_rx(at_engine_handle_t me);
```

**内部结构**（定义在 `.c` 或 `_priv.h`）：

```c
struct at_engine_t {
    at_engine_config_t   config;              // 配置快照
    QueueHandle_t        uart_queue;          // ESP-IDF UART 事件队列
    TaskHandle_t         rx_task;             // UART 接收任务句柄
    SemaphoreHandle_t    rx_task_done_sema;   // RX task 退出同步信号量
    SemaphoreHandle_t    cmd_mutex;           // 命令互斥锁（同时只允许一个命令）
    SemaphoreHandle_t    cmd_done_sema;       // 命令完成信号量
    SemaphoreHandle_t    lock;                // 内部状态/URC 链表保护锁
    at_state_t           state;               // 当前状态
    bool                 destroying;          // destroy 已开始，拒绝新调用
    int                  active_callers;      // 已进入 send_cmd 的调用方数量
    at_cmd_ctx_t         cmd_ctx_storage;     // 当前命令上下文存储
    at_cmd_ctx_t        *cmd_ctx;             // 当前命令上下文（有命令时非空）
    at_urc_handler_t    *urc_handlers;        // URC 链表头
    int                  urc_handler_count;
    char                *line_buf;            // 行组装缓冲区
    char                *line_work_buf;       // 完整行处理缓冲区
    int                  line_buf_pos;        // 当前行已接收字节数
    bool                 line_overflow;       // 当前行过长，丢弃直到 LF
    uint32_t             rx_epoch;            // RX flush 代际，丢弃超时前本地残留字节
    char               **response_pool;       // 响应文本指针池，每行按需分配
    int                  response_pool_lines;
    int                  response_line_size;
    bool                 uart_driver_installed;
    volatile bool        rx_task_stop_requested;
};
```

UART 端口号只保存在配置快照的 `config.uart.uart_num` 中，不在句柄上维护第二份字段。

**关键设计决策**：
- `cmd_mutex` 用 `xSemaphoreCreateMutex()` 创建，保证多线程调用 `send_cmd` 的串行化
- `cmd_done_sema` 用 `xSemaphoreCreateBinary()` 创建，用于阻塞等待命令响应完成
- URC 处理器用单向链表存储，前缀匹配时顺序遍历（handler 数量少，线性查找足够）
- `rx_task` 由 `xTaskCreate()` 在 `at_engine_create()` 中创建，在 `at_engine_destroy()` 中通过停止标志 + `rx_task_done_sema` 协作退出，不强制删除正在运行的任务
- `destroying + active_callers` 用于阻止 destroy 与已进入 `send_cmd` 的调用方并发释放资源；调用方仍必须保证 `destroy` 不与新的同句柄 API 调用并发启动
- `rx_epoch` 在命令超时或 UART overflow flush 时递增，RX task 会丢弃旧 epoch 中已读但尚未处理的本地字节/完整行

### 1.4 `at_response_t` — 命令响应

**所属层**：AT Engine  
**可见性**：层间 API — Modem 层在栈上构造后传给 AT Engine，AT Engine 填入结果  
**OOP 角色**：值对象 — 跨层传递的数据结构

```c
typedef enum {
    AT_RESP_OK = 0,        // 成功终止响应； Successful final response
    AT_RESP_ERROR,         // 通用错误（收到 ERROR）
    AT_RESP_CME_ERROR,     // +CME ERROR: <code>
    AT_RESP_CMS_ERROR,     // +CMS ERROR: <code>
    AT_RESP_TIMEOUT,       // 超时未收到完整响应
    AT_RESP_ABORTED,       // 被主动取消
} at_response_status_t;

typedef struct {
    at_response_status_t  status;          // 响应状态
    int                   error_code;      // CME/CMS 错误码（status 为 CME_ERROR/CMS_ERROR 时有效）
    int                   line_count;      // 数据行数
    int                   max_lines;       // 调用方分配的 lines 数组容量
    char                **lines;           // 调用方分配的字符串数组（AT Engine 填入数据行）
} at_response_t;
```

**调用方使用模式**：

```c
/* 调用方负责分配 lines 数组 */
char *lines[8];
at_response_t resp = {
    .max_lines = 8,
    .lines     = lines,
};
esp_err_t err = at_engine_send_cmd(at, "AT+CSQ", &resp, 3000);
if (err == ESP_OK && resp.status == AT_RESP_OK) {
    // 解析 resp.lines[0]: "+CSQ: 20,99"
    // resp.line_count 为实际行数
}
```

**关键设计决策**：
- `lines` 数组由**调用方分配**，AT Engine 只填入指向实例内 `response_pool` 槽位所拥有字符串的指针
- 调用方不得释放或修改 `lines[i]` 指向的字符串；数据在同一 AT Engine 实例下次 `send_cmd` 前有效
- 实际保存行数按 `min(response->max_lines, config.runtime.max_response_lines)` 截断，防止溢出
- `response_pool` 只在创建时分配指针槽位；每条响应行在收到时按实际长度分配，下次 `send_cmd` 开始或 `at_engine_destroy()` 时统一释放

### 1.5 `at_cmd_options_t` — 单次命令选项

**所属层**：AT Engine
**可见性**：层间 API — Modem 层在调用特殊 AT 命令时构造后传给 AT Engine
**OOP 角色**：值对象 — 描述单次命令的成功终止策略

```c
typedef enum {
    AT_CMD_SUCCESS_MATCH_EXACT = 0,   // 完整匹配
    AT_CMD_SUCCESS_MATCH_PREFIX,      // 前缀匹配
    AT_CMD_SUCCESS_MATCH_ANY_LINE,    // 任意非错误响应行
} at_cmd_success_match_type_t;

typedef struct {
    at_cmd_success_match_type_t  type;   // 匹配类型
    const char                  *value;  // 匹配文本，ANY_LINE 时忽略
} at_cmd_success_match_t;

typedef struct {
    uint32_t                      timeout_ms;          // 超时时间，0 表示使用默认值
    uint32_t                      flags;               // AT_CMD_FLAG_* 标志
    const at_cmd_success_match_t *success_matches;     // 自定义成功匹配规则数组
    int                           success_match_count; // 自定义成功匹配规则数量
} at_cmd_options_t;
```

`at_engine_send_cmd()` 继续覆盖普通 `OK/ERROR` 命令。`at_engine_send_cmd_with_options()` 用于特殊命令，例如 `AT+CIPSHUT` 的 `SHUT OK`、`AT+CIFSR` 的纯 IP 行、以及先返回中间 `OK` 再返回 `CONNACK OK` / `CONNECT OK` 的连接类命令。

关键规则：
- `ERROR`、`+CME ERROR:`、`+CMS ERROR:` 永远作为失败终止响应。
- 默认 `OK` 仍作为成功终止响应。
- `AT_CMD_FLAG_NO_STANDARD_OK_FINAL` 可把 `OK` 降级为中间响应。
- `AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` 可丢弃这个中间 `OK`，不写入 `response.lines`。
- 自定义成功终止行会先写入 `response.lines`，再以 `AT_RESP_OK` 完成命令。

### 1.6 `at_urc_handler_t` — URC 处理器

**所属层**：AT Engine  
**可见性**：层间 API — Modem 层定义 handler 实例，调用 `at_engine_register_urc()` 注册  
**OOP 角色**：回调注册项

```c
typedef void (*at_urc_callback_t)(const char *prefix, const char *line,
                                   void *user_ctx);

typedef struct at_urc_handler {
    const char         *prefix;          // URC 前缀字符串（如 "+CGEV:", "+CEREG:"）
    at_urc_callback_t   callback;        // 匹配时回调
    void               *user_ctx;        // 回调上下文（透传）
    struct at_urc_handler *next;         // 链表下一节点（AT Engine 内部使用）
} at_urc_handler_t;
```

**上层使用模式**（Modem 层注册 URC）：

```c
/* Modem Adapter 定义 URC handler */
static void cgev_handler(const char *prefix, const char *line, void *user_ctx) {
    modem_handle_t me = (modem_handle_t)user_ctx;
    // "+CGEV: ME PDN DEACT 1" → MODEM_EVENT_PDP_DEACTIVATED
    // 生成 modem_event_t 并投递到 me->event_queue；
    // Core 之后由 Modem event_task 通知。
}

/* 注册时 handler 生命周期由调用方管理（通常是 static 或动态分配） */
static at_urc_handler_t cgev_handler_node = {
    .prefix    = "+CGEV:",
    .callback  = cgev_handler,
    .user_ctx  = NULL,  // 在 modem_start 时设置
};
at_engine_register_urc(at, "+CGEV:", &cgev_handler_node);
```

**关键约束**：
- `handler` 节点和 `prefix` 字符串由调用方拥有，注册期间必须保持有效，同一节点不可重复注册
- URC 回调在 AT Engine RX task 中同步执行；为保护 handler 生命周期，回调执行期间持有内部锁，因此回调必须短小非阻塞，且不得在同一 AT Engine 实例上调用 `send_cmd/register/unregister` 等会获取内部锁的 API
- 第一版实现中，命令等待期间收到的非最终响应行优先归入当前命令响应，不同时分发为 URC；无当前命令时才按 URC 前缀分发，以避免查询响应与同前缀 URC 混淆

### 1.7 `at_state_t` — 内部状态枚举

**所属层**：AT Engine  
**可见性**：内部 — 仅 `at_engine.c` 中使用，对层间 API 不可见  
**OOP 角色**：状态枚举

```c
typedef enum {
    AT_STATE_IDLE = 0,        // 空闲，可以发送命令
    AT_STATE_SENDING,         // 正在发送命令字符串
    AT_STATE_WAITING,         // 已发送，等待响应
    AT_STATE_RECEIVING,       // 正在接收响应行
    AT_STATE_ABORTING,        // 正在中止当前命令
} at_state_t;
```

### 1.8 `at_cmd_ctx_t` — 命令上下文（内部）

**所属层**：AT Engine  
**可见性**：内部 — 仅 `at_engine.c` 中使用，随命令生命周期创建和销毁  
**OOP 角色**：临时工作上下文

```c
typedef struct {
    const char     *cmd;                 // 命令字符串（调用方传入，不拷贝）
    uint32_t        timeout_ms;          // 超时时间（毫秒）
    at_response_t  *response;            // 指向调用方的 response 对象
    at_cmd_options_t options;            // 单次命令选项快照
    int             echo_consumed;       // 是否已消费命令回显行
    int             data_line_index;     // 当前填充到 response->lines 的索引
    bool            result_received;     // 已收到最终结果
} at_cmd_ctx_t;
```

### 1.9 AT Engine 线程模型

```
┌─────────────────────────────────────────────────────────────┐
│                     AT Engine 线程模型                       │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  调用方线程（Modem 层）                                       │
│  ┌──────────┐                                               │
│  │ send_cmd │──→ 获取 cmd_mutex                             │
│  └────┬─────┘    ──→ 发送 AT 命令到 UART                     │
│       │          ──→ 等待 cmd_done_sema（阻塞）              │
│       │          ──→ 被 RX 线程或超时唤醒                     │
│       │          ──→ 填充 response → 释放 cmd_mutex → 返回   │
│       │                                                     │
│  UART RX 线程（AT Engine 内部）                              │
│  ┌──────────┐                                               │
│  │ rx_task  │──→ 循环读取 UART 字节                          │
│  └──────────┘    ──→ 组装行（以 \r\n 分割）                  │
│                  ──→ 是 URC？ → 遍历 urc_handlers 链表分发   │
│                  ──→ 是响应行？ → 写入 cmd_ctx → 收到最终结果 │
│                      → 发 cmd_done_sema 信号量                │
│                                                             │
│  超时处理（在调用方线程中完成）                                │
│                  ──→ send_cmd 使用 xSemaphoreTake 等待       │
│                      cmd_done_sema                           │
│                  ──→ 超时 → 设置 AT_RESP_TIMEOUT             │
│                      → 清除 cmd_ctx → flush UART 输入/队列    │
│                      → rx_epoch++ → 释放 cmd_mutex           │
│                      → 返回 ESP_ERR_TIMEOUT                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**实现差异说明**：最初设计写为 RX task 轮询 `timeout_ticks`，实际实现改为调用方线程在 `xSemaphoreTake()` 上等待超时。这样超时归属更清晰，RX task 不需要周期性扫描命令上下文；超时返回前会刷新 UART 输入、重置事件队列和行缓冲，并递增 `rx_epoch` 丢弃旧本地 RX 数据，降低迟到响应污染下一条命令的风险。

---

## 2. Modem Adapter（模块适配层）

Modem Adapter 是 Core Service 的紧邻下层，负责把 Core 的语义操作翻译为具体模块的 AT 指令，并把模块 URC 翻译为 Core 可理解的事件。该层包含通用 Modem 基类、内部 ops 多态表、语义值对象，以及 Air780EP 具体子类。

Core 只通过 `modem_*` 层间包装 API 使用 `modem_handle_t`，不直接调用 AT Engine，不写 AT 指令字符串，也不 include 具体模块头文件。`modem_ops_t` 是 Modem 层内部多态机制，包装 API 内部再调用 `me->ops->method(me, ...)`。

### 2.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `modem_handle_t` | 层间 API (opaque) + 内部基类 | Core + Facade 模块 factory + Modem 实现 | 抽象基类/句柄 | Core 持有的 Modem 句柄，内部保存 ops、AT Engine、事件队列等公共资源 |
| `modem_ops_t` | 内部 | Modem 通用包装函数 + 具体子类 | 虚函数表 | 不暴露给 Core，子类用 `static const` ops 表实现多态 |
| `modem_state_t` | 层间 API | Core + Modem 层 | 状态枚举 | Modem 层本地生命周期和低层连接状态 |
| `modem_reg_status_t` | 层间 API | Core + Modem 层 | 状态枚举 | 蜂窝网络注册状态，来自 `+CEREG` / `+CGREG` / `+CREG` |
| `modem_sim_status_t` | 层间 API | Core + Modem 层 | 状态枚举 | SIM/PIN 状态，来自 `AT+CPIN?` / `+CPIN:` |
| `modem_info_t` | 层间 API | Core + Modem 层 | 值对象 | 模块和 SIM 卡静态信息 |
| `modem_signal_t` | 层间 API | Core + Modem 层 | 值对象 | 信号质量查询结果 |
| `modem_pdp_context_t` | 层间 API | Core + Modem 层 | 值对象 | PDP 上下文配置与激活结果 |
| `modem_mqtt_config_t` | 层间 API | Core + Modem 层 | 值对象 | 完整 MQTT 配置命令参数，Core 执行 `CORE_CMD_MQTT_CONFIGURE` 时使用；Air780EP 缓存后供 TCP/协议连接复用 |
| `modem_mqtt_topic_t` | 层间 API | Core + Modem 层 | 值对象 | MQTT 订阅/取消订阅 topic 参数 |
| `modem_mqtt_publish_t` | 层间 API | Core + Modem 层 | 值对象 | MQTT 发布参数，包含 topic 和 payload 指针 |
| `modem_ping_request_t` | 层间 API | Core + Modem 层 | 值对象 | Ping 请求参数，Core 执行 `CORE_CMD_PING` 时使用 |
| `modem_ping_reply_t` | 层间 API | Core + Modem 层 | 值对象 | 单次 ping reply 结果，包含序号、IP、耗时、TTL 和成功标志 |
| `modem_ping_summary_t` | 层间 API | Core + Modem 层 | 值对象 | Ping 汇总结果，包含 sent/received/lost/min/max/avg |
| `modem_event_id_t` | 层间 API | Core + Modem 层 | 事件枚举 | URC 翻译后的事件类型 |
| `modem_event_t` | 层间 API | Core + Modem 层 | 值对象 | Modem event task 上报给 Core 的事件 |
| `modem_protocol_t` | 层间 API | Core + Modem 层 | 枚举 | Modem 上报的上层协议类型，当前用于 MQTT 和 TCP 事件路由 |
| `modem_protocol_data_t` | 层间 API | Core + Modem 层 | 值对象 | Modem 上报给 Core 的协议数据事件 |
| `modem_event_callback_t` | 层间 API | Core + Modem 层 | 回调接口 | Core 注册，Modem event task 调用 |
| `modem_base_config_t` | 层间 API | 具体 Modem 配置结构体 | 配置结构体 | 通用硬件、时序和事件任务配置组 |
| `modem_air780ep_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | Air780EP GPIO、事件任务、默认超时等参数 |
| `modem_ml307r_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | ML307R GPIO、事件任务、默认超时等参数 |
| `modem_air780ep_t` | 内部 | Air780EP 实现自身 | 子类 | 继承 `modem_handle_t`，实现 Air780EP AT 指令和 URC 翻译 |
| `air780ep_cmd_ctx_t` | 内部 | Air780EP 实现自身 | 工作上下文 | 单次 AT 命令解析的临时数据 |

### 2.2 `modem_handle_t` — 通用 Modem 句柄和基类

**所属层**：Modem Adapter
**可见性**：层间 API opaque + 内部结构体；`src/modem/modem.h` 只暴露前置声明，`struct modem_t` 定义在 `src/modem/modem_priv.h` 或 `.c` 中
**OOP 角色**：抽象基类 + 顶层句柄

**公开类型**：

```c
typedef struct modem_t *modem_handle_t;
```

**声明顺序说明**：实际 `src/modem/modem.h` 中应先完成 `modem_handle_t` 前置声明，再定义状态枚举、值对象、事件对象和回调类型，最后声明以下函数原型。本节为了说明 `modem_handle_t` 的使用方式，先集中列出层间方法。

**层间方法**（`src/modem/modem.h`）：

```c
esp_err_t modem_destroy(modem_handle_t me);
esp_err_t modem_start(modem_handle_t me);
esp_err_t modem_reset(modem_handle_t me);

esp_err_t modem_register_event_callback(modem_handle_t me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

esp_err_t modem_get_state(modem_handle_t me, modem_state_t *state);
esp_err_t modem_get_info(modem_handle_t me, modem_info_t *info);
esp_err_t modem_get_sim_status(modem_handle_t me, modem_sim_status_t *status);
esp_err_t modem_get_signal(modem_handle_t me, modem_signal_t *signal);
esp_err_t modem_get_registration(modem_handle_t me, modem_reg_status_t *status);
esp_err_t modem_get_packet_attach_status(modem_handle_t me, bool *attached);

esp_err_t modem_set_apn(modem_handle_t me, uint8_t cid, const char *apn);
esp_err_t modem_activate_pdp(modem_handle_t me, uint8_t cid);
esp_err_t modem_deactivate_pdp(modem_handle_t me, uint8_t cid);
esp_err_t modem_get_pdp_context(modem_handle_t me, uint8_t cid,
                                 modem_pdp_context_t *pdp);

esp_err_t modem_mqtt_configure(modem_handle_t me,
                               const modem_mqtt_config_t *config);
esp_err_t modem_mqtt_tcp_connect(modem_handle_t me);
esp_err_t modem_mqtt_connect(modem_handle_t me);
esp_err_t modem_mqtt_disconnect(modem_handle_t me);
esp_err_t modem_mqtt_tcp_disconnect(modem_handle_t me);
esp_err_t modem_mqtt_subscribe(modem_handle_t me,
                               const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_unsubscribe(modem_handle_t me,
                                 const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_publish(modem_handle_t me,
                             const modem_mqtt_publish_t *publish);
esp_err_t modem_mqtt_get_status(modem_handle_t me, modem_mqtt_status_t *status);
esp_err_t modem_socket_open(modem_handle_t me,
                            const modem_socket_open_t *open);
esp_err_t modem_socket_send(modem_handle_t me,
                            const modem_socket_send_t *send);
esp_err_t modem_socket_recv(modem_handle_t me,
                            const modem_socket_recv_t *recv,
                            modem_socket_recv_result_t *result);
esp_err_t modem_socket_close(modem_handle_t me,
                             const modem_socket_close_t *close);
esp_err_t modem_ping(modem_handle_t me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary);
```

`modem_start()` 只表示模块动态开机到基础 AT ready：硬复位后轮询 `AT` 直到 `OK`，执行基础 AT 初始化，并注册运行期 URC。它不负责 SIM 检查、网络注册等待、APN/PDP 激活或 IP 查询；这些网络上线步骤由 Core 在处理 `CORE_SIG_START` 时继续执行。

**关键内部字段类别**（非完整代码快照，实际以 `src/modem/modem_priv.h` 为准）：

- `ops`：指向具体模块 `modem_ops_t` 的 vptr。
- `at`：Facade factory 注入的下层 AT Engine 句柄，Modem 借用但不拥有生命周期。
- `lock`、`state`、`destroying`：保护 Modem 本地状态和销毁过程。
- `event_queue`、`event_task` 及同步信号量：把 URC 翻译后的 `modem_event_t` 解耦后上报给 Core。
- `event_cb`、`event_user_ctx`：Core 注册的上行事件回调槽位。
- `name`：模块实现名称，如 `air780ep`，用于日志和诊断。

**关键设计决策**：
- `modem_handle_t` 对 Core opaque，Core 不直接访问 `ops` 或内部字段。
- 通用包装 API 统一做参数检查、状态检查和必填方法检查。
- `event_queue + event_task` 属于基类资源，所有具体模块共用同一套上行事件解耦机制。
- `at` 句柄由 Facade 模块 factory 创建并传入具体模块工厂；Modem 不拥有 AT Engine 生命周期，只在 destroy 前注销自己注册的 URC handler。

### 2.3 `modem_ops_t` — Modem 虚函数表

**所属层**：Modem Adapter
**可见性**：内部；只给 Modem 通用实现和具体子类使用，不放入 `src/modem/modem.h`
**OOP 角色**：虚函数表

**模块命令值对象**（`src/modem/modem.h`）：

```c
typedef struct {
    const char *client_id;
    const char *username;
    const char *password;
    const char *host;
    uint16_t port;
    bool clean_session;
    uint16_t keepalive_s;
} modem_mqtt_config_t;

typedef struct {
    const char *topic;
    uint8_t qos;
} modem_mqtt_topic_t;

typedef struct {
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} modem_mqtt_publish_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} modem_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} modem_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} modem_ping_summary_t;

typedef enum {
    MODEM_SOCKET_PROTO_TCP = 0,
} modem_socket_proto_t;

typedef enum {
    MODEM_SOCKET_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    MODEM_SOCKET_TRANSPORT_TLS,            /**< TLS； TLS */
} modem_socket_transport_t;

typedef struct {
    modem_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
    int *modem_error_code;
    modem_socket_transport_t transport;  /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;              /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
} modem_socket_open_t;

typedef struct {
    uint8_t conn_id;
    const uint8_t *data;
    size_t len;
    uint32_t timeout_ms;
} modem_socket_send_t;

typedef struct {
    uint8_t conn_id;
    size_t max_len;
} modem_socket_recv_t;

typedef struct {
    uint8_t conn_id;
    uint8_t *payload;
    size_t payload_len;
    size_t remaining_len;
    int modem_error_code;
} modem_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;
    uint32_t timeout_ms;
} modem_socket_close_t;
```

这些值对象属于 Modem Adapter 的模块命令语义，不是上层 service API。Core 执行 `core_cmd_t` 时把 Core command 数据转换成这些值对象，再调用对应 `modem_*` 包装 API；MQTT Client Service、TCP Client Service 和 Ping Service 不 include `modem.h`，也不直接调用这些函数。

```c
typedef struct modem_ops {
    esp_err_t (*destroy)(modem_handle_t me);
    esp_err_t (*start)(modem_handle_t me);
    esp_err_t (*reset)(modem_handle_t me);
    esp_err_t (*get_info)(modem_handle_t me, modem_info_t *info);
    esp_err_t (*get_sim_status)(modem_handle_t me, modem_sim_status_t *status);
    esp_err_t (*get_signal)(modem_handle_t me, modem_signal_t *signal);
    esp_err_t (*get_registration)(modem_handle_t me, modem_reg_status_t *status);
    esp_err_t (*get_packet_attach_status)(modem_handle_t me, bool *attached);
    esp_err_t (*set_apn)(modem_handle_t me, uint8_t cid, const char *apn);
    esp_err_t (*activate_pdp)(modem_handle_t me, uint8_t cid);
    esp_err_t (*deactivate_pdp)(modem_handle_t me, uint8_t cid);
    esp_err_t (*get_pdp_context)(modem_handle_t me, uint8_t cid,
                                  modem_pdp_context_t *pdp);
    esp_err_t (*mqtt_configure)(modem_handle_t me,
                                const modem_mqtt_config_t *config);
    esp_err_t (*mqtt_tcp_connect)(modem_handle_t me);
    esp_err_t (*mqtt_connect)(modem_handle_t me);
    esp_err_t (*mqtt_disconnect)(modem_handle_t me);
    esp_err_t (*mqtt_tcp_disconnect)(modem_handle_t me);
    esp_err_t (*mqtt_subscribe)(modem_handle_t me,
                                const modem_mqtt_topic_t *topic);
    esp_err_t (*mqtt_unsubscribe)(modem_handle_t me,
                                  const modem_mqtt_topic_t *topic);
    esp_err_t (*mqtt_publish)(modem_handle_t me,
                              const modem_mqtt_publish_t *publish);
    esp_err_t (*mqtt_get_status)(modem_handle_t me,
                                 modem_mqtt_status_t *status);
    esp_err_t (*socket_open)(modem_handle_t me,
                             const modem_socket_open_t *open);
    esp_err_t (*socket_send)(modem_handle_t me,
                             const modem_socket_send_t *send);
    esp_err_t (*socket_recv)(modem_handle_t me,
                             const modem_socket_recv_t *recv,
                             modem_socket_recv_result_t *result);
    esp_err_t (*socket_close)(modem_handle_t me,
                              const modem_socket_close_t *close);
    esp_err_t (*ping)(modem_handle_t me,
                      const modem_ping_request_t *request,
                      modem_ping_reply_t *replies,
                      size_t max_replies,
                      modem_ping_summary_t *summary);
} modem_ops_t;
```

**调用模式**：

```c
esp_err_t modem_get_signal(modem_handle_t me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_signal,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_signal not supported");

    return me->ops->get_signal(me, signal);
}
```

**关键设计决策**：
- ops 表由具体子类以 `static const modem_ops_t` 定义，放在只读段。
- Core 不直接看到 `modem_ops_t`，只通过 `modem_*` 包装 API 间接使用多态。
- 第一版将表中方法按严格接口处理；具体模块确实不支持时，子类方法返回 `ESP_ERR_NOT_SUPPORTED`。

#### ops 功能与 Air780EP AT 指令映射

`modem_ops_t` 按 Core 需要的语义能力划分，不按 AT 指令逐条暴露。Air780EP 第一版实现可优先使用下表命令；未进入表格的 AT 指令只作为初始化细节、诊断能力或后续扩展保留。

| ops 方法 | 语义边界 | Air780EP 第一版命令 |
|----------|----------|---------------------|
| `start` | 模块动态开机到基础 AT ready：硬复位后轮询 `AT` 直到 `OK`，执行基础 AT 初始化；不激活 PDP | 硬复位后在 ready 总超时内轮询 `AT`，成功后执行 `ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`、`AT*I`，再注册运行期 URC 并发布 ready |
| `reset` | 通过 EN 执行硬复位，并在 `AT OK` 后恢复基础 AT 工作环境 | 拉低 EN、等待 `reset_pulse_ms`、拉高 EN、轮询 `AT` 到 `OK`，再执行基础 AT 初始化命令 |
| `get_info` | 读取模块/SIM 静态标识，Core 不解析原始 AT 行 | `AT+CGSN`、`AT+CIMI`、`AT+ICCID`、`AT+CGMM`、`AT+CGMR`；`ATI`/`AT+VER` 可作为固件信息补充 |
| `get_sim_status` | 查询 SIM/PIN 可用性 | `AT+CPIN?` |
| `get_signal` | 查询当前基础信号质量 | `AT+CSQ`；`AT+CESQ` 只作为后续 LTE 扩展指标来源 |
| `get_registration` | 查询蜂窝网络注册状态 | Air780EP 优先 `AT+CEREG?`，必要时用 `AT+CGREG?` 补充分组域状态，用 `AT+CREG?` 作为通用注册状态兜底 |
| `get_packet_attach_status` | 查询分组域附着状态 | `AT+CGATT?`，要求 `+CGATT: 1` 后才能进入 PDP/TCPIP 激活阶段 |
| `set_apn` | 配置 PDP context 的 APN | `AT+CGDCONT=<cid>,"IP","<apn>"`；APN 用户名/密码后续再通过单独能力扩展，不塞进当前 API |
| `activate_pdp` | 注册和附着已就绪后激活数据面并获得 IP | Air780EP TCPIP 路径使用 `AT+CSTT`、`AT+CIICR`、`AT+CIFSR`；Core 先通过 `get_sim_status`、`get_registration`、`get_packet_attach_status` 等阶段确认前置条件 |
| `deactivate_pdp` | 关闭数据面并清理 Air780EP TCPIP 场景 | 优先 `AT+CIPSHUT`；标准 PDP 路径需要时可使用 `AT+CGACT=0,<cid>` |
| `get_pdp_context` | 返回 APN、激活状态和 IP 地址快照 | 组合缓存值、`AT+CGDCONT?`、`AT+CGACT?`、`AT+CGPADDR=<cid>`；TCPIP 路径下也可使用最近一次 `AT+CIFSR` 结果 |
| `mqtt_configure` | 配置模块内置 MQTT client、broker 和会话参数，并缓存完整配置，不建立网络连接 | `AT+MCONFIG` |
| `mqtt_tcp_connect` | 使用已缓存的 host/port 建立模块 MQTT TCP 通道 | `AT+MIPSTART`，成功接受 `CONNECT OK` / `ALREADY CONNECT`，失败识别 `CONNECT FAIL` |
| `mqtt_connect` | 使用已缓存的 clean_session/keepalive_s 执行 MQTT CONNECT 建立 broker 会话 | `AT+MCONNECT`，成功 `CONNACK OK` |
| `mqtt_disconnect` | 断开 MQTT broker 会话，不关闭底层 TCP 通道 | `AT+MDISCONNECT` |
| `mqtt_tcp_disconnect` | 关闭 MQTT TCP 通道，应在 MQTT 会话断开后执行 | `AT+MIPCLOSE` |
| `mqtt_subscribe` | 订阅 MQTT topic | `AT+MSUB`，成功 `SUBACK` |
| `mqtt_unsubscribe` | 取消订阅 MQTT topic | `AT+MUNSUB`，成功 `UNSUBACK` |
| `mqtt_publish` | 发布定长 MQTT payload | `AT+MPUBEX` + payload prompt |
| `socket_open` | 打开模块内置 TCP socket | Air780EP `AT+CIPSTART`；ML307R `AT+MIPOPEN`；TLS 时分别前置 AT+CIPSSL=1（ctx 0，并先 AT+SSLCFG="hostname",0,...）/ AT+MIPCFG="ssl",<conn>,1,<ssl_id> |
| `socket_send` | 发送定长 socket payload | Air780EP `AT+CIPSEND`；ML307R `AT+MIPSEND` |
| `socket_recv` | 从模块缓存读取 socket payload | Air780EP `AT+CIPRXGET=3,<len>`；ML307R `AT+MIPRD` |
| `socket_close` | 关闭 socket 连接 | Air780EP `AT+CIPCLOSE`；ML307R `AT+MIPCLOSE` |
| `ping` | 执行网络连通性诊断，不参与 Core online 条件 | `AT+CIPPING` |

MQTT command ops、socket ops 和 ping ops 是 Modem Adapter 暴露给 Core 的模块语义能力，用于执行 Core command queue 中的上层 service 命令。它们不改变上层 service 的依赖方向：MQTT service、TCP Client Service 和 Ping Service 仍只调用 Core，不调用 Modem。

`AT+IPR`、`AT+IFC`、`AT&W` 属于板级串口/持久化配置，不进入 Modem ops。`AT+COPS?`、`AT^SYSINFO` 属于诊断或联网自检，第一版可作为 Air780EP 内部 helper，不先扩大 Core 可见 API。`AT+CIPPING` 现在作为 `modem_ping()` 的 Air780EP 映射暴露给 Core command queue，但它仍是用户触发的诊断命令，不参与 Core online 判定。`AT+CSCLK`、`AT+POWERMODE`、`AT+CFGRI` 等低功耗指令需要 Core 低功耗策略后再设计独立 ops。

**特殊响应约束**：`AT+CIFSR` 成功时返回纯 IP 地址且不以 `OK` 结束，`AT+CIPSHUT` 成功终止行为 `SHUT OK`，`MCONNECT` 类命令可能先返回中间 `OK` 再返回 `CONNACK OK` 或 `CONNECT OK`。这些命令应使用 `at_engine_send_cmd_with_options()` 配置自定义成功终止规则，而不是把模块私有响应硬编码进 AT Engine。

### 2.4 `modem_state_t` — Modem 本地状态

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：状态枚举

```c
typedef enum {
    MODEM_STATE_CREATED = 0,       // 已创建，尚未初始化
    MODEM_STATE_INITIALIZING,      // 正在执行模块初始化序列
    MODEM_STATE_READY,             // 初始化完成，可以接受 Core 调用
    MODEM_STATE_REGISTERING,       // 正在等待网络注册
    MODEM_STATE_REGISTERED,        // 已注册到蜂窝网络
    MODEM_STATE_PDP_ACTIVE,        // PDP 已激活
    MODEM_STATE_ERROR,             // 低层错误，需要 Core 决策恢复策略
    MODEM_STATE_OFF,               // 已停机、可重启；EN 未配置时可能仍上电
    MODEM_STATE_DESTROYING,        // 正在销毁
} modem_state_t;
```

**边界说明**：`modem_state_t` 只表示 Modem 层观察到的低层状态，不替代 Core 的网络状态机。重连策略、退避策略和业务状态迁移仍属于 Core。

### 2.5 `modem_reg_status_t` — 网络注册状态

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：状态枚举

```c
typedef enum {
    MODEM_REG_NOT_REGISTERED = 0,  // 未注册，且未搜索
    MODEM_REG_REGISTERED_HOME,     // 已注册，归属网络
    MODEM_REG_SEARCHING,           // 正在搜索网络
    MODEM_REG_DENIED,              // 注册被拒绝
    MODEM_REG_UNKNOWN,             // 状态未知
    MODEM_REG_REGISTERED_ROAMING,  // 已注册，漫游网络
} modem_reg_status_t;
```

### 2.6 `modem_sim_status_t` — SIM 状态

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：状态枚举

```c
typedef enum {
    MODEM_SIM_UNKNOWN = 0,       // 状态未知或尚未查询
    MODEM_SIM_READY,             // SIM 可用，AT+CPIN? 返回 READY
    MODEM_SIM_PIN_REQUIRED,      // 需要 PIN
    MODEM_SIM_PUK_REQUIRED,      // 需要 PUK
    MODEM_SIM_NOT_INSERTED,      // 未检测到 SIM 或 SIM 被移除
    MODEM_SIM_ERROR,             // 其他 SIM 错误状态
} modem_sim_status_t;
```

**关键设计决策**：SIM 状态是运行时状态，不放入 `modem_info_t`。`modem_info_t` 只保存 IMEI、IMSI、ICCID、型号和固件版本等静态信息；`+CPIN:` URC 或 `AT+CPIN?` 查询结果应更新 `modem_sim_status_t` 并通过 `MODEM_EVENT_SIM_CHANGED` 上报。

### 2.7 `modem_info_t` — 模块和 SIM 信息

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：值对象

```c
#define MODEM_IMEI_MAX_LEN      16
#define MODEM_IMSI_MAX_LEN      16
#define MODEM_ICCID_MAX_LEN     24
#define MODEM_MODEL_MAX_LEN     32
#define MODEM_FW_REV_MAX_LEN    64

typedef struct {
    char imei[MODEM_IMEI_MAX_LEN];        // 模块 IMEI，15 位数字 + NUL
    char imsi[MODEM_IMSI_MAX_LEN];        // SIM IMSI，15 位数字 + NUL
    char iccid[MODEM_ICCID_MAX_LEN];      // SIM ICCID
    char model[MODEM_MODEL_MAX_LEN];      // 模块型号
    char fw_revision[MODEM_FW_REV_MAX_LEN]; // 固件版本
} modem_info_t;
```

**来源示例**：Air780EP 可通过 `AT+CGSN`、`AT+CIMI`、`AT+ICCID`、`AT+CGMM`、`AT+CGMR` 等命令填充该对象；`ATI` / `AT+VER` 可作为固件信息补充。Core 不解析这些 AT 响应行。

### 2.8 `modem_signal_t` — 信号质量

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：值对象

```c
typedef struct {
    int  rssi;             // CSQ 原始 RSSI，0..31，99 表示未知
    int  ber;              // CSQ 原始 BER，0..7，99 表示未知
    int  rssi_dbm;         // RSSI 换算后的 dBm
    bool rssi_dbm_valid;   // rssi_dbm 是否有效
} modem_signal_t;
```

**关键设计决策**：第一版只要求支持 `AT+CSQ` 对应的 RSSI/BER。RSRP、RSRQ、SINR 等 LTE 扩展指标后续需要时再增加字段或新值对象。

### 2.9 `modem_pdp_context_t` — PDP 上下文

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：值对象

```c
#define MODEM_APN_MAX_LEN       64
#define MODEM_PDP_TYPE_MAX_LEN  8
#define MODEM_IP_ADDR_MAX_LEN   48

typedef struct {
    uint8_t cid;                              // PDP context id
    char    apn[MODEM_APN_MAX_LEN];          // APN
    char    pdp_type[MODEM_PDP_TYPE_MAX_LEN]; // "IP" / "IPV6" / "IPV4V6"
    bool    active;                           // PDP 是否已激活
    char    ip_addr[MODEM_IP_ADDR_MAX_LEN];   // 模块分配到的 IP 地址
} modem_pdp_context_t;
```

### 2.10 `modem_event_t` — Modem 上行事件

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：值对象 + 回调参数

```c
typedef enum {
    MODEM_EVENT_READY = 0,          // 模块初始化完成
    MODEM_EVENT_SIM_CHANGED,        // SIM/PIN 状态变化
    MODEM_EVENT_REG_CHANGED,        // 网络注册状态变化
    MODEM_EVENT_PDP_ACTIVATED,      // PDP 激活
    MODEM_EVENT_PDP_DEACTIVATED,    // PDP 去激活
    MODEM_EVENT_SIGNAL_CHANGED,     // 信号质量变化
    MODEM_EVENT_ERROR,              // 模块侧错误事件
    MODEM_EVENT_PROTOCOL_DATA,      // 上层协议数据事件，如 MQTT/TCP 下行数据
    MODEM_EVENT_PROTOCOL_CLOSED,    // 上层协议连接关闭事件
} modem_event_id_t;

typedef enum {
    MODEM_PROTOCOL_MQTT = 0,        // MQTT 协议事件
    MODEM_PROTOCOL_TCP,             // TCP 协议事件
} modem_protocol_t;

typedef struct {
    modem_protocol_t protocol;      // 协议类型
    uint8_t          conn_id;       // TCP connection id，MQTT 固定为 0
    const char      *topic;         // MQTT topic，回调期间有效
    size_t           topic_len;     // topic 长度
    const uint8_t   *payload;       // MQTT/TCP payload，回调期间有效
    size_t           payload_len;   // payload 长度
    int              reason;        // 连接关闭或错误原因
    int              modem_error_code; // 模块原始错误码
} modem_protocol_data_t;

typedef struct {
    modem_event_id_t id;
    union {
        modem_sim_status_t   sim_status;
        modem_reg_status_t   reg_status;
        modem_pdp_context_t  pdp;
        modem_signal_t       signal;
        modem_protocol_data_t protocol_data;
        int                  error_code;
    } data;
} modem_event_t;

typedef void (*modem_event_callback_t)(modem_handle_t modem,
                                       const modem_event_t *event,
                                       void *user_ctx);
```

**硬约束**：Air780EP 的 AT Engine URC handler 不得直接调用 `modem_event_callback_t`。URC handler 只能把 `modem_event_t` 投递到 Modem 基类对象的 `event_queue`，由 Modem 基类对象的 `event_task` 调用 Core 注册的回调。

`MODEM_EVENT_PROTOCOL_DATA` 和 `MODEM_EVENT_PROTOCOL_CLOSED` 追加在 `MODEM_EVENT_ERROR` 之后，避免改变既有事件 ID 的数值。

`MODEM_EVENT_PROTOCOL_DATA` 的 `topic` 和 `payload` 指针只在 `modem_event_callback_t` 执行期间有效。Air780EP URC handler 解析 `+MSUB:` 后必须把 topic/payload 复制到 Modem event task 可安全持有的堆内存中；TCP manual RX 路径必须把 payload 复制到同样的事件生命周期中，`topic == NULL` 且 `topic_len == 0`。`modem_post_event()` 成功后由 Modem event task 在 Core 回调返回后释放，`modem_post_event()` 失败时仍由调用者释放。Core 若要继续上报给 MQTT/TCP service，必须再次复制或保证新的事件数据生命周期覆盖 Core event callback。

### 2.11 `modem_air780ep_config_t` / `modem_ml307r_config_t` — 具体模块配置

**所属层**：Modem Adapter
**可见性**：层间 API，放入 `src/modem/modem_air780ep.h` 和 `src/modem/modem_ml307r.h`，只给 Facade 模块 factory 使用
**OOP 角色**：配置结构体

```c
typedef struct {
    gpio_num_t en_pin;                  // EN GPIO，未使用时为 GPIO_NUM_NC
} modem_hardware_config_t;

typedef struct {
    uint32_t reset_pulse_ms;            // 复位脉冲(EN 拉低保持)时长
    uint32_t ready_timeout_ms;          // AT OK 等待总超时
    uint32_t default_cmd_timeout_ms;    // 模块命令默认超时
} modem_timing_config_t;

typedef struct {
    int event_queue_size;               // Modem 事件队列长度
    int event_task_stack;               // Modem event task 栈大小
    int event_task_priority;            // Modem event task 优先级
} modem_event_config_t;

typedef struct {
    modem_hardware_config_t hardware;   // 硬件控制
    modem_timing_config_t timing;       // 时序参数
    modem_event_config_t event;         // 事件任务参数
} modem_base_config_t;

typedef struct {
    modem_base_config_t base;           // Air780EP 通用基础配置
} modem_air780ep_config_t;

typedef struct {
    modem_base_config_t base;           // ML307R 通用基础配置
} modem_ml307r_config_t;

modem_handle_t modem_air780ep_create(at_engine_handle_t at,
                               const modem_air780ep_config_t *config);

modem_handle_t modem_ml307r_create(at_engine_handle_t at,
                                    const modem_ml307r_config_t *config);
```

**关键设计决策**：
- `modem_air780ep_create()` 是具体模块工厂，只应出现在 Facade 模块 factory 装配代码中。
- `modem_ml307r_create()` 也是具体模块工厂，和 Air780EP 一样只在对应 Facade 模块 factory 中使用。
- Core 不 include `modem_air780ep.h`，只接收工厂返回的 `modem_handle_t`。
- GPIO 控制属于 Modem 层职责，Air780EP 实现可以直接使用 ESP-IDF `driver/gpio.h`。
- 硬件复位通过 EN 引脚实现：拉低 EN，等待 reset_pulse_ms，再拉高 EN；随后在 ready 总超时内轮询 `AT` 到 `OK`，再执行基础 AT 初始化命令。`air780ep_start()` 和 `air780ep_reset()` 都使用此方式。

### 2.12 `modem_air780ep_t` — Air780EP 子类

**所属层**：Modem Adapter
**可见性**：内部，定义在 `src/modem/modem_air780ep.c`
**OOP 角色**：具体子类

```c
#define AIR780EP_MAX_PDP_CONTEXTS  4

typedef struct {
    struct modem_t                  base;          // 必须是第一个字段，实现向上转型
    modem_air780ep_config_t  config;        // 配置快照
    at_urc_handler_t         cpin_handler;  // +CPIN: URC handler
    at_urc_handler_t         creg_handler;  // +CREG: URC handler
    at_urc_handler_t         cereg_handler; // +CEREG: URC handler
    at_urc_handler_t         cgreg_handler; // +CGREG: URC handler
    at_urc_handler_t         cgev_handler;  // +CGEV: URC handler
    at_urc_handler_t         pdp_deact_handler;       // +PDP DEACT URC handler
    at_urc_handler_t         pdp_colon_deact_handler; // +PDP:DEACT URC handler
    at_urc_handler_t         msub_handler;  // +MSUB: URC handler
    modem_info_t             cached_info;   // 已查询到的模块/SIM 信息
    modem_sim_status_t       last_sim_status; // 最近一次 SIM 状态
    modem_reg_status_t       last_reg_status; // 最近一次网络注册状态
    modem_signal_t           last_signal;   // 最近一次信号质量
    modem_pdp_context_t      pdp[AIR780EP_MAX_PDP_CONTEXTS];
    modem_mqtt_config_t      mqtt_config;
    bool                     urc_registered;
    bool                     initialized;
    bool                     mqtt_configured;
    bool                     mqtt_tcp_connected;
    bool                     mqtt_session_connected;
    bool                     mqtt_data_enabled;
} modem_air780ep_t;
```

**关键设计决策**：
- `base` 必须位于结构体第一个字段，子类返回给上层时使用 `&self->base`。
- 从 `modem_handle_t` 反推 `modem_air780ep_t *` 时使用 `container_of(me, modem_air780ep_t, base)`，禁止裸强转。
- URC handler 节点生命周期由 Air780EP 对象拥有，基础 AT 初始化完成后注册 `+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:`、`+CGEV:`、`+PDP DEACT`、`+PDP:DEACT`、`+MSUB:`，`destroy` 时注销。
- `+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:` 既可能是查询响应，也可能是空闲期 URC；Air780EP handler 只处理 AT Engine 分发出来的空闲期 URC，命令响应由对应 ops 方法解析。
- `+MSUB:` 是当前 MQTT 下行数据路径入口；handler 只在 `mqtt_data_enabled` 后复制 topic/payload 并投递 `MODEM_EVENT_PROTOCOL_DATA`。

### 2.13 `air780ep_cmd_ctx_t` — Air780EP 命令上下文

**所属层**：Modem Adapter
**可见性**：内部，仅 Air780EP 实现使用
**OOP 角色**：临时工作上下文

**关键内容**（非完整代码快照）：

- 当前 AT 命令字符串和本次命令超时。
- 调用 AT Engine 所需的 `at_response_t` 与响应行指针数组。
- Air780EP 响应解析用的临时工作缓冲。
- 仅在单次命令调用栈内创建和使用，不跨命令保存，也不暴露给 Core。

**使用模式**：Air780EP 普通 `OK/ERROR` 命令可继续使用 `at_engine_send_cmd()`。特殊成功终止命令在栈上创建 `air780ep_cmd_ctx_t` 和 `at_cmd_options_t`，调用 `at_engine_send_cmd_with_options()`，再解析 `response.lines`。该上下文不跨命令保存，不暴露给 Core。

### 2.14 Modem 线程模型

```
┌─────────────────────────────────────────────────────────────┐
│                     Modem Adapter 线程模型                   │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Core 线程 / Core FSM task                                  │
│  ┌────────────────┐                                         │
│  │ modem_* API    │──→ 参数/状态检查                         │
│  └───────┬────────┘    ──→ me->ops->method(me, ...)          │
│          │          ──→ Air780EP 生成 AT 命令                 │
│          │          ──→ at_engine_send_cmd() 阻塞等待响应     │
│          │          ──→ 解析 at_response_t 为 modem_* 值对象  │
│          │                                                  │
│  AT Engine RX task                                          │
│  ┌────────────────┐                                         │
│  │ URC callback   │──→ Air780EP URC handler                  │
│  └───────┬────────┘    ──→ 解析 +CPIN/+CREG/+CEREG/+CGREG       │
│          │              /+CGEV/+PDP DEACT/+MSUB 并生成事件     │
│          │          ──→ xQueueSend(event_queue, ..., 0)      │
│          │              不得直接调用 Core 回调                │
│          │                                                  │
│  Modem event task                                           │
│  ┌────────────────┐                                         │
│  │ event loop     │──→ xQueueReceive(event_queue)            │
│  └───────┬────────┘    ──→ 调用 modem_event_callback_t       │
│          │              Core 回调不在 AT RX task 中执行       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**硬约束**：Modem URC handler 禁止直接调用 Core。它只能投递事件到 `event_queue`，由 `event_task` 执行 Core 回调。这样静态依赖和运行时执行上下文都保持逐层隔离，避免 Core 逻辑阻塞 AT Engine RX task 或在 AT Engine 内部锁未释放时反向进入下层。

**错误处理规则**：
- Modem 层公开 API 和 ops 方法统一返回 `esp_err_t`。
- Modem 层不新增自定义错误码，统一使用 ESP-IDF 标准错误码。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 状态错误返回 `ESP_ERR_INVALID_STATE`。
- 不支持的能力返回 `ESP_ERR_NOT_SUPPORTED`。
- `at_engine_send_cmd()` 超时传播 `ESP_ERR_TIMEOUT`。
- AT Engine 返回 `ESP_OK` 但 `response.status` 为 `AT_RESP_ERROR`、`AT_RESP_CME_ERROR` 或 `AT_RESP_CMS_ERROR` 时，Air780EP 适配层映射为标准 ESP-IDF 错误码，并记录原始错误码。
- 响应行格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。

**与 AT Engine 的边界**：
- Modem 层可以调用 `at_engine_send_cmd()`、`at_engine_send_cmd_with_options()` 和 `at_engine_register_urc()`，因为 AT Engine 是紧邻下层。
- Core 不能调用 AT Engine API。
- AT Engine 不知道 Air780EP 语义，只做 URC 前缀匹配和原始行分发。

---

## 3. Core Service（核心服务层）

Core Service 是内部 service 层，负责网络状态机、PDP 管理和连接恢复。Core 只通过 `modem_*` 包装 API 操作 `modem_handle_t`，不直接调用 AT Engine，不写 AT 指令字符串。Core 通过 `esp_event` 和回调把状态变化交给 LWLTE Facade，由 Facade 再翻译为用户事件。

```
Core Service
├── core_handle_t        层间 API opaque 句柄，Facade 持有并调用
├── core_fsm_t    Core 内部组件，属于 core_handle_t，负责串行处理 Core 信号
├── net_mgr_t     Core 内部组件，属于 core_handle_t，负责网络激活和重连策略
└── pdp_mgr_t     Core 内部组件，属于 core_handle_t，负责 PDP 上下文状态缓存
```

`net_mgr_t`、`pdp_mgr_t`、`core_fsm_t` 不是 `core_handle_t` 子类。它们不能向上转型为 `core_handle_t`，也不实现 `core_ops`；它们是 `core_handle_t` 的组合成员。`modem_air780ep_t` 才是 `modem_handle_t` 的子类，因为它以 `struct modem_t base` 为第一个成员并实现 `modem_ops`。

### 3.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `core_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | Core 创建参数 |
| `core_handle_t` | 层间 API (opaque) | Facade | 句柄 | Core Service 实例句柄 |
| `core_state_t` | 层间 API | Facade + Core 内部 | 状态枚举 | Core 生命周期状态 |
| `core_net_state_t` | 层间 API | Facade + Core 内部 | 状态枚举 | 网络连接状态 |
| `lwlte_event_id_t` | 用户 API | App + esp_event | 事件枚举 | LWLTE_EVENT 上行事件类型 |
| `lwlte_event_data_t` | 用户 API | App + esp_event | 值对象 | 事件携带数据 |
| `core_protocol_t` | 层间 API | MQTT/TCP Client Service + Core 内部 | 枚举 | Core protocol event 所属协议类型 |
| `core_protocol_data_t` | 层间 API | MQTT/TCP Client Service + Core 内部 | 值对象 | Core 上报给上层 protocol service 的数据事件 |
| `core_protocol_callback_t` | 层间 API | MQTT/TCP Client Service | 回调接口 | 私有协议数据回调（Modem → Core → 上层 service，同步） |
| `core_cmd_type_t` | 层间 API | MQTT/TCP Client Service + Ping Service + Core 内部 | 命令枚举 | 上层 service 投递给 Core 的协议命令类型 |
| `core_cmd_result_t` | 层间 API | MQTT/TCP Client Service + Ping Service + Core 内部 | 结果枚举 | Core command 执行结果 |
| `core_ping_reply_t` | 层间 API | Ping Service + Core 内部 | 值对象 | `CORE_CMD_PING` 的单包结果，Core command callback 前写入 |
| `core_ping_summary_t` | 层间 API | Ping Service + Core 内部 | 值对象 | `CORE_CMD_PING` 的汇总结果，Core command callback 前写入 |
| `core_cmd_t` | 层间 API | MQTT/TCP Client Service + Ping Service + Core 内部 | 值对象 | 上层 service 投递到 Core 的 typed command request |
| `core_cmd_done_callback_t` | 层间 API | MQTT/TCP Client Service + Ping Service + Core 内部 | 回调接口 | Core command 完成后回调上层 service，用于投递上层 FSM 信号 |
| `core_fsm_sig_type_t` | 模块私有 API | Core FSM | 信号枚举 | FSM 内部信号类型 |
| `core_fsm_sig_t` | 模块私有 API | Core FSM | 值对象 | FSM 队列中的信号 |
| `core_fsm_t` | 模块私有 API | Core | 组合成员 | FSM 线程 + 队列管理 |
| `net_mgr_step_t` | 模块私有 API | Net Mgr | 状态枚举 | 网络激活子步骤 |
| `net_mgr_t` | 模块私有 API | Core | 组合成员 | 网络激活状态机 + 重连定时器 |
| `pdp_mgr_t` | 模块私有 API | Core | 组合成员 | PDP context 缓存管理 |

### 3.2 `core_config_t` — Core 配置

**所属层**：Core Service
**可见性**：层间 API — Facade 模块 factory 创建 Core 时填充并传入
**OOP 角色**：配置结构体

```c
typedef struct {
    esp_event_loop_handle_t loop;        // 共享事件总线，NULL 使用默认 loop
} core_event_config_t;

typedef struct {
    const char *apn;                     // APN，如 "cmnet"
    uint8_t primary_cid;                 // 主 PDP context ID，默认 1
    uint32_t net_activate_timeout_ms;    // 网络激活总超时（毫秒），默认 120000
    uint32_t reconnect_delay_ms;         // 掉线重连固定延迟（毫秒），默认 5000
} core_network_config_t;

typedef struct {
    int queue_size;                      // FSM 信号队列长度
    int task_stack;                      // FSM 任务栈大小（字节）
    int task_priority;                   // FSM 任务优先级
} core_fsm_config_t;

typedef struct {
    core_event_config_t event;           // 事件总线配置
    core_network_config_t network;       // 网络策略配置
    core_fsm_config_t fsm;               // FSM 资源配置
} core_config_t;
```

Modem 引用在 `core_create()` 参数中单独传入。Facade factory 通过 `config.event.loop` 注入共享事件总线，并通过 `config.network` 与 `config.fsm` 注入 APN、PDP 和 FSM 参数；用户通过 `lwlte_start()` 显式提交启动请求。

### 3.3 `core_handle_t` — Core 句柄

**所属层**：Core Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/core/core_priv.h`
**OOP 角色**：service 对象，组合持有 Core 内部组件

**层间方法**（`src/core/core.h`）：

```c
core_handle_t core_create(const core_config_t *config, modem_handle_t modem);
esp_err_t core_destroy(core_handle_t me);
esp_err_t core_start(core_handle_t me);
esp_err_t core_stop(core_handle_t me);

esp_err_t core_register_protocol_callback(core_handle_t me,
                                          core_protocol_t protocol,
                                          core_protocol_callback_t callback,
                                          void *user_ctx);
esp_err_t core_register_protocol_closed_callback(core_handle_t me,
                                                  core_protocol_t protocol,
                                                  core_protocol_closed_callback_t callback,
                                                  void *user_ctx);

esp_err_t core_get_state(core_handle_t me, core_state_t *state);
esp_err_t core_get_net_state(core_handle_t me, core_net_state_t *state);

esp_err_t core_connect(core_handle_t me);
esp_err_t core_submit_cmd(core_handle_t me, const core_cmd_t *cmd);
```

`core_start()` 是 Facade `lwlte_start()` 的内部入口，成功投递 `CORE_SIG_START` 时在同一锁保护下把 Core 标记为 `CORE_STATE_STARTING` 后返回。Core FSM 消费 `CORE_SIG_START` 后调用阻塞式 `modem_start()`；`modem_start()` 完成硬复位、`AT OK` 和基础 AT 初始化后返回 `ESP_OK`，Core 随后执行 SIM、注册、附着、APN、PDP 激活和 IP 查询流程；最终网络 online 通过 `LWLTE_EVENT_NET_ONLINE` 投递到共享事件总线。

`core_stop()` 是 Facade `lwlte_stop()` 的内部入口，只投递 `CORE_SIG_STOP` 后返回。Core FSM 先去激活网络，再通过 `modem_stop()` 停止模块；配置 EN GPIO 时拉低 EN 断电，`en_pin == GPIO_NUM_NC` 时只进入逻辑 `MODEM_STATE_OFF`，模块可能仍上电。后续 `lwlte_start()` 可重新启动联网。

`core_connect()` 保留为 Core 内部 command helper，不是用户 API；App 不直接调用，也不通过 Facade 暴露为用户连接函数。

**关键内部字段类别**（非完整代码快照，实际以 `src/core/core_priv.h` 为准）：

- `config`：Core 层间配置快照，保存 event/network/fsm 分组参数。
- `modem`：Facade factory 注入的 `modem_handle_t` 句柄，Core 借用但不拥有生命周期。
- `protocol_callback`、`protocol_closed_callback`：按 `core_protocol_t` 保存的私有同步协议数据回调槽，由 MQTT/TCP service 注册。
- `fsm`、`net_mgr`、`pdp_mgr`：`core_handle_t` 的组合成员，分别负责信号串行化、网络激活/重连、PDP 缓存。
- `state`、`destroying`、`lock`：Core 生命周期状态和并发保护。

**关键设计决策**：
- Core 没有 ops 多态；它不面向多种实现，只有一个实现。
- `modem` 句柄由 Facade 模块 factory 传入，Core 不拥有 Modem 生命周期。
- 事件总线通过 `config.event.loop` 借用（NULL = default loop），Core 不创建自己的 loop；事件直接 post 到共享总线的 `LWLTE_EVENT` base。
- `lock` 只保护 `state`/`destroying` 等短字段访问，FSM 线程调用 `modem_*` API 时不持锁。
- Facade 调用的 `start`、`stop` 不执行阻塞 Modem/网络流程；`core_start()` 仅在成功投递 START 时同步标记 `STARTING`，`core_stop()` 仅在成功投递 STOP 时同步标记 `stop_pending`。

### 3.4 Core 状态和事件类型

**所属层**：Core Service
**可见性**：层间 API
**OOP 角色**：状态枚举 + 事件枚举 + 值对象 + 回调接口

```c
typedef enum {
    CORE_STATE_STOPPED = 0,
    CORE_STATE_STARTING,
    CORE_STATE_READY,
    CORE_STATE_NET_ACTIVATING,
    CORE_STATE_ONLINE,
    CORE_STATE_ERROR,
    CORE_STATE_DESTROYING,
} core_state_t;

typedef enum {
    CORE_NET_STATE_OFFLINE = 0,
    CORE_NET_STATE_ACTIVATING,
    CORE_NET_STATE_ONLINE,
    CORE_NET_STATE_ERROR,
} core_net_state_t;

ESP_EVENT_DECLARE_BASE(LWLTE_EVENT);

typedef enum {
    LWLTE_EVENT_STARTED = 0,
    LWLTE_EVENT_READY,
    LWLTE_EVENT_NET_CONNECTING,
    LWLTE_EVENT_NET_ONLINE,
    LWLTE_EVENT_NET_OFFLINE,
    LWLTE_EVENT_NET_ERROR,
    LWLTE_EVENT_STOPPED,
    LWLTE_EVENT_ERROR,
} lwlte_event_id_t;

typedef enum {
    CORE_PROTOCOL_MQTT = 0,
    CORE_PROTOCOL_TCP,
    CORE_PROTOCOL_MAX,
} core_protocol_t;

typedef struct {
    core_protocol_t protocol;
    uint8_t conn_id;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
    int reason;
    int modem_error_code;
} core_protocol_data_t;

typedef struct {
    core_net_state_t net_state;
    int              error_code;
} lwlte_event_data_t;

typedef void (*core_protocol_callback_t)(core_handle_t core,
                                         const core_protocol_data_t *data,
                                         void *user_ctx);
typedef void (*core_protocol_closed_callback_t)(core_handle_t core,
                                                  core_protocol_t protocol,
                                                  const core_protocol_data_t *data,
                                                  void *user_ctx);
```

**边界说明**：`core_state_t` 表示 Core 自身生命周期阶段，`core_net_state_t` 表示纯网络状态。Core 通过 `core_post_event()` 把状态变化直接投递到共享事件总线 `LWLTE_EVENT`，应用层通过 `esp_event_handler_register()` 订阅。Facade 内部注册 `facade_ready_handler` 驱动 `lwlte_wait_ready()` 同步。

`core_protocol_data_t` 中的 `topic` 和 `payload` 指针只在 `core_protocol_callback_t` 执行期间有效；MQTT Client Service 和 TCP Client Service 若要把数据投递到自己的 FSM 队列，必须先复制这些数据。TCP client v1 使用 `CORE_PROTOCOL_TCP`、`conn_id=0`、`payload/payload_len` 和 `reason/modem_error_code`，不使用 MQTT topic 字段。协议数据通过私有同步回调（`core_register_protocol_callback`）传递，不经过事件总线。

### 3.5 Core command queue 类型

**所属层**：Core Service
**可见性**：层间 API — `src/core/core.h`，供 MQTT Client Service、TCP Client Service 和 Ping Service 使用
**OOP 角色**：命令枚举 + 结果枚举 + 值对象 + 回调接口

Core command queue 是本设计中上层 service 使用的 typed command 入口。MQTT Client Service 通过 `core_submit_cmd()` 投递 MQTT 模块命令；TCP Client Service 通过 `core_submit_cmd()` 投递 socket 命令；Ping Service 通过 `core_submit_cmd()` 投递轻量 `CORE_CMD_PING` 诊断命令。Core FSM 串行执行这些 command，执行时调用 `modem_*` API，并在命令完成后通过 callback 把结果交还给上层 service。MQTT、TCP 和 Ping 都不直接调用 Modem 或 AT Engine。HTTP 后续可以复用这个 command 边界，但本节不承诺其具体跨层边界。

```c
typedef enum {
    CORE_CMD_MQTT_CONFIGURE = 0,
    CORE_CMD_MQTT_TCP_CONNECT,
    CORE_CMD_MQTT_CONNECT,
    CORE_CMD_MQTT_DISCONNECT,
    CORE_CMD_MQTT_TCP_DISCONNECT,
    CORE_CMD_MQTT_SUBSCRIBE,
    CORE_CMD_MQTT_UNSUBSCRIBE,
    CORE_CMD_MQTT_PUBLISH,
    CORE_CMD_PING,
    CORE_CMD_SOCKET_OPEN,
    CORE_CMD_SOCKET_SEND,
    CORE_CMD_SOCKET_RECV,
    CORE_CMD_SOCKET_CLOSE,
} core_cmd_type_t;

typedef enum {
    CORE_CMD_RESULT_OK = 0,
    CORE_CMD_RESULT_ERROR,
    CORE_CMD_RESULT_TIMEOUT,
    CORE_CMD_RESULT_INVALID_RESPONSE,
} core_cmd_result_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} core_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} core_ping_summary_t;

typedef enum {
    CORE_SOCKET_PROTO_TCP = 0,
} core_socket_proto_t;

typedef struct {
    core_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
} core_socket_open_t;

typedef struct {
    uint8_t conn_id;
    const uint8_t *data;
    size_t len;
    uint32_t timeout_ms;
} core_socket_send_t;

typedef struct {
    uint8_t conn_id;
    size_t max_len;
} core_socket_recv_t;

typedef struct {
    uint8_t conn_id;
    uint8_t *payload;
    size_t payload_len;
    size_t remaining_len;
    int modem_error_code;
} core_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;
    uint32_t timeout_ms;
} core_socket_close_t;

typedef struct {
    esp_err_t error_code;
    int modem_error_code;
} core_socket_result_t;

typedef void (*core_cmd_done_callback_t)(core_handle_t core,
                                         core_cmd_type_t type,
                                         core_cmd_result_t result,
                                         const void *result_data,
                                         void *user_ctx);

typedef struct {
    core_cmd_type_t type;
    core_cmd_done_callback_t done_cb;
    void *user_ctx;
    uint32_t timeout_ms;

    union {
        struct {
            const char *client_id;
            const char *username;
            const char *password;
            const char *host;
            uint16_t port;
            bool clean_session;
            uint16_t keepalive_s;
        } mqtt_config;

        struct {
            const char *topic;
            uint8_t qos;
        } mqtt_subscribe;

        struct {
            const char *topic;
        } mqtt_unsubscribe;

        struct {
            const char *topic;
            const uint8_t *payload;
            size_t payload_len;
            uint8_t qos;
            bool retain;
        } mqtt_publish;

        struct {
            const char *host;
            uint8_t count;
            uint16_t data_len;
            uint16_t timeout_100ms;
            uint8_t ttl;
            core_ping_reply_t *replies;
            size_t max_replies;
            core_ping_summary_t *summary;
        } ping;

        core_socket_open_t socket_open;
        core_socket_send_t socket_send;
        core_socket_recv_t socket_recv;
        core_socket_close_t socket_close;
    } data;
} core_cmd_t;
```

**关键设计决策**：
- `core_submit_cmd()` 复制异步执行所需的字符串和 payload；调用方传入的指针只需在调用期间有效。
- Socket open 的 host 和 socket send 的 TX payload 由 Core 深拷贝后入队；TCP client v1 固定使用 `conn_id=0`。
- `core_submit_cmd()` 深拷贝 `core_cmd_t` 到 Core-owned heap object，然后发送 `CORE_SIG_SERVICE_CMD` 到 `core_fsm_t.queue`。
- `CORE_CMD_PING` 的 `host` 由 `core_submit_cmd()` 深拷贝；`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer，Core 只在 command 执行期间写入，不拥有其生命周期。
- `ping_client_ping()` 会等待 `CORE_CMD_PING` 完成后才返回，所以 `replies` 和 `summary` 在 `done_cb` 返回前有效。
- Core 网络未 online 时，`CORE_CMD_PING` 返回 `CORE_CMD_RESULT_ERROR`，上层 Ping Service 映射为 `ESP_ERR_INVALID_STATE`。
- Core FSM 成功入队后拥有复制出的 command，执行 command 并调用 `done_cb` 后释放它。
- Core FSM 停止或销毁时若丢弃已入队 command，必须以 `CORE_CMD_RESULT_ERROR` 调用 `done_cb` 后释放它。
- 如果 enqueue 失败，`core_submit_cmd()` 在返回 `ESP_FAIL` 前释放复制出的 command。
- Core FSM 是 command 的唯一执行位置，执行 command 时可以调用 `modem_*` API。
- `done_cb` 必须短小非阻塞；MQTT 的 `done_cb` 只投递 `MQTT_SIG_CORE_CMD_DONE` 到 MQTT FSM 队列，不直接修改 MQTT 状态。
- `result_data` 由 Core 拥有，只在 `done_cb` 调用期间有效；上层 service 如需在 callback 返回后继续使用，必须先复制数据再投递自己的 FSM 信号。第一版 MQTT command 可以传 `NULL`，直到后续 command 需要结构化结果数据。
- Core 调用 `done_cb` 时不得持有 `core->lock`。
- `core_cmd_t` 是内部 service 层命令对象，不是 App 用户 API；App 不 include `core.h`，也不直接调用 `core_submit_cmd()`。

### 3.6 `core_fsm_t` — FSM 组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_handle_t` 的组合成员，管理 FSM 线程和信号队列

```c
typedef enum {
    CORE_SIG_MODEM_EVENT = 0,
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
    CORE_SIG_SERVICE_CMD,
    CORE_SIG_NET_STEP_DONE,
    CORE_SIG_NET_STEP_TIMEOUT,
    CORE_SIG_RECONNECT,
} core_fsm_sig_type_t;

typedef struct {
    core_fsm_sig_type_t type;
    modem_event_t       modem_event;
    core_cmd_t         *service_cmd;
    int                 error_code;
} core_fsm_sig_t;

typedef struct {
    TaskHandle_t      task;
    QueueHandle_t     queue;
    SemaphoreHandle_t task_done_sema;
    bool              running;
    bool              stop_requested;
} core_fsm_t;
```

**关键设计决策**：
- FSM 线程串行处理所有信号，根据 `core_state_t` 分发到对应的处理函数。
- `CORE_SIG_MODEM_EVENT` 是 Modem 回调的唯一入口；Modem event task 回调中只投递信号，不直接推 Core 状态。
- `CORE_SIG_RECONNECT` 由 FreeRTOS software timer 回调发送。

### 3.7 `net_mgr_t` — 网络管理组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_handle_t` 的组合成员，管理网络激活子状态机 + 重连定时器

```c
typedef enum {
    NET_STEP_IDLE = 0,
    NET_STEP_CHECK_SIM,
    NET_STEP_CHECK_SIGNAL,
    NET_STEP_WAIT_REGISTRATION,
    NET_STEP_WAIT_PACKET_ATTACH,
    NET_STEP_SET_APN,
    NET_STEP_ACTIVATE_PDP,
    NET_STEP_QUERY_IP,
    NET_STEP_DONE,
    NET_STEP_ERROR,
} net_mgr_step_t;

typedef struct {
    net_mgr_step_t    current_step;
    uint32_t          step_start_time_ms;
    uint32_t          step_timeout_ms;
    int               retry_count;
    int               max_retry;
    TimerHandle_t     reconnect_timer;
    SemaphoreHandle_t reconnect_cb_done_sema;
    int               reconnect_cb_active;
    core_net_state_t  state;
    bool              reconnect_enabled;
} net_mgr_t;
```

**激活步骤与 `modem_*` API 映射**：

| 步骤 | 操作 | Modem API |
|------|------|-----------|
| `NET_STEP_CHECK_SIM` | 查询 SIM 状态；未 ready 时按轮询间隔继续等待，PIN/PUK/缺卡等终止状态直接失败 | `modem_get_sim_status()` |
| `NET_STEP_CHECK_SIGNAL` | 查询信号质量，成功后进入注册等待阶段 | `modem_get_signal()` |
| `NET_STEP_WAIT_REGISTRATION` | 轮询网络注册状态；home/roaming 后继续，denied 为终止失败，其他状态继续等待 | `modem_get_registration()` |
| `NET_STEP_WAIT_PACKET_ATTACH` | 轮询分组域附着状态；附着完成后才进入 APN/PDP 激活阶段 | `modem_get_packet_attach_status()` |
| `NET_STEP_SET_APN` | APN 非空时配置 APN；APN 为空时跳过 | `modem_set_apn()`（仅非空 APN） |
| `NET_STEP_ACTIVATE_PDP` | 激活 PDP；若模块返回前置状态错误，重新查询 SIM/注册/附着并回到对应等待阶段 | `modem_activate_pdp()` |
| `NET_STEP_QUERY_IP` | 查询 PDP context，要求 active 且 IP 非空后才完成上线 | `modem_get_pdp_context()` |
| `NET_STEP_DONE` | 网络上线 | 发布 `LWLTE_EVENT_NET_ONLINE` |

网络激活由 `net_mgr_start_activation()` 在一次 FSM 信号处理中运行 staged polling loop。进入激活时只发布一次 `LWLTE_EVENT_NET_CONNECTING`，随后按 SIM、信号、注册、分组域附着、APN、PDP 激活、IP 查询顺序同步调用 `modem_*` API；前置条件未满足时返回 `ESP_ERR_NOT_FINISHED`，`run_activation_loop()` 按 `NET_MGR_WAIT_POLL_INTERVAL_MS` 等待后继续轮询。只有 PDP active 且获得有效 IP 后才发布 `LWLTE_EVENT_NET_ONLINE`；终止错误、注册拒绝、SIM 致命状态或整体 `net_activate_timeout_ms` 超时才进入 `NET_STEP_ERROR` 并发布 `LWLTE_EVENT_NET_ERROR`。销毁期间激活流程直接中止并返回，不发布网络错误事件。

**重连逻辑**：
- 收到 `MODEM_EVENT_PDP_DEACTIVATED` → `net_state = OFFLINE` → 发布 `LWLTE_EVENT_NET_OFFLINE` → 启动 `reconnect_timer`（固定 `reconnect_delay_ms`）。
- 定时器回调发送 `CORE_SIG_RECONNECT` → FSM 重新触发网络激活流程。

第一版只做固定延迟重连，不做指数退避。保活机制后续版本再加。

### 3.8 `pdp_mgr_t` — PDP 管理组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_handle_t` 的组合成员，缓存 PDP context 状态

```c
#define CORE_MAX_PDP_CONTEXTS 4

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t             primary_cid;
} pdp_mgr_t;
```

第一版仅做 PDP context 缓存，提供 `pdp_mgr_get()` / `pdp_mgr_update()` 两个内部接口。后续加多 PDP 管理时在这里扩展。

### 3.9 Core 线程模型

```
┌─────────────────────────────────────────────────────────────┐
│                     Core Service 线程模型                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Facade 线程 / App task                                     │
│  ┌────────────────┐                                         │
│  │ core_* API     │──→ 参数/状态检查                         │
│  └───────┬────────┘    ──→ 获取 core->lock                  │
│          │          ──→ 投递 SIG 到 fsm.queue               │
│          │          ──→ 释放 lock → 返回                     │
│          │          ★ Facade 调用不阻塞在 modem 操作上       │
│          │                                                  │
│  MQTT FSM task                                              │
│  └─ core_submit_cmd(core, &cmd)                             │
│       └─ 深拷贝 core_cmd_t → CORE_SIG_SERVICE_CMD → fsm.queue│
│                                                             │
│  Ping Service 同步调用                                      │
│  └─ ping_client_ping()                                      │
│       └─ core_submit_cmd(CORE_CMD_PING)                     │
│            └─ 深拷贝 host → CORE_SIG_SERVICE_CMD → fsm.queue │
│            └─ 等待一次性完成信号量                          │
│                                                             │
│  Modem event task                                           │
│  ┌────────────────┐                                         │
│  │ event_callback │──→ 构造 core_fsm_sig_t                  │
│  └───────┬────────┘    ──→ xQueueSend(fsm.queue, &sig, 0)   │
│          │          ★ 只投递，不推状态，不调 Core API        │
│          │                                                  │
│  Core FSM task                                              │
│  ┌────────────────┐                                         │
│  │ fsm_task       │──→ xQueueReceive(fsm.queue)             │
│  └───────┬────────┘    ──→ 根据 core->state 分发信号        │
│          │          ──→ MODEM_EVENT? → 更新状态/推进 net_mgr │
│          │          ──→ SERVICE_CMD? → 执行 Core-owned cmd   │
│          │          ──→ NET_STEP_DONE? → 调 modem_* API     │
│          │              (阻塞等待 AT 命令完成)               │
│          │          ──→ 状态变化 → 构建 event_data          │
│          │          ──→ esp_event_post_to(event_loop, ...)  │
│          │          ★ 不持有 core->lock 跨阻塞调用          │
│          │                                                  │
│  Core event loop task (esp_event 内部)                       │
│  ┌────────────────┐                                         │
│  │ esp_event loop │──→ 分发给 Facade / 内部订阅者            │
│  └────────────────┘                                         │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

Core FSM 处理 `CORE_SIG_SERVICE_CMD` 时执行 Core-owned `core_cmd_t`，按命令调用对应 `modem_*` API，随后调用 `done_cb` 并释放 command。MQTT 的 `done_cb` 只投递 `MQTT_SIG_CORE_CMD_DONE`；Ping 的 `done_cb` 只写入同步等待上下文并释放一次性完成信号量。

**硬约束**：

| 约束 | 说明 |
|------|------|
| Modem 回调不调 Core API | Modem event_task 中只 `xQueueSend` 到 FSM 队列，与 Modem 层"URC handler 不调 Core 回调"约束一致 |
| FSM 线程不持锁跨阻塞 | `core->lock` 只保护短字段，调用 `modem_*` API 时不持锁 |
| esp_event_post_to 不阻塞 | 投递事件不阻塞 FSM 线程 |
| 网络控制 Facade API 不阻塞 | `start`/`stop` 等网络控制 API 只做信号投递和轻量状态标记后返回；`start` 是用户显式 LTE online 动作，同步 `lwlte_ping()` 是用户诊断 API 例外 |

**Modem 事件 → Core 行为映射**：

| Modem Event | Core 行为 |
|-------------|----------|
| `MODEM_EVENT_READY` | 作为事件桥接通知 core_state → READY 并发布 `LWLTE_EVENT_READY`；Core 启动流程不由该事件推进，而是在阻塞式 `modem_start()` 返回 `ESP_OK` 后继续网络激活 |
| `MODEM_EVENT_SIM_CHANGED` | 更新 net_mgr 可用的 SIM 状态 |
| `MODEM_EVENT_REG_CHANGED` | 更新 net_mgr 可用的注册状态 |
| `MODEM_EVENT_PDP_ACTIVATED` | 可更新 PDP 状态并通知运行期观察者；Core online 需要命令确认路径中 PDP active 且获得有效 IP 后才发布 `LWLTE_EVENT_NET_ONLINE` |
| `MODEM_EVENT_PDP_DEACTIVATED` | net_state → OFFLINE，发布 `LWLTE_EVENT_NET_OFFLINE`，启动重连定时器 |
| `MODEM_EVENT_PROTOCOL_DATA` | Core 将协议数据路由到对应 `core_protocol_callback_t`；MQTT/TCP service 在同步回调中复制必要数据并投递自身 FSM 信号 |
| `MODEM_EVENT_PROTOCOL_CLOSED` | Core 将关闭原因路由到对应 `core_protocol_closed_callback_t`；MQTT/TCP service 视为协议连接关闭并进入自身恢复或通知流程 |
| `MODEM_EVENT_ERROR` | 根据 error_code 决定重试或进入 ERROR 状态 |

`MODEM_EVENT_PROTOCOL_DATA` 中的指针只在 Modem callback 执行期间有效；Core 通过私有同步 protocol callback 传递借用指针，上层 service 在入队前必须复制需要保留的数据。协议数据不通过公共 `LWLTE_EVENT` 事件总线发布；用户可见的数据事件由 MQTT/TCP service 分别发布到 `LWLTE_MQTT_EVENT` / `LWLTE_TCP_EVENT`。

### 3.10 初始化与装配

```c
/* Facade 模块 factory — Core 不依赖具体模块型号 */

core_config_t core_cfg = {
    .event = {
        .loop = event_loop,
    },
    .network = {
        .apn                     = "cmnet",
        .primary_cid             = 1,
        .net_activate_timeout_ms = 120000,
        .reconnect_delay_ms      = 5000,
    },
    .fsm = {
        .queue_size              = 16,
        .task_stack              = 4096,
        .task_priority           = 8,
    },
};

core_handle_t core = core_create(&core_cfg, modem);
esp_event_handler_register(LWLTE_EVENT, LWLTE_EVENT_READY, facade_ready_handler, lte);
```

Facade 模块 factory 到这里结束，只完成装配和事件桥接，不启动模块。用户调用 `lwlte_start()` 后，Facade 通用 API 再把启动请求交给 Core：

```c
esp_err_t lwlte_start(lwlte_handle_t me)
{
    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = core_start(core);
    end_api_call(me);
    return ret;
}
```

**关键设计决策**：
- `core_create()` 接收 `modem_handle_t`，和 `modem_air780ep_create()` 接收 `at_engine_handle_t` 的模式一致。
- Core 不 include 具体模块头文件，只认识 `modem_handle_t`。
- 换模块时 Core 代码零改动。

**错误处理规则**：
- Core 层间 API 统一返回 `esp_err_t` 或 NULL 句柄。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 状态错误返回 `ESP_ERR_INVALID_STATE`。
- Core 不新增自定义错误码，统一使用 ESP-IDF 标准错误码。
- Modem 层返回的错误直接向上传播。

**与 Modem 层的边界**：
- Core 可以调用 `modem_*` 包装 API，因为 Modem 是紧邻下层。
- Core 不能调用 AT Engine API。
- Core 不 include `modem_air780ep.h`。
- Core 通过 `modem_register_event_callback()` 接收 Modem 上行事件，内部回调只投递 FSM 信号。

---

## 4. MQTT Client Service（MQTT 客户端服务层）

MQTT Client Service 是 Core 之上的独立 service，负责 MQTT 连接、订阅、取消订阅、发布和下行数据事件。它拥有自己的 FSM task、FSM queue 和 MQTT 事件；它依赖 Core 的网络状态、Core event loop 和 Core command queue；它不直接调用 Modem Adapter 或 AT Engine。

MQTT 可以直接使用 ESP-IDF / FreeRTOS API，例如 `xTaskCreate()`、`xQueueCreate()`、`xQueueSend()`、software timer 和 `esp_event`。这不改变层间调用规则：MQTT 运行期只能调用 Core 层间 API，不能 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。

### 4.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `mqtt_client_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | endpoint/auth/session/fsm/event 分组配置 |
| `mqtt_client_handle_t` | 层间 API (opaque) | Facade | service 句柄 | MQTT Client Service 实例 |
| `mqtt_client_transport_t` | 层间 API | Facade + MQTT 内部 | 枚举 | MQTT 传输类型，第一版只支持 Plain TCP |
| `mqtt_client_state_t` | 层间 API | Facade + MQTT 内部 | 状态枚举 | MQTT 生命周期和连接状态 |
| `lwlte_mqtt_event_id_t` | 用户 API | App + esp_event | 事件枚举 | LWLTE_MQTT_EVENT 上行事件类型 |
| `lwlte_mqtt_event_data_t` | 用户 API | App | 值对象 | MQTT 事件数据 |
| `lwlte_mqtt_msg_t` | 用户 API | App | 值对象 | 收到的 MQTT 消息 |
| `mqtt_client_publish_t` | 层间 API | Facade | 值对象 | 发布请求 |
| `mqtt_client_operation_t` | 层间 API | Facade + MQTT 内部 | 枚举 | MQTT 操作类型，用于操作完成事件 |
| `mqtt_fsm_sig_type_t` | 模块私有 API | MQTT FSM | 信号枚举 | MQTT FSM 内部信号类型 |
| `mqtt_fsm_sig_t` | 模块私有 API | MQTT FSM | 值对象 | MQTT FSM 队列中的信号 |
| `mqtt_connect_step_t` | 模块私有 API | MQTT FSM | 状态枚举 | MQTT 连接子状态机步骤 |
| `mqtt_pending_cmd_t` | 模块私有 API | MQTT FSM | 工作上下文 | 正在等待 Core command 结果的命令上下文 |

### 4.2 `mqtt_client_config_t` — MQTT 配置

**所属层**：MQTT Client Service
**可见性**：层间 API — Facade 模块 factory 创建 MQTT service 时填充并传入
**OOP 角色**：配置结构体

```c
typedef enum {
    MQTT_CLIENT_TRANSPORT_PLAIN_TCP = 0,
    MQTT_CLIENT_TRANSPORT_TLS,
} mqtt_client_transport_t;

typedef struct {
    mqtt_client_transport_t transport;   // 传输类型；第一版只支持 PLAIN_TCP
    const char *host;                    // Broker 主机名
    uint16_t port;                       // Broker 端口
} mqtt_client_endpoint_config_t;

typedef struct {
    const char *client_id;               // MQTT client id
    const char *username;                // 用户名，可为 NULL
    const char *password;                // 密码，可为 NULL
} mqtt_client_auth_config_t;

typedef struct {
    uint16_t keepalive_s;                // keepalive 秒数，0 使用默认值
    bool clean_session;                  // clean session 标志
} mqtt_client_session_config_t;

typedef struct {
    int queue_size;                      // MQTT FSM 队列长度
    int task_stack;                      // MQTT FSM task 栈大小
    int task_priority;                   // MQTT FSM task 优先级
} mqtt_client_fsm_config_t;

typedef struct {
    esp_event_loop_handle_t loop;        // 共享事件总线，NULL 使用默认 loop
} mqtt_client_event_config_t;

typedef struct {
    mqtt_client_endpoint_config_t endpoint;
    mqtt_client_auth_config_t auth;
    mqtt_client_session_config_t session;
    mqtt_client_fsm_config_t fsm;
    mqtt_client_event_config_t event;
} mqtt_client_config_t;
```

`mqtt_client_create()` 会复制需要长期保存的字符串。`MQTT_CLIENT_TRANSPORT_TLS` 作为类型预留，第一版返回 `ESP_ERR_NOT_SUPPORTED`。

Facade 暴露独立的 `lwlte_mqtt_config_t` 与 `lwlte_mqtt_init()` / `lwlte_mqtt_destroy()`，把 MQTT 客户端对象的生命周期从 LTE 门面配置中解耦。`lwlte_*_config_t` 只承载 Core/Modem/AT 相关字段：

```c
typedef struct {
    const char *host;              // 必填 Broker 主机名
    uint16_t port;                 // 必填 Broker 端口
    const char *client_id;         // 必填 MQTT client id
    const char *username;          // 可选用户名，可为 NULL
    const char *password;          // 可选密码，可为 NULL
    uint16_t keepalive_s;          // keepalive 秒数，0 使用默认值
    bool clean_session;            // clean session 标志
    int fsm_queue_size;            // MQTT FSM 队列长度，0 使用默认值
    int fsm_task_stack;            // MQTT FSM task 栈大小，0 使用默认值
    int fsm_task_priority;         // MQTT FSM task 优先级，0 使用默认值
} lwlte_mqtt_config_t;
```

MQTT 生命周期分为两层：`lwlte_mqtt_init()` / `lwlte_mqtt_destroy()` 管理对象创建与销毁（init↔destroy），`lwlte_mqtt_start()` / `lwlte_mqtt_stop()` 管理连接 FSM（start↔stop）。`lwlte_mqtt_init()` 在 `lwlte_*_init()` 返回句柄之后、`lwlte_destroy()` 之前任意时刻都可调用，与 `lwlte_start()` 无先后要求；MQTT 事件通过共享事件总线 `LWLTE_MQTT_EVENT` 投递，应用层通过 `esp_event_handler_register()` 注册处理函数。`lwlte_mqtt_destroy()` 从任何 FSM 状态安全调用（下层自动 stop）。若应用层未手动调用，`lwlte_destroy()` 兜底清理。`host`、`port`、`client_id` 必填；`username/password` 可为 `NULL`，映射到内部 `mqtt_client_config_t.auth` 时按 NULL 可选字段保存。

### 4.3 `mqtt_client_handle_t` — MQTT 客户端句柄

**所属层**：MQTT Client Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/mqtt_client/mqtt_client_priv.h` 或 `.c` 中
**OOP 角色**：service 对象，组合持有 MQTT FSM、事件和 Core 依赖

**层间方法**（`src/mqtt_client/mqtt_client.h`）：

```c
mqtt_client_handle_t mqtt_client_create(const mqtt_client_config_t *config,
                                  core_handle_t core);
esp_err_t mqtt_client_destroy(mqtt_client_handle_t me);
esp_err_t mqtt_client_start(mqtt_client_handle_t me);
esp_err_t mqtt_client_stop(mqtt_client_handle_t me);

esp_err_t mqtt_client_get_state(mqtt_client_handle_t me,
                                mqtt_client_state_t *state);
esp_err_t mqtt_client_subscribe(mqtt_client_handle_t me,
                                const char *topic,
                                uint8_t qos);
esp_err_t mqtt_client_unsubscribe(mqtt_client_handle_t me,
                                  const char *topic);
esp_err_t mqtt_client_publish(mqtt_client_handle_t me,
                              const mqtt_client_publish_t *request);
```

**关键内部字段类别**：
- `config`：配置快照，包含复制后的 `endpoint.host`、`auth.client_id`、`auth.username`、`auth.password`，以及借用的 `event.loop`（NULL = default loop）。
- `core`：Facade factory 注入的 `core_handle_t` 句柄，MQTT 借用但不拥有生命周期。
- `fsm_task`、`fsm_queue`、`fsm_task_done_sema`：MQTT 独立状态机线程和信号队列。
- `state`、`connect_step`、`pending_cmd`：MQTT 生命周期、连接子步骤和等待中的 Core command。
- `lock`、`destroying`、`started`、`net_online`：短状态字段和销毁保护。

**关键设计决策**：
- MQTT 没有 ops 多态；第一版只有一个 MQTT service 实现。
- MQTT 不是 Core 的子类，不能向上转型为 `core_handle_t`。
- MQTT 不保存 `modem_handle_t`、`at_engine_handle_t` 或具体模块句柄。
- `lock` 只保护短字段，MQTT FSM 调用 `core_submit_cmd()` 时不持锁。
- Facade 通过 public `lwlte_mqtt_*` API 包装本层 `mqtt_client_*` 方法，App 不直接 include `mqtt_client.h`。

### 4.4 MQTT 状态、事件和消息类型

**所属层**：MQTT Client Service
**可见性**：层间 API
**OOP 角色**：状态枚举 + 事件枚举 + 值对象 + 回调接口

```c
typedef enum {
    MQTT_CLIENT_STATE_STOPPED = 0,
    MQTT_CLIENT_STATE_WAITING_NET,
    MQTT_CLIENT_STATE_CONNECTING,
    MQTT_CLIENT_STATE_CONNECTED,
    MQTT_CLIENT_STATE_DISCONNECTING,
    MQTT_CLIENT_STATE_ERROR,
    MQTT_CLIENT_STATE_DESTROYING,
} mqtt_client_state_t;

MQTT Client Service 通过共享事件总线 `LWLTE_MQTT_EVENT` 向应用层发布 MQTT 状态变化和数据事件。事件数据为 `lwlte_mqtt_event_data_t`，其中 `LWLTE_MQTT_EVENT_DATA` 事件的 `msg.topic` 和 `msg.payload` 是堆拥有的缓冲区，handler 必须在返回前调用 `lwlte_mqtt_event_data_release()` 释放。

LWLTE_MQTT_EVENT_DATA payloads carry heap-owned topic/payload that must be released via `lwlte_mqtt_event_data_release` before the event handler returns.

```c
/* Events posted to LWLTE_MQTT_EVENT base */
typedef enum {
    LWLTE_MQTT_EVENT_STARTED = 0,
    LWLTE_MQTT_EVENT_STOPPED,
    LWLTE_MQTT_EVENT_CONNECTING,
    LWLTE_MQTT_EVENT_CONNECTED,
    LWLTE_MQTT_EVENT_DISCONNECTED,
    LWLTE_MQTT_EVENT_SUBSCRIBED,
    LWLTE_MQTT_EVENT_UNSUBSCRIBED,
    LWLTE_MQTT_EVENT_PUBLISHED,
    LWLTE_MQTT_EVENT_DATA,
    LWLTE_MQTT_EVENT_ERROR,
} lwlte_mqtt_event_id_t;

typedef enum {
    MQTT_CLIENT_OPERATION_CONNECT = 0,
    MQTT_CLIENT_OPERATION_DISCONNECT,
    MQTT_CLIENT_OPERATION_SUBSCRIBE,
    MQTT_CLIENT_OPERATION_UNSUBSCRIBE,
    MQTT_CLIENT_OPERATION_PUBLISH,
} mqtt_client_operation_t;

typedef struct {
    const char *topic;
    const uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool retain;
} mqtt_client_publish_t;

typedef struct {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} lwlte_mqtt_msg_t;

typedef struct {
    lwlte_mqtt_state_t mqtt_state;
    int error_code;
    lwlte_mqtt_msg_t msg;
    bool owns_payload;
} lwlte_mqtt_event_data_t;

void lwlte_mqtt_event_data_release(lwlte_mqtt_event_data_t *data);
```

`lwlte_mqtt_event_data_t` 中的 `msg.topic` 和 `msg.payload` 在 `LWLTE_MQTT_EVENT_DATA` 事件中是堆拥有的缓冲区（`owns_payload == true`），handler 必须在返回前调用 `lwlte_mqtt_event_data_release()` 释放。其他事件的 `owns_payload` 为 false，不包含堆缓冲。所有 MQTT 事件通过共享事件总线 `LWLTE_MQTT_EVENT` 投递，应用层通过 `esp_event_handler_register(LWLTE_MQTT_EVENT, ...)` 注册 handler。

### 4.5 `mqtt_fsm_sig_t` — MQTT FSM 信号

**所属层**：MQTT Client Service
**可见性**：模块私有 API — `src/mqtt_client/mqtt_client_priv.h`，只允许 MQTT 源码 include
**OOP 角色**：信号枚举 + 值对象

```c
typedef enum {
    MQTT_SIG_START = 0,
    MQTT_SIG_STOP,
    MQTT_SIG_NET_ONLINE,
    MQTT_SIG_NET_OFFLINE,
    MQTT_SIG_CORE_CMD_DONE,
    MQTT_SIG_SUBSCRIBE,
    MQTT_SIG_UNSUBSCRIBE,
    MQTT_SIG_PUBLISH,
    MQTT_SIG_PROTOCOL_DATA,
    MQTT_SIG_PROTOCOL_CLOSED,
} mqtt_fsm_sig_type_t;

typedef struct {
    mqtt_fsm_sig_type_t type;
    core_cmd_type_t core_cmd_type;
    core_cmd_result_t core_result;
    int error_code;
    void *data;
    size_t data_size;
} mqtt_fsm_sig_t;
```

App/Facade API、Core event handler 和 Core command done callback 都只投递 `mqtt_fsm_sig_t`，不直接推进 MQTT 状态。

### 4.6 `mqtt_pending_cmd_t` — Core 命令等待上下文

**所属层**：MQTT Client Service
**可见性**：模块私有 API
**OOP 角色**：工作上下文

```c
typedef enum {
    MQTT_CONNECT_STEP_IDLE = 0,
    MQTT_CONNECT_STEP_CONFIGURE,
    MQTT_CONNECT_STEP_TCP_CONNECT,
    MQTT_CONNECT_STEP_CONNECT,
    MQTT_CONNECT_STEP_DONE,
    MQTT_CONNECT_STEP_ERROR,
} mqtt_connect_step_t;

typedef struct {
    bool active;
    core_cmd_type_t type;
    mqtt_client_operation_t operation;
    uint32_t started_ms;
    uint32_t timeout_ms;
} mqtt_pending_cmd_t;
```

`mqtt_pending_cmd_t` 只描述 MQTT 当前等待的 Core command，不保存 Core command 内部深拷贝数据。Core command 数据由 Core 拥有并在 command 完成后释放。

### 4.7 Core command queue 边界

MQTT 所有模块命令都通过 `core_submit_cmd()` 投递给 Core。MQTT 不生成 AT 字符串，不调用 `modem_*`，也不注册 AT Engine URC。Core command queue 的作用是把 MQTT Client Service 的业务命令串行化到 Core FSM，再由 Core 调用 Modem Adapter。TCP Client Service 已复用这个命令边界；HTTP 后续可以复用，但具体边界不在本节承诺。

| MQTT 操作 | Core command | Air780EP 第一版底层命令 |
|-----------|--------------|--------------------------|
| 配置 MQTT 参数 | `CORE_CMD_MQTT_CONFIGURE` | `AT+MCONFIG`，Modem 缓存 client、broker 和会话参数 |
| 建立 MQTT TCP 通道 | `CORE_CMD_MQTT_TCP_CONNECT` | `AT+MIPSTART`，使用缓存 host/port，成功接受 `CONNECT OK` / `ALREADY CONNECT` |
| MQTT 协议连接 | `CORE_CMD_MQTT_CONNECT` | `AT+MCONNECT`，使用缓存 clean_session/keepalive_s，成功接受 `CONNACK OK` |
| 断开 MQTT 会话 | `CORE_CMD_MQTT_DISCONNECT` | `AT+MDISCONNECT` |
| 断开 MQTT TCP 通道 | `CORE_CMD_MQTT_TCP_DISCONNECT` | `AT+MIPCLOSE` |
| 订阅 | `CORE_CMD_MQTT_SUBSCRIBE` | `AT+MSUB`，成功 `SUBACK` |
| 取消订阅 | `CORE_CMD_MQTT_UNSUBSCRIBE` | `AT+MUNSUB`，成功 `UNSUBACK` |
| 发布 | `CORE_CMD_MQTT_PUBLISH` | `AT+MPUBEX` + payload prompt |

表中的 Air780EP 命令只是 Core/Modem 第一版实现映射，不属于 MQTT service 的实现细节。

### 4.8 MQTT 连接与操作流程

```text
STOPPED
  └─ mqtt_client_start()
      ├─ Core net offline → WAITING_NET
      └─ Core net online  → CONNECTING
            └─ CONFIGURE → TCP_CONNECT → CONNECT → CONNECTED
```

```text
CONNECTED
  └─ LWLTE_EVENT_NET_OFFLINE
      ├─ 清除 connected 状态
      ├─ 发布 LWLTE_MQTT_EVENT_DISCONNECTED
      └─ WAITING_NET
```

```text
CONNECTED 或 transport 已打开
  └─ mqtt_client_stop()
      ├─ 若 MQTT 会话已建立，提交 CORE_CMD_MQTT_DISCONNECT
      ├─ 若 MQTT TCP 通道已建立，提交 CORE_CMD_MQTT_TCP_DISCONNECT
      ├─ 等待 command 完成或 stop 超时
      ├─ 注销 Core event handler
      ├─ 停止 MQTT FSM task
      └─ 发布 LWLTE_MQTT_EVENT_STOPPED
```

第一版不隐藏缓存 publish/subscribe/unsubscribe 请求。MQTT 未连接时，这些 API 返回 `ESP_ERR_INVALID_STATE`。

### 4.9 MQTT URC / 数据上行路径

Air780EP 第一版使用 `+MSUB:` 作为 MQTT 下行数据 URC。新的依赖方向不允许 MQTT 直接注册 AT Engine URC handler，数据上行路径如下：

```text
AT Engine RX task
  └─ Modem Air780EP URC handler
       └─ 解析 +MSUB: 为 modem_event_t
             └─ Modem event_task 调用 Core 回调
                  └─ Core FSM 调用 protocol_callback (同步)
                       └─ MQTT mqtt_protocol_data_cb 深拷贝 topic/payload
                            └─ xQueueSend(mqtt.fsm_queue)
                                 └─ MQTT FSM 发布 LWLTE_MQTT_EVENT_DATA
```

Core protocol data 通过私有同步回调（`core_register_protocol_callback`）传递，MQTT service 入队前必须复制 topic 和 payload。MQTT 事件通过共享事件总线 `LWLTE_MQTT_EVENT` 发布。

### 4.10 MQTT 线程模型

```text
Facade/App task
  └─ mqtt_client_start/publish/subscribe
       └─ 参数检查 + 请求深拷贝
            └─ xQueueSend(mqtt.fsm_queue)

Core event loop task
  └─ LWLTE_EVENT_NET_ONLINE / OFFLINE
       └─ MQTT core_event_handler
            └─ 投递网络状态信号到 MQTT FSM

Core FSM task
  └─ core_register_protocol_callback(CORE_PROTOCOL_MQTT)
       └─ MQTT mqtt_protocol_data_cb 深拷贝 topic/payload
            └─ xQueueSend(mqtt.fsm_queue)

MQTT FSM task
  └─ 串行处理 MQTT 信号
       ├─ CONNECTING: submit CORE_CMD_MQTT_CONFIGURE/TCP_CONNECT/CONNECT
       ├─ CONNECTED: submit publish/subscribe/unsubscribe
       └─ 状态变化后 post LWLTE_MQTT_EVENT

Core FSM task
  └─ 串行处理 core_cmd_t
       └─ 调 modem_* API
            └─ Air780EP 子类发送 AT 命令
```

**硬约束**：
- MQTT FSM 可以阻塞等待自身队列，但 App task 不阻塞等待 MQTT 连接完成。
- MQTT FSM 调用 `core_submit_cmd()` 时不持有 `mqtt->lock`。
- Core command done callback 不直接修改 MQTT 状态，只投递 `MQTT_SIG_CORE_CMD_DONE`。
- Core event handler 只做校验、深拷贝和入队。
- MQTT destroy 先停止 FSM，再注销 Core event handler，再释放已排队信号和配置副本。
- `mqtt_client_stop()` 在连接已建立或 transport 已打开时按 `CORE_CMD_MQTT_DISCONNECT` → `CORE_CMD_MQTT_TCP_DISCONNECT` 顺序清理，并且必须有超时兜底。

### 4.11 错误处理规则

- MQTT 层间 API 统一返回 `esp_err_t` 或 NULL 句柄。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 生命周期错误、未连接时发布/订阅/取消订阅返回 `ESP_ERR_INVALID_STATE`。
- 队列提交失败返回 `ESP_FAIL`。
- Core command 超时映射为 `ESP_ERR_TIMEOUT`。
- 第一版 TLS 返回 `ESP_ERR_NOT_SUPPORTED`。
- 协议数据或响应格式异常返回 `ESP_ERR_INVALID_RESPONSE`。
- MQTT 保存最近一次错误码，并通过 `LWLTE_MQTT_EVENT_ERROR` 上报。

### 4.12 与 Core / Modem / AT Engine 的边界

- MQTT 可以调用 `core_register_protocol_callback()`、`core_get_net_state()` 和 `core_submit_cmd()`，因为 Core 是 MQTT 的直接依赖。
- MQTT 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。
- MQTT 不直接调用 `modem_*`、`at_engine_*` 或具体 Air780EP helper。
- MQTT 不注册 AT Engine URC handler；MQTT 数据 URC 经 Modem → Core → MQTT 上行。
- Core 仍只负责网络状态机、PDP、重连和命令串行化，不持有 MQTT 业务状态机。
- Facade 是 composition root，负责创建 Core 和 MQTT，并把 MQTT 事件翻译为用户 API 事件。

---

## 5. TCP Client Service（TCP 客户端服务层）

TCP Client Service 是 Core 之上的独立异步 service，负责 plain TCP client 的打开、发送、缓存读取、关闭和事件投递。它与 MQTT 使用同一 service boundary：运行期只调用 Core 层间 API 和 `core_submit_cmd()`，不直接调用 Modem Adapter 或 AT Engine。

`tcp_client_handle_t` owns the TCP FSM task, one `tcp_client_conn_t`, Core protocol callbacks for `CORE_PROTOCOL_TCP`, and event posting to `LWLTE_TCP_EVENT`.

`tcp_client_conn_t` is the internal object behind public `lwlte_tcp_conn_t`. It owns connection state, `conn_id=0`, user context, pending Core command metadata, and a FIFO of copied TX payloads.

TCP event handlers must call `lwlte_tcp_event_data_release()` before returning when `owns_event` or `owns_payload` is true. `LWLTE_TCP_EVENT_DATA` payloads additionally carry heap-owned bytes when `owns_payload` is true.

关键约束：

- TCP client v1 只支持 plain TCP 和一个 client connection；连接 id 固定为 `conn_id=0`。
- TX payload 在进入 TCP FSM 队列前复制，保证调用方 buffer 可在 API 返回后释放或复用。
- RX 数据由 Modem 通过 Core protocol callback 上报到 `tcp_client`，再由 TCP FSM 投递 `LWLTE_TCP_EVENT_DATA`。
- `tcp_client` 不 include `modem.h`、`modem_air780ep.h`、`modem_ml307r.h`、`at_engine.h` 或任意下层 `_priv.h`。

### 5.1 Socket Commands

Core socket commands are `CORE_CMD_SOCKET_OPEN`, `CORE_CMD_SOCKET_SEND`, `CORE_CMD_SOCKET_RECV`, and `CORE_CMD_SOCKET_CLOSE`. Core deep-copies socket hosts and TX payloads before enqueueing commands.

---

## 5. Ping Service（Ping 诊断服务层）

> 编号兼容说明：TCP Client Service 已作为 `## 5. TCP Client Service` 插入在 MQTT 与 Ping 之间；本节保留 `## 5. Ping Service` 锚点以兼容既有静态文档契约。

Ping Service 是 Core 之上的轻量 service，负责把用户同步 `lwlte_ping()` 请求转换成 `CORE_CMD_PING`，并等待 Core command 完成后把详细 ping 结果返回给调用方。它不负责网络状态机、不参与 Core online 判定，也不维护长期连接状态。

Ping Service 不创建自己的 FSM task、FSM queue 或 esp_event loop。它只在一次 `ping_client_ping()` 调用期间创建短生命周期同步对象，提交 Core command，然后阻塞等待结果。Ping Service 运行期只能调用 Core 层间 API，不能 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。

### 5.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `ping_client_handle_t` | 层间 API (opaque) | Facade | service 句柄 | Ping Service 实例，持有 Core 依赖和短生命周期同步逻辑 |
| `ping_client_request_t` | 层间 API | Facade + Ping Service | 值对象 | Ping 请求参数，Facade 从用户 `lwlte_ping_request_t` 映射而来 |
| `ping_wait_ctx_t` | 模块私有 API | Ping Service | 工作上下文 | 单次同步 ping 调用的完成信号量、结果码和 Core command result |

### 5.2 `ping_client_handle_t` — Ping 服务句柄

**所属层**：Ping Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/ping_client/ping_client_priv.h` 或 `.c` 中
**OOP 角色**：轻量 service 对象，借用 Core 依赖

**层间方法**（`src/ping_client/ping_client.h`）：

```c
typedef struct ping_client_t *ping_client_handle_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} ping_client_request_t;

ping_client_handle_t ping_client_create(core_handle_t core);
esp_err_t ping_client_destroy(ping_client_handle_t me);
esp_err_t ping_client_ping(ping_client_handle_t me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary);
```

**关键内部字段类别**：
- `core`：Facade factory 注入的 `core_handle_t` 句柄，Ping Service 借用但不拥有生命周期。
- `lock`、`destroying`：保护短生命周期状态，销毁开始后拒绝新的 ping 调用。

**关键设计决策**：
- Ping Service 没有独立状态机；`ping_client_handle_t` 不保存 connected/error 等长期状态。
- Ping Service 不保存 `modem_handle_t`、`at_engine_handle_t` 或具体模块句柄。
- `ping_client_ping()` 可以阻塞调用 task，直到 Core command 完成或总超时到达。
- `timeout_100ms == 0` 是无效参数；`total_timeout_ms == 0` 表示根据 `count * timeout_100ms * 100` 加命令开销派生默认总等待预算。

### 5.3 `ping_wait_ctx_t` — 同步等待上下文

**所属层**：Ping Service
**可见性**：模块私有 API
**OOP 角色**：单次调用工作上下文

```c
typedef struct {
    SemaphoreHandle_t done_sema;
    core_cmd_result_t core_result;
    esp_err_t esp_result;
    bool completed;
} ping_wait_ctx_t;
```

`ping_wait_ctx_t` 只在 `ping_client_ping()` 栈上或短生命周期堆对象中存在。Core command done callback 只写入该上下文并 `xSemaphoreGive(done_sema)`，不直接调用 Facade 或用户回调。

### 5.4 Core command queue 边界

Ping Service 只通过 `core_submit_cmd()` 投递 ping：

```text
Facade/App task
  └─ lwlte_ping()
       └─ ping_client_ping()
            ├─ 参数检查 + Ping/Core 类型映射
            ├─ 构造 CORE_CMD_PING
            ├─ core_submit_cmd(CORE_CMD_PING)
            ├─ xSemaphoreTake(done_sema, total_timeout)
            └─ Core done callback 写入结果后返回
```

`CORE_CMD_PING` 的 `host` 由 Core 深拷贝；`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer。Core 只在 command 执行期间写入这些 buffer，不释放它们。`ping_client_ping()` 必须等 Core command 完成后才返回，保证输出 buffer 在 `done_cb` 返回前有效。

Facade、Core 和 Modem 各自保留自己的层间值对象。实现时必须逐字段复制，不能把 `lwlte_ping_reply_t *` 强转成 `core_ping_reply_t *`，也不得把 `core_ping_reply_t *` 强转成 `modem_ping_reply_t *`。

### 5.5 Ping 操作流程

```text
lwlte_ping()
  └─ 检查 me/request/replies/max_replies
  └─ 映射 lwlte_ping_request_t -> ping_client_request_t
  └─ ping_client_ping()
       └─ 构造 core_cmd_t { .type = CORE_CMD_PING }
       └─ core_submit_cmd()
            └─ CORE_SIG_SERVICE_CMD 入 Core FSM queue
       └─ 等待 done_sema
            └─ Core FSM 调 modem_ping()
                 └─ Air780EP 发送 AT+CIPPING
                 └─ 解析多行 +CIPPING 结果
            └─ Core done callback 唤醒 Ping Service
  └─ 映射 core_ping_reply_t/core_ping_summary_t -> lwlte_ping_reply_t/lwlte_ping_summary_t
  └─ 返回 esp_err_t
```

`lwlte_ping()` 是同步阻塞 API，不应在时间敏感 callback 中调用。Ping command 与 MQTT command 共享 Core FSM 串行化，不能并发打断其他 AT 命令。

### 5.6 错误处理规则

- `host == NULL`、空字符串、`count == 0`、`count > 100`、`replies == NULL`、`max_replies < count` 返回 `ESP_ERR_INVALID_ARG`。
- `data_len > 1024`、`timeout_100ms` 不在 `1..600`、`ttl == 0` 返回 `ESP_ERR_INVALID_ARG`。
- Core 网络未 online 时返回 `ESP_ERR_INVALID_STATE`。
- `core_submit_cmd()` 入队失败返回 `ESP_FAIL`。
- 等待超过有效总超时返回 `ESP_ERR_TIMEOUT`；有效总超时是调用方非零 `total_timeout_ms` 或 `total_timeout_ms == 0` 时派生出的默认值。
- AT command 超时返回 `ESP_ERR_TIMEOUT`。
- AT 返回 `ERROR`、`+CME ERROR` 或 `+CMS ERROR` 时沿用 Modem 层标准错误映射。
- `+CIPPING:` 响应格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。
- 部分丢包不是 API 错误；丢包通过 `replies[].success == false` 表达，`summary` 非 NULL 时同时通过 `summary.lost` 表达。

### 5.7 与 Core / Modem / AT Engine 的边界

- Ping Service 可以调用 `core_submit_cmd()` 和必要的 Core 状态查询 API，因为 Core 是 Ping 的直接依赖。
- Ping Service 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。
- Ping Service 不直接调用 `modem_ping()`、`at_engine_*` 或具体 Air780EP helper。
- Ping Service 不注册 AT Engine URC handler；`AT+CIPPING` 是命令响应，不是 Ping Service URC 数据路径。
- Core 仍只负责网络状态机、PDP、重连和命令串行化，不持有 Ping 业务状态机。
- Facade 是 composition root，负责创建 Ping Service，并把用户 `lwlte_ping()` 参数映射到内部 Ping Service。

### 5.8 后续异步预留

第一版只实现同步阻塞 `lwlte_ping()`。后续可以增加：

```c
esp_err_t lwlte_ping_async(lwlte_handle_t me,
                           const lwlte_ping_request_t *request,
                           void *user_ctx);
```

异步版本仍复用 `CORE_CMD_PING`，但需要 Ping Service 持有 heap-owned request/result，并定义取消和 destroy 语义。第一版不增加 `LWLTE_EVENT_PING_DONE` 或异步事件 ID。

---

## 6. App（应用层）

> App 层不定义内部框架类，只有用户自己的业务类型。LWLTE Facade 暴露的用户 API 类型位于 `src/include/lwlte.h`，用于把 App 请求映射到内部 service。

基础生命周期用户 API：

```c
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_handle_t *out_lte);
esp_err_t lwlte_start(lwlte_handle_t me);
esp_err_t lwlte_stop(lwlte_handle_t me);
esp_err_t lwlte_destroy(lwlte_handle_t me);
```

`lwlte_air780ep_init()` 只创建和装配 Facade、AT Engine、Modem、Core、Ping service；`lwlte_start()` 才是用户显式启动入口，异步提交启动请求，最终 online 结果通过 `LWLTE_EVENT_NET_ONLINE` 上报。`lwlte_stop()` 是对称停机入口，异步提交停止请求，停止 MQTT、去激活网络；配置 EN GPIO 时拉低 EN 断电，`en_pin == GPIO_NUM_NC` 时降级为逻辑 `MODEM_STATE_OFF`，模块可能仍上电。后续 `lwlte_start()` 可重新启动联网。

MQTT 第一版会增加这些用户可见类型和函数：

- `lwlte_mqtt_config_t`：独立 MQTT 客户端配置（Broker/client_id/认证/keepalive/FSM 参数），不再嵌套在 `lwlte_*_config_t` 中。
- `lwlte_mqtt_init()` / `lwlte_mqtt_destroy()`：MQTT 客户端对象生命周期（init↔destroy）；`lwlte_destroy()` 兜底清理未手动 destroy 的 MQTT 客户端。
- `lwlte_mqtt_state_t`：用户可查询的 MQTT 状态，由 Facade 从内部 `mqtt_client_state_t` 映射而来。
- `lwlte_mqtt_msg_t`：用户 MQTT 数据事件的值对象，`topic/payload` 指针只在用户事件回调期间有效。
- `lwlte_mqtt_start()`、`lwlte_mqtt_stop()`、`lwlte_mqtt_get_state()`、`lwlte_mqtt_subscribe()`、`lwlte_mqtt_unsubscribe()`、`lwlte_mqtt_publish()`：Facade 用户 API，内部只调用 `mqtt_client_*`，不直接操作 Core command 或 Modem。

Ping 第一版会增加这些用户可见类型和函数：

- `lwlte_ping_request_t`：用户传入的 ping 请求，包含 host、count、data_len、timeout_100ms、ttl 和 total_timeout_ms。
- `lwlte_ping_reply_t`：单包 ping 结果，包含 seq、ip、time_ms、ttl 和 success。
- `lwlte_ping_summary_t`：ping 汇总结果，包含 sent、received、lost、min_time_ms、max_time_ms 和 avg_time_ms。
- `lwlte_ping()`：同步阻塞 Facade 用户 API，内部只调用 `ping_client_ping()`，不直接操作 Core command 或 Modem。

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} lwlte_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} lwlte_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} lwlte_ping_summary_t;

esp_err_t lwlte_ping(lwlte_handle_t me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);
```

调用方负责传入 `lwlte_ping_reply_t` 数组，`max_replies` 必须大于等于 `request->count`；组件不分配也不释放该数组。`summary` 可以为 `NULL`。第一版只实现同步阻塞 `lwlte_ping()`；`lwlte_ping_async()` 只作为后续设计方向，不进入第一版用户 API。

TCP client v1 增加这些用户可见类型、事件和函数：

- `lwlte_tcp_config_t`：TCP client service 配置，v1 只支持 `max_conns` 为 0 或 1，并提供 TX/RX 事件长度、超时和 FSM 资源配置。
- `lwlte_tcp_open_config_t`：单次 TCP open 参数，包含 host、port 和事件中原样返回的 `user_ctx`。
- `lwlte_tcp_conn_t`：opaque TCP connection 句柄，公开 API 不暴露内部 `conn_id`。
- `lwlte_tcp_event_data_t`：`LWLTE_TCP_EVENT` 数据；事件数据带 `owns_event` 或 `owns_payload` 时 handler 必须调用 `lwlte_tcp_event_data_release()`，其中 `LWLTE_TCP_EVENT_DATA` 额外携带 heap-owned payload。
- `LWLTE_TCP_EVENT`：TCP client public event base，包含 STARTED、STOPPED、CONNECTED、DISCONNECTED、SENT、DATA 和 ERROR。
- `lwlte_tcp_init()` / `lwlte_tcp_destroy()`：TCP client service 生命周期。
- `lwlte_tcp_open()` / `lwlte_tcp_send()` / `lwlte_tcp_close()` / `lwlte_tcp_conn_get_state()` / `lwlte_tcp_conn_destroy()`：Facade 用户 API，内部只调用 `tcp_client_*`，不直接操作 Core command 或 Modem。

App 仍不 include `src/mqtt_client/mqtt_client.h`、`src/ping_client/ping_client.h`、`src/core/core.h`、`src/modem/modem.h` 或任何 `_priv.h`。
