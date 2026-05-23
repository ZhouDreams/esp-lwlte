# 类定义

在编码前先确定每个模块有哪些类（struct + 配套函数），是理解项目架构最重要的文档。每个类的定义包含：**所属层、职责、可见性、关键字段、关键方法、OOP 角色**。

## 可见性三级定义

| 可见性 | 落入哪个头文件 | 谁能看到 | 命名前缀 |
|--------|-------------|---------|---------|
| **用户 API** | `include/lwlte_*.h` | App 开发者 | `lwlte_` |
| **层间 API** | `include/at_engine.h`、`include/modem.h` | 紧邻上层 + Board Init | `at_engine_`、`modem_` |
| **内部** | `.c` 或 `_priv.h` | 当前模块自身 | 无限制 |

**核心区别**：用户 API 是给 App 开发者用的，层间 API 是层与层之间、以及 Board Init 装配时用的。AT Engine 没有任何用户 API——它被 Modem 层完全封装，最终用户看不到它的存在。

---

## 1. AT Engine（AT 引擎层）

AT Engine 是四层架构的最底层，负责 AT 协议解析和 UART 硬件操作。该层有以下类：

### 1.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `at_engine_config_t` | 层间 API | Board Init | 配置结构体 | UART 硬件参数 + 任务参数 |
| `at_engine_t` | 层间 API (opaque) | Modem 层 + Board Init | 句柄 | AT Engine 实例句柄 |
| `at_response_t` | 层间 API | Modem 层 | 值对象 | 一次 AT 命令的响应结果 |
| `at_urc_handler_t` | 层间 API | Modem 层 | 回调注册项 | URC 前缀 + 回调 + 用户上下文 |
| `at_state_t` | 内部 | AT Engine 自身 | 状态枚举 | 命令处理状态机 |
| `at_cmd_ctx_t` | 内部 | AT Engine 自身 | 工作上下文 | 一次命令执行期间的临时数据 |

### 1.2 `at_engine_config_t` — 引擎配置

**所属层**：AT Engine  
**可见性**：层间 API — Board Init 创建 AT Engine 时填充并传入  
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
**可见性**：层间 API (opaque) — Board Init 创建，Modem 层持有句柄；struct 定义在 `.c` 中  
**OOP 角色**：顶层对象，持有该层所有资源

**公开方法**：

```c
at_engine_t *at_engine_create(const at_engine_config_t *config);
esp_err_t    at_engine_destroy(at_engine_t *me);

/* 发送 AT 命令（阻塞调用，直到响应完成或超时） */
esp_err_t    at_engine_send_cmd(at_engine_t *me, const char *cmd,
                                at_response_t *response, uint32_t timeout_ms);

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
    AT_RESP_OK = 0,        // 成功（收到 OK）
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

### 1.5 `at_urc_handler_t` — URC 处理器

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
    // "+CGEV: ME PDN DEACT 1" → 翻译为 MODEM_EVENT_PDP_DEACT
    // 通过 modem 的内部事件回调通知 Core 层
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

### 1.6 `at_state_t` — 内部状态枚举

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

### 1.7 `at_cmd_ctx_t` — 命令上下文（内部）

**所属层**：AT Engine  
**可见性**：内部 — 仅 `at_engine.c` 中使用，随命令生命周期创建和销毁  
**OOP 角色**：临时工作上下文

```c
typedef struct {
    const char     *cmd;                 // 命令字符串（调用方传入，不拷贝）
    uint32_t        timeout_ms;          // 超时时间（毫秒）
    at_response_t  *response;            // 指向调用方的 response 对象
    int             echo_consumed;       // 是否已消费命令回显行
    int             data_line_index;     // 当前填充到 response->lines 的索引
    bool            result_received;     // 已收到最终结果（OK/ERROR/CME ERROR 等）
} at_cmd_ctx_t;
```

### 1.8 AT Engine 线程模型

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

> 待详细设计。该层包含 `modem_interface`（抽象基类）和 `modem_air780ep`（具体子类实现）。

---

## 3. Core Service（核心服务层）

> 待详细设计。该层包含网络状态机、PDP 管理器、MQTT/HTTP 客户端等。

---

## 4. App（应用层）

> App 层不定义框架类，只有用户自己的业务类型。此处不展开。
