# TCP over TLS（SSL socket）设计

## 目标

为现有 TCP client 增加可选 TLS，使 `lwlte_ssl_provision()` 配出的 SSL context 能被 socket/TCP 路径复用（此前只有 MQTT 能消费）。第一版面向 Air780EP 和 ML307R。TLS 协商全部由模块在 socket 打开命令内完成，TCP client FSM 不改逻辑，只增加透传字段。

本设计是 `2026-06-25-mqtt-tls-ssl-design.md` 的延续：那份 spec 已把 SSL context 模型预留为"后续 HTTPS 和 TCP-TLS 复用"，本扩展兑现 TCP-TLS 部分。

## 范围

包含：

- 在 TCP 打开路径上增加可选 TLS 传输（明文/TLS）。
- 让 TCP socket 引用一个已 provision 的 SSL context id。
- 复用现有三种认证模式（NONE/SERVER/MUTUAL），由 `lwlte_ssl_provision()` 的 context 决定，TCP 路径不重复证书生命周期管理。
- Air780EP 用模块 SSL TCP 通道（`CIPSSL=1` + `CIPSTART`，固定 context `0`）。
- ML307R 用 `MIPCFG="ssl"` + `MIPOPEN`（context `0..5`）。
- 扩展 TCP example 与 Kconfig，支持 TLS 验证路径。
- 在真实硬件上用国内 TLS 端点验证。

不包含：

- 多连接 socket（v1 仍 `max_conns=1`，与现状一致）。
- UDP over TLS（`proto` 仍只有 TCP）。
- HTTP over TLS（HTTPS 是独立后续工作，沿用同一 SSL context 模型但不在本扩展内）。
- 自动在 socket 打开/重连时写证书（证书仍由 `lwlte_ssl_provision()` 显式管理，与 MQTT 一致）。
- TLS 会话恢复、PSK、分块大证书上传（沿用 MQTT-TLS spec 的 out-of-scope）。

## 选定方案

对称扩展 TCP 路径，与 MQTT TLS 完全镜像：

- 公开 API 在 `lwlte_tcp_open_config_t` 增加 `transport` + `ssl_context_id`，零值仍为明文 TCP（向后兼容）。
- TLS 选项逐层透传：Facade → tcp_client → `CORE_CMD_SOCKET_OPEN` → `modem_socket_open`。
- TLS 协商由 modem 在 open 命令内完成，tcp_client 只投递参数、不改 FSM。
- 复用 `lwlte_ssl_provision()` 写证书（Air780EP 侧需把硬编码的 ctx 88 参数化并放宽 guard，详见 SSL Provisioning 节），不新增证书写入路径。
- 明文路径显式关闭模块 SSL 绑定，避免上次 TLS 残留（镜像 MQTT "plain 关闭 SSL 绑定" 的处理）。

被否决的备选：

- 新增独立 `lwlte_ssl_socket_*` API：违反 architecture 中"TCP Client Service 即 socket service"的边界（classes.md §3、§5.4），且重复一套 connect/send/recv/close FSM。
- TCP-TLS 只支持 no-auth：无谓砍掉已免费继承的 server/mutual 认证。
- 公开一个抽象 context id、由 modem 按协议透明映射到 0/88：Air780EP 的 `0` 与 `88` 是两套独立证书存储，无法透明映射。

## 公开 API

在 `src/include/lwlte.h` 增加 TCP 传输枚举，并扩展打开配置：

```c
/**
 * @brief TCP 传输类型
 * @details TCP transport type
 */
typedef enum {
    LWLTE_TCP_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    LWLTE_TCP_TRANSPORT_TLS,            /**< TLS； TLS */
} lwlte_tcp_transport_t;
```

```c
/**
 * @brief TCP 打开连接配置
 * @details TCP open connection configuration
 */
typedef struct {
    const char           *host;            /**< 目标主机或 IP； Target host or IP */
    uint16_t              port;            /**< 目标端口； Target port */
    lwlte_tcp_transport_t transport;       /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t               ssl_context_id;  /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
    void                  *user_ctx;       /**< 用户上下文，事件中原样返回； User context returned in events */
} lwlte_tcp_open_config_t;
```

语义：

- `transport = LWLTE_TCP_TRANSPORT_PLAIN_TCP`（0）时忽略 `ssl_context_id`，行为与当前一致。
- `transport = LWLTE_TCP_TRANSPORT_TLS` 时要求 `ssl_context_id` 已通过 `lwlte_ssl_provision()` 配好，否则打开失败返回 `ESP_ERR_INVALID_STATE`。
- socket 与 MQTT 各自引用自己需要的 context id，互不影响：Air780EP 上 socket 用 `0`、MQTT 用 `88`（两个独立 context，需分别 `lwlte_ssl_provision()`）；ML307R 上两者都用 `0..5` 范围内的 ssl_id。

## 公开使用流程

明文 TCP 流程不变。

TLS socket 流程：

```c
lwlte_ssl_provision(lte, &ssl_cfg, &creds);          /* 配置 context（auth/cert 等） */
lwlte_ssl_get_context_status(lte, ssl_cfg.context_id, &status);

const lwlte_tcp_open_config_t open_cfg = {
    .host = "www.baidu.com",
    .port = 443,
    .transport = LWLTE_TCP_TRANSPORT_TLS,
    .ssl_context_id = ssl_cfg.context_id,
};
lwlte_tcp_open(lte, &open_cfg, &conn);
```

`lwlte_tcp_open()` 与既有发送/接收/关闭 API 不变；TLS 握手失败按现有 socket 打开失败处理。

## 层职责

Facade：

- 在 `lwlte_tcp_open()` 校验公开参数，把 `transport`/`ssl_context_id` 映射进 tcp_client 打开参数。

TCP Client service：

- 把 `transport`/`ssl_context_id` 放入 `CORE_CMD_SOCKET_OPEN` 命令数据。
- 不处理 PEM、不调用 Modem。FSM 不变。

Core：

- `core_socket_open_t` 增加 `transport` + `ssl_context_id`；命令 clone/free 路径复制。
- 执行 socket 命令时把 core 值对象映射为 `modem_socket_open_t`。

Modem common：

- `modem_socket_open_t` 增加 `transport` + `ssl_context_id`。
- 不在通用包装层做模块相关校验（ctx 合法性由具体模块判定）。

Module adapters：

- Air780EP / ML307R 的 `socket_open` 按 `transport` 选择明文或 TLS 命令序列，做 context 合法性校验，AT 语法全部留在模块文件内。

AT Engine：无改动。

## 分层穿线（镜像 MQTT 的 transport + ssl_context_id）

| 层 | 类型 | 变更 |
|----|------|------|
| Facade | `lwlte_tcp_open_config_t` | 增加 `transport` + `ssl_context_id` |
| TCP Client | `submit_socket_open` | 把两字段写入 `CORE_CMD_SOCKET_OPEN.data.socket_open` |
| Core | `core_socket_open_t` | 增加 `transport`（`core_socket_transport_t`）+ `ssl_context_id`；clone 复制；映射到 modem |
| Modem | `modem_socket_open_t` | 增加 `transport`（`modem_socket_transport_t`）+ `ssl_context_id` |

传输枚举三层各设一套，与现有 `proto`（`core_socket_proto_t` / `modem_socket_proto_t`）风格一致：

- 公开：`lwlte_tcp_transport_t`（`LWLTE_TCP_TRANSPORT_*`）。
- Core：`core_socket_transport_t`（`CORE_SOCKET_TRANSPORT_*`）。
- Modem：`modem_socket_transport_t`（`MODEM_SOCKET_TRANSPORT_*`）。

各层边界做枚举映射（Facade→Core、Core→Modem），`PLAIN_TCP` 恒为 `0`。

## Air780EP 映射

Air780EP 的 SSL TCP 通道使用固定 SSL context `0`（与 MQTT 的 `88` 区分）。证书/auth 由 `lwlte_ssl_provision(context_id=0)` 预先配好（写文件 + `SSLCFG` 绑定到 ctx 0）。

`socket_open` 命令序列：

- TLS：
  1. `AT+SSLCFG="hostname",0,"<host>"`（SNI/hostname 取自 `open->host`，动态写入）
  2. `AT+CIPSSL=1`
  3. `AT+CIPSTART="TCP","<host>",<port>`
- 明文：
  1. `AT+CIPSSL=0`（显式关闭，避免上次 TLS 残留）
  2. `AT+CIPSTART="TCP","<host>",<port>`

校验：`transport=TLS` 时 `ssl_context_id` 必须为 `0`，否则 `ESP_ERR_INVALID_ARG`。

连接结果沿用现有解析：`CONNECT OK` / `CONNECT FAIL` / `STATE:<state>` 等。

> 注：`AT+CIPSSL` 支持与固件相关（见 `at_cmd_air780ep.md` 的 EC716S 平台 `_FS`/`_MS` 固件说明）。Air780EP 为当前目标平台并支持；若验证时遇到不支持，记录固件版本。

## ML307R 映射

ML307R 的 SSL socket 用 `MIPCFG="ssl",<conn_id>,<ssl_enable>,<ssl_id>` 在 `MIPOPEN` 前把连接绑定到某个 ssl_id（`0..5`）。ML307R 无 SNI 命令，`hostname` 字段忽略。

`socket_open` 命令序列：

- TLS：
  1. `AT+MIPCFG="ssl",<conn_id>,1,<ssl_context_id>`
  2. `AT+MIPOPEN=<conn_id>,"TCP","<host>",<port>,<timeout>,2`
- 明文：
  1. `AT+MIPCFG="ssl",<conn_id>,0,0`（显式关闭，避免上次 TLS 残留）
  2. `AT+MIPOPEN=<conn_id>,"TCP","<host>",<port>,<timeout>,2`

校验：`transport=TLS` 时 `ssl_context_id < ML307R_SSL_MAX_CONTEXTS`（6），且该 context 已 provision，否则分别返回 `ESP_ERR_INVALID_ARG` / `ESP_ERR_INVALID_STATE`。

连接结果沿用现有 `+MIPOPEN: <conn_id>,<result>` 解析（`result=0` 成功，非 0 保留原始码进日志与事件）。

## SSL Provisioning

实现中发现并修正了一个 spec 偏差：Air780EP 的 `ssl_provision`/`get_context_status` 并非协议无关——它把 SSL context 硬编码为 `88`（既在入口校验里，也在 `SSLCFG` 命令串字面量里）。为实现 TCP-TLS（ctx 0），做了两处必要修改：

- **命令参数化**：Air780EP `ssl_provision`、`get_context_status`、`bind_ssl_file`、`query_ssl_auth_mode` 中所有 `AT+SSLCFG "<tag>",88,...` 的字面量 `88` 改为用各自已接收的 `context_id` 参数（`%u`）。MQTT 传 `88` 行为不变；TCP 传 `0` 正确绑定到 ctx 0。
- **guard 放宽**：上述函数的入口校验由 `context_id == 88` 放宽为 `context_id == AIR780EP_SSL_TCP_CONTEXT_ID(0) || context_id == AIR780EP_SSL_MQTT_CONTEXT_ID(88)`；`query_ssl_auth_mode` 的响应解析由校验 `parsed_context == 88` 改为校验 `parsed_context == context_id`。

ML307R `ssl_provision` 本就协议无关（校验 `context_id < 6`），无需改动。

不动的部分：MQTT 消费点（`air780ep_mqtt_configure` 及 MQTT 连接路径）仍校验 `==88`——MQTT 专用 ctx，与本扩展无关。新增的 socket 消费点（`air780ep_socket_open`/`ml307r_socket_open`）各自校验 ctx 合法性（Air780EP `==0`、ML307R `<6`）。

## 错误处理

- 非法公开参数返回 `ESP_ERR_INVALID_ARG`。
- `transport=TLS` 但 context 未 provision 返回 `ESP_ERR_INVALID_STATE`。
- Air780EP TLS 用非 0 context、ML307R 用 `>=6` 的 context 返回 `ESP_ERR_INVALID_ARG`。
- TLS 握手失败映射到**现有** socket 打开失败路径：
  - Air780EP `CONNECT FAIL`、ML307R `+MIPOPEN: <conn_id>,<err>`（如 753 协商超时）→ tcp_client 上报 `LWLTE_TCP_EVENT_ERROR` / `LWLTE_TCP_EVENT_DISCONNECTED`，并带 `modem_error_code`。
  - **不新增事件类型**，复用现有 `LWLTE_TCP_EVENT` 集。
- AT 命令失败按既有 socket 错误传播规则映射为标准 `esp_err_t`，保留原始模块错误码。

## 已知网络限制（非本扩展回归）

当前 example broker `admin.jovisdreams.site`（`168.138.32.69`，Oracle Cloud 日本大阪）为海外端点。两块 LTE 模块经蜂窝网络出国时，TCP 三次握手可完成、明文 MQTT 1883 可用，但 8883 TLS 握手会被国际链路干扰而失败（Air780EP `CONNECT FAIL`、ML307R `conn_state 1→6` / err 753）。这是网络路径问题，与本扩展的正确性无关；TLS 硬件验证改用国内端点。

## 验证

静态与编译：

- 公开头加枚举与新字段后编译通过。
- 现有 plain TCP example 默认仍为明文（`transport=0`），行为不变。
- Core 命令 clone/free 路径正确复制新字段，无泄漏。
- 非法 transport、TLS 未 provision、Air780EP ctx≠0、ML307R ctx≥6 返回预期错误。

模块命令验证：

- Air780EP：TLS 路径覆盖 `SSLCFG hostname,0` + `CIPSSL=1` + `CIPSTART`；明文路径覆盖 `CIPSSL=0`。
- ML307R：TLS 路径覆盖 `MIPCFG="ssl",<conn>,1,<id>` + `MIPOPEN`；明文路径覆盖 `MIPCFG="ssl",<conn>,0,0`。

真实硬件验证：

- 硬件须连接并在声明完成前实际使用。
- 用国内 TLS 端点 `www.baidu.com:443` 做一次 socket TLS 冒烟（no-auth 或装其 CA 做 server-auth），Air780EP 与 ML307R 各跑一次，确认能建立 TLS 并完成一次收发后正常关闭。
- 扩展 TCP example：新增 `EXAMPLE_TCP_TLS_ENABLE` 与 `EXAMPLE_TCP_TLS_CA_CERT_PEM`（镜像 MQTT example 的 Kconfig 结构），`EXAMPLE_TCP_TLS_ENABLE=y` 时端口默认切 443、transport=TLS，用国内端点跑通。
- MQTT-TLS 仍指向海外 broker，作为已知网络限制记录，不作为本扩展的回归判据。
- 两块板子尽量都验证；若只有一块可用，记录实际验证的模块。

## 文档更新

- 更新 `src/include/lwlte.h` 公开注释（新枚举与新字段）。
- 更新 `docs/agents/classes.md`：`modem_socket_open_t` 增加 `transport`/`ssl_context_id`，socket ops 表的 `socket_open` 补 Air780EP `CIPSSL`/ML307R `MIPCFG="ssl"` 说明。
- 更新 `docs/agents/feature-roadmap.md`：把 TCP-TLS 从"后续"标记为已实现。
- 更新 `example/README.md`：TCP example 的 TLS 配置说明。
- 只在实现需要时对 `docs/agents/at_cmd_air780ep.md` / `at_cmd_ml307r.md` 做定点补充，不覆盖无关内容。
