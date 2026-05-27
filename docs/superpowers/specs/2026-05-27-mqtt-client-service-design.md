# MQTT Client Service 类设计

## 设计决策摘要

| 决策 | 选择 |
|------|------|
| 插入位置 | `docs/agents/classes.md` 的 Core Service 与 App 之间，作为新的 MQTT Client Service 章节 |
| 依赖方向 | MQTT Client Service 依赖 Core，不依赖 Modem 或 AT Engine |
| 下行命令 | MQTT 通过 Core command queue 投递 typed command request |
| 状态机 | MQTT 拥有独立 FSM task 和 FSM queue |
| 上行事件 | MQTT 订阅 Core event loop，收到网络状态和 protocol data 后投递到 MQTT FSM |
| ESP-IDF 使用 | MQTT service 可以直接使用 FreeRTOS task/queue/timer 和 `esp_event` |
| 第一版传输 | Plain TCP MQTT；TLS 返回 `ESP_ERR_NOT_SUPPORTED` |

---

## 1. 目标与边界

MQTT Client Service 是 Core 之上的独立 service。它拥有自己的状态机、队列和事件；它依赖 Core 的网络状态、事件循环和 command queue；它不直接调用 Modem Adapter 或 AT Engine。所有 MQTT 模块命令都通过 `core_submit_cmd()` 投递给 Core，由 Core 串行调用 Modem 层完成。

设计目标：

- 在 `classes.md` 中明确 MQTT Client Service 的类、状态、信号、线程模型和跨层边界。
- 保留旧实现中有效的 MQTT FSM 思路和 Air780EP MQTT 命令流程。
- 修正旧实现中 MQTT 直接依赖 Core AT helper / URC helper 的问题。
- 为后续实现 `src/mqtt_client/mqtt_client.h` 与 `src/mqtt_client/mqtt_client.c` 提供清晰接口。

非目标：

- 不实现 MQTT 代码。
- 不设计 TCP/HTTP service。
- 不让 App 直接 include `core.h` 或内部 service 头文件。
- 不把 MQTT 业务状态机塞进 Core。

---

## 2. 架构关系

```text
Facade
  ├─ 调用 Core：联网、断网、网络状态查询
  ├─ 调用 MQTT：start/stop/publish/subscribe/unsubscribe
  └─ 持有 Core + MQTT 生命周期

MQTT Client Service
  ├─ 依赖 Core
  ├─ 订阅 Core event loop 的 NET_ONLINE / NET_OFFLINE / PROTOCOL_DATA
  ├─ 拥有独立 mqtt_fsm_t 和 fsm_queue
  ├─ 不 include modem.h
  ├─ 不调用 modem_* / at_engine_*
  └─ 只通过 Core command queue 下发模块命令

Core Service
  ├─ 仍然是唯一能下行调用 modem_* 的 service
  ├─ 继续负责网络状态机、PDP、重连
  └─ 提供 core_submit_cmd()，供 MQTT/TCP/HTTP 等上层 service 投递命令
```

Core 是 LTE 数据面和模块命令串行化的边界；MQTT 是协议业务状态机。MQTT service 不知道具体模块型号，也不写 AT 字符串。Air780EP 的 MQTT AT 命令只出现在 Core command 到 Modem Adapter 的第一版映射说明中。

---

## 3. `classes.md` 章节调整

现有章节：

```markdown
## 3. Core Service（核心服务层）
...
## 4. App（应用层）
```

调整为：

```markdown
## 3. Core Service（核心服务层）
...
## 4. MQTT Client Service（MQTT 客户端服务层）
...
## 5. App（应用层）
```

MQTT 章节包含：

```markdown
### 4.1 类总览
### 4.2 `mqtt_client_config_t` — MQTT 配置
### 4.3 `mqtt_client_t` — MQTT 客户端句柄
### 4.4 MQTT 状态、事件和消息类型
### 4.5 `mqtt_fsm_sig_t` — MQTT FSM 信号
### 4.6 `mqtt_pending_cmd_t` — Core 命令等待上下文
### 4.7 Core command queue 边界
### 4.8 MQTT 连接与操作流程
### 4.9 MQTT URC / 数据上行路径
### 4.10 MQTT 线程模型
### 4.11 错误处理规则
### 4.12 与 Core / Modem / AT Engine 的边界
```

同步调整：

- 可见性定义表增加 `src/mqtt_client/mqtt_client.h`，命名前缀为 `mqtt_client_`。
- Core Service 类总览增加 `core_cmd_t`、`core_cmd_type_t`、`core_cmd_result_t`、`core_cmd_done_callback_t`。
- Core Service 方法增加 `core_submit_cmd()`。
- App 章节编号从 `4` 改为 `5`。

---

## 4. MQTT Client Service 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `mqtt_client_config_t` | 层间 API | Facade factory | 配置结构体 | Broker、client_id、认证、keepalive、FSM 参数 |
| `mqtt_client_t` | 层间 API opaque | Facade | service 句柄 | MQTT Client Service 实例 |
| `mqtt_client_state_t` | 层间 API | Facade + MQTT 内部 | 状态枚举 | MQTT 生命周期和连接状态 |
| `mqtt_client_event_id_t` | 层间 API | Facade + esp_event | 事件枚举 | MQTT 上行事件类型 |
| `mqtt_client_event_data_t` | 层间 API | Facade | 值对象 | MQTT 事件数据 |
| `mqtt_client_publish_t` | 层间 API | Facade | 值对象 | 发布请求 |
| `mqtt_client_msg_t` | 层间 API | Facade | 值对象 | 收到的 MQTT 消息 |
| `mqtt_fsm_sig_t` | 模块私有 API | MQTT FSM | 信号对象 | MQTT FSM 队列中的信号 |
| `mqtt_connect_step_t` | 模块私有 API | MQTT FSM | 状态枚举 | 连接子状态机步骤 |
| `mqtt_pending_cmd_t` | 模块私有 API | MQTT FSM | 工作上下文 | 正在等待 Core command 结果的命令上下文 |

---

## 5. MQTT 层间 API 草案

```c
typedef struct mqtt_client mqtt_client_t;

typedef enum {
    MQTT_CLIENT_TRANSPORT_PLAIN_TCP = 0,
    MQTT_CLIENT_TRANSPORT_TLS,
} mqtt_client_transport_t;

typedef struct {
    mqtt_client_transport_t transport;
    const char *host;
    uint16_t port;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t keepalive_s;
    bool clean_session;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
} mqtt_client_config_t;

typedef enum {
    MQTT_CLIENT_STATE_STOPPED = 0,
    MQTT_CLIENT_STATE_WAITING_NET,
    MQTT_CLIENT_STATE_CONNECTING,
    MQTT_CLIENT_STATE_CONNECTED,
    MQTT_CLIENT_STATE_DISCONNECTING,
    MQTT_CLIENT_STATE_ERROR,
    MQTT_CLIENT_STATE_DESTROYING,
} mqtt_client_state_t;

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

Lifecycle and operations:

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

Rules:

- `mqtt_client_create()` receives and borrows `core_t *`; Facade owns Core lifetime.
- MQTT copies required configuration strings.
- TLS is present in type space but first version returns `ESP_ERR_NOT_SUPPORTED`.
- Publish/subscribe/unsubscribe only enqueue requests; completion is reported through MQTT events.
- Publish/subscribe/unsubscribe while not connected return `ESP_ERR_INVALID_STATE`.

---

## 6. MQTT 内部结构

```c
typedef enum {
    MQTT_CONNECT_STEP_IDLE = 0,
    MQTT_CONNECT_STEP_CONFIG,
    MQTT_CONNECT_STEP_OPEN,
    MQTT_CONNECT_STEP_LOGIN,
    MQTT_CONNECT_STEP_DONE,
    MQTT_CONNECT_STEP_ERROR,
} mqtt_connect_step_t;

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

typedef struct {
    bool active;
    core_cmd_type_t type;
    mqtt_client_operation_t operation;
    uint32_t started_ms;
    uint32_t timeout_ms;
} mqtt_pending_cmd_t;

struct mqtt_client {
    mqtt_client_config_t config;
    core_t *core;

    esp_event_loop_handle_t event_loop;
    esp_event_handler_instance_t core_event_any_handler;

    TaskHandle_t fsm_task;
    QueueHandle_t fsm_queue;
    SemaphoreHandle_t fsm_task_done_sema;
    SemaphoreHandle_t lock;

    mqtt_client_state_t state;
    mqtt_connect_step_t connect_step;
    mqtt_pending_cmd_t pending_cmd;

    mqtt_client_event_callback_t event_callback;
    void *event_user_ctx;

    bool started;
    bool net_online;
    bool destroying;
};
```

Key decisions:

- MQTT has no ops table; there is only one MQTT service implementation.
- MQTT is a service object, not a subclass of Core.
- MQTT does not store `modem_t *`, `at_engine_t *`, or `modem_air780ep_t *`.
- `lock` protects short state/lifecycle fields only; MQTT FSM does not hold it while calling `core_submit_cmd()`.

---

## 7. Core Command Queue 边界

Core exposes a typed command queue to upper services. MQTT uses this queue instead of calling Modem or AT Engine.

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

esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd);
```

Rules:

- `core_submit_cmd()` copies all data required for asynchronous execution.
- Pointer fields in `core_cmd_t` only need to remain valid during the call.
- Core FSM is the only place where submitted commands execute.
- Core may call `modem_*` while executing a submitted command.
- Core command completion callback must be lightweight.
- MQTT command completion callback only queues `MQTT_SIG_CORE_CMD_DONE` into the MQTT FSM queue.
- Core must not hold `core->lock` while invoking `done_cb`.

`core_cmd_t` is a service-layer command object, not a user API. App code must not include `core.h` or submit Core commands directly.

---

## 8. Command 映射

MQTT connection flow:

| MQTT step | Core command | Air780EP first-version command |
|-----------|--------------|--------------------------------|
| `MQTT_CONNECT_STEP_CONFIG` | `CORE_CMD_MQTT_CONFIG` | `AT+MCONFIG` |
| `MQTT_CONNECT_STEP_OPEN` | `CORE_CMD_MQTT_OPEN` | `AT+MIPSTART`; success accepts `CONNECT OK` / `ALREADY CONNECT` |
| `MQTT_CONNECT_STEP_LOGIN` | `CORE_CMD_MQTT_LOGIN` | `AT+MCONNECT`; success accepts `CONNACK OK` |

Runtime operations:

| MQTT API | Core command | Air780EP first-version command |
|----------|--------------|--------------------------------|
| `mqtt_client_subscribe()` | `CORE_CMD_MQTT_SUBSCRIBE` | `AT+MSUB`; success `SUBACK` |
| `mqtt_client_unsubscribe()` | `CORE_CMD_MQTT_UNSUBSCRIBE` | `AT+MUNSUB`; success `UNSUBACK` |
| `mqtt_client_publish()` | `CORE_CMD_MQTT_PUBLISH` | `AT+MPUBEX` + payload prompt |
| `mqtt_client_stop()` | `CORE_CMD_MQTT_DISCONNECT` | `AT+MDISCONNECT` |

The mapping is documentation for the Core/Modem implementation path. MQTT service itself does not generate these AT strings.

---

## 9. MQTT FSM 流程

Start flow:

```text
STOPPED
  └─ mqtt_client_start()
      ├─ Core net offline → WAITING_NET
      └─ Core net online  → CONNECTING
            └─ CONFIG → OPEN → LOGIN → CONNECTED
```

Network loss flow:

```text
CONNECTED
  └─ CORE_EVENT_NET_OFFLINE
      ├─ clear connected state
      ├─ post MQTT_CLIENT_EVENT_DISCONNECTED
      └─ WAITING_NET
```

Stop flow:

```text
CONNECTED or transport-open
  └─ mqtt_client_stop()
      ├─ submit CORE_CMD_MQTT_DISCONNECT
      ├─ wait for command completion or stop timeout
      ├─ unregister Core event handler
      ├─ stop MQTT FSM task
      └─ post MQTT_CLIENT_EVENT_STOPPED
```

MQTT must not silently replay publish/subscribe/unsubscribe requests while disconnected. First version returns `ESP_ERR_INVALID_STATE` for those calls unless state is `MQTT_CLIENT_STATE_CONNECTED`.

---

## 10. Events and Data Flow

MQTT event base:

```c
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
```

Core publishes network events as before. Core additionally publishes protocol events for data/connection closure observed by Modem URCs:

```c
typedef enum {
    CORE_EVENT_STARTED = 0,
    CORE_EVENT_READY,
    CORE_EVENT_NET_CONNECTING,
    CORE_EVENT_NET_ONLINE,
    CORE_EVENT_NET_OFFLINE,
    CORE_EVENT_NET_ERROR,
    CORE_EVENT_PROTOCOL_DATA,
    CORE_EVENT_PROTOCOL_CLOSED,
    CORE_EVENT_STOPPED,
    CORE_EVENT_ERROR,
} core_event_id_t;
```

```c
typedef enum {
    CORE_PROTOCOL_MQTT = 0,
} core_protocol_t;

typedef struct {
    core_protocol_t protocol;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} core_protocol_data_t;
```

`core_protocol_data_t` pointer fields are valid only during the Core event callback. MQTT service deep-copies topic/payload before queueing work to its FSM.

URC path:

```text
AT Engine RX task
  └─ Modem Air780EP URC handler
       └─ parse +MSUB: into modem_event_t
            └─ Modem event_task calls Core callback
                 └─ Core FSM posts protocol event
                      └─ MQTT service event handler queues MQTT FSM signal
                           └─ MQTT FSM posts MQTT_CLIENT_EVENT_DATA
```

MQTT does not register AT Engine URC handlers and does not include Modem private headers.

---

## 11. Thread Model

```text
Facade/App task
  └─ mqtt_client_start/publish/subscribe
       └─ parameter check + request deep-copy
            └─ xQueueSend(mqtt.fsm_queue)

Core event loop task
  └─ CORE_EVENT_NET_ONLINE / OFFLINE / PROTOCOL_DATA
       └─ MQTT core_event_handler
            └─ deep-copy required data
                 └─ xQueueSend(mqtt.fsm_queue)

MQTT FSM task
  └─ serially handles MQTT signals
       ├─ CONNECTING: submit CORE_CMD_MQTT_CONFIG/OPEN/LOGIN
       ├─ CONNECTED: submit publish/subscribe/unsubscribe
       └─ post MQTT_CLIENT_EVENT on state changes

Core FSM task
  └─ serially handles core_cmd_t
       └─ calls modem_* API
            └─ Air780EP subclass sends AT commands
```

Hard constraints:

- MQTT FSM may block on its own queue but App task must not block on MQTT connection progress.
- MQTT FSM does not hold `mqtt->lock` while calling `core_submit_cmd()`.
- Core command done callback does not mutate MQTT state directly.
- Core event handler performs only validation, deep-copy, and queue submission.
- MQTT destroy stops FSM before freeing queue/config data.
- MQTT destroy unregisters Core event handler before releasing the borrowed Core dependency.
- Stop flow has a timeout fallback for `CORE_CMD_MQTT_DISCONNECT`.

---

## 12. Error Handling

MQTT service uses ESP-IDF standard `esp_err_t` values:

| Situation | Error |
|-----------|-------|
| Invalid argument | `ESP_ERR_INVALID_ARG` |
| Invalid lifecycle or disconnected operation | `ESP_ERR_INVALID_STATE` |
| Queue submit failure | `ESP_FAIL` |
| Command timeout | `ESP_ERR_TIMEOUT` |
| Unsupported TLS | `ESP_ERR_NOT_SUPPORTED` |
| Malformed response or protocol data | `ESP_ERR_INVALID_RESPONSE` |

MQTT keeps the latest error code and publishes it through `MQTT_CLIENT_EVENT_ERROR`.

---

## 13. Required Documentation Changes

Update `docs/agents/classes.md` only after this spec is approved:

1. Update visibility table with `src/mqtt_client/mqtt_client.h` and `mqtt_client_` prefix.
2. Extend Core Service section with `core_cmd_*` types and `core_submit_cmd()`.
3. Insert MQTT Client Service section between Core and App.
4. Renumber App section from `4` to `5`.
5. State that MQTT service may use ESP-IDF/FreeRTOS directly.
6. State that MQTT service must not include `modem.h`, `modem_air780ep.h`, `at_engine.h`, or any `_priv.h` outside its own module.
7. State that Air780EP AT commands are mapping documentation, not MQTT service implementation details.

---

## 14. Review Notes

The design is scoped to one documentation change and one future service boundary. It does not require creating a git worktree. Existing `.gitignore` changes are unrelated and must not be modified by this work.
