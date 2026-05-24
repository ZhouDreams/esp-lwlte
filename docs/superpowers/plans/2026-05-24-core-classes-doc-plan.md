# Core Service classes.md 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 Core Service 层的完整类设计替换 `classes.md` 第 3 节占位符

**Architecture:** 单文件修改 — `docs/agents/classes.md`。将第 764-772 行的占位文本替换为完整的 Core Service 类定义，遵循已有 AT Engine（第 1 节）和 Modem Adapter（第 2 节）的文档格式

**Tech Stack:** Markdown

---

### Task 1: 用完整 Core Service 类设计替换 classes.md 占位符

**Files:**
- Modify: `docs/agents/classes.md:764-772`

- [ ] **Step 1: 将第 764-772 行替换为 Core Service 全部类定义**

替换内容：

```
## 3. Core Service（核心服务层）

Core Service 是四层架构的第三层，负责网络状态机、PDP 管理和连接恢复。Core 只通过 `modem_*` 包装 API 操作 `modem_t`，不直接调用 AT Engine，不写 AT 指令字符串。Core 通过 `esp_event` 向 App 发布上行事件，同时提供便捷回调注册 API。

### 3.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `lwlte_core_config_t` | 用户 API | App / Board Init | 配置结构体 | Core 创建参数 |
| `lwlte_core_t` | 用户 API (opaque) | App | 句柄 | Core 实例句柄 |
| `lwlte_core_state_t` | 用户 API | App | 状态枚举 | Core 生命周期状态 |
| `lwlte_net_state_t` | 用户 API | App | 状态枚举 | 网络连接状态 |
| `lwlte_core_event_id_t` | 用户 API | App + esp_event | 事件枚举 | Core 上行事件类型，同时作为 esp_event event_id |
| `lwlte_core_event_data_t` | 用户 API | App + esp_event | 值对象 | 事件携带数据 |
| `lwlte_core_event_callback_t` | 用户 API | App | 回调接口 | 便捷回调签名（esp_event 的薄封装） |
| `core_fsm_sig_type_t` | 内部 | Core FSM | 信号枚举 | FSM 内部信号类型 |
| `core_fsm_sig_t` | 内部 | Core FSM | 值对象 | FSM 队列中的信号 |
| `core_fsm_t` | 内部 | Core | 子模块 | FSM 线程 + 队列管理 |
| `net_mgr_step_t` | 内部 | Net Mgr | 状态枚举 | 网络激活子步骤 |
| `net_mgr_t` | 内部 | Core | 子模块 | 网络激活状态机 + 重连定时器 |
| `pdp_mgr_t` | 内部 | Core | 子模块 | PDP context 缓存管理 |

### 3.2 `lwlte_core_config_t` — Core 配置

**所属层**：Core Service
**可见性**：用户 API — Board Init 创建 Core 时填充并传入
**OOP 角色**：配置结构体

```c
typedef struct {
    const char *apn;                     // APN，如 "cmnet"
    uint8_t     primary_cid;             // 主 PDP context ID，默认 1
    uint32_t    net_activate_timeout_ms; // 网络激活总超时，默认 120000
    uint32_t    reconnect_delay_ms;      // 掉线重连固定延迟，默认 5000
    bool        auto_connect;            // 启动后自动激活网络
    int         fsm_queue_size;          // FSM 信号队列长度
    int         fsm_task_stack;          // FSM 任务栈大小
    int         fsm_task_priority;       // FSM 任务优先级
} lwlte_core_config_t;
```

Event loop 参数不放入 config，Core 内部用默认值创建。Modem 引用在 `lwlte_core_create()` 参数中单独传入。

### 3.3 `lwlte_core_t` — Core 句柄

**所属层**：Core Service
**可见性**：用户 API (opaque) — App 持有句柄；struct 定义在 `src/core/core_priv.h`
**OOP 角色**：门面对象，持有该层所有子模块

**公开方法**：

```c
lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 modem_t *modem);
esp_err_t     lwlte_core_destroy(lwlte_core_t *me);
esp_err_t     lwlte_core_start(lwlte_core_t *me);
esp_err_t     lwlte_core_stop(lwlte_core_t *me);

esp_err_t     lwlte_core_register_event_callback(lwlte_core_t *me,
                                                  lwlte_core_event_callback_t callback,
                                                  void *user_ctx);
esp_event_loop_handle_t lwlte_core_get_event_loop(lwlte_core_t *me);

esp_err_t     lwlte_core_get_state(lwlte_core_t *me, lwlte_core_state_t *state);
esp_err_t     lwlte_core_get_net_state(lwlte_core_t *me, lwlte_net_state_t *state);

esp_err_t     lwlte_core_connect(lwlte_core_t *me);
esp_err_t     lwlte_core_disconnect(lwlte_core_t *me);
```

**内部结构**（定义在 `src/core/core_priv.h`）：

```c
struct lwlte_core {
    lwlte_core_config_t      config;
    modem_t                 *modem;
    esp_event_loop_handle_t   event_loop;
    core_fsm_t                fsm;
    net_mgr_t                 net_mgr;
    pdp_mgr_t                 pdp_mgr;
    lwlte_core_state_t       state;
    bool                      destroying;
    SemaphoreHandle_t         lock;
};
```

**关键设计决策**：
- Core 没有 ops 多态——它不面向多种实现，只有一个实现
- `modem` 句柄由 Board Init 传入，Core 不拥有 Modem 生命周期
- `event_loop` 由 Core 在 `lwlte_core_create()` 中创建，在 `destroy` 中删除
- `lock` 只保护 `state`/`destroying` 等短字段访问，FSM 线程调用 `modem_*` API 时不持锁
- App API（`start`、`connect`、`disconnect`）只投递信号到 FSM 队列即返回，不阻塞

### 3.4 `lwlte_core_state_t` — Core 生命周期状态

**所属层**：Core Service
**可见性**：用户 API
**OOP 角色**：状态枚举

```c
typedef enum {
    LWLTE_CORE_STATE_STOPPED = 0,       // 未启动
    LWLTE_CORE_STATE_STARTING,          // 启动中，等待 Modem READY
    LWLTE_CORE_STATE_READY,             // Modem 就绪，可接受 App 请求
    LWLTE_CORE_STATE_NET_ACTIVATING,    // 正在激活网络
    LWLTE_CORE_STATE_ONLINE,            // 网络在线
    LWLTE_CORE_STATE_ERROR,             // 错误，需 App 决策
    LWLTE_CORE_STATE_DESTROYING,        // 正在销毁
} lwlte_core_state_t;
```

**边界说明**：`lwlte_core_state_t` 表示 Core 自身生命周期阶段，不替代 App 的业务状态机。

### 3.5 `lwlte_net_state_t` — 网络连接状态

**所属层**：Core Service
**可见性**：用户 API
**OOP 角色**：状态枚举

```c
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,        // 离线
    LWLTE_NET_STATE_ACTIVATING,         // 激活中
    LWLTE_NET_STATE_ONLINE,             // 在线
    LWLTE_NET_STATE_ERROR,              // 激活失败
} lwlte_net_state_t;
```

**关键设计决策**：`lwlte_core_state_t` 和 `lwlte_net_state_t` 分开——前者是 Core 自身生命周期，后者是纯网络状态。App 可以只关心 `lwlte_net_state_t`。

### 3.6 事件系统 — 基于 esp_event

**所属层**：Core Service
**可见性**：用户 API
**OOP 角色**：事件枚举 + 值对象 + 回调接口

Core 基于 `esp_event` 实现事件分发，不引入自定义链表。

```c
ESP_EVENT_DECLARE_BASE(LWLTE_CORE_EVENT);

typedef enum {
    LWLTE_CORE_EVENT_STARTED = 0,       // Core FSM 已启动
    LWLTE_CORE_EVENT_READY,             // Modem 就绪
    LWLTE_CORE_EVENT_NET_CONNECTING,    // 开始网络激活
    LWLTE_CORE_EVENT_NET_ONLINE,        // 网络已上线
    LWLTE_CORE_EVENT_NET_OFFLINE,       // 网络已掉线
    LWLTE_CORE_EVENT_NET_ERROR,         // 网络激活失败
    LWLTE_CORE_EVENT_STOPPED,           // Core FSM 已停止
    LWLTE_CORE_EVENT_ERROR,             // Core 级错误
} lwlte_core_event_id_t;

typedef struct {
    lwlte_net_state_t net_state;        // 网络状态变化时有效
    int               error_code;        // 错误事件时有效
} lwlte_core_event_data_t;

typedef void (*lwlte_core_event_callback_t)(lwlte_core_t *core,
                                             lwlte_core_event_id_t event_id,
                                             const lwlte_core_event_data_t *data,
                                             void *user_ctx);
```

**关键设计决策**：
- `lwlte_core_register_event_callback()` 是便捷 API，内部通过 `esp_event_handler_register_with()` 注册到 Core 自己的 event loop，用 adapter 做签名转换
- `lwlte_core_get_event_loop()` 暴露 event loop 句柄，允许其他模块（MQTT 等）直接用 `esp_event` API 订阅
- `lwlte_core_event_data_t` 作为 `void *event_data` 传给 `esp_event`，类型安全由 `event_id` 保证

### 3.7 `core_fsm_t` — FSM 子模块

**所属层**：Core Service
**可见性**：内部 — 仅 `src/core/core_fsm.c` 和 `lwlte_core_t` 使用
**OOP 角色**：子模块 — 管理 FSM 线程和信号队列

```c
typedef enum {
    CORE_SIG_MODEM_EVENT = 0,      // Modem 上行事件
    CORE_SIG_START,                // 启动 FSM
    CORE_SIG_STOP,                 // 停止 FSM
    CORE_SIG_NET_ACTIVATE,         // 开始网络激活
    CORE_SIG_NET_DEACTIVATE,       // 去激活网络
    CORE_SIG_NET_STEP_DONE,        // 当前激活步骤完成
    CORE_SIG_NET_STEP_TIMEOUT,     // 当前激活步骤超时
    CORE_SIG_RECONNECT,            // 重连定时器到期
} core_fsm_sig_type_t;

typedef struct {
    core_fsm_sig_type_t  type;
    void                *data;     // 信号附加数据（如 modem_event_t *）
} core_fsm_sig_t;

typedef struct {
    TaskHandle_t         task;
    QueueHandle_t        queue;
    SemaphoreHandle_t    task_done_sema;
    bool                 running;
    bool                 stop_requested;
} core_fsm_t;
```

**关键设计决策**：
- FSM 线程串行处理所有信号，根据 `lwlte_core_state` 分发到对应的处理函数
- `CORE_SIG_MODEM_EVENT` 是 Modem 回调的唯一入口——Modem event_task 回调中只做 `xQueueSend(fsm.queue, &sig, 0)`，不直接推 Core 状态
- 信号内存由发送者分配（栈上），FSM 消费时拷贝用到的 data
- `CORE_SIG_RECONNECT` 由 FreeRTOS software timer 回调发送

### 3.8 `net_mgr_t` — 网络管理器

**所属层**：Core Service
**可见性**：内部 — 仅 `src/core/net_mgr.c` 和 `lwlte_core_t` 使用
**OOP 角色**：子模块 — 网络激活子状态机 + 重连定时器

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
    net_mgr_step_t      current_step;
    uint32_t            step_start_time_ms;
    uint32_t            step_timeout_ms;
    int                 retry_count;
    int                 max_retry;
    TimerHandle_t       reconnect_timer;
    lwlte_net_state_t   state;
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

每个步骤由 `CORE_SIG_NET_STEP_DONE` 驱动推进。FSM 处理函数调用对应的 `modem_*` API（阻塞），完成后向自己队列发送 `CORE_SIG_NET_STEP_DONE`。步骤超时发 `CORE_SIG_NET_STEP_TIMEOUT`，重试计数超过 `max_retry` 后进入 `NET_STEP_ERROR`，Core 发布 `LWLTE_CORE_EVENT_NET_ERROR`。

**重连逻辑**：
- 收到 `MODEM_EVENT_PDP_DEACTIVATED` → `net_state = OFFLINE` → 发布 `LWLTE_CORE_EVENT_NET_OFFLINE` → 启动 `reconnect_timer`（固定 `reconnect_delay_ms`）
- 定时器回调发送 `CORE_SIG_RECONNECT` → FSM 重新触发网络激活流程

第一版只做固定延迟重连，不做指数退避。保活机制后续版本再加。

### 3.9 `pdp_mgr_t` — PDP 管理器

**所属层**：Core Service
**可见性**：内部 — 仅 `src/core/pdp_mgr.c` 和 `lwlte_core_t` 使用
**OOP 角色**：子模块 — PDP context 缓存

```c
#define CORE_MAX_PDP_CONTEXTS 4

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t             primary_cid;
} pdp_mgr_t;
```

第一版仅做 PDP context 缓存，提供 `pdp_mgr_get()` / `pdp_mgr_update()` 两个内部接口。后续加多 PDP 管理时在这里扩展。

### 3.10 Core 线程模型

```
┌─────────────────────────────────────────────────────────────┐
│                     Core Service 线程模型                    │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  App 线程 / App task                                        │
│  ┌────────────────┐                                         │
│  │ lwlte_core_*   │──→ 参数/状态检查                         │
│  └───────┬────────┘    ──→ 获取 core->lock                  │
│          │          ──→ 投递 SIG 到 fsm.queue               │
│          │          ──→ 释放 lock → 返回                     │
│          │          ★ App 调用不阻塞在 modem 操作上          │
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
│  │ esp_event loop │──→ 分发到已注册的 handler               │
│  └────────────────┘    ★ 便捷回调 + esp_event 订阅者        │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**硬约束**：

| 约束 | 说明 |
|------|------|
| Modem 回调不调 Core API | Modem event_task 中只 `xQueueSend` 到 FSM 队列，与 Modem 层"URC handler 不调 Core 回调"约束一致 |
| FSM 线程不持锁跨阻塞 | `core->lock` 只保护短字段，调用 `modem_*` API 时不持锁 |
| esp_event_post_to 不阻塞 | 投递事件不阻塞 FSM 线程 |
| App API 不阻塞 | `start`/`connect`/`disconnect` 只投递信号即返回 |

**Modem 事件 → Core 行为映射**：

| Modem Event | Core 行为 |
|-------------|----------|
| `MODEM_EVENT_READY` | core_state → READY，发布 `CORE_EVENT_READY`，若 auto_connect 则启动网络激活 |
| `MODEM_EVENT_SIM_CHANGED` | 更新 net_mgr 可用的 SIM 状态 |
| `MODEM_EVENT_REG_CHANGED` | 更新 net_mgr 可用的注册状态 |
| `MODEM_EVENT_PDP_ACTIVATED` | net_state → ONLINE，发布 `CORE_EVENT_NET_ONLINE` |
| `MODEM_EVENT_PDP_DEACTIVATED` | net_state → OFFLINE，发布 `CORE_EVENT_NET_OFFLINE`，启动重连定时器 |
| `MODEM_EVENT_ERROR` | 根据 error_code 决定重试或进入 ERROR 状态 |

### 3.11 初始化与装配

```c
/* Board Init — Core 不依赖具体模块型号 */

lwlte_core_config_t core_cfg = {
    .apn                     = "cmnet",
    .primary_cid             = 1,
    .net_activate_timeout_ms = 120000,
    .reconnect_delay_ms      = 5000,
    .auto_connect            = true,
    .fsm_queue_size          = 16,
    .fsm_task_stack          = 4096,
    .fsm_task_priority       = 8,
};

lwlte_core_t *core = lwlte_core_create(&core_cfg, modem);

/* App 注册便捷回调 */
lwlte_core_register_event_callback(core, app_event_handler, NULL);

/* 其他模块（MQTT 等）可通过 esp_event 订阅 */
esp_event_handler_register_with(
    lwlte_core_get_event_loop(core),
    LWLTE_CORE_EVENT, ESP_EVENT_ANY_ID,
    mqtt_event_handler, NULL
);

lwlte_core_start(core);  // 异步，结果通过事件通知
```

**关键设计决策**：
- `lwlte_core_create()` 接收 `modem_t *`，和 `modem_air780ep_create()` 接收 `at_engine_t *` 的模式一致
- Core 不 include 具体模块头文件，只认识 `modem_t`
- 换模块时 Core 代码零改动

**错误处理规则**：
- Core 公开 API 统一返回 `esp_err_t`
- 参数错误返回 `ESP_ERR_INVALID_ARG`
- 状态错误返回 `ESP_ERR_INVALID_STATE`
- Core 不新增自定义错误码，统一使用 ESP-IDF 标准错误码
- Modem 层返回的错误直接向上传播

**与 Modem 层的边界**：
- Core 可以调用 `modem_*` 包装 API，因为 Modem 是紧邻下层
- Core 不能调用 AT Engine API
- Core 不 include `modem_air780ep.h`
- Core 通过 `modem_register_event_callback()` 接收 Modem 上行事件，内部回调只投递 FSM 信号

---
```

- [ ] **Step 2: 验证 classes.md 格式一致**

检查：
- 标题层级与已有 AT Engine / Modem Adapter 部分一致
- 代码块使用 ```c 标记
- 表格格式正确
- 枚举值有 `/**< 中文； English */` 行尾注释

- [ ] **Step 3: 提交**

```bash
git add docs/agents/classes.md
git commit -m "docs: add Core Service class design to classes.md"
```
