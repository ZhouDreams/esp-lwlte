# 功能 Roadmap

本文档是 esp-lwlte 的功能规划索引，回答“这个库要实现哪些功能、每个功能要在哪一层改什么”。它是**活文档**：能力落地时就地更新状态。每个规划能力真正开工时，仍各自走 brainstorm → spec → plan 流程，本文档只做顶层规划与分层分解。

文档按项目实际分层组织成三部分：

1. **功能 Overview** — 要哪些功能模块、状态、优先级。
2. **Modem 层修改** — 具体操作转 AT 指令（ops → AT），要给 `modem_ops` 增加哪些函数指针。
3. **Service 层修改** — 调用 `modem_ops` 干活的编排逻辑。

---

## 设计哲学（核心约束）

**Service 层对接模块内置协议栈**：模块有什么原生 AT 协议能力（TCP、HTTP、MQTT…），就做对应的一个 modem ops 族 + 一个 service 编排。

- **不**把应用协议架在通用 socket 抽象之上（不是“TCP 之上自己实现 HTTP/MQTT”）。
- 在 Air780EP / ML307R 上，TCP、HTTP、MQTT 各自是**独立的 AT 指令族**，都只依赖 PDP 已激活，互不依赖。
- TLS/SSL 是**横向能力**：配置到一个 SSL context，再被 HTTPS / MQTT-S / TCP-TLS 引用，不是独立协议。

MQTT 是这个哲学的范例：`modem_ops` 把 MQTT 拆成 `mqtt_configure / mqtt_tcp_connect / mqtt_connect / mqtt_subscribe / mqtt_publish …`，`mqtt_client` service 通过 `core_submit_cmd()` 串行驱动这些 ops。

---

## 第一部分：功能 Overview

### 能力矩阵

| 能力 | 状态 | 优先级 | Service 编排范式 |
|------|------|--------|------------------|
| Core 网络保活 / PDP / 重连 | 已完成 | — | Core FSM |
| MQTT | 已完成 | — | 专属 service + 自有 FSM（`mqtt_client`） |
| Ping | 已完成 | — | Core 同步命令（`CORE_CMD_PING`） |
| 低功耗 / 休眠管理 | 规划 | P1 | Core 策略（跨层） |
| TCP/UDP Socket | TCP client v1 + TCP-TLS (SSL socket) 已完成；UDP/扩展规划 | P2 | 专属 service + 自有 FSM |
| TLS/SSL 配置 | 已落地（供 MQTT-TLS / TCP-TLS 复用） | P3 | 横向能力，非独立 service |
| HTTP/HTTPS | 规划 | P4 | Core 同步命令（v1） |

### 落地顺序

**低功耗 → TCP/UDP → TLS/SSL → HTTP/HTTPS**

- 低功耗优先：面向电池供电场景的核心诉求，且不依赖其它新能力。
- TCP 次之：最通用的传输能力，并暴露出“连接 id 维度”这一共享前置改造（见第二部分）。
- TLS 第三：横向能力，先打通证书/context 配置，供 HTTPS / MQTT-S / TCP-TLS 复用。
- HTTP 最后：可复用 TLS context；请求/响应模型相对独立。

### 暂不纳入（记录缓办理由）

| 能力 | 缓办理由 |
|------|----------|
| SMS 短信收发 | 当前目标场景（数据上云）无短信需求；两模块 AT 参考尚未收录，需额外调研。 |
| GNSS / 定位 | 依赖具体模块型号是否带 GNSS；目标硬件未确认带定位。 |
| FOTA / 文件系统 / FTP | 模块固件升级与文件下载非当前主路径；待有明确 OTA 需求再规划。 |
| 语音通话 | 与库定位（数据连接）不符。 |

未来如需纳入，在能力矩阵新增行并补充第二、三部分对应分解即可。

---

## 第二部分：Modem 层修改（ops → AT）

Modem 层职责：把语义操作翻译成具体 AT 指令，差异封装在 `modem_impl`，通过 `modem_ops` 虚表多态转发。新增能力 = 给 `modem_ops_t` 增加函数指针 + 增加 `modem_*` 公共包装 + 增加参数结构/事件，再由 `modem_air780ep` 与 `modem_ml307r` 各自实现。

### 共享前置改造：协议数据事件泛化（Socket / HTTP 的基础）

TCP client v1 已完成 `MODEM_PROTOCOL_TCP` 和 `conn_id` 维度，`modem_protocol_data_t` 可承载 MQTT 的 `topic / payload` 和 TCP 的 `conn_id / payload / reason / modem_error_code`。UDP（多连接 0..5）和 HTTP（多实例 httpid）落地前仍需在此基础上继续扩展：

- 扩展 `modem_protocol_t`：现有 `MODEM_PROTOCOL_MQTT` / `MODEM_PROTOCOL_TCP`，后续新增 `MODEM_PROTOCOL_UDP` / `MODEM_PROTOCOL_HTTP`。
- 泛化 `modem_protocol_data_t`：复用已存在的 `conn_id` / `reason` 字段，使 UDP/HTTP RX 数据与关闭事件能路由到正确实例。
- 复用现有 `MODEM_EVENT_PROTOCOL_DATA` / `MODEM_EVENT_PROTOCOL_CLOSED` 上行通道，并保持 id 维度。

这是 P2/P4 的公共前置，应在 Socket 能力中一并完成。

### 各能力新增的 modem_ops 函数指针

#### P1 低功耗 / 休眠管理

| 项 | 内容 |
|----|------|
| 新增 ops | `set_power_mode`、`get_power_mode` |
| 新增参数结构 | `modem_power_config_t`（睡眠模式：关闭/浅睡/深睡/PSM；可选延迟、唤醒过滤） |
| 新增事件 | `MODEM_EVENT_SLEEP`、`MODEM_EVENT_WAKE` |
| Air780EP AT | `AT+CSCLK=<0..3>`、`AT+POWERMODE=PRO/STD/PSM+/CLOSE`、`AT+WAKETIM`、`AT+CFGRI`、`AT^WAKEUPHEX` |
| ML307R AT | `AT+MLPMCFG="sleepmode"/"sleepind"/"delaysleep"/"psind"`；URC `+MLPMENTER` / `+MLPMEXIT` / `+MLPMPSTA` |
| 跨层注记 | **睡眠后 AT 通道不立即响应**，需 AT Engine 提供“先唤醒再发命令”的钩子配合，故此能力不是纯 ops→AT，涉及 AT Engine 改动。 |

#### P2 TCP/UDP Socket

TCP client v1 已在 Air780EP 和 ML307R 上实现 plain TCP；TCP-TLS (SSL socket) 复用 `lwlte_ssl_provision()` 配置的 SSL context 亦已完成。当前限定一个 client connection，支持 binary-safe TX/RX，并通过 ESP event 投递连接、发送、接收和错误事件。UDP、server/listen mode、local port binding、multiple simultaneous connections、automatic reconnect 和 high-throughput streaming 仍是后续规划工作。

| 项 | 内容 |
|----|------|
| 状态 | TCP client v1 (plain TCP) 与 TCP-TLS (SSL socket) 已完成 Air780EP / ML307R 单连接；UDP 和高级 socket 能力规划中 |
| 新增 ops | `socket_open`、`socket_send`、`socket_close`、`socket_recv`（缓存读模式） |
| v1 参数结构 | `modem_socket_open_t`（TCP proto、host、port、conn_id、timeout、原始模块错误码输出）、`modem_socket_send_t`（conn_id、data、len）、`modem_socket_recv_t`、`modem_socket_recv_result_t`、`modem_socket_close_t` |
| v1 协议类型 | `MODEM_PROTOCOL_TCP`（带 conn_id） |
| Air780EP AT | TCP client v1 使用 `AT+CIPSTART`、`AT+CIPSEND`、`AT+CIPRXGET=5`、`AT+CIPRXGET=3,<len>`、`AT+CIPCLOSE`；TCP-TLS 前置 `AT+CIPSSL=1`（SSL context 0，SNI 经 `AT+SSLCFG="hostname",0,...`），复用 `lwlte_ssl_provision()`；`+RECEIVE` / `+IPD` 属直推 RX 或多连接路径，留作非 v1/后续能力参考 |
| ML307R AT | `AT+MIPOPEN`、`AT+MIPSEND`、`AT+MIPCLOSE`、`AT+MIPRD`、`AT+MIPCFG`；TCP-TLS 前置 `AT+MIPCFG="ssl",<conn>,1,<ssl_id>` 再 `AT+MIPOPEN`，复用 `lwlte_ssl_provision()`；TCP v1 使用 `+MIPURC: "rtcp"` 接收缓存通知和 `+MIPURC: "disconn"` 断链通知 |
| 规划工作 | UDP (`MODEM_PROTOCOL_UDP`)、server/listen mode、local port binding、multiple simultaneous connections、automatic reconnect、high-throughput streaming。 |

#### P3 TLS/SSL 配置

| 项 | 内容 |
|----|------|
| 新增 ops | `ssl_provision`、`ssl_get_context_status` |
| 新增参数结构 | `modem_ssl_context_config_t`（context id、认证级别、TLS 版本、协商超时、证书时间/SNI 选项）、`modem_ssl_credentials_t`（CA/client cert/client key PEM 数据） |
| Air780EP AT | `AT+SSLCFG="<tag>",<ctx>[,<value>]`（ctx：0..5 TCP / 88 MQTT / 153 HTTP）；证书需先写入文件系统 |
| ML307R AT | `AT+MSSLCFG="<cmd>",<ssl_id>`、`AT+MSSLCERTWR`、`AT+MSSLKEYWR`（`>` 数据下载） |
| 建模要点 | SSL context id 被其它协议引用，**TLS 本身不建连**；ops 只负责配置与证书写入，连接由各协议 ops 在 open 时引用 ctx。 |

#### P4 HTTP/HTTPS

| 项 | 内容 |
|----|------|
| 前置 | 协议数据事件泛化；HTTPS 依赖 P3 的 SSL context |
| 新增 ops | `http_create`、`http_set_param`、`http_set_content`、`http_request`、`http_read`、`http_term` |
| 新增参数结构 | `modem_http_config_t`（url、header、content-type、ssl ctx）、`modem_http_request_t`（method、body） |
| 新增协议类型 | `MODEM_PROTOCOL_HTTP` |
| Air780EP AT | `AT+HTTPINIT`、`AT+HTTPPARA`、`AT+HTTPDATA`、`AT+HTTPACTION`、`AT+HTTPREAD`、`AT+HTTPTERM`；URC `+HTTPACTION:<method>,<code>,<len>` |
| ML307R AT | `AT+MHTTPCREATE`、`AT+MHTTPCFG`、`AT+MHTTPHEADER`、`AT+MHTTPCONTENT`、`AT+MHTTPREQUEST`、`AT+MHTTPREAD`、`AT+MHTTPDEL`；URC `+MHTTPURC: "rsp/header/content/err/closed"` |
| 开放问题 | 响应体读取按长度（可能二进制），不能按行解析；大体量响应是否需要流式（见第三部分）。 |

---

## 第三部分：Service 层修改（调用 ops 的编排）

Service 层职责：通过 `core_submit_cmd()` 串行驱动 `modem_ops`，管理自身状态机与事件上行。本项目已有三种编排范式，新能力按其特性归属其一：

- **Core 策略**：能力与 Core 已持有的网络状态机/命令串行化强耦合，并入 Core，不新建 service。
- **专属 service + 自有 FSM**：能力是有连接生命周期、需要独立状态机和 RX 流的上层协议（如 MQTT）。
- **Core 同步命令**：一次性请求/响应、调用方阻塞等结果（如 Ping）。

### 各能力编排分解

#### P1 低功耗 / 休眠管理 —— Core 策略（不新建 service）

| 项 | 内容 |
|----|------|
| 归属 | Core。power 配置并入 `core_config`。 |
| 新增命令/事件 | 睡眠/唤醒事件经 CORE → Facade 上行（如 `LWLTE_EVENT_SLEEP` / `LWLTE_EVENT_WAKE`）。 |
| 编排逻辑 | Core 在命令队列空闲时才允许模块进入睡眠；有命令待发时先唤醒再发；统一管理睡眠/唤醒状态并上抛事件。 |
| Facade API | `lwlte_set_power_mode()` / `lwlte_get_power_mode()`；事件 `LWLTE_EVENT_SLEEP` / `LWLTE_EVENT_WAKE`。 |

#### P2 TCP/UDP Socket —— 专属 service + 自有 FSM

| 项 | 内容 |
|----|------|
| 归属 | `tcp_client` service（类比 `mqtt_client`），TCP client v1 只管理一个 client connection。 |
| 新增命令 | `CORE_CMD_SOCKET_OPEN` / `CORE_CMD_SOCKET_SEND` / `CORE_CMD_SOCKET_RECV` / `CORE_CMD_SOCKET_CLOSE`。 |
| 新增协议数据 | `CORE_PROTOCOL_TCP`；`core_protocol_data_t` 携带 conn_id。 |
| 编排逻辑 | v1 固定 `conn_id=0`，RX 使用缓存读模式保证二进制安全；TCP-TLS 复用 `lwlte_ssl_provision()` 配置的 SSL context（Air780EP ctx 0、ML307R ssl_id 0），由 `lwlte_tcp_open_config_t.transport` 选择明文/TLS；后续再扩展多连接、UDP 和重连策略。 |
| Facade API | `lwlte_tcp_open/send/close()`；`lwlte_tcp_open_config_t` 新增 `transport`（`lwlte_tcp_transport_t`：plain/TLS）与 `ssl_context_id`；数据事件通过 `LWLTE_TCP_EVENT` 投递。 |

#### P3 TLS/SSL 配置 —— 横向能力（非独立 service）

| 项 | 内容 |
|----|------|
| 归属 | 不新建 service。证书/配置写入走 Core 命令，连接时由各协议 service 引用已配好的 context。 |
| 新增命令 | `CORE_CMD_SSL_PROVISION`、`CORE_CMD_SSL_GET_CONTEXT_STATUS` |
| 新增公开 API | `lwlte_ssl_provision()`、`lwlte_ssl_get_context_status()` |
| 查询语义 | 查询模块端证书/密钥对象是否存在；Air780EP 用文件系统查询，ML307R 用 `MSSLLIST`/`MSSLCHECK`。 |
| 编排逻辑 | 证书与安全参数一次性下发并绑定 context id；各协议 config 增加 `transport=TLS` + ssl context 引用。 |
| MQTT TLS 验证目标 | 使用当前 MQTT example server，端口切换为 `8883`，验证实机连接、订阅和 telemetry 发布。 |
| 复用现状 | 打通 `mqtt_client_config_t` 已预留的 `MQTT_CLIENT_TRANSPORT_TLS`；TCP-TLS (SSL socket) 已复用 `lwlte_ssl_provision()` 落地（Air780EP ctx 0 / ML307R ssl_id 0）；socket/http config 引用 ssl context。 |

#### P4 HTTP/HTTPS —— Core 同步命令（v1）

| 项 | 内容 |
|----|------|
| 归属 | Core 同步命令（类比 Ping），v1 不建独立 service。 |
| 新增命令 | `CORE_CMD_HTTP_REQUEST`（提交请求，阻塞等 done 返回 status code + body）。 |
| 流式扩展 | 大体量响应后续可升级为 `CORE_PROTOCOL_HTTP` 事件流式上报。 |
| 依赖 | HTTPS 依赖 P3 的 SSL context。 |
| Facade API | `lwlte_http_request()`（同步），返回状态码与响应体。 |

---

## 维护约定

- 能力状态变化（规划 → 进行中 → 已完成）时，更新「能力矩阵」对应行。
- 新增能力：在能力矩阵加行，并补全第二、三部分的 modem / service 分解。
- 每个能力开工时，仍按项目流程各自产出 spec（`docs/superpowers/specs/`）与 plan（`docs/superpowers/plans/`）；本文档只做顶层规划，不替代单能力详细设计。
- 本文档与 [架构概览](architecture.md) 的分层职责保持一致；若分层规则调整，同步更新本文档的三段结构。
