# MQTT Client Service Classes Doc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update `docs/agents/classes.md` so the MQTT Client Service is designed between Core Service and App, with an independent FSM and a Core command queue boundary.

**Architecture:** MQTT Client Service is a service-layer object above Core. MQTT owns its FSM and events, depends on Core for network state and command submission, and never calls Modem or AT Engine directly. Core gains typed protocol command objects so upper services can enqueue MQTT commands while Core remains the only service that calls `modem_*`.

**Tech Stack:** Markdown documentation, ESP-IDF naming conventions, FreeRTOS task/queue concepts, `esp_event`, existing esp-lwlte C OOP documentation style.

---

## File Structure

- Modify: `docs/agents/classes.md`
- Read-only source: `docs/superpowers/specs/2026-05-27-mqtt-client-service-design.md`
- Do not modify: `.gitignore`
- Do not modify: `docs/agents/at_cmd_air780ep.md`

`docs/agents/classes.md` remains the canonical class design document. The approved spec is the source of the design decisions, but the implementation result belongs in `classes.md` with that file's existing Chinese prose, table format, and section numbering style.

---

### Task 1: Update Visibility And Core Command Surface

**Files:**
- Modify: `docs/agents/classes.md:5-16`
- Modify: `docs/agents/classes.md:769-832`

- [ ] **Step 1: Update the visibility table row**

Replace the current layer API row with this row:

```markdown
| 层间 API | `src/core/core.h`、`src/mqtt_client/mqtt_client.h`、`src/modem/modem.h`、`src/modem/modem_air780ep.h`、`src/at_engine/at_engine.h` | 组件内部相邻层；Facade factory 作为 composition root 可见全部装配 API | `core_`、`mqtt_client_`、`modem_`、`modem_air780ep_`、`at_engine_` |
```

- [ ] **Step 2: Update the visibility explanation**

Replace the sentence that says AT Engine, Modem, and Core have no user API with this sentence:

```markdown
**核心区别**：用户 API 是给 App 开发者用的，层间 API 是层与层之间、以及 Facade 模块 factory 装配时用的。AT Engine、Modem、Core 和 MQTT Client Service 都没有任何用户 API——它们被 LWLTE Facade 封装，最终用户看不到它们的存在。
```

- [ ] **Step 3: Add Core command queue classes to the Core overview table**

In the Core Service `### 3.1 类总览` table, add these rows after `core_event_callback_t` and before `core_fsm_sig_type_t`:

```markdown
| `core_cmd_type_t` | 层间 API | MQTT/TCP/HTTP 等上层 service + Core 内部 | 命令枚举 | 上层 service 投递给 Core 的协议命令类型 |
| `core_cmd_result_t` | 层间 API | MQTT/TCP/HTTP 等上层 service + Core 内部 | 结果枚举 | Core command 执行结果 |
| `core_cmd_t` | 层间 API | MQTT/TCP/HTTP 等上层 service + Core 内部 | 值对象 | 上层 service 投递到 Core 的 typed command request |
| `core_cmd_done_callback_t` | 层间 API | MQTT/TCP/HTTP 等上层 service + Core 内部 | 回调接口 | Core command 完成后回调上层 service，用于投递上层 FSM 信号 |
```

- [ ] **Step 4: Add `core_submit_cmd()` to the Core method list**

In the Core Service `core_t` layer API method code block, add this prototype after `core_disconnect(core_t *me);`:

```c
esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd);
```

- [ ] **Step 5: Add the Core command queue section**

Insert this section after `### 3.4 Core 状态和事件类型` and before the existing `### 3.5 core_fsm_t` section:

````markdown
### 3.5 Core command queue 类型

**所属层**：Core Service
**可见性**：层间 API — `src/core/core.h`，供 MQTT/TCP/HTTP 等上层 service 使用
**OOP 角色**：命令枚举 + 结果枚举 + 值对象 + 回调接口

Core command queue 是 Core 暴露给上层 service 的 typed command 入口。MQTT Client Service 通过 `core_submit_cmd()` 投递 MQTT 模块命令；Core FSM 串行执行这些命令，并在命令完成后通过 callback 把结果交还给上层 service。Core 仍然是唯一运行期调用 `modem_*` 的 service；MQTT/TCP/HTTP 不直接调用 Modem 或 AT Engine。

```c
typedef enum {
    CORE_CMD_MQTT_CONFIG = 0,
    CORE_CMD_MQTT_OPEN,
    CORE_CMD_MQTT_LOGIN,
    CORE_CMD_MQTT_DISCONNECT,
    CORE_CMD_MQTT_SUBSCRIBE,
    CORE_CMD_MQTT_UNSUBSCRIBE,
    CORE_CMD_MQTT_PUBLISH,
} core_cmd_type_t;

typedef enum {
    CORE_CMD_RESULT_OK = 0,
    CORE_CMD_RESULT_ERROR,
    CORE_CMD_RESULT_TIMEOUT,
    CORE_CMD_RESULT_INVALID_RESPONSE,
} core_cmd_result_t;

typedef void (*core_cmd_done_callback_t)(core_t *core,
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
        } mqtt_config;

        struct {
            const char *host;
            uint16_t port;
        } mqtt_open;

        struct {
            bool clean_session;
            uint16_t keepalive_s;
        } mqtt_login;

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
    } data;
} core_cmd_t;
```

**关键设计决策**：
- `core_submit_cmd()` 复制异步执行所需的字符串和 payload；调用方传入的指针只需在调用期间有效。
- Core FSM 是 command 的唯一执行位置，执行 command 时可以调用 `modem_*` API。
- `done_cb` 必须短小非阻塞；MQTT 的 `done_cb` 只投递 `MQTT_SIG_CORE_CMD_DONE` 到 MQTT FSM 队列，不直接修改 MQTT 状态。
- Core 调用 `done_cb` 时不得持有 `core->lock`。
- `core_cmd_t` 是内部 service 层命令对象，不是 App 用户 API；App 不 include `core.h`，也不直接调用 `core_submit_cmd()`。
````

- [ ] **Step 6: Renumber the later Core subsections**

Apply these exact heading replacements in `docs/agents/classes.md`:

```text
### 3.5 `core_fsm_t` — FSM 组件 -> ### 3.6 `core_fsm_t` — FSM 组件
### 3.6 `net_mgr_t` — 网络管理组件 -> ### 3.7 `net_mgr_t` — 网络管理组件
### 3.7 `pdp_mgr_t` — PDP 管理组件 -> ### 3.8 `pdp_mgr_t` — PDP 管理组件
### 3.8 Core 线程模型 -> ### 3.9 Core 线程模型
### 3.9 初始化与装配 -> ### 3.10 初始化与装配
```

- [ ] **Step 7: Verify Core command surface appears in the document**

Run: `rg "core_submit_cmd|core_cmd_t|core_cmd_done_callback_t|### 3\.5 Core command queue" docs/agents/classes.md`

Expected: output contains all four patterns in `docs/agents/classes.md`.

---

### Task 2: Insert MQTT Client Service Section

**Files:**
- Modify: `docs/agents/classes.md:1113-1117`

- [ ] **Step 1: Insert the MQTT section before App**

Insert the new MQTT section immediately before the current `## 4. App（应用层）` heading.

Use this exact top-level heading and opening paragraph:

```markdown
## 4. MQTT Client Service（MQTT 客户端服务层）

MQTT Client Service 是 Core 之上的独立 service，负责 MQTT 连接、订阅、取消订阅、发布和下行数据事件。它拥有自己的 FSM task、FSM queue 和 MQTT 事件；它依赖 Core 的网络状态、Core event loop 和 Core command queue；它不直接调用 Modem Adapter 或 AT Engine。

MQTT 可以直接使用 ESP-IDF / FreeRTOS API，例如 `xTaskCreate()`、`xQueueCreate()`、`xQueueSend()`、software timer 和 `esp_event`。这不改变层间调用规则：MQTT 运行期只能调用 Core 层间 API，不能 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。
```

- [ ] **Step 2: Add MQTT class overview**

Add this `### 4.1 类总览` table after the opening paragraph:

```markdown
### 4.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `mqtt_client_config_t` | 层间 API | Facade 模块 factory | 配置结构体 | Broker、client_id、认证、keepalive、FSM 参数 |
| `mqtt_client_t` | 层间 API (opaque) | Facade | service 句柄 | MQTT Client Service 实例 |
| `mqtt_client_transport_t` | 层间 API | Facade + MQTT 内部 | 枚举 | MQTT 传输类型，第一版只支持 Plain TCP |
| `mqtt_client_state_t` | 层间 API | Facade + MQTT 内部 | 状态枚举 | MQTT 生命周期和连接状态 |
| `mqtt_client_event_id_t` | 层间 API | Facade + esp_event | 事件枚举 | MQTT 上行事件类型，同时作为 esp_event event_id |
| `mqtt_client_event_data_t` | 层间 API | Facade | 值对象 | MQTT 事件数据 |
| `mqtt_client_publish_t` | 层间 API | Facade | 值对象 | 发布请求 |
| `mqtt_client_msg_t` | 层间 API | Facade | 值对象 | 收到的 MQTT 消息 |
| `mqtt_client_operation_t` | 层间 API | Facade + MQTT 内部 | 枚举 | MQTT 操作类型，用于操作完成事件 |
| `mqtt_fsm_sig_type_t` | 模块私有 API | MQTT FSM | 信号枚举 | MQTT FSM 内部信号类型 |
| `mqtt_fsm_sig_t` | 模块私有 API | MQTT FSM | 值对象 | MQTT FSM 队列中的信号 |
| `mqtt_connect_step_t` | 模块私有 API | MQTT FSM | 状态枚举 | MQTT 连接子状态机步骤 |
| `mqtt_pending_cmd_t` | 模块私有 API | MQTT FSM | 工作上下文 | 正在等待 Core command 结果的命令上下文 |
```

- [ ] **Step 3: Add MQTT config and service handle subsections**

Add these subsections after the class overview:

````markdown
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
    const char *client_id;               // MQTT client id
    const char *username;                // 用户名，可为 NULL
    const char *password;                // 密码，可为 NULL
    uint16_t keepalive_s;                // keepalive 秒数，0 使用默认值
    bool clean_session;                  // clean session 标志
    int fsm_queue_size;                  // MQTT FSM 队列长度
    int fsm_task_stack;                  // MQTT FSM task 栈大小
    int fsm_task_priority;               // MQTT FSM task 优先级
} mqtt_client_config_t;
```

`mqtt_client_create()` 会复制需要长期保存的字符串。`MQTT_CLIENT_TRANSPORT_TLS` 作为类型预留，第一版返回 `ESP_ERR_NOT_SUPPORTED`。

### 4.3 `mqtt_client_t` — MQTT 客户端句柄

**所属层**：MQTT Client Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/mqtt_client/mqtt_client_priv.h` 或 `.c` 中
**OOP 角色**：service 对象，组合持有 MQTT FSM、事件和 Core 依赖

**层间方法**（`src/mqtt_client/mqtt_client.h`）：

```c
mqtt_client_t *mqtt_client_create(const mqtt_client_config_t *config,
                                  core_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_t *me);
esp_err_t mqtt_client_start(mqtt_client_t *me);
esp_err_t mqtt_client_stop(mqtt_client_t *me);

esp_err_t mqtt_client_register_event_callback(mqtt_client_t *me,
                                              mqtt_client_event_callback_t callback,
                                              void *user_ctx);
esp_event_loop_handle_t mqtt_client_get_event_loop(mqtt_client_t *me);

esp_err_t mqtt_client_get_state(mqtt_client_t *me,
                                mqtt_client_state_t *state);
esp_err_t mqtt_client_subscribe(mqtt_client_t *me,
                                const char *topic,
                                uint8_t qos);
esp_err_t mqtt_client_unsubscribe(mqtt_client_t *me,
                                  const char *topic);
esp_err_t mqtt_client_publish(mqtt_client_t *me,
                              const mqtt_client_publish_t *request);
```

**关键内部字段类别**：
- `config`：配置快照，包含复制后的 host、client_id、username、password。
- `core`：Facade factory 注入的 `core_t` 句柄，MQTT 借用但不拥有生命周期。
- `event_loop`、`event_callback`、`event_user_ctx`：MQTT 事件分发和 Facade 桥接回调。
- `fsm_task`、`fsm_queue`、`fsm_task_done_sema`：MQTT 独立状态机线程和信号队列。
- `state`、`connect_step`、`pending_cmd`：MQTT 生命周期、连接子步骤和等待中的 Core command。
- `lock`、`destroying`、`started`、`net_online`：短状态字段和销毁保护。

**关键设计决策**：
- MQTT 没有 ops 多态；第一版只有一个 MQTT service 实现。
- MQTT 不是 Core 的子类，不能向上转型为 `core_t *`。
- MQTT 不保存 `modem_t *`、`at_engine_t *` 或具体模块句柄。
- `lock` 只保护短字段，MQTT FSM 调用 `core_submit_cmd()` 时不持锁。
````

- [ ] **Step 4: Add MQTT state, event, signal, and pending command subsections**

Add these subsections after `### 4.3`:

````markdown
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

ESP_EVENT_DECLARE_BASE(MQTT_CLIENT_EVENT);

typedef enum {
    MQTT_CLIENT_EVENT_STARTED = 0,
    MQTT_CLIENT_EVENT_STOPPED,
    MQTT_CLIENT_EVENT_CONNECTING,
    MQTT_CLIENT_EVENT_CONNECTED,
    MQTT_CLIENT_EVENT_DISCONNECTED,
    MQTT_CLIENT_EVENT_SUBSCRIBED,
    MQTT_CLIENT_EVENT_UNSUBSCRIBED,
    MQTT_CLIENT_EVENT_PUBLISHED,
    MQTT_CLIENT_EVENT_DATA,
    MQTT_CLIENT_EVENT_ERROR,
} mqtt_client_event_id_t;

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
} mqtt_client_msg_t;

typedef struct {
    mqtt_client_state_t state;
    int error_code;
    union {
        mqtt_client_operation_t operation;
        mqtt_client_msg_t msg;
    } data;
} mqtt_client_event_data_t;

typedef void (*mqtt_client_event_callback_t)(mqtt_client_t *client,
                                             mqtt_client_event_id_t event_id,
                                             const mqtt_client_event_data_t *data,
                                             void *user_ctx);
```

`mqtt_client_msg_t` 中的指针只在 MQTT 事件回调期间有效；Facade 若要把数据继续传给用户异步保存，必须复制 topic 和 payload。

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
    MQTT_CONNECT_STEP_CONFIG,
    MQTT_CONNECT_STEP_OPEN,
    MQTT_CONNECT_STEP_LOGIN,
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
````

- [ ] **Step 5: Add Core boundary, flow, URC, thread, error, and dependency subsections**

Add these subsections after `### 4.6`:

````markdown
### 4.7 Core command queue 边界

MQTT 所有模块命令都通过 `core_submit_cmd()` 投递给 Core。MQTT 不生成 AT 字符串，不调用 `modem_*`，也不注册 AT Engine URC。Core command queue 的作用是把 MQTT/TCP/HTTP 等上层 service 的业务命令串行化到 Core FSM，再由 Core 调用 Modem Adapter。

| MQTT 操作 | Core command | Air780EP 第一版底层命令 |
|-----------|--------------|--------------------------|
| 配置 MQTT 参数 | `CORE_CMD_MQTT_CONFIG` | `AT+MCONFIG` |
| 打开 MQTT TCP 通道 | `CORE_CMD_MQTT_OPEN` | `AT+MIPSTART`，成功接受 `CONNECT OK` / `ALREADY CONNECT` |
| MQTT 登录 | `CORE_CMD_MQTT_LOGIN` | `AT+MCONNECT`，成功接受 `CONNACK OK` |
| 断开 MQTT | `CORE_CMD_MQTT_DISCONNECT` | `AT+MDISCONNECT` |
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
            └─ CONFIG → OPEN → LOGIN → CONNECTED
```

```text
CONNECTED
  └─ CORE_EVENT_NET_OFFLINE
      ├─ 清除 connected 状态
      ├─ 发布 MQTT_CLIENT_EVENT_DISCONNECTED
      └─ WAITING_NET
```

```text
CONNECTED 或 transport 已打开
  └─ mqtt_client_stop()
      ├─ 提交 CORE_CMD_MQTT_DISCONNECT
      ├─ 等待 command 完成或 stop 超时
      ├─ 注销 Core event handler
      ├─ 停止 MQTT FSM task
      └─ 发布 MQTT_CLIENT_EVENT_STOPPED
```

第一版不隐藏缓存 publish/subscribe/unsubscribe 请求。MQTT 未连接时，这些 API 返回 `ESP_ERR_INVALID_STATE`。

### 4.9 MQTT URC / 数据上行路径

Air780EP 第一版使用 `+MSUB:` 作为 MQTT 下行数据 URC。新的依赖方向不允许 MQTT 直接注册 AT Engine URC handler，数据上行路径如下：

```text
AT Engine RX task
  └─ Modem Air780EP URC handler
       └─ 解析 +MSUB: 为 modem_event_t
            └─ Modem event_task 调用 Core 回调
                 └─ Core FSM 发布 CORE_EVENT_PROTOCOL_DATA
                      └─ MQTT core_event_handler 深拷贝 topic/payload
                           └─ xQueueSend(mqtt.fsm_queue)
                                └─ MQTT FSM 发布 MQTT_CLIENT_EVENT_DATA
```

Core protocol event 数据使用回调期间有效的指针，MQTT service 入队前必须复制 topic 和 payload。

### 4.10 MQTT 线程模型

```text
Facade/App task
  └─ mqtt_client_start/publish/subscribe
       └─ 参数检查 + 请求深拷贝
            └─ xQueueSend(mqtt.fsm_queue)

Core event loop task
  └─ CORE_EVENT_NET_ONLINE / OFFLINE / PROTOCOL_DATA
       └─ MQTT core_event_handler
            └─ 深拷贝必要数据
                 └─ xQueueSend(mqtt.fsm_queue)

MQTT FSM task
  └─ 串行处理 MQTT 信号
       ├─ CONNECTING: submit CORE_CMD_MQTT_CONFIG/OPEN/LOGIN
       ├─ CONNECTED: submit publish/subscribe/unsubscribe
       └─ 状态变化后 post MQTT_CLIENT_EVENT

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
- `mqtt_client_stop()` 在连接已建立或 transport 已打开时提交 `CORE_CMD_MQTT_DISCONNECT`，并且必须有超时兜底。

### 4.11 错误处理规则

- MQTT 层间 API 统一返回 `esp_err_t` 或 NULL 句柄。
- 参数错误返回 `ESP_ERR_INVALID_ARG`。
- 生命周期错误、未连接时发布/订阅/取消订阅返回 `ESP_ERR_INVALID_STATE`。
- 队列提交失败返回 `ESP_FAIL`。
- Core command 超时映射为 `ESP_ERR_TIMEOUT`。
- 第一版 TLS 返回 `ESP_ERR_NOT_SUPPORTED`。
- 协议数据或响应格式异常返回 `ESP_ERR_INVALID_RESPONSE`。
- MQTT 保存最近一次错误码，并通过 `MQTT_CLIENT_EVENT_ERROR` 上报。

### 4.12 与 Core / Modem / AT Engine 的边界

- MQTT 可以调用 `core_get_event_loop()`、`core_get_net_state()` 和 `core_submit_cmd()`，因为 Core 是 MQTT 的直接依赖。
- MQTT 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。
- MQTT 不直接调用 `modem_*`、`at_engine_*` 或具体 Air780EP helper。
- MQTT 不注册 AT Engine URC handler；MQTT 数据 URC 经 Modem → Core → MQTT 上行。
- Core 仍只负责网络状态机、PDP、重连和命令串行化，不持有 MQTT 业务状态机。
- Facade 是 composition root，负责创建 Core 和 MQTT，并把 MQTT 事件翻译为用户 API 事件。
````

- [ ] **Step 6: Verify MQTT section appears in the document**

Run: `rg "## 4\. MQTT Client Service|mqtt_client_create|MQTT_SIG_CORE_CMD_DONE|CORE_CMD_MQTT_PUBLISH|\+MSUB:" docs/agents/classes.md`

Expected: output contains all five patterns in `docs/agents/classes.md`.

---

### Task 3: Renumber App And Verify Document Consistency

**Files:**
- Modify: `docs/agents/classes.md:1115-1117`

- [ ] **Step 1: Renumber App section**

Replace this heading:

```markdown
## 4. App（应用层）
```

with this heading:

```markdown
## 5. App（应用层）
```

- [ ] **Step 2: Verify old App heading is gone**

Run: `rg "## 4\. App|## 5\. App" docs/agents/classes.md`

Expected: output contains `## 5. App（应用层）` and does not contain `## 4. App（应用层）`.

- [ ] **Step 3: Verify MQTT dependency boundaries are explicit**

Run: `rg "不 include `modem\.h`|不直接调用 `modem_\*`|不注册 AT Engine URC|core_submit_cmd\(\)" docs/agents/classes.md`

Expected: output contains each boundary phrase in the MQTT section.

- [ ] **Step 4: Verify no unresolved marker text was introduced**

Run: `rg "\x54\x42\x44|\x54\x4f\x44\x4f|\x3f\x3f" docs/agents/classes.md docs/superpowers/plans/2026-05-27-mqtt-client-service-classes-doc-plan.md`

Expected: no output.

- [ ] **Step 5: Review the final diff**

Run: `git diff -- docs/agents/classes.md`

Expected: diff modifies only `docs/agents/classes.md`, adds MQTT Client Service between Core and App, extends Core with command queue types, and renumbers App to section 5.

---

## Self-Review Checklist

- Spec coverage: The plan covers visibility table updates, Core command queue additions, MQTT service section insertion, App renumbering, dependency boundaries, URC flow, thread model, and error handling from `docs/superpowers/specs/2026-05-27-mqtt-client-service-design.md`.
- Placeholder scan: The plan uses concrete file paths, concrete headings, concrete Markdown snippets, and concrete verification commands.
- Type consistency: MQTT types use `mqtt_client_` prefix for layer API and `mqtt_` prefix for private FSM helpers; Core command types use `core_cmd_` prefix.
- Commit policy: Do not commit plan or documentation changes unless the user explicitly asks for a commit.
