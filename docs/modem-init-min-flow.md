# LTE 模块最小初始化流程

本文整理 Air780EP 与 ML307R 从上电到应用层可开始工作的最小流程。目标是给 Core FSM / Modem Adapter 的初始化状态机提供边界：哪些状态可以靠 URC 推进，哪些步骤必须主动查询或发送 AT 命令，应用层何时才可以开始 TCP/MQTT/HTTP 等业务。

依据：

- `reference/上海合宙Cat.1模组(移芯EC618&EC716&EC718平台系列)AT命令手册V1.6.7.pdf`
- `reference/中移物联ML307R/AT_Commands_Reference_Guide_4G_Series_V2.0.5.pdf`
- `reference/中移物联ML307R/ML307R_通信流程示例-V1.1.2.pdf`
- `reference/中移物联ML307R/TCP_IP用户手册_5.1.2-R.pdf`
- `reference/中移物联ML307R/MQTT用户手册_V6.8.3.pdf`
- 2026-06-04 Air780EP 实机串口日志，模块固件 `AirM2M_780EP_V1010_LTE_AT`

## 最小可用状态

应用层最小可工作条件不是“模块已响应 AT”，而是以下条件同时成立：

- AT 通道可用：模块 ready，`ATE0`/`AT` 能稳定返回 `OK`。
- SIM 可用：`AT+CPIN?` 返回 `+CPIN: READY`，或可靠收到等价 `+CPIN: READY` URC。
- LTE/EPS 已注册：`AT+CEREG?` 或 `+CEREG` URC 中 `stat=1` 本地注册或 `stat=5` 漫游注册。若只实现一个注册判断，Cat.1 LTE 主路径优先用 `CEREG`。
- 分组域已附着：`AT+CGATT?` 返回 `+CGATT: 1`。某些流程中 PDP 激活会隐含附着，但初始化状态机仍建议显式查询作为诊断边界。
- 应用数据面已激活并拿到 IP：Air780EP 至少完成 `CSTT/CIICR/CIFSR` 或等价 PDP 激活并获得 IP；ML307R 至少完成 `MIPCALL` 并获得 IP。

TCP、MQTT、HTTP 是应用数据面之上的业务状态：

- TCP socket 可用还需要连接命令后的连接成功 URC，例如 Air780EP `CONNECT OK` / `<n>,CONNECT OK`，ML307R `+MIPOPEN: <connect_id>,0`。
- MQTT 可用还需要协议连接成功 URC，例如 Air780EP `CONNACK OK`，ML307R `+MQTTURC: "conn",<connect_id>,0`。
- HTTP 请求可用还需要请求命令后的结果 URC，例如 Air780EP `+HTTPACTION:`，ML307R `+MHTTPURC:`。

## Air780EP 最小流程

### 推荐状态机

| 阶段 | 最小动作 | 成功条件 | URC 可用性 | 说明 |
|------|----------|----------|------------|------|
| Power on | 拉 EN/供电后等待 AT 通道 | 收到 `RDY` 或 `AT`/`ATE0` 返回 `OK` | `RDY` 为自发启动提示 | 实测会出现重复 `RDY`，不要把第二个 `RDY` 视为错误。若 `ATE0` 因启动期 URC 穿插超时，应重试。 |
| AT parser setup | `ATE0`，`AT+CMEE=1` | 均返回 `OK` | 无 | 关闭 echo 后再进入稳定解析；`CMEE=1` 便于数字错误码映射。 |
| Enable registration URC | `AT+CEREG=2`，`AT+CGREG=2`，可选 `AT+CREG=2` | 均返回 `OK` | 这些命令只开启后续注册变化 URC | 不应只等 URC；若状态已稳定，设置命令后可能没有立即主动上报。 |
| Identify | `ATI`/`AT*I`/`AT+CGMM`/`AT+CGMR` 等 | 信息查询返回 `OK` | 无 | 非应用联网的硬门槛，但建议初始化期缓存型号、固件、IMEI/ICCID/IMSI。 |
| SIM ready | `AT+CPIN?` 轮询 | `+CPIN: READY` | `+CPIN:<code>` 可能自发上报 | 手册示例明确重启后 PIN 状态会自动上报，也明确 SIM 在位检测可触发 `+CPIN: READY` / `+CPIN: SIM REMOVED`；但实测和现有实现都不应依赖它必达。 |
| Registered | `AT+CEREG?`，必要时 `AT+CGREG?`/`AT+CREG?` 轮询 | `stat=1` 或 `stat=5` | 需先 `AT+CEREG=<n>` / `AT+CGREG=<n>` / `AT+CREG=<n>` 才有注册变化 URC | Air780EP 手册定义 `<n>=1/2/3` 开启不同字段的主动上报。 |
| PS attached | `AT+CGATT?` | `+CGATT: 1` | 无可靠独立 attach URC | PDP 激活失败时该状态用于定位是注册/附着问题还是 PDP 问题。 |
| Data plane | `AT+CSTT`，`AT+CIICR`，`AT+CIFSR` | `CIFSR` 返回本地 IP | `+CGEV` 可提示 PDN 激活/去激活 | `CSTT` 只在 `IP INITIAL` 有效；`CIICR` 只在 `IP START` 有效；`CIFSR` 返回纯 IP 行，不是 `OK` 终止的常规响应。 |

### Air780EP URC 分类

| URC | 是否自发 | 是否需要先配置 | 初始化用途 | 注意事项 |
|-----|----------|----------------|------------|----------|
| `RDY` | 是 | 否 | 模块启动/AT ready 候选信号 | 仍需 `ATE0`/`AT` 成功作为 AT 通道最终确认。 |
| `+CPIN:<code>` | 可能 | 否 | SIM 状态提示 | 手册示例包含重启后自动上报和 SIM 插拔检测上报；初始化必须保留 `AT+CPIN?` 轮询兜底。 |
| `+CREG:` | 否，除非已开启 | `AT+CREG=<n>`，`n=1/2/3` | 通用注册状态变化 | 同前缀也用于 `AT+CREG?` 查询响应。 |
| `+CEREG:` | 否，除非已开启 | `AT+CEREG=<n>`，`n=1/2/3/4/5` | LTE/EPS 注册状态变化 | Cat.1 LTE 主路径优先使用。 |
| `+CGREG:` | 否，除非已开启 | `AT+CGREG=<n>`，`n=1/2/3/4/5` | 分组域注册状态变化 | 同前缀也用于 `AT+CGREG?` 查询响应。 |
| `+CGEV: ME PDN ACT <cid>[,...]` | 是 | 手册列 `AT+CGEREP` 可控制，但当前 Air780EP 实测/项目摘录显示该命令不支持，`+CGEV` 默认可出现 | PDN 激活提示 | 实机在 AT 初始化完成前出现 `+CGEV: ME PDN ACT 1,0`，说明模块可先自动激活某个 PDN；仍需 `CIFSR`/`CGPADDR` 获取并确认 IP。 |
| `+CGEV: NW/ME PDN DEACT <cid>` | 是 | 同上 | 网络侧或模块侧去激活提示 | 可作为掉线/恢复入口。 |
| `^MODE:` / `+E_UTRAN Service` / `+NITZ:` | 是 | 否 | 诊断/日志 | 可辅助观察制式、服务和网络时间，但不应作为最小联网 gate。 |
| `CONNECT OK` / `<n>,CONNECT OK` | 命令结果 URC | 先发 `CIPSTART` 或 MQTT transport 连接命令 | TCP/MQTT transport connected | 不是 PDP ready 条件，是 socket/transport ready 条件。 |
| `CONNACK OK` | 命令结果 URC | 先发 `MCONNECT` | MQTT connected | 收到后才允许 publish/subscribe。 |

### Air780EP 实机记录

2026-06-04 使用 `docs/agents/serial_monitor.py --timeout 45` 抓取当前连接硬件，关键顺序如下：

```text
I (1311) at_engine: RX:|RDY
I (1311) at_engine: TX:|ATE0
I (2051) at_engine: RX:|RDY
I (4361) at_engine: RX:|^MODE: 17,17
I (4361) at_engine: RX:|+E_UTRAN Service
I (4361) at_engine: RX:|+CGEV: ME PDN ACT 1,0
I (4421) at_engine: RX:|+NITZ: 26/06/04,09:04:18+32,0
W (10311) modem_air780ep: ATE0 failed (attempt 1/3): ESP_ERR_TIMEOUT
I (10311) at_engine: TX:|ATE0
I (10311) at_engine: RX:|ATE0
I (10311) at_engine: RX:|OK
I (10321) at_engine: TX:|AT+CEREG=2
I (10321) at_engine: RX:|OK
I (10321) at_engine: TX:|AT+CGREG=2
I (10331) at_engine: RX:|OK
I (10331) at_engine: TX:|AT+CREG=2
I (10331) at_engine: RX:|OK
I (10381) at_engine: TX:|AT+CPIN?
I (10391) at_engine: RX:|+CPIN: READY
I (10411) at_engine: TX:|AT+CEREG?
I (10421) at_engine: RX:|+CGREG: 2,1,"2797","02B82E17",7
I (10431) at_engine: TX:|AT+CGATT?
I (10441) at_engine: RX:|+CGATT: 1
I (10441) at_engine: TX:|AT+CSTT
I (10441) at_engine: RX:|OK
I (10441) at_engine: TX:|AT+CIICR
I (10451) at_engine: RX:|OK
I (10451) at_engine: TX:|AT+CIFSR
I (10461) at_engine: RX:|10.76.1.35
I (10471) at_engine: RX:|+CGACT: 1,1
I (10481) at_engine: RX:|+CGPADDR: 1,"10.76.1.35"
I (10481) appmain: LTE event: NET_ONLINE net=ONLINE err=0
I (11681) appmain: ping summary: sent=4 recv=4 lost=0 min=250ms max=400ms avg=290ms
```

结论：当前 Air780EP 硬件可以利用自发 `RDY`、`+CGEV`、`^MODE`、`+E_UTRAN Service`、`+NITZ` 做快速状态提示，但最小初始化仍应以命令确认闭环为准：`ATE0 OK`、`CPIN READY`、注册查询、`CGATT=1`、`CIFSR` 返回 IP。实测还暴露一个解析风险：`AT+CEREG?` 期间收到的行是 `+CGREG: 2,1,...`，说明命令期 URC/响应穿插必须由 AT Engine 或上层轮询兜底处理，不能盲信单次查询响应。

## ML307R 最小流程

### 推荐状态机

| 阶段 | 最小动作 | 成功条件 | URC 可用性 | 说明 |
|------|----------|----------|------------|------|
| Power on | 等待 `+MATREADY`；若自适应波特率无上报，则周期发送 `AT` | `+MATREADY` 或 `AT` 返回 `OK` | `+MATREADY` 为自发启动提示，但自适应波特率模式可能没有 | 通信流程手册明确：`+MATREADY` 表示模块初始化完成，可进行 AT 操作。 |
| AT serialization | 确保上一条 AT 完成后再发下一条 | 无并发 AT | 无 | 手册明确不能同时发送多条 AT 命令。 |
| AT parser setup | `ATE0`，`AT+CMEE=1` | 均返回 `OK` | 无 | 默认 echo 可能已关闭，仍建议显式设置。 |
| Optional CFUN gate | `AT+CFUN?`，必要时 `AT+CFUN=1` | `+CFUN: 1` | 无 | 手册要求 `+MATREADY` 后至少等待 2 秒才能执行 `AT+CFUN=0/1`。 |
| Enable registration URC | `AT+CEREG=2`，`AT+CGREG=2`，可选 `AT+CREG=2` | 均返回 `OK` | 这些命令只开启后续注册变化 URC | 正常上电会自动驻网，仍需 `AT+CEREG?` 查询确认。 |
| SIM ready | `AT+CPIN?` 轮询 | `+CPIN: READY` | `+CPIN:` 可作为 SIM 状态事件 | 不应只等 URC。 |
| Registered | `AT+CEREG?` 轮询，必要时查 `CGREG/CREG` | `stat=1` 或 `stat=5` | 需先 `AT+CEREG=<n>` / `AT+CGREG=<n>` / `AT+CREG=<n>` 才有注册变化 URC | 通信流程手册明确正常上电后自动驻网，可用 `AT+CEREG?` 查询是否成功。 |
| Data plane | 先查 `AT+MIPCALL?`，若未激活则 `AT+CGDCONT=<cid>,...` 后 `AT+MIPCALL=1,<cid>` | `+MIPCALL: <cid>,1,"<ip>"[...]` | `+MIPCALL` 可能来自自动拨号，也可能是命令结果 | ML307R 应用层联网主路径是 `MIPCALL`，不是 Air780EP 的 `CSTT/CIICR/CIFSR`。 |

### ML307R URC 分类

| URC | 是否自发 | 是否需要先配置 | 初始化用途 | 注意事项 |
|-----|----------|----------------|------------|----------|
| `+MATREADY` | 是，但自适应波特率模式可能没有 | 否 | 模块 AT ready gate | 无该 URC 时用 `AT` 返回 `OK` 作为兜底。 |
| `+CPIN:` | 可能 | 否 | SIM 状态事件 | 同前缀也用于 `AT+CPIN?` 查询响应。 |
| `+CREG:` | 否，除非已开启 | `AT+CREG=<n>`，`n=1/2/3` | 通用注册状态变化 | 手册定义 `<n>=1` 上报注册变化，`<n>=2` 增加小区信息，`<n>=3` 增加原因值。 |
| `+CEREG:` | 否，除非已开启 | `AT+CEREG=<n>`，`n=1/2/3/4/5` | LTE/EPS 注册状态变化 | LTE 主路径优先使用。 |
| `+CGREG:` | 否，除非已开启 | `AT+CGREG=<n>`，`n=1/2/3`，手册也描述 PSM 相关扩展形态 | 分组域注册状态变化 | 同前缀也用于查询响应。 |
| `+MIPCALL: <cid>,1,"<ip>"[...]` | 可能 | 自动拨号无需配置；手动拨号需先发 `AT+MIPCALL=1,<cid>` | 应用层数据面激活成功 | 上电可能已自动拨号激活 `cid=1`，所以初始化应先 `AT+MIPCALL?`。 |
| `+MIPCALL:<cid>,0` / `+MIPCALL: <cid>,0` | 可能 | 可由 `AT+MIPCALL=0,<cid>`、`AT+CFUN=4` 或网络变化触发 | 应用层数据面断开 | `AT+MIPCALL=0,<cid>` 只断开网络连接，不保证去激活 PDP；需去激活 PDP 时手册建议 `AT+CFUN=4`。 |
| `+MIPOPEN: <connect_id>,0` | 命令结果 URC | 先发 `AT+MIPOPEN=...` | TCP/UDP socket connected | `<result>` 非 0 表示连接失败；透传模式成功为 `CONNECT`。 |
| `+MIPURC: "rbuf"/...` | 连接后自发 | 取决于 `MIPOPEN` access mode 和 `MIPCFG` | socket RX/closed/error/ack 事件 | 缓存模式更适合避免 payload 混入 AT 流。 |
| `+MQTTURC: "conn",<id>,0` | 命令结果 URC | 先发 `AT+MQTTCONN=...` | MQTT connected | `conn_state=0` 才是成功；`2/3/4/5/6/255` 都不是 connected。 |
| `+MQTTURC: "suback"/"puback"/...` | 命令结果或运行期事件 | 先建立 MQTT 并执行订阅/发布等命令 | MQTT 操作完成 | `pingresp` 需要配置心跳回显才会上报。 |
| `+MLPMENTER:` / `+MLPMEXIT:` | 否，除非已开启 | `AT+MLPMCFG="sleepind",<n>` | 低功耗状态事件 | 与最小联网无关；长连接场景可能需锁定睡眠，避免深睡断链。 |

### ML307R 手册关键边界

- `+MATREADY` 表示模块初始化完成，可进行 AT 操作。
- 自适应波特率模式时可能没有 `+MATREADY`，串口需先输入 `AT`，返回 `OK` 后继续。
- 每条 AT 命令执行完毕后才能发下一条，不能并发。
- `+MATREADY` 后至少等待 2 秒才能执行 `AT+CFUN=0` 或 `AT+CFUN=1`。
- 正常上电后模块会自动完成驻网，可通过 `AT+CEREG?` 查询是否驻网成功。
- 上电可能已自动拨号激活网络，可通过 `AT+MIPCALL?` 查询。
- 如果未激活，典型流程是 `AT+CGDCONT=1,"IPV4V6","<apn>"` 后 `AT+MIPCALL=1,1`，等待 `+MIPCALL: 1,1,"<ipv4>"[...]`。
- `AT+MIPCALL=0,1` 仅断开网络连接，不会去激活 PDP；需要去激活 PDP 时建议 `AT+CFUN=4`。

## URC 驱动与轮询的取舍

初始化可以用 URC 加速，但不能只靠 URC：

- 启动 ready 可以由 `RDY` 或 `+MATREADY` 快速触发，但最终仍以 `AT`/`ATE0` 返回 `OK` 确认 AT 通道可用。
- SIM ready 可以消费 `+CPIN: READY`，但必须保留 `AT+CPIN?` 轮询，因为手册只保证某些场景会主动上报，不保证初始化期必达。
- 注册状态建议先发送 `CEREG/CGREG/CREG=<n>` 开启后续变化 URC，再立即查询当前状态；如果已经注册，设置 URC 后可能没有状态变化，因此不会主动上报。
- PDP/PDN 事件 URC 适合作为异步状态变化输入，但应用层上线必须以“拿到 IP”为准。Air780EP 是 `CIFSR`/`CGPADDR`，ML307R 是 `MIPCALL` 返回 IP。
- TCP/MQTT/HTTP 的连接/请求结果通常本来就是异步 URC；这些 URC 属于业务操作结果，不应混入基础 PDP ready gate。

当前 `at_engine` 第一版在命令等待期间会把非最终响应行优先放入当前命令响应；只有空闲期才按 URC 前缀分发。因此实现状态机时应按以下规则处理：

- 命令相关异步结果由发起命令的方法等待并解析，例如 `MIPCALL`、`MIPOPEN`、`MQTTURC "conn"`。
- 空闲期 URC 由注册前缀分发为全局事件，例如注册状态变化、PDN deact、socket closed。
- 对 `+CREG:`、`+CEREG:`、`+CGREG:`、`+CPIN:`、`+MIPCALL:` 这类查询响应与 URC 同前缀的行，不要假设命令期间一定能被独立分发。

## 模块差异速查

| 主题 | Air780EP | ML307R |
|------|----------|--------|
| 启动 ready | `RDY`，再用 `AT`/`ATE0` 确认 | `+MATREADY`；自适应波特率无上报时用 `AT` 兜底 |
| LTE 注册主判断 | `CEREG`，辅以 `CGREG/CREG` | `CEREG`，辅以 `CGREG/CREG` |
| 注册 URC 开启 | `AT+CEREG=<n>`、`AT+CGREG=<n>`、`AT+CREG=<n>` | 同左 |
| SIM 判断 | `AT+CPIN?`，可消费 `+CPIN:` | `AT+CPIN?`，可消费 `+CPIN:` |
| 数据面主路径 | `AT+CSTT` -> `AT+CIICR` -> `AT+CIFSR` | `AT+MIPCALL?` / `AT+MIPCALL=1,<cid>` |
| 数据面成功条件 | `CIFSR` 返回 IP，必要时 `CGACT?/CGPADDR` 校验 | `+MIPCALL: <cid>,1,"<ip>"[...]` |
| PDP/PDN 事件 | `+CGEV` | `+MIPCALL` 更贴近应用层拨号；标准 `CGACT/CGPADDR` 可作诊断 |
| TCP 连接结果 | `CONNECT OK` / `<n>,CONNECT OK` | `+MIPOPEN: <connect_id>,0` |
| MQTT 连接结果 | `CONNECT OK` 后 `CONNACK OK` | `+MQTTURC: "conn",<connect_id>,0` |
| 去激活注意 | `CIPSHUT` 关闭 TCPIP 场景；`CGACT=0,<cid>` 为标准 PDP 路径 | `MIPCALL=0,<cid>` 不保证 PDP 去激活；需去激活 PDP 时用 `CFUN=4` |

## 推荐最小实现策略

1. 上电后进入 `WAIT_AT_READY`，消费 ready URC，但用 `AT`/`ATE0` 成功作为出口。
2. 进入 `CONFIG_AT`，关闭 echo，开启 `CMEE=1`，设置注册 URC 为 `CEREG=2`、`CGREG=2`、`CREG=2`。
3. 进入 `WAIT_SIM`，消费 `+CPIN:`，同时周期 `AT+CPIN?`，直到 `READY`。
4. 进入 `WAIT_REGISTERED`，消费 `+CEREG/+CGREG/+CREG`，同时周期查询 `AT+CEREG?`，直到 `stat=1/5`。
5. 进入 `WAIT_ATTACHED`，查询 `AT+CGATT?`，直到 `1` 或让数据面激活失败回退到该状态。
6. Air780EP 进入 `ACTIVATE_AIR780EP_DATA`，执行 `CSTT/CIICR/CIFSR`，以 IP 地址作为上线条件。
7. ML307R 进入 `ACTIVATE_ML307R_DATA`，先 `MIPCALL?`，未激活再 `CGDCONT` + `MIPCALL=1,<cid>`，以 `+MIPCALL` 中 IP 地址作为上线条件。
8. 只有数据面 online 后，才允许 socket、MQTT、HTTP 等应用层对象启动自己的连接状态机。
