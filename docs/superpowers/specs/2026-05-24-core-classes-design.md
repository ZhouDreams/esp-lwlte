# Core Service 类设计

## 设计决策摘要

| 决策 | 选择 |
|------|------|
| 边界 | 核心网络管理（网络状态机、PDP 管理、重连）。MQTT/HTTP 独立模块 |
| 线程模型 | FSM 线程 + 事件队列 |
| 状态机 | 分层：外层生命周期 + 内层网络激活子状态机 |
| 事件机制 | esp_event 作为基础设施 + 便捷回调注册 API |
| 重连策略 | 基础重连（固定延迟），保活后续版本 |
| 代码组织 | 子模块分解（core_fsm / net_mgr / pdp_mgr） |

---

## 1. 类总览

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

---

## 2. 用户 API 类型

### 2.1 `lwlte_core_config_t`

```c
typedef struct {
    const char *apn;                    // APN，如 "cmnet"
    uint8_t     primary_cid;            // 主 PDP context ID，默认 1
    uint32_t    net_activate_timeout_ms; // 网络激活总超时，默认 120000
    uint32_t    reconnect_delay_ms;      // 掉线重连固定延迟，默认 5000
    bool        auto_connect;            // 启动后自动激活网络
    int         fsm_queue_size;          // FSM 信号队列长度
    int         fsm_task_stack;          // FSM 任务栈大小
    int         fsm_task_priority;       // FSM 任务优先级
} lwlte_core_config_t;
```

Event loop 参数不放入 config，Core 内部用默认值创建。

### 2.2 `lwlte_core_state_t`

```c
typedef enum {
    LWLTE_CORE_STATE_STOPPED = 0,       // 未启动
    LWLTE_CORE_STATE_STARTING,          // 启动中，等待 Modem READY
    LWLTE_CORE_STATE_READY,             // Modem 就绪
    LWLTE_CORE_STATE_NET_ACTIVATING,    // 正在激活网络
    LWLTE_CORE_STATE_ONLINE,            // 网络在线
    LWLTE_CORE_STATE_ERROR,             // 错误
    LWLTE_CORE_STATE_DESTROYING,        // 正在销毁
} lwlte_core_state_t;
```

### 2.3 `lwlte_net_state_t`

```c
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,        // 离线
    LWLTE_NET_STATE_ACTIVATING,         // 激活中
    LWLTE_NET_STATE_ONLINE,             // 在线
    LWLTE_NET_STATE_ERROR,              // 激活失败
} lwlte_net_state_t;
```

`lwlte_core_state_t` 和 `lwlte_net_state_t` 分开——前者是 Core 自身生命周期，后者是纯网络状态。

### 2.4 事件系统

基于 `esp_event`，不引入自定义链表。

```c
ESP_EVENT_DECLARE_BASE(LWLTE_CORE_EVENT);

typedef enum {
    LWLTE_CORE_EVENT_STARTED = 0,
    LWLTE_CORE_EVENT_READY,
    LWLTE_CORE_EVENT_NET_CONNECTING,
    LWLTE_CORE_EVENT_NET_ONLINE,
    LWLTE_CORE_EVENT_NET_OFFLINE,
    LWLTE_CORE_EVENT_NET_ERROR,
    LWLTE_CORE_EVENT_STOPPED,
    LWLTE_CORE_EVENT_ERROR,
} lwlte_core_event_id_t;

typedef struct {
    lwlte_net_state_t net_state;        // 网络状态变化时有效
    int               error_code;        // 错误事件时有效
} lwlte_core_event_data_t;
```

### 2.5 便捷回调

```c
typedef void (*lwlte_core_event_callback_t)(lwlte_core_t *core,
                                             lwlte_core_event_id_t event_id,
                                             const lwlte_core_event_data_t *data,
                                             void *user_ctx);
```

内部通过 `esp_event_handler_register_with()` 注册到 Core 的 event loop，用一个 adapter 做签名转换。

### 2.6 公开方法

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

`lwlte_core_get_event_loop()` 暴露 event loop 句柄，允许其他模块直接用 `esp_event` API 订阅。

---

## 3. 内部结构

### 3.1 `lwlte_core_t` — 门面对象

定义在 `src/core/core_priv.h`，App 不可见。

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

### 3.2 `core_fsm_t` — FSM 子模块

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
    void                *data;     // 信号附加数据
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
- FSM 线程串行处理所有信号，根据 `lwlte_core_state` 分发
- `CORE_SIG_MODEM_EVENT` 是 Modem 回调的唯一入口——Modem event_task 中只发信号
- 信号内存由发送者分配（栈上），FSM 消费后不负责释放 data 指向的外部内存
- `CORE_SIG_RECONNECT` 由 FreeRTOS software timer 回调发送

### 3.3 `net_mgr_t` — 网络管理器

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

**重连逻辑**：
- 收到 `MODEM_EVENT_PDP_DEACTIVATED` → `net_state = OFFLINE` → 发布 `CORE_EVENT_NET_OFFLINE` → 启动 `reconnect_timer`
- 定时器到期 → 发送 `CORE_SIG_RECONNECT` → FSM 重新触发网络激活

### 3.4 `pdp_mgr_t` — PDP 管理器

```c
#define CORE_MAX_PDP_CONTEXTS 4

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t             primary_cid;
} pdp_mgr_t;
```

第一版仅做 PDP context 缓存，提供 `pdp_mgr_get()` / `pdp_mgr_update()` 内部接口。

---

## 4. 线程模型

```
App 线程
  └─ lwlte_core_start/connect/stop → 投递 SIG 到 fsm.queue → 立即返回

Modem event task
  └─ core_modem_event_cb → xQueueSend(fsm.queue, &sig, 0)
     ★ 只投递，不推状态，不调 Core API

Core FSM task
  └─ xQueueReceive → 分发信号 → 调 modem_* API（阻塞）
     → 状态变化 → esp_event_post_to(event_loop, ...)

Core event loop task (esp_event 内部)
  └─ 分发到已注册的 handler（便捷回调 + 其他 esp_event 订阅者）
```

**硬约束**：
- Modem 回调不调 Core API — 只 `xQueueSend`
- FSM 线程不持 `core->lock` 跨阻塞调用
- `esp_event_post_to` 不阻塞 FSM 线程
- App API 不阻塞 — 投递信号即返回

**Modem 事件 → Core 行为映射**：

| Modem Event | Core 行为 |
|-------------|----------|
| `MODEM_EVENT_READY` | core_state → READY，发布 `CORE_EVENT_READY`，若 auto_connect 则启动网络激活 |
| `MODEM_EVENT_SIM_CHANGED` | 更新 net_mgr 可用的 SIM 状态 |
| `MODEM_EVENT_REG_CHANGED` | 更新 net_mgr 可用的注册状态 |
| `MODEM_EVENT_PDP_ACTIVATED` | net_state → ONLINE，发布 `CORE_EVENT_NET_ONLINE` |
| `MODEM_EVENT_PDP_DEACTIVATED` | net_state → OFFLINE，发布 `CORE_EVENT_NET_OFFLINE`，启动重连 |
| `MODEM_EVENT_ERROR` | 根据 error_code 决定重试或进入 ERROR 状态 |

---

## 5. 初始化与装配

```c
/* Board Init */
modem_t *modem = modem_air780ep_create(at, &modem_cfg);
modem_init(modem);

lwlte_core_config_t core_cfg = {
    .apn                    = "cmnet",
    .primary_cid            = 1,
    .net_activate_timeout_ms = 120000,
    .reconnect_delay_ms     = 5000,
    .auto_connect           = true,
    .fsm_queue_size         = 16,
    .fsm_task_stack         = 4096,
    .fsm_task_priority      = 8,
};

lwlte_core_t *core = lwlte_core_create(&core_cfg, modem);

// App 注册便捷回调
lwlte_core_register_event_callback(core, app_event_handler, NULL);

// 其他模块可通过 esp_event 订阅
esp_event_handler_register_with(
    lwlte_core_get_event_loop(core),
    LWLTE_CORE_EVENT, ESP_EVENT_ANY_ID,
    mqtt_event_handler, NULL
);

lwlte_core_start(core);  // 异步，结果通过事件通知
```

---

## 6. 与旧架构的关键区别

| 旧架构 | 新架构 |
|--------|--------|
| 全局单例 `s_ctx` | `lwlte_core_create()` 返回句柄，支持多实例 |
| Core 直接发 AT 命令、处理 UART | Core 只调 `modem_*` API，不碰 AT |
| AT manager、URC manager 内嵌在 Core | AT/URC 在 AT Engine 和 Modem 层，Core 不可见 |
| 自定义 event loop | 基于 `esp_event` |
| Core 认识模块型号 | Core 只认识 `modem_t` |
| 内置 MQTT（example 层面） | MQTT 作为独立模块，通过 esp_event 订阅 Core 事件 |
| ping test 内嵌在 net stepper | 第一版不包含 ping test |
| flags 位掩码 + state 枚举双轨 | 分层状态枚举（core_state + net_state），signals 驱动 |
