# Modem Adapter Classes Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Modem Adapter placeholder in `docs/agents/classes.md` with the approved class design for the modem layer.

**Architecture:** This is a documentation-only change. The design keeps `modem_t` opaque to Core, uses `modem_*` wrapper APIs as the layer boundary, keeps `modem_ops_t` as Modem-internal polymorphism, and gives Air780EP its own Board Init factory. URC events are decoupled through a Modem event queue and event task, so Core callbacks never run in the AT Engine RX task.

**Tech Stack:** Markdown, C API design snippets, ESP-IDF `esp_err_t`, ESP-IDF GPIO types, FreeRTOS queue/task concepts, project OOP documentation conventions.

---

## User Constraints

- Do not create git commits unless the user explicitly asks for a commit.
- Do not add or modify C source files in this task.
- Do not run ESP-IDF build for this documentation-only change.

## Scope Check

The approved spec originally covered one documentation unit: `docs/agents/classes.md` section 2, plus the visibility table at the top of the same file. Final-review consistency fixes also update `docs/agents/architecture.md` API snippets and the stale modem type name in `docs/agents/err.md`. This remains documentation-only work and does not need decomposition into multiple implementation plans.

## File Structure

- Modify: `docs/agents/classes.md`
  - Update the layer API visibility row to include `include/modem_air780ep.h` and `modem_air780ep_`, while making clear that `modem_air780ep.h` is Board Init only.
  - Replace the `## 2. Modem Adapter（模块适配层）` placeholder with full class design content.
- Modify: `docs/agents/architecture.md`
  - Align final-review stale snippets with `modem_air780ep_create(at, &modem_cfg)`, `modem_init(modem)`, stack `at_response_t`, and current Modem operation names.
- Modify: `docs/agents/err.md`
  - Replace the stale old modem parameter type with `modem_t *modem`.
- Read-only reference: `docs/superpowers/specs/2026-05-23-modem-adapter-classes-design.md`
  - Approved design source for this implementation.

## Task 1: Update Visibility Table

**Files:**
- Modify: `docs/agents/classes.md:7-11`

- [ ] **Step 1: Update the layer API row**

Use `apply_patch` to change the visibility table row exactly as shown:

````patch
*** Begin Patch
*** Update File: docs/agents/classes.md
@@
-| **层间 API** | `include/at_engine.h`、`include/modem.h` | 紧邻上层 + Board Init | `at_engine_`、`modem_` |
+| **层间 API** | `include/at_engine.h`、`include/modem.h`、`include/modem_air780ep.h` | `at_engine.h`、`modem.h`：紧邻上层 + Board Init；`modem_air780ep.h`：仅 Board Init | `at_engine_`、`modem_`、`modem_air780ep_` |
*** End Patch
````

- [ ] **Step 2: Verify the row still renders as a Markdown table**

Run: `git diff -- docs/agents/classes.md`

Expected: the only change so far is the layer API table row, and the table still has four columns.

## Task 2: Replace Modem Adapter Placeholder

**Files:**
- Modify: `docs/agents/classes.md:279-282`

- [ ] **Step 1: Replace the placeholder block**

Replace this existing block:

````markdown
## 2. Modem Adapter（模块适配层）

> 待详细设计。该层包含 `modem_interface`（抽象基类）和 `modem_air780ep`（具体子类实现）。
````

with this exact Markdown:

`````markdown
## 2. Modem Adapter（模块适配层）

Modem Adapter 是四层架构的第二层，负责把 Core 的语义操作翻译为具体模块的 AT 指令，并把模块 URC 翻译为 Core 可理解的事件。该层包含通用 Modem 基类、内部 ops 多态表、语义值对象，以及 Air780EP 具体子类。

Core 只通过 `modem_*` 层间包装 API 使用 `modem_t`，不直接调用 AT Engine，不写 AT 指令字符串，也不 include 具体模块头文件。`modem_ops_t` 是 Modem 层内部多态机制，包装 API 内部再调用 `me->ops->method(me, ...)`。

### 2.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `modem_t` | 层间 API (opaque) + 内部基类 | Core + Board Init + Modem 实现 | 抽象基类/句柄 | Core 持有的 Modem 句柄，内部保存 ops、AT Engine、事件队列等公共资源 |
| `modem_ops_t` | 内部 | Modem 通用包装函数 + 具体子类 | 虚函数表 | 不暴露给 Core，子类用 `static const` ops 表实现多态 |
| `modem_state_t` | 层间 API | Core + Modem 层 | 状态枚举 | Modem 层本地生命周期和低层连接状态 |
| `modem_reg_status_t` | 层间 API | Core + Modem 层 | 状态枚举 | 蜂窝网络注册状态，来自 `+CEREG` / `+CGREG` |
| `modem_info_t` | 层间 API | Core + Modem 层 | 值对象 | 模块和 SIM 卡静态信息 |
| `modem_signal_t` | 层间 API | Core + Modem 层 | 值对象 | 信号质量查询结果 |
| `modem_pdp_context_t` | 层间 API | Core + Modem 层 | 值对象 | PDP 上下文配置与激活结果 |
| `modem_event_id_t` | 层间 API | Core + Modem 层 | 事件枚举 | URC 翻译后的事件类型 |
| `modem_event_t` | 层间 API | Core + Modem 层 | 值对象 | Modem event task 上报给 Core 的事件 |
| `modem_event_callback_t` | 层间 API | Core + Modem 层 | 回调接口 | Core 注册，Modem event task 调用 |
| `modem_air780ep_config_t` | 层间 API | Board Init | 配置结构体 | Air780EP GPIO、事件任务、默认超时等参数 |
| `modem_air780ep_t` | 内部 | Air780EP 实现自身 | 子类 | 继承 `modem_t`，实现 Air780EP AT 指令和 URC 翻译 |
| `air780ep_cmd_ctx_t` | 内部 | Air780EP 实现自身 | 工作上下文 | 单次 AT 命令解析的临时数据 |

### 2.2 `modem_t` — 通用 Modem 句柄和基类

**所属层**：Modem Adapter
**可见性**：层间 API opaque + 内部结构体；`include/modem.h` 只暴露前置声明，`struct modem` 定义在 `src/modem/modem_priv.h` 或 `.c` 中
**OOP 角色**：抽象基类 + 顶层句柄

**公开类型**：

```c
typedef struct modem modem_t;
```

**声明顺序说明**：实际 `include/modem.h` 中应先完成 `modem_t` 前置声明，再定义状态枚举、值对象、事件对象和回调类型，最后声明以下函数原型。本节为了说明 `modem_t` 的使用方式，先集中列出公开方法。

**公开方法**（`include/modem.h`）：

```c
esp_err_t modem_destroy(modem_t *me);
esp_err_t modem_init(modem_t *me);
esp_err_t modem_reset(modem_t *me);

esp_err_t modem_register_event_callback(modem_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

esp_err_t modem_get_state(modem_t *me, modem_state_t *state);
esp_err_t modem_get_info(modem_t *me, modem_info_t *info);
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal);
esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status);

esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn);
esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp);
```

**内部结构**（定义在 `src/modem/modem_priv.h` 或 `.c`）：

```c
struct modem {
    const modem_ops_t      *ops;                   // vptr，指向具体模块 ops 表
    at_engine_t            *at;                    // 下层 AT Engine 句柄
    SemaphoreHandle_t       lock;                  // 保护 state/callback/destroying
    QueueHandle_t           event_queue;           // URC 翻译后的事件队列
    TaskHandle_t            event_task;            // 调用 Core 回调的事件任务
    SemaphoreHandle_t       event_task_done_sema;  // event task 退出同步
    modem_event_callback_t  event_cb;              // Core 注册的事件回调
    void                   *event_user_ctx;        // Core 事件回调上下文
    modem_state_t           state;                 // Modem 层本地状态
    bool                    destroying;            // destroy 已开始，拒绝新调用
    bool                    event_task_stop_requested;
    const char             *name;                  // 模块名称，如 "air780ep"
};
```

**关键设计决策**：
- `modem_t` 对 Core opaque，Core 不直接访问 `ops` 或内部字段。
- 通用包装 API 统一做参数检查、状态检查和必填方法检查。
- `event_queue + event_task` 属于基类资源，所有具体模块共用同一套上行事件解耦机制。
- `at` 句柄由 Board Init 创建并传入具体模块工厂；Modem 不拥有 AT Engine 生命周期，只在 destroy 前注销自己注册的 URC handler。

### 2.3 `modem_ops_t` — Modem 虚函数表

**所属层**：Modem Adapter
**可见性**：内部；只给 Modem 通用实现和具体子类使用，不放入 `include/modem.h`
**OOP 角色**：虚函数表

```c
typedef struct modem_ops {
    esp_err_t (*destroy)(modem_t *me);
    esp_err_t (*init)(modem_t *me);
    esp_err_t (*reset)(modem_t *me);
    esp_err_t (*get_info)(modem_t *me, modem_info_t *info);
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

### 2.6 `modem_info_t` — 模块和 SIM 信息

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

**来源示例**：Air780EP 可通过 `AT+CGSN`、`AT+CIMI`、`AT+CCID`、`ATI` 等命令填充该对象。Core 不解析这些 AT 响应行。

### 2.7 `modem_signal_t` — 信号质量

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

### 2.8 `modem_pdp_context_t` — PDP 上下文

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

### 2.9 `modem_event_t` — Modem 上行事件

**所属层**：Modem Adapter
**可见性**：层间 API
**OOP 角色**：值对象 + 回调参数

```c
typedef enum {
    MODEM_EVENT_READY = 0,          // 模块初始化完成
    MODEM_EVENT_REG_CHANGED,        // 网络注册状态变化
    MODEM_EVENT_PDP_ACTIVATED,      // PDP 激活
    MODEM_EVENT_PDP_DEACTIVATED,    // PDP 去激活
    MODEM_EVENT_SIGNAL_CHANGED,     // 信号质量变化
    MODEM_EVENT_ERROR,              // 模块侧错误事件
} modem_event_id_t;

typedef struct {
    modem_event_id_t id;
    union {
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

### 2.10 `modem_air780ep_config_t` — Air780EP 配置

**所属层**：Modem Adapter
**可见性**：层间 API，放入 `include/modem_air780ep.h`，只给 Board Init 使用
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
- `modem_air780ep_create()` 是具体模块工厂，只应出现在 Board Init 装配代码中。
- Core 不 include `modem_air780ep.h`，只接收工厂返回的 `modem_t *`。
- GPIO 控制属于 Modem 层职责，Air780EP 实现可以直接使用 ESP-IDF `driver/gpio.h`。

### 2.11 `modem_air780ep_t` — Air780EP 子类

**所属层**：Modem Adapter
**可见性**：内部，定义在 `src/modem/modem_air780ep.c`
**OOP 角色**：具体子类

```c
#define AIR780EP_MAX_PDP_CONTEXTS  4

typedef struct {
    modem_t                  base;          // 必须是第一个字段，实现向上转型
    modem_air780ep_config_t  config;        // 配置快照
    at_urc_handler_t         cereg_handler; // +CEREG: URC handler
    at_urc_handler_t         cgreg_handler; // +CGREG: URC handler
    at_urc_handler_t         cgev_handler;  // +CGEV: URC handler
    at_urc_handler_t         cpin_handler;  // +CPIN: URC handler
    modem_info_t             cached_info;   // 已查询到的模块/SIM 信息
    modem_signal_t           last_signal;   // 最近一次信号质量
    modem_pdp_context_t      pdp[AIR780EP_MAX_PDP_CONTEXTS];
    bool                     urc_registered;
    bool                     initialized;
} modem_air780ep_t;
```

**关键设计决策**：
- `base` 必须位于结构体第一个字段，子类返回给上层时使用 `&self->base`。
- 从 `modem_t *` 反推 `modem_air780ep_t *` 时使用 `container_of(me, modem_air780ep_t, base)`，禁止裸强转。
- URC handler 节点生命周期由 Air780EP 对象拥有，create/init 时注册，destroy 时注销。

### 2.12 `air780ep_cmd_ctx_t` — Air780EP 命令上下文

**所属层**：Modem Adapter
**可见性**：内部，仅 Air780EP 实现使用
**OOP 角色**：临时工作上下文

```c
#define AIR780EP_MAX_RESPONSE_LINES  8
#define AIR780EP_PARSE_BUF_SIZE      96

typedef struct {
    const char    *cmd;                                // 当前 AT 命令字符串
    uint32_t       timeout_ms;                         // 本次命令超时
    char          *lines[AIR780EP_MAX_RESPONSE_LINES]; // at_response_t lines 存储
    at_response_t  response;                           // AT Engine 响应对象
    char           parse_buf[AIR780EP_PARSE_BUF_SIZE]; // 解析辅助缓冲
} air780ep_cmd_ctx_t;
```

**使用模式**：Air780EP 每个 ops 方法在栈上创建 `air780ep_cmd_ctx_t`，初始化 `response.max_lines/lines`，调用 `at_engine_send_cmd()`，再解析 `response.lines`。该上下文不跨命令保存，不暴露给 Core。

### 2.13 Modem 线程模型

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
│  └───────┬────────┘    ──→ 解析 +CEREG/+CGREG/+CGEV          │
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
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 状态错误返回 `ESP_ERR_INVALID_STATE`。
- 不支持的能力返回 `ESP_ERR_NOT_SUPPORTED`。
- `at_engine_send_cmd()` 超时传播 `ESP_ERR_TIMEOUT`。
- AT Engine 返回 `ESP_OK` 但 `response.status` 为 `AT_RESP_ERROR`、`AT_RESP_CME_ERROR` 或 `AT_RESP_CMS_ERROR` 时，Air780EP 适配层映射为标准 ESP-IDF 错误码，并记录原始错误码。
- 响应行格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。

**与 AT Engine 的边界**：
- Modem 层可以调用 `at_engine_send_cmd()` 和 `at_engine_register_urc()`，因为 AT Engine 是紧邻下层。
- Core 不能调用 AT Engine API。
- AT Engine 不知道 Air780EP 语义，只做 URC 前缀匹配和原始行分发。
`````

- [ ] **Step 2: Confirm the Core section remains untouched**

Run: `git diff -- docs/agents/classes.md`

Expected: the diff replaces only the Modem placeholder and the visibility table row. The `## 3. Core Service（核心服务层）` heading and following content remain after the new Modem section.

## Task 3: Verify Documentation Change

**Files:**
- Verify: `docs/agents/classes.md`
- Verify: `docs/agents/architecture.md`
- Verify: `docs/agents/err.md`
- Verify: `docs/superpowers/specs/2026-05-23-modem-adapter-classes-design.md`
- Verify: `docs/superpowers/plans/2026-05-23-modem-adapter-classes-doc-plan.md`

- [ ] **Step 1: Check Markdown whitespace**

Run: `git diff --check`

Expected: no output and exit status 0.

- [ ] **Step 2: Check the old Modem placeholder was removed**

Run: `rg -n "modem_interface|具体子类实现" docs/agents/classes.md`

Expected: no output. This confirms the old Modem placeholder text is gone without checking unrelated Core/App sections.

- [ ] **Step 3: Review final diff**

Run: `git diff -- docs/agents/classes.md docs/agents/architecture.md docs/agents/err.md docs/superpowers/plans/2026-05-23-modem-adapter-classes-doc-plan.md docs/superpowers/specs/2026-05-23-modem-adapter-classes-design.md`

Expected: documentation-only consistency changes in `classes.md`, `architecture.md`, `err.md`, this plan, and the spec if it was touched; no C source changes.

- [ ] **Step 4: Report verification result**

Report the exact verification commands run and whether each passed. Do not claim an ESP-IDF build passed, because this task does not run one.
