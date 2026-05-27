# MQTT Client 端到端实现设计

## 1. 目标与边界

本次实现目标是把 `docs/agents/classes.md` 第四部分定义的 MQTT Client Service 落地为端到端可用能力：App 通过 `lwlte_mqtt_*` 用户 API 操作 MQTT，内部通过 MQTT Client Service → Core command queue → Modem MQTT ops → Air780EP MQTT AT 指令完成连接、订阅、取消订阅、发布和下行消息上报。

本次实现包含：

- 用户公共 MQTT API：`lwlte_mqtt_start()`、`lwlte_mqtt_stop()`、`lwlte_mqtt_get_state()`、`lwlte_mqtt_subscribe()`、`lwlte_mqtt_unsubscribe()`、`lwlte_mqtt_publish()`。
- Air780EP 用户配置中的嵌套 MQTT 子配置：`lwlte_air780ep_config_mqtt_client_t mqtt_client`。
- 内部 `src/mqtt_client/` 模块：`mqtt_client.h`、`mqtt_client_priv.h`、`mqtt_client.c`。
- Core command queue：`core_submit_cmd()`、`core_cmd_t` 深拷贝和 `CORE_SIG_SERVICE_CMD` 执行路径。
- Modem MQTT ops：通用 wrapper + Air780EP 子类实现。
- Air780EP MQTT AT 命令：`AT+MCONFIG`、`AT+MIPSTART`、`AT+MCONNECT`、`AT+MDISCONNECT`、`AT+MSUB`、`AT+MUNSUB`、`AT+MPUBEX`。
- MQTT 下行消息路径：`+MSUB:` → Modem protocol event → Core protocol event → MQTT event → LWLTE 用户事件。

本次不包含：

- TLS 连接；`MQTT_CLIENT_TRANSPORT_TLS` 第一版返回 `ESP_ERR_NOT_SUPPORTED`。
- 离线 publish/subscribe/unsubscribe 缓存。
- 网络恢复后的订阅自动重放。
- MQTT QoS2 完整事务；第一版优先支持 QoS 0/1，QoS2 在实现无法可靠确认时返回 `ESP_ERR_NOT_SUPPORTED`。
- 独立 TCP/HTTP service 设计。

---

## 2. 用户公共 API

`src/include/lwlte.h` 增加 MQTT 用户状态、消息值对象和操作 API。所有 MQTT 用户 API 都是异步提交：返回 `ESP_OK` 只表示请求已提交，不表示 Broker 已完成对应操作。

```c
typedef enum {
    LWLTE_MQTT_STATE_STOPPED = 0,
    LWLTE_MQTT_STATE_WAITING_NET,
    LWLTE_MQTT_STATE_CONNECTING,
    LWLTE_MQTT_STATE_CONNECTED,
    LWLTE_MQTT_STATE_DISCONNECTING,
    LWLTE_MQTT_STATE_ERROR,
} lwlte_mqtt_state_t;

typedef struct {
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
} lwlte_mqtt_msg_t;

esp_err_t lwlte_mqtt_start(lwlte_t *me);
esp_err_t lwlte_mqtt_stop(lwlte_t *me);
esp_err_t lwlte_mqtt_get_state(lwlte_t *me, lwlte_mqtt_state_t *state);
esp_err_t lwlte_mqtt_subscribe(lwlte_t *me, const char *topic, uint8_t qos);
esp_err_t lwlte_mqtt_unsubscribe(lwlte_t *me, const char *topic);
esp_err_t lwlte_mqtt_publish(lwlte_t *me, const char *topic,
                             const uint8_t *payload, size_t payload_len,
                             uint8_t qos, bool retain);
```

`lwlte_event_id_t` 增加 MQTT 事件：

```c
LWLTE_EVENT_MQTT_STARTED,
LWLTE_EVENT_MQTT_STOPPED,
LWLTE_EVENT_MQTT_CONNECTING,
LWLTE_EVENT_MQTT_CONNECTED,
LWLTE_EVENT_MQTT_DISCONNECTED,
LWLTE_EVENT_MQTT_SUBSCRIBED,
LWLTE_EVENT_MQTT_UNSUBSCRIBED,
LWLTE_EVENT_MQTT_PUBLISHED,
LWLTE_EVENT_MQTT_DATA,
LWLTE_EVENT_MQTT_ERROR,
```

`lwlte_event_data_t` 增加 MQTT 状态和 MQTT 消息 union 数据：

```c
typedef struct {
    lwlte_net_state_t net_state;
    lwlte_mqtt_state_t mqtt_state;
    int error_code;
    union {
        lwlte_mqtt_msg_t mqtt_msg;
    } data;
} lwlte_event_data_t;
```

事件生命周期规则：

- `LWLTE_EVENT_MQTT_DATA` 的 `topic` 和 `payload` 指针只在用户回调执行期间有效。
- `SUBSCRIBED`、`UNSUBSCRIBED`、`PUBLISHED` 第一版只携带 `mqtt_state` 和 `error_code`，不携带 topic。
- 未启用 MQTT service 时，`lwlte_mqtt_*` 返回 `ESP_ERR_INVALID_STATE`。
- MQTT 未连接时，publish/subscribe/unsubscribe 返回 `ESP_ERR_INVALID_STATE`。

---

## 3. Air780EP MQTT 嵌套配置

为避免 `lwlte_air780ep_config_t` 继续平铺膨胀，MQTT 配置使用嵌套子结构体：

```c
typedef struct {
    bool enabled;
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
} lwlte_air780ep_config_mqtt_client_t;

typedef struct {
    /* existing fields */
    lwlte_air780ep_config_mqtt_client_t mqtt_client;
} lwlte_air780ep_config_t;
```

配置规则：

- `config->mqtt_client.enabled == false`：不创建 MQTT service。
- `config->mqtt_client.enabled == true`：`host`、`port`、`client_id` 必填。
- `username` 和 `password` 可为 `NULL`，内部传给 Air780EP `AT+MCONFIG` 时按空字符串处理。
- `keepalive_s == 0` 使用 MQTT service 默认值，默认值为 300 秒。
- `fsm_queue_size`、`fsm_task_stack`、`fsm_task_priority` 为 0 时使用 MQTT service 默认值，非 0 必须大于 0。

Facade factory 映射规则：

- `mqtt_client.enabled == true` 时，在 `core_start(core)` 前创建 `mqtt_client_t` 并注册 MQTT 事件回调。
- `mqtt_client.enabled == false` 时，`lwlte_t->mqtt` 保持 NULL。
- 初始化成功不会自动启动 MQTT 连接；App 需要调用 `lwlte_mqtt_start()`。
- 如果调用 `lwlte_mqtt_start()` 时 LTE 网络未在线，MQTT FSM 进入 `WAITING_NET`，收到 Core 网络在线事件后继续连接。
- `lwlte_destroy()` 按 `mqtt_client_destroy()` → `core_destroy()` → `modem_destroy()` → `at_engine_destroy()` 顺序释放资源。

---

## 4. 内部 MQTT Client Service

`src/mqtt_client/mqtt_client.h` 是层间 API，只给 Facade factory 和 Facade 通用文件使用。MQTT service 不暴露给 App。

核心接口：

```c
mqtt_client_t *mqtt_client_create(const mqtt_client_config_t *config,
                                  core_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_t *me);
esp_err_t mqtt_client_start(mqtt_client_t *me);
esp_err_t mqtt_client_stop(mqtt_client_t *me);
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

依赖规则：

- MQTT service 只 include `core.h` 和自身头文件。
- MQTT service 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块 `_priv.h`。
- MQTT service 通过 `core_get_event_loop()` 订阅 Core 事件，通过 `core_get_net_state()` 读取当前网络状态，通过 `core_submit_cmd()` 提交模块命令。
- Core command done callback 只向 MQTT FSM queue 投递 `MQTT_SIG_CORE_CMD_DONE`，不直接修改 MQTT 状态。
- Core event handler 只做校验、必要深拷贝和投递 MQTT FSM 信号。

MQTT FSM 状态流：

```text
STOPPED
  -> START
  -> WAITING_NET 或 CONNECTING
  -> CONFIG
  -> OPEN
  -> LOGIN
  -> CONNECTED
```

运行规则：

- 连接过程按 `CORE_CMD_MQTT_CONFIG` → `CORE_CMD_MQTT_OPEN` → `CORE_CMD_MQTT_LOGIN` 串行执行。
- 第一版一次只允许一个 pending Core command。
- `publish/subscribe/unsubscribe` 只在 `CONNECTED` 状态入队。
- 任一 Core command 失败，MQTT 进入 `ERROR` 并发布错误事件。
- `CORE_EVENT_NET_OFFLINE` 或 `CORE_EVENT_PROTOCOL_CLOSED` 触发 MQTT disconnected 事件，并进入 `WAITING_NET`。
- `mqtt_client_stop()` 在连接已建立或连接过程中提交 `CORE_CMD_MQTT_DISCONNECT`，断开失败或超时时仍清理本地状态并发布 stopped/error。

---

## 5. Core Command Queue

Core 增加 typed command queue 作为 MQTT service 和 Modem 之间的唯一运行期下行边界。

```c
esp_err_t core_submit_cmd(core_t *me, const core_cmd_t *cmd);
```

`core_submit_cmd()` 行为：

- 校验参数、Core 生命周期和 command type。
- 深拷贝 command 中异步执行所需的字符串和 payload。
- 构造 `CORE_SIG_SERVICE_CMD`，投递到 Core FSM queue。
- 入队成功后 command 由 Core FSM 拥有。
- 入队失败时释放深拷贝对象并返回错误。
- Core FSM 执行完成后调用 `done_cb`，然后释放 command。

Core FSM 执行规则：

- Core FSM 是唯一执行 `core_cmd_t` 的位置。
- 执行 MQTT command 时，Core 调用 `modem_mqtt_*` wrapper。
- Core 调用 `done_cb` 时不得持有 `core->lock`。
- `result_data` 第一版可为 NULL；若未来增加结构化结果，指针只在 `done_cb` 回调期间有效。

---

## 6. Modem MQTT Ops 与 Air780EP 实现

`src/modem/modem.h` 增加 MQTT 值对象和 wrapper，`src/modem/modem_priv.h` 的 `modem_ops_t` 增加对应虚函数。

```c
esp_err_t modem_mqtt_config(modem_t *me,
                            const modem_mqtt_config_t *config);
esp_err_t modem_mqtt_open(modem_t *me,
                          const modem_mqtt_open_t *open);
esp_err_t modem_mqtt_login(modem_t *me,
                           const modem_mqtt_login_t *login);
esp_err_t modem_mqtt_disconnect(modem_t *me);
esp_err_t modem_mqtt_subscribe(modem_t *me,
                               const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_unsubscribe(modem_t *me,
                                 const modem_mqtt_topic_t *topic);
esp_err_t modem_mqtt_publish(modem_t *me,
                             const modem_mqtt_publish_t *publish);
```

Air780EP 第一版映射：

| Core command | Air780EP 命令 | 成功判定 |
|--------------|---------------|----------|
| `CORE_CMD_MQTT_CONFIG` | `AT+MCONFIG` | `OK` |
| `CORE_CMD_MQTT_OPEN` | `AT+MIPSTART` | `CONNECT OK` 或 `ALREADY CONNECT` |
| `CORE_CMD_MQTT_LOGIN` | `AT+MCONNECT` | `CONNACK OK` |
| `CORE_CMD_MQTT_DISCONNECT` | `AT+MDISCONNECT` | `OK` |
| `CORE_CMD_MQTT_SUBSCRIBE` | `AT+MSUB` | `SUBACK` |
| `CORE_CMD_MQTT_UNSUBSCRIBE` | `AT+MUNSUB` | `UNSUBACK` |
| `CORE_CMD_MQTT_PUBLISH` | `AT+MPUBEX` + payload | QoS0: `OK`；QoS1: `PUBACK` |

实现细节：

- `MIPSTART`、`MCONNECT`、`MSUB`、`MUNSUB` 使用 `at_engine_send_cmd_with_options()` 设置自定义成功终止行。
- `MCONNECT` 的中间 `OK` 不是最终成功，必须等待 `CONNACK OK`。
- `MPUBEX` 需要 AT Engine 支持 prompt 写 payload；如果现有 AT Engine 尚不支持裸 payload 写入，本次实现必须补齐最小发送能力，不能在 Air780EP 中绕过 AT Engine 直接写 UART。
- MQTT 字符串参数必须做 AT 字符串转义，至少覆盖双引号、反斜杠、CR、LF。
- QoS 2 第一版不承诺，无法可靠完成 `PUBREC`/`PUBCOMP` 链路时返回 `ESP_ERR_NOT_SUPPORTED`。

---

## 7. MQTT 下行数据路径

Air780EP 第一版采用 MQTT 直接打印模式，解析 `+MSUB:<topic>,<len>,<message>`。

```text
AT Engine RX task
  -> Air780EP +MSUB URC handler
  -> 深拷贝 topic/payload 到 Modem event 所需内存
  -> Modem event_task 调用 Core 回调
  -> Core FSM 复制协议数据并发布 CORE_EVENT_PROTOCOL_DATA
  -> MQTT Core event handler 深拷贝 topic/payload 并投递 MQTT FSM
  -> MQTT FSM 发布 MQTT_CLIENT_EVENT_DATA
  -> Facade 转换为 LWLTE_EVENT_MQTT_DATA
  -> App 用户回调
```

生命周期规则：

- Air780EP URC handler 不直接调用 Core 回调，只投递 Modem event。
- `MODEM_EVENT_PROTOCOL_DATA` 的指针只在 Modem callback 期间有效。
- `MODEM_EVENT_PROTOCOL_DATA` 的 topic/payload 必须来自堆内存；`modem_post_event()` 成功后所有权转移给 Modem event task，失败时仍由调用者释放。
- `CORE_EVENT_PROTOCOL_DATA` 的指针只在 Core event callback 期间有效；MQTT service 必须先复制数据，并在回调返回前调用 `core_release_event_payload()` 释放 Core 为该事件分配的堆内存。
- `MQTT_CLIENT_EVENT_DATA` 的指针只在 MQTT event callback 期间有效。
- `LWLTE_EVENT_MQTT_DATA` 的指针只在用户回调期间有效。

第一版只解析直接模式 `+MSUB:<topic>,<len>,<message>`。缓存模式 `+MSUB:<store_addr>` 和 `AT+MQTTMSGGET` 后续再设计。

---

## 8. 错误处理

统一使用 ESP-IDF 标准 `esp_err_t`：

| 场景 | 错误码 |
|------|--------|
| 参数错误 | `ESP_ERR_INVALID_ARG` |
| MQTT 未启用、未创建或状态不允许 | `ESP_ERR_INVALID_STATE` |
| 内存不足 | `ESP_ERR_NO_MEM` |
| 队列满或命令超时 | `ESP_ERR_TIMEOUT` |
| TLS 或 QoS2 暂不支持 | `ESP_ERR_NOT_SUPPORTED` |
| AT 响应或 URC 格式异常 | `ESP_ERR_INVALID_RESPONSE` |
| 通用 AT 命令失败 | `ESP_FAIL` 或下层返回值 |

MQTT service 保存最近一次错误码，并通过 `MQTT_CLIENT_EVENT_ERROR` 和 `LWLTE_EVENT_MQTT_ERROR` 上报。

---

## 9. 文档同步要求

由于本次实际实现设计相对已提交的第四部分文档增加了 public API 和嵌套配置，必须同步修正文档：

- `docs/agents/classes.md`：说明 `lwlte_air780ep_config_mqtt_client_t` 嵌套配置会映射到内部 `mqtt_client_config_t`，并说明 public `lwlte_mqtt_*` API 由 Facade 包装 MQTT service。
- `docs/agents/architecture.md`：把 MQTT 示例配置和 Facade factory 映射改为 `config->mqtt_client.*` 嵌套字段，并增加 `config->mqtt_client.enabled` 判断。
- 本 spec 使用中文并作为实际实现设计依据。

---

## 10. 验证计划

实现完成后执行：

1. 新增 host 静态回归测试，检查 public API、嵌套配置、MQTT service 文件、Core command queue、Modem MQTT ops 和 `+MSUB:` 数据路径符号存在。
2. 运行现有 host tests，确保 Air780EP CPIN 策略和 net_mgr 激活流程未回退。
3. 使用 ESP-IDF MCP build 或 `idf.py build` 编译验证。
4. 如连接硬件和 broker，再执行实机 MQTT start/subscribe/publish 验证；本次默认完成标准是静态测试与编译通过。
