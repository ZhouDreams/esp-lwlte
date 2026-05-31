# Modem MQTT API 命名调整设计

## 1. 目标与边界

本次调整目标是澄清 Modem 层 MQTT wrapper 的语义，使函数名准确表达 Air780EP 内置 MQTT client 的三段连接流程：配置 MQTT 参数、建立底层 TCP 通道、建立 MQTT 协议会话。

本次包含：

- 将 `modem_mqtt_config()` 改为动词形式 `modem_mqtt_configure()`。
- 将 `modem_mqtt_open()` 改为 `modem_mqtt_tcp_connect()`，明确该步骤对应 MQTT 底层 TCP 通道建立。
- 将 `modem_mqtt_login()` 改为 `modem_mqtt_connect()`，对齐 MQTT 协议的 CONNECT 语义。
- 将 `modem_mqtt_open_t` 改为 `modem_mqtt_tcp_config_t`。
- 将 `modem_mqtt_login_t` 改为 `modem_mqtt_connect_config_t`。
- 保持 `modem_mqtt_config_t` 不变，因为当前项目没有 MQTT server 侧 API，Modem 层的 `mqtt` 默认代表模块内置 MQTT client。
- 保持 `modem_mqtt_disconnect()` 不变，因为它仍表示断开 MQTT 协议会话。

本次不包含：

- 不合并现有 `configure -> tcp_connect -> connect` 三步流程。
- 不改变 Air780EP AT 指令、响应匹配、超时或状态机行为。
- 不保留旧名称 wrapper。该接口是内部层间 API，直接重命名可以避免 API 面臃肿。
- 不新增 MQTT transport/session 抽象层。

## 2. 命名映射

| 当前名称 | 新名称 | 语义 |
|----------|--------|------|
| `modem_mqtt_config_t` | 保持 `modem_mqtt_config_t` | MQTT client id、username、password 参数 |
| `modem_mqtt_config()` | `modem_mqtt_configure()` | 发送 `AT+MCONFIG` 配置 MQTT 参数 |
| `modem_mqtt_open_t` | `modem_mqtt_tcp_config_t` | MQTT TCP 通道 host、port 参数 |
| `modem_mqtt_open()` | `modem_mqtt_tcp_connect()` | 发送 `AT+MIPSTART` 建立 MQTT 底层 TCP 通道 |
| `modem_mqtt_login_t` | `modem_mqtt_connect_config_t` | MQTT CONNECT 的 clean session、keepalive 参数 |
| `modem_mqtt_login()` | `modem_mqtt_connect()` | 发送 `AT+MCONNECT` 建立 MQTT 协议会话 |
| `modem_mqtt_disconnect()` | 保持 `modem_mqtt_disconnect()` | 发送 `AT+MDISCONNECT` 断开 MQTT 协议会话 |

## 3. 行为语义

调整后连接流程保持现有顺序：

```text
CORE_CMD_MQTT_CONFIGURE
  -> modem_mqtt_configure()
  -> AT+MCONFIG

CORE_CMD_MQTT_TCP_CONNECT
  -> modem_mqtt_tcp_connect()
  -> AT+MIPSTART
  -> CONNECT OK / ALREADY CONNECT

CORE_CMD_MQTT_CONNECT
  -> modem_mqtt_connect()
  -> AT+MCONNECT
  -> CONNACK OK
```

`modem_mqtt_tcp_connect()` 成功只表示 MQTT 使用的底层 TCP 通道已建立，不表示 broker 会话已可 publish/subscribe。只有 `modem_mqtt_connect()` 成功后，MQTT service 才进入 connected 状态并允许后续 subscribe、unsubscribe、publish。

`modem_mqtt_disconnect()` 继续映射 `AT+MDISCONNECT`，表示断开 MQTT 协议会话。Air780EP 文档还存在 `AT+MIPCLOSE` 关闭 TCP 通道能力，但当前实现没有独立 wrapper，本次不扩展该能力。

## 4. 修改范围

需要同步修改以下位置，确保全仓不残留旧符号：

- `src/modem/modem.h`：类型定义、函数声明和 Doxygen 注释。
- `src/modem/modem.c`：wrapper 函数名、参数类型、`ops` 调用和错误日志字符串。
- `src/modem/modem_priv.h`：`modem_ops_t` 函数字段名、参数类型和注释。
- `src/modem/modem_air780ep.c`：static 函数声明、函数定义、`air780ep_ops` 初始化字段和参数类型。
- `src/core/core.h`：`core_cmd_type_t` 枚举名、`core_cmd_t` union 字段名和注释。
- `src/core/core.c`：command clone、free、valid 逻辑中的 command type 和 union 字段名。
- `src/core/core_fsm.c`：service command 分发到 Modem wrapper 的调用。
- `src/mqtt_client/mqtt_client.c` 和 `src/mqtt_client/mqtt_client_priv.h`：connect step 名称、Core command 提交流程和完成处理。
- `tests/host/test_mqtt_end_to_end_contract.py`：字符串契约断言。
- `docs/agents/classes.md`、`docs/agents/at_cmd_air780ep.md`、`docs/interview-preparation/modem-module-analysis.md`：文档中的 API 名称和语义说明。

## 5. 测试与验证

实现后至少执行：

```sh
python -m pytest tests/host/test_mqtt_end_to_end_contract.py
```

如 ESP-IDF 环境可用，再执行项目构建，验证 C 符号重命名没有遗漏：

```sh
idf.py build
```

验证重点：

- 旧符号 `modem_mqtt_open`、`modem_mqtt_login`、`CORE_CMD_MQTT_OPEN`、`CORE_CMD_MQTT_LOGIN` 不再出现在源码和测试中。
- 旧类型 `modem_mqtt_open_t`、`modem_mqtt_login_t` 不再出现在源码和测试中。
- 旧 Core command union 字段 `mqtt_open`、`mqtt_login` 不再出现在源码和测试中。
- `modem_mqtt_config_t` 仍保持原名。
- MQTT service 的连接状态行为保持不变：`configure -> tcp_connect -> connect -> connected`。
- Air780EP 仍使用 `AT+MCONFIG`、`AT+MIPSTART`、`AT+MCONNECT`、`AT+MDISCONNECT`。
