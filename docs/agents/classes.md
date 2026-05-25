# 类定义

在编码前先确定每个模块有哪些类（struct + 配套函数），是理解项目架构最重要的文档。每个类的定义包含：**所属层、职责、可见性、关键字段、关键方法、OOP 角色**。

## 可见性定义

| 可见性 | 落入哪个头文件 | 谁能看到 | 命名前缀 |
|--------|-------------|---------|---------|
| 用户 API | `src/include/lwlte*.h` | App 开发者 | `lwlte_` |
| 层间 API | `src/core/core.h`、`src/modem/modem.h`、`src/modem/modem_air780ep.h`、`src/at_engine/at_engine.h` | 组件内部相邻层；Facade factory 作为 composition root 可见全部装配 API | `core_`、`modem_`、`modem_air780ep_`、`at_engine_` |
| 模块私有 API | `*_priv.h` | 当前模块自己的 `.c` 文件 | 模块内部命名 |
| 文件内部 | `.c` 中 static | 当前 `.c` 文件 | 无限制 |

**核心区别**：用户 API 是给 App 开发者用的，层间 API 是层与层之间、以及 Facade 模块 factory 装配时用的。AT Engine、Modem 和 Core 都没有任何用户 API——它们被 LWLTE Facade 封装，最终用户看不到它们的存在。

`*_priv.h` 虽然通过 `PRIV_INCLUDE_DIRS` 在编译上可见，但约束上只允许同模块源码 include。Core 不 include `modem_priv.h`，Modem 不 include `core_priv.h`，Facade 不 include 任意 `_priv.h`。

---

## 1. AT Engine（AT 引擎层）

AT Engine 是内部最底层，负责 AT 协议解析和 UART 硬件操作。该层有以下类：

### 1.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `at_engine_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | UART 硬件参数 + 任务参数 |
| `at_engine_t` | 层间 API (opaque) | Modem 层 + Facade 模块 factory | 句柄 | AT Engine 实例句柄 |
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
    /* UART 硬件参数 */
    uart_port_t uart_num;         // UART 端口号（如 UART_NUM_1）
    int         tx_pin;           // TX GPIO
    int         rx_pin;           // RX GPIO
    int         baud_rate;        // 波特率（如 115200）
    int         rx_buf_size;      // UART RX 环形缓冲区大小（字节）

    /* 接收任务参数 */
    int         rx_task_stack;    // 接收任务栈大小（字节）
    int         rx_task_priority; // 接收任务优先级
    int         rx_line_buf_size; // 单行最大长度（字节）

    /* 命令响应参数 */
    int         cmd_default_timeout_ms;  // 默认命令超时（毫秒）
    int         max_response_lines;      // 单次响应最大行数
} at_engine_config_t;
```

### 1.3 `at_engine_t` — 引擎句柄

**所属层**：AT Engine  
**可见性**：层间 API (opaque) — Facade 模块 factory 创建，Modem 层持有句柄；struct 定义在 `.c` 中
**OOP 角色**：顶层对象，持有该层所有资源

**层间方法**：

```c
at_engine_t *at_engine_create(const at_engine_config_t *config);
esp_err_t    at_engine_destroy(at_engine_t *me);

/* 发送普通 AT 命令（阻塞调用，直到 OK/ERROR/CME/CMS 或超时） */
esp_err_t    at_engine_send_cmd(at_engine_t *me, const char *cmd,
                                at_response_t *response, uint32_t timeout_ms);

/* 使用单次命令选项发送 AT 命令，支持自定义成功终止响应 */
esp_err_t    at_engine_send_cmd_with_options(at_engine_t *me, const char *cmd,
                                             at_response_t *response,
                                             const at_cmd_options_t *options);

/* URC 回调注册 / 注销 */
esp_err_t    at_engine_register_urc(at_engine_t *me, const char *prefix,
                                     at_urc_handler_t *handler);
esp_err_t    at_engine_unregister_urc(at_engine_t *me, const char *prefix);
```

**内部结构**（定义在 `.c` 或 `_priv.h`）：

```c
struct at_engine {
    at_engine_config_t   config;              // 配置快照
    uart_port_t          uart_num;            // UART 端口号
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
    char                *response_pool;       // 响应文本池
    int                  response_pool_lines;
    int                  response_line_size;
    bool                 uart_driver_installed;
    volatile bool        rx_task_stop_requested;
};
```

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
- `lines` 数组由**调用方分配**，AT Engine 只填入指向实例内 `response_pool` 的字符串指针
- 调用方不得释放或修改 `lines[i]` 指向的字符串；数据在同一 AT Engine 实例下次 `send_cmd` 前有效
- 实际保存行数按 `min(response->max_lines, config.max_response_lines)` 截断，防止溢出

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
    modem_t *me = (modem_t *)user_ctx;
    // "+CGEV: ME PDN DEACT 1" → MODEM_EVENT_PDP_DEACTIVATED
    // 生成 modem_event_t 并投递到 me->event_queue；
    // Core 之后由 Modem event_task 通知。
}

/* 注册时 handler 生命周期由调用方管理（通常是 static 或动态分配） */
static at_urc_handler_t cgev_handler_node = {
    .prefix    = "+CGEV:",
    .callback  = cgev_handler,
    .user_ctx  = NULL,  // 在 modem_init 时设置
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

Core 只通过 `modem_*` 层间包装 API 使用 `modem_t`，不直接调用 AT Engine，不写 AT 指令字符串，也不 include 具体模块头文件。`modem_ops_t` 是 Modem 层内部多态机制，包装 API 内部再调用 `me->ops->method(me, ...)`。

### 2.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `modem_t` | 层间 API (opaque) + 内部基类 | Core + Facade 模块 factory + Modem 实现 | 抽象基类/句柄 | Core 持有的 Modem 句柄，内部保存 ops、AT Engine、事件队列等公共资源 |
| `modem_ops_t` | 内部 | Modem 通用包装函数 + 具体子类 | 虚函数表 | 不暴露给 Core，子类用 `static const` ops 表实现多态 |
| `modem_state_t` | 层间 API | Core + Modem 层 | 状态枚举 | Modem 层本地生命周期和低层连接状态 |
| `modem_reg_status_t` | 层间 API | Core + Modem 层 | 状态枚举 | 蜂窝网络注册状态，来自 `+CEREG` / `+CGREG` / `+CREG` |
| `modem_sim_status_t` | 层间 API | Core + Modem 层 | 状态枚举 | SIM/PIN 状态，来自 `AT+CPIN?` / `+CPIN:` |
| `modem_info_t` | 层间 API | Core + Modem 层 | 值对象 | 模块和 SIM 卡静态信息 |
| `modem_signal_t` | 层间 API | Core + Modem 层 | 值对象 | 信号质量查询结果 |
| `modem_pdp_context_t` | 层间 API | Core + Modem 层 | 值对象 | PDP 上下文配置与激活结果 |
| `modem_event_id_t` | 层间 API | Core + Modem 层 | 事件枚举 | URC 翻译后的事件类型 |
| `modem_event_t` | 层间 API | Core + Modem 层 | 值对象 | Modem event task 上报给 Core 的事件 |
| `modem_event_callback_t` | 层间 API | Core + Modem 层 | 回调接口 | Core 注册，Modem event task 调用 |
| `modem_air780ep_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | Air780EP GPIO、事件任务、默认超时等参数 |
| `modem_air780ep_t` | 内部 | Air780EP 实现自身 | 子类 | 继承 `modem_t`，实现 Air780EP AT 指令和 URC 翻译 |
| `air780ep_cmd_ctx_t` | 内部 | Air780EP 实现自身 | 工作上下文 | 单次 AT 命令解析的临时数据 |

### 2.2 `modem_t` — 通用 Modem 句柄和基类

**所属层**：Modem Adapter
**可见性**：层间 API opaque + 内部结构体；`src/modem/modem.h` 只暴露前置声明，`struct modem` 定义在 `src/modem/modem_priv.h` 或 `.c` 中
**OOP 角色**：抽象基类 + 顶层句柄

**公开类型**：

```c
typedef struct modem modem_t;
```

**声明顺序说明**：实际 `src/modem/modem.h` 中应先完成 `modem_t` 前置声明，再定义状态枚举、值对象、事件对象和回调类型，最后声明以下函数原型。本节为了说明 `modem_t` 的使用方式，先集中列出层间方法。

**层间方法**（`src/modem/modem.h`）：

```c
esp_err_t modem_destroy(modem_t *me);
esp_err_t modem_init(modem_t *me);
esp_err_t modem_reset(modem_t *me);

esp_err_t modem_register_event_callback(modem_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

esp_err_t modem_get_state(modem_t *me, modem_state_t *state);
esp_err_t modem_get_info(modem_t *me, modem_info_t *info);
esp_err_t modem_get_sim_status(modem_t *me, modem_sim_status_t *status);
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal);
esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status);

esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn);
esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp);
```

**关键内部字段类别**（非完整代码快照，实际以 `src/modem/modem_priv.h` 为准）：

- `ops`：指向具体模块 `modem_ops_t` 的 vptr。
- `at`：Facade factory 注入的下层 AT Engine 句柄，Modem 借用但不拥有生命周期。
- `lock`、`state`、`destroying`：保护 Modem 本地状态和销毁过程。
- `event_queue`、`event_task` 及同步信号量：把 URC 翻译后的 `modem_event_t` 解耦后上报给 Core。
- `event_cb`、`event_user_ctx`：Core 注册的上行事件回调槽位。
- `name`：模块实现名称，如 `air780ep`，用于日志和诊断。

**关键设计决策**：
- `modem_t` 对 Core opaque，Core 不直接访问 `ops` 或内部字段。
- 通用包装 API 统一做参数检查、状态检查和必填方法检查。
- `event_queue + event_task` 属于基类资源，所有具体模块共用同一套上行事件解耦机制。
- `at` 句柄由 Facade 模块 factory 创建并传入具体模块工厂；Modem 不拥有 AT Engine 生命周期，只在 destroy 前注销自己注册的 URC handler。

### 2.3 `modem_ops_t` — Modem 虚函数表

**所属层**：Modem Adapter
**可见性**：内部；只给 Modem 通用实现和具体子类使用，不放入 `src/modem/modem.h`
**OOP 角色**：虚函数表

```c
typedef struct modem_ops {
    esp_err_t (*destroy)(modem_t *me);
    esp_err_t (*init)(modem_t *me);
    esp_err_t (*reset)(modem_t *me);
    esp_err_t (*get_info)(modem_t *me, modem_info_t *info);
    esp_err_t (*get_sim_status)(modem_t *me, modem_sim_status_t *status);
    esp_err_t (*get_signal)(modem_t *me, modem_signal_t *signal);
    esp_err_t (*get_registration)(modem_t *me, modem_reg_status_t *status);
    esp_err_t (*set_apn)(modem_t *me, uint8_t cid, const char *apn);
    esp_err_t (*activate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*deactivate_pdp)(modem_t *me, uint8_t cid);
    esp_err_t (*get_pdp_context)(modem_t *me, uint8_t cid,
                                  modem_pdp_context_t *pdp);
} modem_ops_t;
```

**调用模式**：

```c
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal)
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
| `init` | 建立 AT 口基础工作环境，注册 URC，不激活 PDP | `ATE0`、`AT+CMEE=1`、`AT+CGEREP=1`，注册 `RDY`、`+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:`、`+CGEV:`、`+PDP DEACT`、`+PDP:DEACT` |
| `reset` | 模块软件复位，复位后由 Core 决定是否重新 `init` | 优先 `AT+RESET`；需要切换功能模式时才考虑 `AT+CFUN=1,1` |
| `get_info` | 读取模块/SIM 静态标识，Core 不解析原始 AT 行 | `AT+CGSN`、`AT+CIMI`、`AT+ICCID`、`AT+CGMM`、`AT+CGMR`；`ATI`/`AT+VER` 可作为固件信息补充 |
| `get_sim_status` | 查询 SIM/PIN 可用性 | `AT+CPIN?` |
| `get_signal` | 查询当前基础信号质量 | `AT+CSQ`；`AT+CESQ` 只作为后续 LTE 扩展指标来源 |
| `get_registration` | 查询蜂窝网络注册状态 | Air780EP 优先 `AT+CEREG?`，必要时用 `AT+CGREG?` 补充分组域状态，用 `AT+CREG?` 作为通用注册状态兜底 |
| `set_apn` | 配置 PDP context 的 APN | `AT+CGDCONT=<cid>,"IP","<apn>"`；APN 用户名/密码后续再通过单独能力扩展，不塞进当前 API |
| `activate_pdp` | 确认 SIM/注册/附着后激活数据面并获得 IP | 先检查 `AT+CPIN?`、`AT+CEREG?`/`AT+CGREG?`、`AT+CGATT?`；Air780EP TCPIP 路径使用 `AT+CSTT`、`AT+CIICR`、`AT+CIFSR` |
| `deactivate_pdp` | 关闭数据面并清理 Air780EP TCPIP 场景 | 优先 `AT+CIPSHUT`；标准 PDP 路径需要时可使用 `AT+CGACT=0,<cid>` |
| `get_pdp_context` | 返回 APN、激活状态和 IP 地址快照 | 组合缓存值、`AT+CGDCONT?`、`AT+CGACT?`、`AT+CGPADDR=<cid>`；TCPIP 路径下也可使用最近一次 `AT+CIFSR` 结果 |

`AT+IPR`、`AT+IFC`、`AT&W` 属于板级串口/持久化配置，不进入 Modem ops。`AT+COPS?`、`AT^SYSINFO`、`AT+CIPPING` 属于诊断或联网自检，第一版可作为 Air780EP 内部 helper，不先扩大 Core 可见 API。`AT+CSCLK`、`AT+POWERMODE`、`AT+CFGRI` 等低功耗指令需要 Core 低功耗策略后再设计独立 ops。

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
} modem_event_id_t;

typedef struct {
    modem_event_id_t id;
    union {
        modem_sim_status_t   sim_status;
        modem_reg_status_t   reg_status;
        modem_pdp_context_t  pdp;
        modem_signal_t       signal;
        int                  error_code;
    } data;
} modem_event_t;

typedef void (*modem_event_callback_t)(modem_t *modem,
                                       const modem_event_t *event,
                                       void *user_ctx);
```

**硬约束**：Air780EP 的 AT Engine URC handler 不得直接调用 `modem_event_callback_t`。URC handler 只能把 `modem_event_t` 投递到 `modem_t.event_queue`，由 `modem_t.event_task` 调用 Core 注册的回调。

### 2.11 `modem_air780ep_config_t` — Air780EP 配置

**所属层**：Modem Adapter
**可见性**：层间 API，放入 `src/modem/modem_air780ep.h`，只给 Facade 模块 factory 使用
**OOP 角色**：配置结构体

```c
typedef struct {
    gpio_num_t pwrkey_pin;              // PWRKEY GPIO，未使用时为 GPIO_NUM_NC
    gpio_num_t reset_pin;               // RESET GPIO，未使用时为 GPIO_NUM_NC
    gpio_num_t status_pin;              // STATUS GPIO，未使用时为 GPIO_NUM_NC
    uint32_t   power_on_pulse_ms;       // PWRKEY 上电脉冲宽度
    uint32_t   reset_pulse_ms;          // RESET 脉冲宽度
    uint32_t   boot_wait_ms;            // 上电后等待模块启动时间
    uint32_t   default_cmd_timeout_ms;  // Air780EP 命令默认超时
    int        event_queue_size;        // Modem 事件队列长度
    int        event_task_stack;        // Modem event task 栈大小
    int        event_task_priority;     // Modem event task 优先级
} modem_air780ep_config_t;

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config);
```

**关键设计决策**：
- `modem_air780ep_create()` 是具体模块工厂，只应出现在 Facade 模块 factory 装配代码中。
- Core 不 include `modem_air780ep.h`，只接收工厂返回的 `modem_t *`。
- GPIO 控制属于 Modem 层职责，Air780EP 实现可以直接使用 ESP-IDF `driver/gpio.h`。

### 2.12 `modem_air780ep_t` — Air780EP 子类

**所属层**：Modem Adapter
**可见性**：内部，定义在 `src/modem/modem_air780ep.c`
**OOP 角色**：具体子类

```c
#define AIR780EP_MAX_PDP_CONTEXTS  4

typedef struct {
    modem_t                  base;          // 必须是第一个字段，实现向上转型
    modem_air780ep_config_t  config;        // 配置快照
    at_urc_handler_t         rdy_handler;   // RDY URC handler
    at_urc_handler_t         cpin_handler;  // +CPIN: URC handler
    at_urc_handler_t         creg_handler;  // +CREG: URC handler
    at_urc_handler_t         cereg_handler; // +CEREG: URC handler
    at_urc_handler_t         cgreg_handler; // +CGREG: URC handler
    at_urc_handler_t         cgev_handler;  // +CGEV: URC handler
    at_urc_handler_t         pdp_deact_handler;       // +PDP DEACT URC handler
    at_urc_handler_t         pdp_colon_deact_handler; // +PDP:DEACT URC handler
    modem_info_t             cached_info;   // 已查询到的模块/SIM 信息
    modem_sim_status_t       last_sim_status; // 最近一次 SIM 状态
    modem_reg_status_t       last_reg_status; // 最近一次网络注册状态
    modem_signal_t           last_signal;   // 最近一次信号质量
    modem_pdp_context_t      pdp[AIR780EP_MAX_PDP_CONTEXTS];
    bool                     urc_registered;
    bool                     initialized;
} modem_air780ep_t;
```

**关键设计决策**：
- `base` 必须位于结构体第一个字段，子类返回给上层时使用 `&self->base`。
- 从 `modem_t *` 反推 `modem_air780ep_t *` 时使用 `container_of(me, modem_air780ep_t, base)`，禁止裸强转。
- URC handler 节点生命周期由 Air780EP 对象拥有，`init` 时注册 `RDY`、`+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:`、`+CGEV:`、`+PDP DEACT`、`+PDP:DEACT`，`destroy` 时注销。
- `+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:` 既可能是查询响应，也可能是空闲期 URC；Air780EP handler 只处理 AT Engine 分发出来的空闲期 URC，命令响应由对应 ops 方法解析。

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
│  └───────┬────────┘    ──→ 解析 RDY/+CPIN/+CREG/+CEREG       │
│          │              /+CGREG/+CGEV/+PDP DEACT             │
│          │          ──→ 生成 modem_event_t                  │
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

Core Service 是内部 service 层，负责网络状态机、PDP 管理和连接恢复。Core 只通过 `modem_*` 包装 API 操作 `modem_t`，不直接调用 AT Engine，不写 AT 指令字符串。Core 通过 `esp_event` 和回调把状态变化交给 LWLTE Facade，由 Facade 再翻译为用户事件。

```
Core Service
├── core_t        层间 API opaque 句柄，Facade 持有并调用
├── core_fsm_t    Core 内部组件，属于 core_t，负责串行处理 Core 信号
├── net_mgr_t     Core 内部组件，属于 core_t，负责网络激活和重连策略
└── pdp_mgr_t     Core 内部组件，属于 core_t，负责 PDP 上下文状态缓存
```

`net_mgr_t`、`pdp_mgr_t`、`core_fsm_t` 不是 `core_t` 子类。它们不能向上转型为 `core_t *`，也不实现 `core_ops`；它们是 `core_t` 的组合成员。`modem_air780ep_t` 才是 `modem_t` 的子类，因为它以 `modem_t base` 为第一个成员并实现 `modem_ops`。

### 3.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `core_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | Core 创建参数 |
| `core_t` | 层间 API (opaque) | Facade | 句柄 | Core Service 实例句柄 |
| `core_state_t` | 层间 API | Facade + Core 内部 | 状态枚举 | Core 生命周期状态 |
| `core_net_state_t` | 层间 API | Facade + Core 内部 | 状态枚举 | 网络连接状态 |
| `core_event_id_t` | 层间 API | Facade + esp_event | 事件枚举 | Core 上行事件类型，同时作为 esp_event event_id |
| `core_event_data_t` | 层间 API | Facade + esp_event | 值对象 | 事件携带数据 |
| `core_event_callback_t` | 层间 API | Facade | 回调接口 | Facade 接收 Core 事件的便捷回调签名 |
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
    const char *apn;                     // APN，如 "cmnet"
    uint8_t     primary_cid;             // 主 PDP context ID，默认 1
    uint32_t    net_activate_timeout_ms; // 网络激活总超时（毫秒），默认 120000
    uint32_t    reconnect_delay_ms;      // 掉线重连固定延迟（毫秒），默认 5000
    bool        auto_connect;            // Core 内部自动连接选项；Facade factory 通常设为 false，由用户公共配置决定是否 ready 后调用 lwlte_connect()
    int         fsm_queue_size;          // FSM 信号队列长度
    int         fsm_task_stack;          // FSM 任务栈大小（字节）
    int         fsm_task_priority;       // FSM 任务优先级
} core_config_t;
```

Event loop 参数不放入 config，Core 内部用默认值创建。Modem 引用在 `core_create()` 参数中单独传入。`auto_connect` 是 Core 层间选项，不等同于用户公共 `lwlte_air780ep_config_t.auto_connect`；Facade 模块 factory 通常把 Core `auto_connect` 设为 false，并在 Core ready 后按用户公共配置决定是否调用 `lwlte_connect()`。

### 3.3 `core_t` — Core 句柄

**所属层**：Core Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/core/core_priv.h`
**OOP 角色**：service 对象，组合持有 Core 内部组件

**层间方法**（`src/core/core.h`）：

```c
core_t *core_create(const core_config_t *config, modem_t *modem);
esp_err_t core_destroy(core_t *me);
esp_err_t core_start(core_t *me);
esp_err_t core_stop(core_t *me);

esp_err_t core_register_event_callback(core_t *me,
                                       core_event_callback_t callback,
                                       void *user_ctx);
esp_event_loop_handle_t core_get_event_loop(core_t *me);

esp_err_t core_get_state(core_t *me, core_state_t *state);
esp_err_t core_get_net_state(core_t *me, core_net_state_t *state);

esp_err_t core_connect(core_t *me);
esp_err_t core_disconnect(core_t *me);
```

**关键内部字段类别**（非完整代码快照，实际以 `src/core/core_priv.h` 为准）：

- `config`：Core 层间配置快照；`auto_connect` 为内部选项，Facade 通常设为 false。
- `modem`：Facade factory 注入的 `modem_t` 句柄，Core 借用但不拥有生命周期。
- `event_loop`、`event_callback`、`event_user_ctx`：Core 事件分发和 Facade 桥接回调。
- `fsm`、`net_mgr`、`pdp_mgr`：`core_t` 的组合成员，分别负责信号串行化、网络激活/重连、PDP 缓存。
- `state`、`destroying`、`lock`：Core 生命周期状态和并发保护。

**关键设计决策**：
- Core 没有 ops 多态；它不面向多种实现，只有一个实现。
- `modem` 句柄由 Facade 模块 factory 传入，Core 不拥有 Modem 生命周期。
- `event_loop` 由 Core 在 `core_create()` 中创建，在 `destroy` 中删除。
- `lock` 只保护 `state`/`destroying` 等短字段访问，FSM 线程调用 `modem_*` API 时不持锁。
- Facade 调用的 `start`、`connect`、`disconnect` 只投递信号到 FSM 队列即返回，不阻塞。

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

ESP_EVENT_DECLARE_BASE(CORE_EVENT);

typedef enum {
    CORE_EVENT_STARTED = 0,
    CORE_EVENT_READY,
    CORE_EVENT_NET_CONNECTING,
    CORE_EVENT_NET_ONLINE,
    CORE_EVENT_NET_OFFLINE,
    CORE_EVENT_NET_ERROR,
    CORE_EVENT_STOPPED,
    CORE_EVENT_ERROR,
} core_event_id_t;

typedef struct {
    core_net_state_t net_state;
    int              error_code;
} core_event_data_t;

typedef void (*core_event_callback_t)(core_t *core,
                                      core_event_id_t event_id,
                                      const core_event_data_t *data,
                                      void *user_ctx);
```

**边界说明**：`core_state_t` 表示 Core 自身生命周期阶段，`core_net_state_t` 表示纯网络状态。Facade 负责把这些层间状态翻译为 `lwlte_state_t`、`lwlte_net_state_t` 和用户事件。

### 3.5 `core_fsm_t` — FSM 组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_t` 的组合成员，管理 FSM 线程和信号队列

```c
typedef enum {
    CORE_SIG_MODEM_EVENT = 0,
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
    CORE_SIG_NET_DEACTIVATE,
    CORE_SIG_NET_STEP_DONE,
    CORE_SIG_NET_STEP_TIMEOUT,
    CORE_SIG_RECONNECT,
} core_fsm_sig_type_t;

typedef struct {
    core_fsm_sig_type_t type;
    modem_event_t       modem_event;
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

### 3.6 `net_mgr_t` — 网络管理组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_t` 的组合成员，管理网络激活子状态机 + 重连定时器

```c
typedef enum {
    NET_STEP_IDLE = 0,
    NET_STEP_CHECK_SIM,
    NET_STEP_CHECK_SIGNAL,
    NET_STEP_CHECK_REGISTRATION,
    NET_STEP_SET_APN,
    NET_STEP_ACTIVATE_PDP,
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
    core_net_state_t  state;
    bool              reconnect_enabled;
} net_mgr_t;
```

**激活步骤与 `modem_*` API 映射**：

| 步骤 | 操作 | Modem API |
|------|------|-----------|
| `CHECK_SIM` | 查询 SIM 状态 | `modem_get_sim_status()` |
| `CHECK_SIGNAL` | 查询信号质量 | `modem_get_signal()` |
| `CHECK_REGISTRATION` | 查询网络注册 | `modem_get_registration()` |
| `SET_APN` | 配置 APN | `modem_set_apn()` |
| `ACTIVATE_PDP` | 激活 PDP | `modem_activate_pdp()` |
| `DONE` | 网络上线 | 发布 `CORE_EVENT_NET_ONLINE` |

每个步骤由 `CORE_SIG_NET_STEP_DONE` 驱动推进。FSM 处理函数调用对应的 `modem_*` API（阻塞），完成后向自己队列发送 `CORE_SIG_NET_STEP_DONE`。步骤超时发 `CORE_SIG_NET_STEP_TIMEOUT`，重试计数超过 `max_retry` 后进入 `NET_STEP_ERROR`，Core 发布 `CORE_EVENT_NET_ERROR`。

**重连逻辑**：
- 收到 `MODEM_EVENT_PDP_DEACTIVATED` → `net_state = OFFLINE` → 发布 `CORE_EVENT_NET_OFFLINE` → 启动 `reconnect_timer`（固定 `reconnect_delay_ms`）。
- 定时器回调发送 `CORE_SIG_RECONNECT` → FSM 重新触发网络激活流程。

第一版只做固定延迟重连，不做指数退避。保活机制后续版本再加。

### 3.7 `pdp_mgr_t` — PDP 管理组件

**所属层**：Core Service
**可见性**：模块私有 API — `src/core/core_priv.h`，只允许 Core 源码 include
**OOP 角色**：`core_t` 的组合成员，缓存 PDP context 状态

```c
#define CORE_MAX_PDP_CONTEXTS 4

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t             primary_cid;
} pdp_mgr_t;
```

第一版仅做 PDP context 缓存，提供 `pdp_mgr_get()` / `pdp_mgr_update()` 两个内部接口。后续加多 PDP 管理时在这里扩展。

### 3.8 Core 线程模型

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

**硬约束**：

| 约束 | 说明 |
|------|------|
| Modem 回调不调 Core API | Modem event_task 中只 `xQueueSend` 到 FSM 队列，与 Modem 层"URC handler 不调 Core 回调"约束一致 |
| FSM 线程不持锁跨阻塞 | `core->lock` 只保护短字段，调用 `modem_*` API 时不持锁 |
| esp_event_post_to 不阻塞 | 投递事件不阻塞 FSM 线程 |
| Facade API 不阻塞 | `start`/`connect`/`disconnect` 只投递信号即返回 |

**Modem 事件 → Core 行为映射**：

| Modem Event | Core 行为 |
|-------------|----------|
| `MODEM_EVENT_READY` | core_state → READY，发布 `CORE_EVENT_READY`；Facade 可在 ready 后按用户公共配置提交 `lwlte_connect()` |
| `MODEM_EVENT_SIM_CHANGED` | 更新 net_mgr 可用的 SIM 状态 |
| `MODEM_EVENT_REG_CHANGED` | 更新 net_mgr 可用的注册状态 |
| `MODEM_EVENT_PDP_ACTIVATED` | net_state → ONLINE，发布 `CORE_EVENT_NET_ONLINE` |
| `MODEM_EVENT_PDP_DEACTIVATED` | net_state → OFFLINE，发布 `CORE_EVENT_NET_OFFLINE`，启动重连定时器 |
| `MODEM_EVENT_ERROR` | 根据 error_code 决定重试或进入 ERROR 状态 |

### 3.9 初始化与装配

```c
/* Facade 模块 factory — Core 不依赖具体模块型号 */

core_config_t core_cfg = {
    .apn                     = "cmnet",
    .primary_cid             = 1,
    .net_activate_timeout_ms = 120000,
    .reconnect_delay_ms      = 5000,
    .auto_connect            = false,  /* Facade 持有用户级 auto-connect 策略 */
    .fsm_queue_size          = 16,
    .fsm_task_stack          = 4096,
    .fsm_task_priority       = 8,
};

core_t *core = core_create(&core_cfg, modem);
core_register_event_callback(core, facade_core_event_handler, lte);
core_start(core);  // 异步，结果通过事件通知 Facade

/* Facade 等待 CORE_EVENT_READY 后，如果用户公共 config 要求自动联网，再提交连接请求。 */
if (user_config->auto_connect) {
    lwlte_connect(lte);
}
```

**关键设计决策**：
- `core_create()` 接收 `modem_t *`，和 `modem_air780ep_create()` 接收 `at_engine_t *` 的模式一致。
- Core 不 include 具体模块头文件，只认识 `modem_t`。
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

## 4. App（应用层）

> App 层不定义框架类，只有用户自己的业务类型。此处不展开。
