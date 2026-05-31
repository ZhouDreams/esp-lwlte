# Air780EP AT 指令摘录

本文从 `reference/上海合宙Cat.1模组(移芯EC618&EC716&EC718平台系列)AT命令手册V1.6.7.pdf` 中摘录 Air780EP 在 `modem_air780ep_t` 中需要优先整合的 AT 指令。

本文不是完整 AT 手册，只覆盖后续 Modem Adapter 层实现所需的基础能力：模块识别、AT 口初始化、SIM/信号/网络注册、PDP 与 TCPIP 激活、TCP/UDP Socket、PING、HTTP、MQTT、休眠低功耗和相关 URC。

## 使用边界

本轮包含：

- 模块身份与基础控制
- 串口、回显、错误结果码和配置保存
- SIM 状态、信号质量、网络注册状态
- 分组域、PDP、TCPIP 激活和基础连通性检查
- TCP/UDP Socket、DNS、SSL、手动取数、保活和心跳
- HTTP 请求、响应读取、下载到文件系统和 HTTP URC
- MQTT 连接、发布、订阅、消息缓存和 MQTT URC
- 休眠、低功耗和 RI 唤醒相关配置
- 系统/联网相关 URC 注册清单

本轮不包含：

- FTP 等未进入当前实现优先级的上层业务协议指令
- SMS、语音、GNSS、文件系统等业务功能指令
- 固件升级、GPIO/ADC、VSIM 等非基础联网流程指令

## 实现注意事项

当前 `at_engine` 第一版在有当前命令时，会把非最终响应行优先放入当前 `at_response_t`；只有无当前命令时才按 URC 前缀分发。因此 `+CREG:`、`+CEREG:`、`+CGREG:` 这类既可作为查询响应、又可作为 URC 的前缀，在命令等待期间不会被可靠地区分为独立 URC。

`modem_air780ep_t` 第一版应按以下规则实现：

- 查询响应由对应命令方法解析。
- 空闲期 URC 由注册前缀分发并翻译为 Modem 事件。
- 不依赖命令等待期间的自发 URC 独立分发。
- 若后续需要命令期间关键 URC 分发，应先扩展 AT Engine 的 URC 判定策略。

默认超时说明：手册未单独列出的 AT 命令最大响应时间为 9 秒；手册例外包括 `CPIN=180s`、`COPS=300s`、`CGACT=108s`、`CGATT=108s`、`CFUN=45s`、`CSTT=60s`、`CIICR=90s`、`CIPSHUT=90s`。文档中的默认超时是给 `modem_air780ep_t` 单次调用 `at_engine_send_cmd()` 的实现建议；当实现应使用上层轮询/重试总预算时，表格可推荐短于手册最大响应时间的单次查询超时。

## 文档字段说明

| 字段 | 含义 |
|------|------|
| 能力 | 该指令服务的 modem 能力 |
| AT 指令 | 需要发送到模块的命令格式 |
| 响应格式 | 需要解析的典型响应 |
| 关键参数/数据 | 需要关注的参数和值 |
| 默认超时 | `modem_air780ep_t` 调用 `at_engine_send_cmd()` 的建议超时 |
| 映射建议 | 后续 Modem 方法、初始化步骤或 URC handler 的建议映射 |
| 注意事项 | 手册中的限制、持久化要求或实现风险 |

## 模块身份与基础控制

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 查询产品信息 | `ATI` | `<module info>` + `OK` | 固件/产品信息字符串 | 9s | `modem_air780ep_t` 内部 get_info 步骤，填充后由 `modem_get_info()` 读取 | 响应与 `AT+VER` 一致，可二选一 |
| 查询固件版本 | `AT+VER` | `<firmware ver>` + `OK` | 固件版本字符串，如 `AirM2M_780E_V1120_LTE_AT` | 9s | `modem_info_t.fw_revision` | 手册说明返回结果与 `ATI` 一致 |
| 查询模块型号 | `AT+CGMM` | `+CGMM: "<model>"` + `OK` | 型号字符串，如 `Air780E` | 9s | `modem_info_t.model` | Air780EP 可能返回具体系列型号 |
| 查询版本信息 | `AT+CGMR` | `+CGMR: "<revision>"` + `OK` | 软件版本标识 | 9s | `modem_info_t.fw_revision` | 可用于判断 AT 固件版本能力 |
| 查询 IMEI | `AT+CGSN` | `<IMEI>` + `OK` | 15 位 IMEI | 9s | `modem_info_t.imei` | 纯数据行，无 `+CGSN:` 前缀 |
| 查询 IMSI | `AT+CIMI` | `<IMSI>` + `OK` | 15 位 IMSI | 9s | `modem_info_t.imsi` | SIM 未 ready 时可能失败 |
| 重启模块 | `AT+RESET` | `OK` | 无 | 9s | 保留为 AT 参考，不映射当前 `modem_reset()` | 当前 Air780EP reset 通过 EN 硬复位实现，等待 `RDY` 后重新执行初始化序列 |
| 功能模式 | `AT+CFUN=<fun>[,<rst>]`，`AT+CFUN?` | `+CFUN: <fun>` + `OK` | `0` 最少功能；`1` 全功能；`4` 飞行模式；`rst=1` 复位 ME | 45s | Air780EP 初始化/复位内部辅助步骤 | `AT+CFUN=1,1` 可主动重启模块 |

## 串口与结果码配置

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 关闭回显 | `ATE0` | `OK` | `0` 关闭；`1` 打开 | 9s | 初始化第一步 | 关闭 echo 后可减少响应解析复杂度 |
| 错误结果码 | `AT+CMEE=<n>`，`AT+CMEE?` | `+CMEE: <n>` + `OK` | `0` 使用 `ERROR`；`1` 数字型 `+CME ERROR:<err>`；`2` 冗长文本 | 9s | 初始化设置 `AT+CMEE=1` | 数字型最适合 `at_response_t.error_code` |
| 固定波特率 | `AT+IPR=<rate>`，`AT+IPR?` | `+IPR: <rate>` + `OK` | `0` 自适应；支持 `9600`、`115200`、`921600` 等 | 9s | 板级初始化或调试配置 | 需要持久化时使用 `AT+IPR=<rate>;&W` |
| 流控 | `AT+IFC=<dce_by_dte>,<dte_by_dce>`，`AT+IFC?` | `+IFC: <dce_by_dte>,<dte_by_dce>` + `OK` | `0` 无流控；`1` 软件流控；`2` 硬件流控 | 9s | 高吞吐场景配置硬件流控 | 使用硬件流控建议 `AT+IFC=2,2;&W` |
| 保存配置 | `AT&W` | `OK` | 无 | 9s | 仅配置持久化时调用 | 不要在每次启动都无意义写 NV |

## SIM 与网络状态

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 查询 PIN/SIM 状态 | `AT+CPIN?` | `+CPIN: <code>` + `OK`；SIM 初始化忙时可能返回 `+CME ERROR: 14` | `READY` 可用；`SIM PIN` 等待 PIN；`SIM REMOVED` 未检出；`+CME ERROR: 14` 为 `SIM busy` | 9s | SIM-ready 初始化内部步骤 | 单次查询保持 9s；SIM-ready 等待应由上层轮询/重试总预算实现，而不是一次 `AT+CPIN?` 等待 180s。按合宙快速入门建议，遇到 SIM busy 应每 1 秒轮询，10 秒内仍未 ready 再按超时处理。手册列出 URC `+CPIN:<code>`，但示例只明确重启后 PIN 状态自动上报和 `AT+CSDT` SIM 在位检测触发；实测 Air780EP `AT+CPIN?` 返回 `+CME ERROR: 14` 后不保证主动上报 `+CPIN: READY`，必须保留轮询兜底。 |
| 查询 ICCID | `AT+CCID` 或 `AT+ICCID` | `AT+CCID` 返回 `<iccid>` + `OK`；`AT+ICCID` 返回 `+ICCID:<iccid>` + `OK` | ICCID 通常 20 位数字 | 9s | `modem_info_t.iccid` | `AT+ICCID` 有前缀，解析更明确 |
| 查询信号质量 | `AT+CSQ` | `+CSQ: <rssi>,<ber>` + `OK` | `rssi=0..31,99`；`ber=0..7,99` | 9s | `modem_get_signal()` | dBm 可按常见公式约算：`rssi <= 31` 时 `-113 + 2*rssi` |
| 查询扩展信号 | `AT+CESQ` | `+CESQ: <rxlev>,<rxqual>,<rscp>,<ecno>,<rsrq>,<rsrp>` + `OK` | LTE 重点解析 `rsrq`、`rsrp`；`255` 表示未知 | 9s | 内部诊断/未来扩展；第一版可继续用基于 CSQ 的 `modem_get_signal()` | 可作为 `AT+CSQ` 的补充，不必第一版强依赖 |
| 网络注册状态 | `AT+CREG=<n>`，`AT+CREG?` | `+CREG: <n>,<stat>[,<lac>,<ci>,<act>]` + `OK` | `stat=1` 本地已注册；`5` 漫游已注册；`2` 搜索中；`3` 拒绝 | 9s | 2G/通用注册状态查询和 URC | `n=1/2/3` 会启用 URC；同前缀也用于查询响应 |
| EPS 注册状态 | `AT+CEREG=<n>`，`AT+CEREG?` | `+CEREG: <n>,<stat>[,<tac>,<ci>,<act>...]` + `OK` | LTE/E-UTRAN 注册状态；`act=7` 为 E-UTRAN | 9s | Air780EP 优先使用的注册状态 | 建议 `n=2` 或 `n=3` 视是否需要失败原因 |
| GPRS 注册状态 | `AT+CGREG=<n>`，`AT+CGREG?` | `+CGREG: <n>,<stat>[,<lac>,<ci>[,...]]` + `OK` | 分组域注册状态 | 9s | 数据域注册状态和 URC | `n=1/2/3/4/5` 控制不同 URC 和扩展字段内容 |
| 运营商查询 | `AT+COPS?` | `+COPS: <mode>[,<format>,<oper>,<act>]` + `OK` | `oper` 运营商；`act=7` LTE | 300s | 诊断接口或日志字段 | 查询可能较慢，初始化主路径不应频繁调用 |
| 系统信息 | `AT^SYSINFO` | `^SYSINFO:<srv_status>,<srv_domain>,<roam_status>,<sys_mode>,<sim_state>,<sys_submode>` + `OK` | `srv_status=2` 有效服务；`sys_mode=17` LTE；`sim_state=1` SIM 有效 | 9s | 诊断或状态快照 | 可补充判断服务域和漫游状态 |

## 分组域/PDP 与 TCPIP 激活

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 查询 GPRS 附着 | `AT+CGATT?` | `+CGATT: <state>` + `OK` | `0` 分离；`1` 附着 | 108s | `modem_air780ep_t` 联网初始化内部附着检查 | 只查询时通常远小于最大超时，设置附着/分离才可能较慢 |
| PDP 上下文定义 | `AT+CGDCONT=<cid>,"IP","<apn>"`，`AT+CGDCONT?` | `+CGDCONT: <cid>,<PDP_type>,<APN>,<PDP_addr>,...` + `OK` | `cid=1..15`；`PDP_type="IP"`；`APN` 可为空请求签约值 | 9s | `modem_set_apn()` | 模块注册后常已有默认 PDP 上下文供 RNDIS/TCPIP 使用 |
| PDP 鉴权 | `AT+CGAUTH=<cid>,<auth_prot>,<userid>,<password>` | `OK` | `auth_prot=0` none；`1` PAP；`2` CHAP | 9s | 专网卡 APN 鉴权内部配置；需要时再扩展公开 API | 仅在 APN 需要用户名密码时使用 |
| PDP 激活/去激活 | `AT+CGACT=<state>,<cid>`，`AT+CGACT?` | `+CGACT: <cid>,<state>` + `OK` | `state=0` 未激活；`1` 激活 | 108s | `modem_activate_pdp()` / `modem_deactivate_pdp()` 标准 PDP 路径或诊断 | 本项目第一阶段可优先使用 `CSTT/CIICR/CIFSR` TCPIP 流程 |
| 查询 PDP 地址 | `AT+CGPADDR=<cid>` | `+CGPADDR: <cid>,<PDP_ipv4_addr>[,<PDP_ipv6_addr>]` + `OK` | IPv4/IPv6 地址字符串 | 9s | `modem_get_pdp_context()` 组合/缓存 `AT+CGDCONT?`、`AT+CGACT?` 和 `AT+CGPADDR` 的结果 | 第一版实现只校验并缓存 IPv4 地址；IPv6 地址后续扩展。`AT+CGPADDR` 只提供地址，不能单独还原 APN/激活状态；PDP 未建立时无法查询地址 |
| ~~GPRS 事件 URC 开关~~ | ~~`AT+CGEREP=<mode>[,<bfr>]`，`AT+CGEREP?`~~ | ~~`+CGEREP: <mode>,<bfr>` + `OK`~~ | ~~`mode=0` 缓冲不转发；`mode=1` 空闲时转发，在线数据状态丢弃~~ | ~~9s~~ | ~~初始化时启用 `AT+CGEREP=1`~~ | ~~Air780EP 模块不支持此指令，始终返回 ERROR，已从初始化流程中移除。`+CGEV` PDN 事件 URC 默认启用，无需额外配置。~~ |
| TCPIP 设置 APN | `AT+CSTT` 或 `AT+CSTT=<apn>[,<username>,<password>]` | `OK` | APN 为空时模块按自动获取 APN 设置 | 60s | `modem_activate_pdp()` 的 Air780EP TCPIP 激活步骤 | 仅在 `IP INITIAL` 状态设置有效，成功后进入 `IP START`；该 TCPIP 路径为全局移动场景，本实现仅支持 `cid=1` 激活，`cid=2..4` 返回 `ESP_ERR_NOT_SUPPORTED` |
| 激活移动场景 | `AT+CIICR` | `OK` 或 `ERROR` | 激活 GPRS/PDP 场景 | 90s | `modem_activate_pdp()` 的 Air780EP TCPIP 激活步骤 | 只在 `IP START` 状态有效，成功后进入 `IP GPRSACT`；该命令不携带 CID，本实现仅支持 `cid=1` |
| 查询本地 IP | `AT+CIFSR` | `<IP address>` 或 `ERROR` | 本地 IP 字符串 | 9s | `modem_activate_pdp()` 成功后填充 `modem_pdp_context_t.ip_addr` | 仅在场景已激活后可用；返回纯 IP 成功行，不是 `OK` 终止的常规响应，也不是 `+CIFSR:` 前缀；`AT+CIFSR` 使用 `at_engine_send_cmd_with_options()` 和 `AT_CMD_SUCCESS_MATCH_ANY_LINE` 处理纯 IP 成功行；该命令不携带 CID，本实现仅用于 `cid=1`。 |
| 查询 TCPIP 状态 | `AT+CIPSTATUS` | `OK` + `STATE: <state>` | `IP INITIAL`、`IP STATUS`、`CONNECT OK`、`PDP DEACT` 等 | 9s | Air780EP 错误恢复内部状态检查 | 可用于决定是否先 `AT+CIPSHUT`；若 `OK` 先于 `STATE: <state>` 出现，当前 `at_engine` 可能提前结束响应，实现前必须验证顺序或使用自定义解析 |
| 关闭移动场景 | `AT+CIPSHUT` | `SHUT OK` 或 `ERROR` | 无 | 90s | `modem_deactivate_pdp()` 或错误恢复清理步骤 | 多连接时会关闭所有 IP 连接；`AT+CIPSHUT` 使用 `at_engine_send_cmd_with_options()` 和 exact match `SHUT OK` 处理非标准成功终止行；该 TCPIP 关闭路径为全局操作，本实现仅支持 `cid=1` 去激活，`cid=2..4` 返回 `ESP_ERR_NOT_SUPPORTED`。 |
| Ping 连通性 | `AT+CIPPING="<host>"[,<retryNum>,<dataLen>,<timeout>,<ttl>]` | 多行 `+CIPPING: <replyId>,<IpAddress>,<replyTime>,<ttl>` + `OK` | `retryNum=1..100`；`replyTime` 毫秒 | 9s | 内部联网自检或诊断命令 | 执行前需已激活 GPRS/PDP 上下文 |

### `+CGEV` 分组域事件

| URC | 含义 | 映射建议 |
|-----|------|----------|
| `+CGEV: ME PDN ACT <cid>[,<pdnReason>]` | 模块激活 PDP/PDN | `MODEM_EVENT_PDP_ACTIVATED` |
| `+CGEV: NW PDN DEACT <cid>` | 网络侧去激活 PDP/PDN | `MODEM_EVENT_PDP_DEACTIVATED` + 网络掉线 |
| `+CGEV: ME PDN DEACT <cid>[,<pdnReason>]` | 模块侧去激活 PDP/PDN | `MODEM_EVENT_PDP_DEACTIVATED` |
| `+CGEV: NW MODIFY ...` | 网络修改上下文 | 第一版记录日志 |
| `+CGEV: ME MODIFY ...` | 模块修改上下文 | 第一版记录日志 |

## TCPIP 连接层与 PING

本节摘录手册第 12 章“嵌入式 TCPIP 命令”和 `AT+CIPPING`。其中 `CSTT/CIICR/CIFSR/CIPSTATUS/CIPSHUT/CIPPING` 已在 PDP 激活表中列出，这里按连接层实现补充完整 TCP/UDP、SSL、DNS、收发、关闭、保活和心跳相关指令。

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 多连接开关 | `AT+CIPMUX=<n>`，`AT+CIPMUX?` | `+CIPMUX: <n>` + `OK` | `0` 单连接；`1` 多连接 | 9s | Socket 层初始化配置 | 只在 `IP INITIAL` 状态可设置；第一版可先固定单连接或按 socket 层能力开启多连接 |
| TCP SSL 开关 | `AT+CIPSSL=<n>`，`AT+CIPSSL?` | `+CIPSSL: <n>` + `OK` | `0` 关闭；`1` 开启 | 9s | TLS socket 连接前配置 | `CIPSTART` 前设置；当前仅作为 SSL Client；部分固件变体才支持 |
| SSL 参数 | `AT+SSLCFG="<tag>",<ctx>[,<value>]` | `+SSLCFG: "<tag>",<ctx>,<value>` + `OK` 或 `OK` | `ctx=0..5` TCP；`88` MQTT；`153` HTTP；`tag` 包括 `sslversion`、`ciphersuite`、`cacert`、`clientcert`、`clientkey`、`seclevel`、`hostname`、`ignorelocaltime`、`negotiatetimeout` | 9s | TLS 证书与安全等级配置 | `sslversion=3` 为 TLS1.2；`seclevel=0` 不鉴权、`1` 服务器鉴权、`2` 双向鉴权；证书文件需先写入文件系统 |
| 本地端口 | `AT+CLPORT=<mode>,<port>` 或 `AT+CLPORT=<n>,<mode>,<port>`，`AT+CLPORT?` | `+CLPORT: ...` + `OK` | `mode="TCP"/"UDP"`；`port=1..65535`；多连接 `n=0..5` | 9s | 可选 socket bind/local port 能力 | 单连接和多连接语法不同；非必须功能 |
| 建立 TCP/UDP 连接 | 单连接：`AT+CIPSTART=<mode>,<server>,<port>`；多连接：`AT+CIPSTART=<n>,<mode>,<server>,<port>` | 立即 `OK`，随后 `CONNECT OK`、`<n>,CONNECT OK`、`CONNECT`、`ALREADY CONNECT`、`CONNECT FAIL` 或 `STATE:<state>` | `mode="TCP"/"UDP"`；`server` 为 IP 或域名；`port=1..65535`；`n=0..5` | 9s 发送命令；连接结果按业务预算等待 | `modem_socket_connect()` | 异步 URC 表示最终结果；单连接需处于 `IP INITIAL`、`IP STATUS` 或 `TCP/UDP CLOSED`；状态异常时先 `AT+CIPSHUT` |
| TCPIP 应用模式 | `AT+CIPMODE=<mode>`，`AT+CIPMODE?` | `+CIPMODE: <mode>` + `OK` | `0` 非透传；`1` 透传 | 9s | Socket 发送/接收模式配置 | 只在 `IP INITIAL` 状态可设置；只有 TCP 单连接支持透传；第一版建议非透传 |
| 非透传发送模式 | `AT+CIPQSEND=<n>`，`AT+CIPQSEND?` | `+CIPQSEND: <n>` + `OK` | `0` 快发且返回 `SEND OK`；`1` 快发且返回 `DATA ACCEPT`；`2` 慢发 | 9s | Socket send 策略 | 手册推荐快发，避免慢发阻塞 AT 通道；可配合 `AT+CIPACK` 确认对端 ACK |
| 接收数据尾部 CRLF | `AT+CIPRXF=<n>`，`AT+CIPRXF?` | `+CIPRXF:<n>` + `OK` | `0` 接收数据末尾自动加 `\r\n`；`1` 不添加 | 9s | Socket 接收格式配置 | 二进制数据建议设置为 `1`，避免数据被额外 CRLF 污染 |
| 透明传输参数 | `AT+CIPCCFG=<NmRetry>,<WaitTm>,<SendSz>,<esc>[,<Rxmode>,<RxSize>,<Rxtimer>,<BufClean>]`，`AT+CIPCCFG?` | `+CIPCCFG: ...` + `OK` | `NmRetry=3..8`；`WaitTm=2..10`；`SendSz=1..1460`；`RxSize=50..1460`；`Rxtimer=20..1000ms` | 9s | 透传模式高级配置 | 仅单连接且 `AT+CIPMODE=1` 时可设置；第一版可不开放 |
| 发送数据 | 单连接：`AT+CIPSEND[=<length>]`；多连接：`AT+CIPSEND=<n>[,<length>]` | `>` 后输入数据；成功 `SEND OK`、`DATA ACCEPT:<length>` 或 `DATA ACCEPT:<n>,<length>`；失败 `SEND FAIL`、`<n>,SEND FAIL` | `length` 必须小于 `size`；单次最大值当前 1460 字节；变长发送用 `Ctrl+Z` 结束 | 9s 进入 prompt；发送完成按业务预算等待 | `modem_socket_send()` | 命令行建议以 `\r\n` 结尾，避免待发送数据首字节 `\n` 被吞；仅连接建立后可发送 |
| 自动发送定时 | `AT+CIPATS=<mode>[,<time>]`，`AT+CIPATS?` | `+CIPATS: <mode>,<time>` + `OK` | `mode=0/1`；`time=1..100s` | 9s | 变长发送辅助 | 可使 `CIPSEND` 在定时器到期后自动发送；第一版可不使用 |
| 发送提示开关 | `AT+CIPSPRT=<send_prompt>`，`AT+CIPSPRT?` | `+CIPSPRT: <send_prompt>` + `OK` | `0` 不显示 `>` 但返回发送结果；`1` 显示 `>` 并返回结果；`2` 都不显示 | 9s | AT 解析策略配置 | 默认 `1` 最适合明确等待 prompt；不要关闭发送结果，避免丢失 send 完成判断 |
| 查询连接状态 | `AT+CIPSTATUS`；多连接也支持 `AT+CIPSTATUS=<n>` | 单连接：`OK` + `STATE: <sl_state>`；多连接：`OK` + `STATE:<ml_state>` + `C:<n>,...` | 单连接状态包括 `IP INITIAL`、`IP STATUS`、`CONNECT OK`、`PDP DEACT`；多连接状态包括 `IP PROCESSING` | 9s | Socket/PDP 状态机诊断与恢复 | `OK` 可能早于 `STATE`，解析实现需避免提前结束响应 |
| 查询发送 ACK | 单连接：`AT+CIPACK`；多连接：`AT+CIPACK=<n>` | `+CIPACK: <txlen>,<acklen>,<nacklen>` + `OK` | 累计发送、已 ACK、未 ACK 字节数 | 9s | 可选发送可靠性诊断 | `CIPSHUT` 或断链重连后计数清零 |
| GPRS/CSD 连接模式 | `AT+CIPCSGP=<mode>[,<apn>,<user>,<pwd>]`，`AT+CIPCSGP?` | `+CIPCSGP: <mode>,<apn>,<user>,<pwd>` + `OK` | `mode=1` GPRS；`2` CSD | 9s | 旧 TCPIP APN 配置参考 | 当前 Air780EP 数据业务建议使用 `CSTT` 或标准 PDP 指令，不作为主路径 |
| DNS 配置 | `AT+CDNSCFG=<pri_dns>[,<sec_dns>[,<cid>]]`，`AT+CDNSCFG?` | `PrimaryDns: <pri_dns>` + `SecondaryDns: <sec_dns>` + `OK` | 主/备 DNS IP；`cid=1..3` 用于 SAPBR 场景 | 9s | DNS 配置 API 或诊断 | TCPIP/MQTT 使用 `CSTT/CIICR/CIFSR` 后可查询默认 DNS；HTTP/FTP 的 SAPBR 场景需带 `cid` 设置 |
| 域名解析 | `AT+CDNSGIP=<domain>` | `OK` + `+CDNSGIP: 1,<domain>,<IPaddress>`；失败 `OK` + `+CDNSGIP:0,<dns error code>` | `dns error code=10..15` 常见 DNS 错误 | 9s | `modem_dns_lookup()` 或连接前诊断 | 需先完成 `CSTT/CIICR/CIFSR`；结果是异步行，不能只等首个 `OK` |
| 单连接来源地址显示 | `AT+CIPSRIP=<mode>`，`AT+CIPSRIP?` | `+CIPSRIP: <mode>` + `OK` | `0` 不显示；`1` 接收时上报 `RECV FROM:<IP>:<PORT>` | 9s | UDP 或调试接收来源 | 仅单连接有效 |
| 单连接 IP 头显示 | `AT+CIPHEAD=<mode>`，`AT+CIPHEAD?` | `+CIPHEAD: <mode>` + `OK` | `0` 无 IP 头；`1` 接收时显示 `+IPD,<len>:` | 9s | Socket 接收解析模式 | 单连接建议开启，以便解析数据长度；多连接固定用 `+RECEIVE` |
| IP 头协议显示 | `AT+CIPSHOWTP=<mode>`，`AT+CIPSHOWTP?` | `+CIPSHOWTP: <mode>` + `OK` | `0` 不显示协议；`1` 显示 `+IPD,<len>,<TCP/UDP>:` | 9s | 单连接接收诊断 | 仅单连接且 `AT+CIPHEAD=1` 时有效 |
| 多连接接收数据 URC | `+RECEIVE,<n>,<length>:` | 下一行 `Received data` | `n=0..5`；`length` 为接收字节数 | URC | Socket RX handler | 多连接模式的数据路径；正文和头分行上报 |
| 保存 TCPIP 上下文 | `AT+CIPSCONT`，`AT+CIPSCONT?` | 多行 TCPIP 参数 + `OK` | 保存 `CIPMUX`、`CIPQSEND`、`CIPMODE` 等上下文 | 9s | 配置持久化参考 | 不要在每次启动频繁写 NV；第一版不建议依赖 |
| 手动取数 | `AT+CIPRXGET=<mode>[,<len>]` 或 `AT+CIPRXGET=<mode>,<n>[,<len>]`，`AT+CIPRXGET?` | `+CIPRXGET: 1[,<n>]` URC；读取返回 `+CIPRXGET:<mode>,...` + 数据 + `OK` | `mode=0` 关闭；`1` 首次有数据上报；`5` 每次有数据上报；`2` 普通读取；`3` HEX 读取；`4` 查询未读长度 | 9s | Socket 接收缓冲模式 | 可避免数据直接穿插进 AT 流；普通读取 `len=1..1460`，HEX 读取 `len=1..730` |
| 关闭 TCP/UDP 连接 | 单连接：`AT+CIPCLOSE[=<id>]`；多连接：`AT+CIPCLOSE=<n>[,<id>]` | `CLOSE OK` 或 `<n>,CLOSE OK`；失败 `ERROR` | `id=0` 慢关；`1` 快关 | 9s | `modem_socket_close()` | 单连接执行命令只在 `TCP/UDP CONNECTING` 或 `CONNECT OK` 状态有效；多连接必须带连接号 |
| 关闭移动场景 | `AT+CIPSHUT` | `SHUT OK` 或 `ERROR` | 无 | 90s | PDP/TCPIP 全局清理 | 多连接时会关闭所有 IP 连接；收到 PDP 去激活后仍需执行该命令回到 `IP INITIAL` |
| RNDIS 网关 IP | `AT+ROUTEIP=<ip>`，`AT+ROUTEIP?` | `<ip>` + `OK` | 仅支持 `192.168.X.2` | 9s | RNDIS 诊断/配置 | 与 AT Socket 路径无直接关系，低优先级 |
| PING 回声请求 | `AT+CIPPING=<IPaddr>[,<retryNum>[,<dataLen>[,<timeout>[,<ttl>]]]]`，`AT+CIPPING?` | 多行 `+CIPPING: <replyId>,<IpAddress>,<replyTime>,<ttl>` + `OK` | `retryNum=1..100`，`0` 连续 ping；`dataLen=0..1024`；`timeout=1..600` 单位 100ms；`ttl=1..255` | 9s | `modem_ping()` 或联网自检 | 执行前需激活 GPRS PDP 上下文；无回应时 `replyTime=600` 且 `ttl=255`；PDP 去激活会终止命令 |
| TCP Keep-Alive | `AT+CIPTKA=<mode>[,<keepIdle>[,<keepInterval>[,<keepCount>]]]`，`AT+CIPTKA?` | `+CIPTKA:<mode>,<keepIdle>,<keepInterval>,<keepCount>` + `OK` | `mode=0/1`；`keepIdle=30..7200s`；`keepInterval=30..600s`；`keepCount=1..9` | 9s | 长连接保活配置 | 属 TCP 协议栈 keep-alive；不同于应用层心跳 |
| 心跳参数 | `AT^HEARTCONFIG=<option>,<socket_id>,<heartbeat_time>`，`AT^HEARTCONFIG?` | `^HEARTCONFIG:<enable>,<socket_id>,<heartbeat_time>` + `OK` | `option=0/1`；`socket_id=0..5`；`heartbeat_time=5..600s` | 9s | 应用层心跳配置 | 当前仅支持一路连接；单连接固定 `socket_id=0`；默认心跳内容为 IMEI |
| 心跳内容 | `AT^HEARTBEAT=<socket_id>,<data>`，`AT^HEARTBEAT?` | `^HEARTBEAT: <socket_id>,<data>` + `OK` | `data` 最长 256 字节 | 9s | 应用层心跳内容配置 | 字符串内容；二进制内容用 HEX 指令 |
| HEX 心跳内容 | 单连接：`AT^HEARTBEATHEX=<len>,<data>`；多连接：`AT^HEARTBEATHEX=<socket_id>,<len>,<data>` | `OK`；设置后心跳内容会自动发送 | `data` 为 HEX 可见字符串，最长 256 字节 | 9s | 二进制心跳内容配置 | 多连接时需指定 socket id |
| 心跳发送情况 | `AT^HEARTINQUIRE?` | `^HEARTINQUIRE:<suctime>,<nextime>,<heartbeat_time>` + `OK` | 上次成功距今秒数、下次发送剩余秒数、累计发送条数 | 9s | 长连接诊断 | 关闭心跳后统计清零 |
| 数据模式切命令模式 | `+++` | `OK` | 输入前 1s 无字符；0.5s 内连续三个 `+`；输入后 0.5s 无字符 | 9s | 透传模式逃逸 | EC716S 系列不支持；仅透传/PPP 在线模式使用 |
| 命令模式切数据模式 | `ATO` | `CONNECT` 或 `NO CARRIER` | 无 | 9s | 透传模式恢复 | EC716S 系列不支持；仅已保持数据连接时使用 |

### TCP/UDP 错误码

TCP 应用错误会以 `TCP ERROR:<err code>` 上报，UDP 应用错误会以 `UDP ERROR:<err code>` 上报。连接层实现应至少把以下错误归类为连接失败、连接断开、网络错误或参数错误。

| 错误范围 | 来源 | 含义 | 映射建议 |
|----------|------|------|----------|
| `0` | TCP/UDP | 成功 | 不作为错误处理 |
| `1..4` | TCP/UDP | TCPIP 线程空闲、无可用 tsapi、无效 tsapi、无缓冲 | `ESP_ERR_INVALID_STATE` 或 `ESP_ERR_NO_MEM` |
| `5..8` | TCP | 网络错误、远端不可达、地址占用、地址无效 | 连接失败或 DNS/网络错误 |
| `9..12` | TCP | 数据大小异常、参数无效、远端拒绝、超时 | `ESP_ERR_INVALID_ARG`、`ESP_ERR_TIMEOUT` 或连接失败 |
| `13..17` | TCP | 连接终止、连接重置、已连接、未连接、已 shutdown | socket closed/connection reset |
| `6..13` | UDP | 网络错误、远端拒绝/不可达、地址错误、数据大小异常、参数无效、TCPIP busy | UDP send/connect 失败 |
| `18` TCP 或 `14` UDP | TCP/UDP | 未知错误 | `ESP_FAIL` 并记录原始错误码 |

## HTTP 相关指令

HTTP 指令来自手册第 14 章。HTTP 功能通常使用 `AT+SAPBR` 管理承载，HTTPS 证书参数使用 `AT+SSLCFG` 且 SSL 上下文 ID 固定为 `153`。

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| HTTP 承载管理 | `AT+SAPBR=<cmd_type>,<cid>[,<tag>,<value>]` | 查询 `+SAPBR: <cid>,<Status>,<IP_Addr>` + `OK`；URC `+SAPBR <cid>: DEACT` | `cmd_type=0` 关闭；`1` 打开；`2` 查询；`3` 设置；`4` 获取；`cid=1..3`；`tag="CONTYPE"/"APN"/"USER"/"PWD"` | 9s | HTTP/FTP 类应用承载辅助 | 设置 APN 可用 `AT+SAPBR=3,<cid>,"APN",""` 让模块使用自动获取 APN；与 `CSTT/CIICR` 是不同应用承载路径 |
| 初始化 HTTP | `AT+HTTPINIT` | `OK` | 无 | 9s | `http_client_init()` | 使用 HTTP 服务前调用；HTTPS 还需配置 `HTTPSSL` 与 `SSLCFG` |
| HTTP SSL 开关 | `AT+HTTPSSL=<n>`，`AT+HTTPSSL?` | `+HTTPSSL: <n>` + `OK` | `0` 关闭；`1` 开启 | 9s | HTTPS 开关 | 需要证书/加密套件/安全等级时使用 `AT+SSLCFG`，HTTP SSL context 为 `153` |
| 设置 HTTP 参数 | `AT+HTTPPARA=<HTTPParamTag>,<HTTPParamValue>`，`AT+HTTPPARA?` | 多行 `+HTTPPARA:` 参数 + `OK` | 必选 `"CID"`、`"URL"`；可选 `"UA"`、`"PROIP"`、`"PROPORT"`、`"REDIR"`、`"BREAK"`、`"BREAKEND"`、`"TIMEOUT"`、`"CONTENT"`、`"USER_DEFINED"`、`"USERDATA"` | 9s | HTTP 请求参数构造 | `URL` 最长 500 字节；`TIMEOUT` 默认 120s；多条 `USER_DEFINED` 可逐条设置；`USERDATA` 可用 `\r\n` 拼接多行头 |
| 写 POST 数据 | `AT+HTTPDATA=<size>,<time>` | `DOWNLOAD`，输入数据后 `OK` | AT 固件 `size=0..3356`；LSAT 固件 `0..130048`；`time=1000..120000ms` | 按 `<time>` 加输入预算 | HTTP POST body 写入 | 实际输入数据不能大于 `size`；`size=0` 表示清除内容 |
| HTTP 动作 | `AT+HTTPACTION=<method>` | 立即 `OK`，随后 `+HTTPACTION: <Method>,<StatusCode>,<DataLen>`；错误可返回 `+CME ERROR:<err>` 后仍跟 URC | `method=0` GET；`1` POST；`2` HEAD；`StatusCode=100..606` | 命令 9s；请求完成按 `TIMEOUT` 预算 | `http_client_perform()` | 结果由 URC 返回；`StatusCode=600..606` 为模块侧 HTTP/网络/SSL 错误 |
| HTTP 动作扩展 | `AT+HTTPEXACTION=<method>[,<len>]` | `OK` 或 `+CME ERROR:<err>` | `method=0` GET；`1` POST；`len` 为 POST 长度 | 9s | 大数据/扩展 HTTP 流程 | EC618 >= V1106 支持；后续配合 `HTTPEXPOST/HTTPEXGET` |
| 读取 HTTP 响应 | `AT+HTTPREAD` 或 `AT+HTTPREAD=<start_address>,<byte_size>` | `+HTTPREAD:<date_len>` + `<data>` + `OK` | `start_address=0..3356`；`byte_size=1..3356` | 9s 或按响应长度预算 | 读取 `HTTPACTION` 响应体 | 数据可能包含任意文本/二进制，解析应按长度处理，不应只按行处理 |
| 获取 HTTP 响应到路径 | `AT+HTTPGET=<path>` | `+HTTPGET:<date_len>` + `OK` | `path` 最长 255 ASCII 字符 | 9s | 旧式 HTTP GET 读取参考 | 与 `HTTPACTION/HTTPREAD` 功能重叠，低优先级 |
| 扩展 GET 读取 | `AT+HTTPEXGET[=<len>]` | `+HTTPGET:<date_len>` + `data` + `OK` | `len` 为读取长度 | 9s 或按长度预算 | 扩展 HTTP 响应读取 | EC618 >= V1106 支持；手册返回前缀写为 `+HTTPGET` |
| 扩展 POST 写入 | `AT+HTTPEXPOST=<len>[,<timeout>]` | `>` 后输入数据，随后 `OK` + `+HTTPEXPOST: <len>` | `len` 不超过 `HTTPEXACTION` 设置长度；`timeout=1..120000ms` | 按 `<timeout>` 加输入预算 | 扩展 POST body 写入 | EC618 >= V1106 支持；与 `HTTPEXACTION=1,<len>` 配套 |
| 下载到文件系统 | `AT+HTTPGETTOFS=<loc>,<filename>`，`AT+HTTPGETTOFS?` | `OK`/`ERROR`；URC `+HTTPGETTOFS:<HTTP响应码>,<writelen>`；查询 `+HTTPGETTOFS:<status>,<writelen>,<totalLength>` | `loc=0` 保存到 ROM，目录固定 `/USER/HTTP`；`filename` 最长 64 字符 | 9s 启动；下载按业务预算 | OTA/文件下载辅助 | EC618 >= V1148 支持；同名文件会覆盖 |
| 查询 HTTP 头 | `AT+HTTPHEAD` | `+HTTPREAD:<date_len>` + `<data>` + `OK` | 头信息长度和内容 | 9s | HEAD 或响应头读取 | 手册响应前缀写为 `+HTTPREAD`；需按长度读取 |
| 保存 HTTP 上下文 | `AT+HTTPSCONT`，`AT+HTTPSCONT?` | `+HTTPSCONT:<mode>` + 多行 HTTP 参数 + `OK` | `mode=0` 保存到 NVRAM；`1` 未保存，取自 RAM | 9s | HTTP 参数持久化参考 | 避免启动路径频繁写 NV |
| 终止 HTTP | `AT+HTTPTERM` | `OK` | 无 | 9s | `http_client_cleanup()` | 每次 HTTP 会话结束后调用，释放 HTTP 服务状态 |

### HTTP 错误码

HTTP 模块错误会以 `ERROR:<err code>` 或 `+HTTPACTION` 的 `StatusCode=600..606` 体现。

| 错误码 | 英文说明 | 中文说明 | 映射建议 |
|--------|----------|----------|----------|
| `0` | Unknown session id | 未知会话 ID | 内部状态错误 |
| `1` | File is too short | 文件内容太短 | 文件/数据错误 |
| `2` | DNS is fail | 域名解析失败 | DNS 错误 |
| `3` | HTTP is busy | HTTP 任务正忙 | `ESP_ERR_INVALID_STATE`，稍后重试 |
| `4` | Socket is wrong | 套接字失败 | 网络连接错误 |
| `5` | Connect fail | 连接失败 | 连接失败 |
| `6` | File is error | 文件错误 | 文件系统错误 |
| `7` | Connection is closed | 连接已关闭 | 连接关闭 |
| `8` | Connection is destroyed | 连接已销毁 | 连接关闭/状态错误 |
| `9` | HTTP header is not found | HTTP 头不存在 | 响应格式错误 |
| `10` | HTTP authentication scheme is not supported | HTTP 认证机制不支持 | 不支持 |
| `11` | PDP active is wrong | PDP 激活失败 | PDP/网络错误 |
| `12` | Param is wrong | 参数有误 | `ESP_ERR_INVALID_ARG` |
| `13` | No buffer | 缓冲区不足 | `ESP_ERR_NO_MEM` |
| `14` | PDP deactive is wrong | PDP 去激活失败 | PDP 清理失败 |

## MQTT 相关指令

MQTT 指令来自手册第 16 章。手册说明 EC716S 系列需 `_MU`、`_MS`、`_MULP` 固件支持；Air780EP 是否支持需以实际 AT 固件能力为准。MQTT over SSL 使用 `AT+SSLCFG`，SSL 上下文 ID 固定为 `88`。

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| MQTT 参数配置 | `AT+MCONFIG=<clientid>[,<username>,<password>[,<will_qos>,<will_retain>,<will_topic>,<will_message>]]` | `OK` 或 `ERROR` | `clientid`、`username`、`password` 最长 256 字节；`will_qos=0..2`；`will_retain=0/1`；`will_message` 最长 1360 字节 | 9s | `modem_mqtt_configure()` | 客户端 ID 不能与服务器上其他连接重复；遗嘱主题和消息需要加引号 |
| 建立 MQTT TCP 连接 | 普通：`AT+MIPSTART=<svraddr>,<port>`；SSL：`AT+SSLMIPSTART=<svraddr>,<port>` | 立即 `OK`，后续 `CONNECT OK`、`ALREADY CONNECT`、`CONNECT FAIL`；多连接可能上报 `7,CONNECT OK` | `svraddr` 为 IP 或域名；`port=1..65535` | 命令 9s；连接结果按业务预算等待 | `modem_mqtt_tcp_connect()` | 使用 SSL 时先配置 `SSLCFG` context `88`；等待 `CONNECT OK` 后立即发送 `MCONNECT`，否则可能被服务器踢掉 |
| MQTT 协议连接 | `AT+MCONNECT=<clean_session>,<keepalive>[,<mode>]` | 立即 `OK`；成功 URC `CONNACK OK`；失败 `ERROR` | `clean_session=0/1`；`keepalive=1..65535s`；`mode=1` 启用大于 300s 长心跳支持 | 命令 9s；CONNACK 按业务预算等待 | `modem_mqtt_connect()` | 收到 `CONNACK OK` 后才能 publish/subscribe；建议 keepalive 取 300s 以上 |
| 发布消息 | `AT+MPUB=<topic>,<qos>,<retain>,<message>` | `qos=0`：`OK`；`qos=1`：`OK` + `PUBACK`；`qos=2`：`OK` + `PUBREC` + `PUBCOMP`；失败 `ERROR` | `topic` 最长 256 字节；`message` 最长 4100 字节；`qos=0..2`；`retain=0/1` | 9s 或按 QoS ACK 预算 | `mqtt_client_publish()` | 消息内双引号用 `\22`，回车 `\0D`，换行 `\0A`，反斜杠 `\5C`；MCU 字符串中可能需再次转义 |
| 定长发布 | `AT+MPUBEX=<topic>,<qos>,<retain>[,<len>]` | 先返回 `>`；输入指定长度或 `Ctrl+Z`/5s 超时后发送；随后按 QoS 返回 `OK`、`PUBACK`、`PUBREC`、`PUBCOMP` | `len=1..4100` | prompt 9s；发送按 QoS 预算 | 二进制或大 payload 发布 | 适合包含特殊字符的数据；最大 4100 字节 |
| 订阅主题 | `AT+MSUB=<topic>,<qos>` | `OK` + `SUBACK`；失败 `ERROR` | `topic` 最长 256 字节；`qos=0..2` | 9s 或按 SUBACK 预算 | `mqtt_client_subscribe()` | 收到订阅消息后的 URC 由 `MQTTMSGSET` 决定 |
| 取消订阅 | `AT+MUNSUB=<topic>` | `OK` + `UNSUBACK`；失败 `ERROR` | `topic` 最长 256 字节 | 9s 或按 UNSUBACK 预算 | `mqtt_client_unsubscribe()` | 主题最长 256 字节 |
| 读取缓存消息 | `AT+MQTTMSGGET`，`AT+MQTTMSGGET?` | 执行命令输出最多 4 条 `+MSUB: <topic>,<len>,<message>` + `OK`；查询 `+MQTTMSGGET:<slot>,<status>` | `slot=0..3`；`status=VALID/INVALID`；`message` 最大 4100 字节 | 9s | MQTT RX 缓存读取 | 仅 `AT+MQTTMSGSET=1` 缓存模式下使用；超过 4 条时最新覆盖最旧 |
| 设置订阅消息打印模式 | `AT+MQTTMSGSET=<mode>`，`AT+MQTTMSGSET?` | `+MQTTMSGSET:<mode>` + `OK` | `0` 新消息直接 URC `+MSUB:<topic>,<len>,<message>`；`1` 缓存模式，URC `+MSUB:<store_addr>` | 9s | MQTT RX 模式配置 | 直接模式解析简单但会把 payload 混入 AT 流；二进制/长消息建议缓存模式 |
| MQTT 发布编码 | `AT+MQTTMODE=<mode>`，`AT+MQTTMODE?` | `+MQTTMODE:<mode>` + `OK` | `0` ASCII；`1` HEX | 9s | MQTT payload 编码配置 | HEX 模式下 `AT+MPUB="test",0,0,"313233"` 实际发布 `123` |
| 关闭 MQTT 会话 | `AT+MDISCONNECT` | `OK` 或 `ERROR` | 无 | 9s | `modem_mqtt_disconnect()` | 关闭 MQTT broker 会话，不关闭底层 TCP 通道 |
| 关闭 MQTT TCP 连接 | `AT+MIPCLOSE` | `OK` 或 `ERROR` | 无 | 9s | `modem_mqtt_tcp_disconnect()` | 只关闭 TCP 连接；通常先 `MDISCONNECT` 再 `MIPCLOSE` |
| 查询 MQTT 状态 | `AT+MQTTSTATU` | `+MQTTSTATU :<state>` + `OK` 或 `ERROR` | `0` 离线；`1` 已认证可发布；`2` TCP 已连但未认证，需 `MCONNECT` | 9s | `mqtt_client_get_state()` | 注意手册命令名为 `MQTTSTATU`，缺少末尾 `S` |

### MQTT 常见 URC 与 ACK

| URC/ACK | 含义 | 映射建议 | 注意事项 |
|---------|------|----------|----------|
| `CONNECT OK` | MQTT TCP 连接建立 | 进入 MQTT transport connected | `MIPSTART/SSLMIPSTART` 后上报 |
| `CONNACK OK` | MQTT 会话认证成功 | `MQTT_EVENT_CONNECTED` | `MCONNECT` 后上报 |
| `PUBACK` | QoS1 发布确认 | publish 完成 | `MPUB/MPUBEX` QoS1 使用 |
| `PUBREC` + `PUBCOMP` | QoS2 发布流程确认 | publish 完成 | QoS2 需等待两步确认 |
| `SUBACK` | 订阅成功 | subscribe 完成 | `MSUB` 后上报 |
| `UNSUBACK` | 取消订阅成功 | unsubscribe 完成 | `MUNSUB` 后上报 |
| `+MSUB:<topic>,<len>,<message>` | 直接模式收到订阅消息 | MQTT RX event | `MQTTMSGSET=0` |
| `+MSUB:<store_addr>` | 缓存模式收到订阅消息 | 触发 `MQTTMSGGET` | `MQTTMSGSET=1`，`store_addr=0..3` |
| `CLOSED` | TCP 断链 | MQTT disconnected | 手册建议 `MQTTSTATU` 查询后从 `MIPSTART` 重新连接 |

## 休眠与低功耗

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| UART 睡眠唤醒 | `AT+CSCLK=<n>`，`AT+CSCLK?` | `+CSCLK: <n>` + `OK` | `0` 关闭睡眠；`1` DTR 控制睡眠；`2` 串口无数据自动睡眠；`3` 超低功耗睡眠 | 9s | Air780EP 低功耗内部配置；需要 Core 控制时再扩展公开 API | 睡眠前提是不接 USB；URC、来电、短信、串口输入可唤醒 |
| IDLE 睡眠等待 | `AT+WAKETIM=<wait_time>`，`AT+WAKETIM?` | `+WAKETIM: <wait_time>` + `OK` | `0..65` 秒；默认 `1` | 9s | 睡眠策略内部配置 | 与 `AT+CSCLK` 配合使用 |
| 数传睡眠等待 | `AT*RTIME=<wait_time>`，`AT*RTIME?` | `*RTIME: <wait_time>` + `OK` | `0..20` 秒；`0` 关闭 | 9s | 透传/数据模式低功耗内部配置 | 信号差或重传多时应增大等待时间 |
| 超低功耗模式 | `AT+POWERMODE=<mode>[,<para>][,<para2>]`，`AT+POWERMODE?` | `+POWERMODE: <mode>[,<para>]` + `OK` | `"PRO"` 响应优先；`"STD"` 平衡；`"PSM+"` 超低功耗；`"CLOSE"` 退出 | 9s | Air780EP 低功耗内部配置；需要 Core 控制时再扩展公开 API | 手册建议低功耗场景使用 9600 波特率保证唤醒可靠 |
| RI 指示 | `AT+CFGRI=<status>[,<h_time>,<l_time>,<count>]`，`AT+CFGRI?` | `+CFGRI: <status>` + `OK` | `status=0` 关闭；`1` 打开；默认低脉冲 120ms | 9s | RI 唤醒内部配置；可由 `modem_air780ep_config_t` 驱动 | 数据业务 URC 触发 RI 低脉冲需 `AT+CFGRI=1` |
| 保存 RI 设置 | `AT+CFGRISAVE=<save>`，`AT+CFGRISAVE?` | `+CFGRISAVE: <save>` + `OK` | `0` 不保存；`1` 保存 | 9s | 仅持久化 RI 设置时调用 | 先设置 `AT+CFGRI`，再保存 |
| URC 唤醒过滤 | `AT^WAKEUPHEX=<str>`，`AT^WAKEUPHEX?` | `^WAKEUPHEX: <str>` + `OK` | ASCII 字符串的十六进制表示 | 9s | 低功耗场景按业务 URC 过滤 RI 唤醒 | 字符串必须为 ASCII hex；空字符串用于禁用 |

## 系统 URC 注册清单

以下前缀是基础系统层建议优先注册并翻译为 `modem_event_t` 的 URC。TCP、HTTP、MQTT 等连接层 URC 在下一小节列出，建议由对应连接层对象处理，避免系统层和业务层重复消费同一行。

| 前缀/完整行 | 来源 | 触发条件 | 映射建议 | 注意事项 |
|-------------|------|----------|----------|----------|
| `RDY` | 模块启动 | 模块重启完成 | 释放初始化 RDY 等待；AT 初始化完成后再投递 `MODEM_EVENT_READY` | PDF 片段未系统列出，但旧实现已注册，实机常见 |
| `+CPIN:` | SIM | 重启后 PIN 状态自动上报、`AT+CSDT` SIM 在位检测触发，或其他 SIM 状态变化 | 更新 SIM 状态，必要时触发重新注册流程 | 同时也是 `AT+CPIN?` 查询响应前缀；SIM busy 恢复为 READY 不保证主动上报，不能替代 `AT+CPIN?` 轮询兜底 |
| `+CREG:` | 网络注册 | CREG URC 开启后注册状态变化 | 更新通用注册状态 | 同时也是 `AT+CREG?` 查询响应前缀 |
| `+CEREG:` | EPS 注册 | CEREG URC 开启后 LTE 注册状态变化 | 优先用于 LTE 注册状态 | 同时也是 `AT+CEREG?` 查询响应前缀 |
| `+CGREG:` | GPRS 注册 | CGREG URC 开启后分组域注册状态变化 | 更新分组域注册状态 | 同时也是 `AT+CGREG?` 查询响应前缀 |
| `+CGEV:` | 分组域事件 | PDP/PDN 激活、去激活或修改 | 触发 `MODEM_EVENT_PDP_ACTIVATED` / `MODEM_EVENT_PDP_DEACTIVATED` | 当前实现不发送 `AT+CGEREP`；若模块默认上报则处理 |
| `+PDP DEACT` / `+PDP:DEACT` | TCPIP 示例 | PDP 上下文被网络释放 | 网络离线，执行恢复流程 | 手册 TCPIP 章节写法不完全一致，实现时可兼容两个文本 |

### 后续连接层 URC

以下 URC/ACK 属于 TCP、HTTP、MQTT 连接层或数据路径。系统层不应直接翻译为通用网络事件，除非它们表示全局 PDP 去激活或 TCP 断链恢复入口。

| 前缀/完整行 | 来源 | 触发条件 | 后续映射建议 | 注意事项 |
|-------------|------|----------|--------------|----------|
| `CONNECT OK` / `<n>,CONNECT OK` / `CONNECT` | TCPIP / MQTT TCP | `CIPSTART`、`MIPSTART`、`SSLMIPSTART` 连接成功 | socket connected / MQTT transport connected | `CONNECT` 也可能表示透传模式连接成功 |
| `ALREADY CONNECT` / `<n>,ALREADY CONNECT` | TCPIP / MQTT TCP | 连接已存在 | 按已连接处理或返回 busy | 多连接时可能带连接号 |
| `CONNECT FAIL` / `<n>,CONNECT FAIL` | TCPIP / MQTT TCP | 建链失败 | connect failed | 可能伴随 `STATE:<state>` |
| `CLOSED` | TCPIP / MQTT 示例 | TCP 断链 | socket closed 或 MQTT disconnected | MQTT 章节建议收到后查询 `AT+MQTTSTATU` 并从 `MIPSTART` 重连 |
| `TCP ERROR:<err code>` / `UDP ERROR:<err code>` | TCPIP | TCP/UDP 协议栈错误 | socket error | 保留原始错误码，按 TCP/UDP 错误码表分类 |
| `SEND OK` / `<n>,SEND OK` | TCPIP send | 慢发或快发模式 0 发送完成 | send complete | 发送模式由 `AT+CIPQSEND` 决定 |
| `DATA ACCEPT:<length>` / `DATA ACCEPT:<n>,<length>` | TCPIP send | 快发模式 1 模块接收待发送数据 | send accepted | 不代表对端已 ACK，可用 `AT+CIPACK` 查询 |
| `+IPD,` | TCPIP 单连接接收 | `AT+CIPHEAD=1` 后收到数据 | socket RX event | 需按 `+IPD,<len>[:或,<TCP/UDP>:]` 的长度字段读取数据 |
| `RECV FROM:` | TCPIP 单连接接收 | `AT+CIPSRIP=1` 后收到数据 | 记录远端地址 | 常与 `+IPD` 相邻出现 |
| `+RECEIVE,` | TCPIP 多连接接收 | 多连接收到数据 | socket RX event | 头部与数据分行，按 `<length>` 读取下一行数据 |
| `+CIPRXGET:` | TCPIP 手动取数 | 手动接收模式收到数据或读取结果 | 数据到达/读取完成 | `mode=1/5` 是到达通知，`mode=2/3/4` 是命令响应 |
| `+CDNSGIP:` | DNS | 域名解析完成 | DNS result | `AT+CDNSGIP` 先返回 `OK`，结果随后上报 |
| `+SAPBR <cid>: DEACT` | HTTP/SAPBR | SAPBR 承载去激活 | HTTP 承载失效 | 可触发 HTTP 层重建承载 |
| `+HTTPACTION:` | HTTP | GET/POST/HEAD 完成 | HTTP response ready | 格式 `+HTTPACTION:<method>,<status>,<len>`，随后用 `HTTPREAD` 读取 |
| `+HTTPEXPOST:` | HTTP 扩展 | 扩展 POST 数据写入完成 | POST body accepted | 与 `HTTPEXACTION/HTTPEXGET` 流程配套 |
| `+HTTPEXACTION:` | HTTP 扩展 | 扩展 HTTP 会话完成 | HTTP extended done | 手册示例为 `+HTTPEXACTION: <method>,<status>` |
| `+HTTPGETTOFS:` | HTTP 下载 | 下载到文件系统完成或进度 | download complete/progress | 同名文件会覆盖，状态码在第一个字段 |
| `CONNACK OK` | MQTT | MQTT CONNECT 成功 | MQTT connected | `MCONNECT` 后等待该行 |
| `PUBACK` / `PUBREC` / `PUBCOMP` | MQTT publish | QoS1/QoS2 发布确认 | publish complete | QoS2 需等 `PUBREC` 和 `PUBCOMP` |
| `SUBACK` / `UNSUBACK` | MQTT subscribe | 订阅/取消订阅成功 | subscribe complete | `MSUB/MUNSUB` 后等待 |
| `+MSUB:` | MQTT receive | 收到订阅消息或缓存位置 | MQTT RX event | 直接模式含 topic/len/message；缓存模式只有 `store_addr` |

## 推荐初始化与联网流程

第一阶段建议 `modem_air780ep_t` 使用旧实现验证过的 TCPIP 激活路径，标准 PDP 指令作为显式 APN、鉴权和诊断路径。TCP Socket 可复用该激活路径；HTTP 使用 `SAPBR` 承载；MQTT 按手册可在模块已有默认 PDP 承载后直接进入 `MIPSTART`，但实现中仍应先确认注册和附着状态。

基础 TCPIP/PDP 激活流程：

1. `ATE0`
2. `AT+CMEE=1`
3. `AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`，启用注册状态 URC
4. `AT+CPIN?`，要求 `+CPIN: READY`
5. `AT+CSQ`，解析 `+CSQ: <rssi>,<ber>` 并按 Core 阈值判断信号可用性
6. `AT+CEREG?` 或 `AT+CGREG?`，要求 `stat=1` 或 `stat=5`
7. `AT+CGATT?`，要求 `+CGATT: 1`
8. `AT+CSTT`，使用模块自动获取 APN；本实现的 TCPIP 激活路径仅支持 `cid=1`
9. `AT+CIICR`，激活移动场景
10. `AT+CIFSR`，读取本地 IP
11. 可选 `AT+CIPPING="<host>",4,32,10,64`，执行基础连通性检查

TCP 单连接非透传推荐流程：

1. 完成基础 TCPIP/PDP 激活流程，确认 `AT+CIFSR` 返回本地 IP。
2. `AT+CIPMUX=0`，使用单连接模式。
3. `AT+CIPQSEND=1`，使用快发并以 `DATA ACCEPT` 判断模块接收待发送数据。
4. 可选 `AT+CIPHEAD=1`，接收时使用 `+IPD,<len>:` 头便于按长度解析。
5. `AT+CIPSTART="TCP","<host>",<port>`，等待 `CONNECT OK`。
6. `AT+CIPSEND=<len>`，等待 `>` 后输入精确长度的数据，等待 `DATA ACCEPT:<len>`。
7. 可选 `AT+CIPACK`，确认 `<nacklen>` 是否为 0。
8. `AT+CIPCLOSE` 关闭连接，必要时 `AT+CIPSHUT` 关闭移动场景。

HTTP GET/POST 推荐流程：

1. `AT+SAPBR=3,1,"CONTYPE","GPRS"`。
2. `AT+SAPBR=3,1,"APN",""`，使用模块自动获取 APN。
3. `AT+SAPBR=1,1`，激活 HTTP 承载。
4. `AT+SAPBR=2,1`，确认返回 `+SAPBR: 1,1,<ip>`。
5. `AT+HTTPINIT`。
6. HTTPS 可选 `AT+HTTPSSL=1`，证书校验场景先配置 `AT+SSLCFG` context `153`。
7. `AT+HTTPPARA="CID",1`。
8. `AT+HTTPPARA="URL","<url>"`。
9. POST 时先 `AT+HTTPPARA="CONTENT","<content-type>"`，再 `AT+HTTPDATA=<size>,<time>` 写入 body。
10. `AT+HTTPACTION=0` 执行 GET，或 `AT+HTTPACTION=1` 执行 POST，等待 `+HTTPACTION:<method>,<status>,<len>`。
11. `AT+HTTPREAD` 按长度读取响应体。
12. `AT+HTTPTERM` 结束 HTTP 服务。

MQTT 推荐流程：

1. 完成 SIM、注册、附着检查，确认 `AT+CGATT?` 返回 `+CGATT: 1`。
2. TLS 场景先写入证书文件，并配置 `AT+SSLCFG` context `88`。
3. `modem_mqtt_configure()` 保存完整 MQTT 配置，并发送 `AT+MCONFIG=<clientid>,<username>,<password>`；用户名密码为空时使用 `"",""`。
4. `modem_mqtt_tcp_connect()` 使用已缓存的 host/port 发送普通连接 `AT+MIPSTART="<host>",<port>`；TLS 后续可映射 `AT+SSLMIPSTART="<host>",<port>`。
5. 等待 `CONNECT OK` 后立即通过 `modem_mqtt_connect()` 使用已缓存的 clean session/keepalive 发送 `AT+MCONNECT=<clean_session>,<keepalive>[,<mode>]`。
6. 等待 `CONNACK OK` 后执行 `AT+MSUB="<topic>",<qos>` 或 `AT+MPUB="<topic>",<qos>,<retain>,"<message>"`。
7. 需要缓存订阅消息时先 `AT+MQTTMSGSET=1`，收到 `+MSUB:<store_addr>` 后用 `AT+MQTTMSGGET` 读取。
8. 断开时先 `AT+MDISCONNECT`，再 `AT+MIPCLOSE`。

错误恢复建议：

- 收到 `+CGEV: NW PDN DEACT` 或 `+CGEV: ME PDN DEACT` 后，Core 应进入网络恢复流程。
- 收到 `+PDP DEACT` 或 `+PDP:DEACT` 后，先执行 `AT+CIPSHUT`，再重新走联网流程。
- `AT+CIPSTATUS` 显示 `PDP DEACT` 时，也应执行 `AT+CIPSHUT` 回到 `IP INITIAL`。
- `AT+CSTT/AT+CIICR/AT+CIFSR` 失败时，不要盲目重复同一步；先 `AT+CIPSHUT` 清理移动场景。
- TCP/MQTT 收到 `CLOSED` 后，先查询对应连接状态；MQTT 可用 `AT+MQTTSTATU` 判断是离线还是只需重新 `MCONNECT`。
- HTTP 收到 `+SAPBR <cid>: DEACT` 或 `+HTTPACTION` 状态码 `601..606` 后，应关闭 HTTP 服务并重建承载。
