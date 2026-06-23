# ML307R AT 指令摘录

本文从 `reference/中移物联ML307R/` 下的 ML307R/4G series AT、通信流程示例、TCP/IP、HTTP/HTTPS、SSL、MQTT 和扩展 AT 手册中摘录 `modem_ml307r_t` 后续实现需要优先整合的 AT 指令。

本文不是完整 AT 手册，只覆盖 Modem Adapter 层实现所需的基础能力：模块识别、AT 口初始化、SIM/信号/网络注册、PDP 与应用层拨号、TCP/UDP Socket、DNS、PING、HTTP/HTTPS、SSL、MQTT、休眠低功耗和相关 URC。

## 使用边界

本轮包含：

- 模块身份、版本、IMEI/IMSI/ICCID 和基础控制
- 串口、回显、错误结果码和配置保存
- SIM 状态、信号质量、网络注册状态和 ML307R 网络诊断
- 标准分组域、PDP 上下文、鉴权和 ML307R 应用层拨号
- TCP/UDP Socket、DNS、PING、NTP、缓存收发、透传、SSL socket 和 TCP ACK/保活
- HTTP/HTTPS 实例、请求、响应读取、文件下载和 HTTP URC
- SSL context、证书/私钥写入、证书检查和密码套件查询
- MQTT 连接参数、连接、发布、订阅、缓存读取、状态和 MQTT URC
- ML307R 休眠、低功耗和唤醒相关配置
- 系统/联网/连接层 URC 注册清单

本轮不包含：

- FTP、短信、语音、GNSS、GPIO/ADC/PWM、文件系统等未进入当前联网实现优先级的业务功能
- 固件升级、锁频、制式优先级、上位机拨号、USB 网卡等非 MCU AT Socket 主路径功能
- 旧项目或其他厂商模块的专有 TCPIP/MQTT 流程

## 实现注意事项

当前 `at_engine` 第一版在有当前命令时，会把非最终响应行优先放入当前 `at_response_t`；只有无当前命令时才按 URC 前缀分发。因此 `+CREG:`、`+CEREG:`、`+CGREG:`、`+MIPCALL:`、`+MIPOPEN:`、`+MIPCLOSE:`、`+MIPSEND:`、`+MQTTURC:` 这类既可能作为命令相关响应、又可能作为异步事件的前缀，在命令等待期间不会被可靠地区分为独立 URC。

`modem_ml307r_t` 第一版应按以下规则实现：

- 开机就绪使用 `+MATREADY`，不是其他模块的启动完成文本。
- 自适应波特率模式可能没有 `+MATREADY` 上报；串口需先发送 `AT`，收到 `OK` 后再执行后续命令。
- 每条 AT 命令执行完毕后才能发送下一条命令，不能并发发送多条 AT 命令。
- 模块开机返回 `+MATREADY` 后，至少等待 2 秒才能执行 `AT+CFUN=0` 或 `AT+CFUN=1`。
- 查询响应由对应命令方法解析；空闲期 URC 由注册前缀分发并翻译为 Modem 事件。
- 不依赖命令等待期间的自发 URC 独立分发；若后续需要命令期间关键 URC 分发，应先扩展 AT Engine 的 URC 判定策略。
- 应用层联网主路径使用 `AT+MIPCALL`；`AT+MIPCALL=0,<cid>` 只断开应用层网络连接，可能不去激活 PDP。通信流程手册建议需要去激活 PDP 时使用 `AT+CFUN=4`。
- 默认超时未单独列出时按 9s 作为单次 `at_engine_send_cmd()` 建议值；注册、附着、PDP 激活、DNS、HTTP、MQTT 等异步结果应使用上层业务预算等待对应 URC。

## 文档字段说明

| 字段 | 含义 |
|------|------|
| 能力 | 该指令服务的 modem 能力 |
| AT 指令 | 需要发送到模块的命令格式 |
| 响应格式 | 需要解析的典型响应 |
| 关键参数/数据 | 需要关注的参数和值 |
| 默认超时 | `modem_ml307r_t` 调用 `at_engine_send_cmd()` 的建议超时 |
| 映射建议 | 后续 Modem 方法、初始化步骤或 URC handler 的建议映射 |
| 注意事项 | 手册中的限制、持久化要求或实现风险 |

## 模块身份与基础控制

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| AT 连通性 | `AT` | `OK` | 无 | 9s | 自适应波特率启动探测和基础探活 | 自适应波特率模式可能没有 `+MATREADY`，需先用 `AT` 同步 |
| 开机就绪 | `+MATREADY` | URC `+MATREADY` | AT 初始化完成 | URC | 初始化 ready gate | 收到后至少等 2s 再执行 `AT+CFUN=0/1` |
| 查询产品 ID | `ATI` | `<manufacturer>` + `<model>` + `<revision>` + `OK` | 厂商、型号、版本 | 9s | `modem_info_t.manufacturer/model/fw_revision` | 扩展手册列为产品 ID 信息 |
| 查询模块型号 | `AT+CGMM` | `+CGMM: <model>` 或 `<model>` + `OK` | 型号字符串 | 9s | `modem_info_t.model` | 响应形式以基础 AT 手册实测为准 |
| 查询固件版本 | `AT+CGMR` | `<revision>` + `OK` | 软件版本字符串 | 9s | `modem_info_t.fw_revision` | 扩展手册示例为纯版本行 |
| 查询 IMEI | `AT+CGSN` | `<sn>` + `OK` | IMEI/序列号 | 9s | `modem_info_t.imei` | 纯数据行，避免依赖前缀 |
| 查询 IMSI | `AT+CIMI` | `<IMSI>` + `OK` | IMSI | 9s | `modem_info_t.imsi` | SIM 未 ready 时可能失败 |
| 软重启 | `AT+MREBOOT[=<mode>]` | `OK` + `REBOOTING` | ML307R 仅支持 `mode=0` | 9s | 诊断/恢复辅助 | 重启后重新等待 `+MATREADY` 或用 `AT` 同步 |
| 软关机 | `AT+MPOF[=<mode>]` | `OK` + `POWER OFF` | ML307R 仅支持 `mode=0` | 9s | 关机辅助 | 收到 `POWER OFF` 后再执行断电动作 |
| 功能模式 | `AT+CFUN=<fun>[,<rst>]`，`AT+CFUN?` | `+CFUN: <fun>` + `OK` | `0` 最少功能；`1` 全功能；`4` 飞行模式 | 45s | 注册恢复、PDP 去激活和飞行模式控制 | `+MATREADY` 后至少等 2s 再执行 `CFUN=0/1`；`CFUN=4` 会断开连接并去激活 PDP |

## 串口与结果码配置

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 关闭回显 | `ATE0` | `OK` | `0` 关闭；`1` 打开 | 9s | 初始化第一步 | 默认值为 0，仍建议显式关闭以稳定解析 |
| 错误结果码 | `AT+CMEE=<n>`，`AT+CMEE?` | `+CMEE: <n>` + `OK` | `0` 仅 `ERROR`；`1` 数字型；`2` 文本型 | 9s | 初始化设置 `AT+CMEE=1` | 数字型适合映射 `at_response_t.error_code` |
| 固定波特率 | `AT+IPR=<rate>`，`AT+IPR?` | `+IPR: <rate>` + `OK` | `0` 自适应；常用 `115200`、`921600` | 9s | 板级初始化或调试配置 | 自适应模式无 `+MATREADY` 时需先发送 `AT` |
| 流控 | `AT+IFC=<dce_by_dte>,<dte_by_dce>`，`AT+IFC?` | `+IFC: <dce_by_dte>,<dte_by_dce>` + `OK` | `0` 无流控；`1` 软件流控；`2` 硬件流控 | 9s | 高吞吐场景配置硬件流控 | 是否支持和持久化以实际固件为准 |
| 保存配置 | `AT&W` | `OK` | 无 | 9s | 仅配置持久化时调用 | 避免每次启动写 Flash/NV |

## SIM 与网络状态

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 查询 PIN/SIM 状态 | `AT+CPIN?` | `+CPIN: <code>` + `OK` | `READY` 可用；`SIM PIN` 等待 PIN | 9s | SIM-ready 初始化内部步骤 | 开机后先查询 SIM 初始化状态，轮询总预算由上层控制 |
| SIM 状态 URC | `+CPIN: <code>` | URC | SIM ready、PIN、移除等状态 | URC | SIM 状态事件 | 与 `AT+CPIN?` 查询响应同前缀 |
| 查询 ICCID | `AT+MCCID` | `<iccid>` + `OK` | ICCID | 9s | `modem_info_t.iccid` | 扩展 AT 手册列为 ML307R ICCID 读取命令 |
| 查询信号质量 | `AT+CSQ` | `+CSQ: <rssi>,<ber>` + `OK` | `rssi=0..31,99`；`ber=0..7,99` | 9s | `modem_get_signal()` | dBm 可按常见公式 `-113 + 2*rssi` 估算 |
| 查询扩展信号 | `AT+CESQ` | `+CESQ: <rxlev>,<rxqual>,<rscp>,<ecno>,<rsrq>,<rsrp>` + `OK` | LTE 重点解析 `rsrq`、`rsrp`；`255` 未知 | 9s | 诊断/未来扩展 | 可补充 `AT+CSQ`，第一版不必强依赖 |
| 网络注册状态 | `AT+CREG=<n>`，`AT+CREG?` | `+CREG: <n>,<stat>[,<lac>,<ci>,<act>]` + `OK` | `stat=1/5` 已注册；`2` 搜索；`3` 拒绝 | 9s | 通用注册状态和 URC | 同前缀也用于查询响应 |
| EPS 注册状态 | `AT+CEREG=<n>`，`AT+CEREG?` | `+CEREG: <n>,<stat>[,<tac>,<ci>,<act>...]` + `OK` | LTE/EPS 注册状态；`stat=1/5` 可用 | 9s | ML307R LTE 主注册状态 | 建议初始化启用 URC，再轮询查询兜底 |
| GPRS 注册状态 | `AT+CGREG=<n>`，`AT+CGREG?` | `+CGREG: <n>,<stat>[,<lac>,<ci>[,...]]` + `OK` | 分组域注册状态 | 9s | 数据域注册状态和 URC | 同前缀也用于查询响应 |
| 运营商查询 | `AT+COPS?` | `+COPS: <mode>[,<format>,<oper>,<act>]` + `OK` | `oper` 运营商；`act` 接入制式 | 300s | 诊断接口或日志字段 | 初始化主路径不应频繁调用 |
| UE 网络诊断 | `AT+MUESTATS[=<type>]` | 多类 `+MUESTATS:` 信息 + `OK` | `radio`、`cell`、`bler`、`thp`、`sband` | 9s | 诊断快照 | 扩展手册提供，第一版可仅日志化 |

## 分组域/PDP 与应用层拨号

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 查询 PS 附着 | `AT+CGATT?` | `+CGATT: <state>` + `OK` | `0` 分离；`1` 附着 | 108s | 联网初始化内部检查 | 只查询通常很快，总等待由上层轮询控制 |
| PDP 上下文定义 | `AT+CGDCONT=<cid>,"<PDP_type>","<apn>"`，`AT+CGDCONT?` | `+CGDCONT: <cid>,<PDP_type>,<APN>,<PDP_addr>,...` + `OK` | `cid=1..15`；`PDP_type` 可用 `IP`、`IPV6`、`IPV4V6` | 9s | `modem_set_apn()` | `cid=1` 是默认承载，多路拨号可配置其他 CID；`cid=8` 为 IMS 专用，不用于业务拨号 |
| PDP 鉴权 | `AT+CGAUTH=<cid>,<auth_prot>,<userid>,<password>` | `OK` | `auth_prot=0` none；`1` PAP；`2` CHAP | 9s | 专网卡 APN 鉴权内部配置 | 仅 APN 需要用户名密码时使用 |
| PDP 激活/去激活 | `AT+CGACT=<state>,<cid>`，`AT+CGACT?` | `+CGACT: <cid>,<state>` + `OK` | `state=0/1` | 108s | 标准 PDP 诊断路径 | 通信流程手册不建议在自动拨号模式下用它作为主路径 |
| 查询 PDP 地址 | `AT+CGPADDR=<cid>` | `+CGPADDR: <cid>,<PDP_addr>[,<PDP_addr2>]` + `OK` | IPv4/IPv6 地址字符串 | 9s | PDP 诊断/缓存地址 | 应用层拨号主路径优先从 `+MIPCALL` 获取地址 |
| 查询应用层拨号 | `AT+MIPCALL?` | `+MIPCALL: <cid>,<state>[,"<ipv4>"[,"<ipv6>"]]` + `OK` | `state=0/1`；IPv4/IPv6 地址 | 9s | `modem_get_pdp_context()` / 激活前检查 | 上电可能已自动拨号激活 `cid=1` |
| 建立应用层拨号 | `AT+MIPCALL=1,<cid>` | `OK`，随后 `+MIPCALL: <cid>,1,"<ip>"[...]` | `cid=1..15`，建议当前最多 5 路 | 命令 9s；结果按业务预算等待 | `modem_activate_pdp()` 主路径 | 先确保 `CGDCONT` 已配置；已激活时跳过 |
| 断开应用层拨号 | `AT+MIPCALL=0,<cid>` | `OK`，随后 `+MIPCALL:<cid>,0` 或 `+MIPCALL: <cid>,0` | `cid` | 命令 9s；结果按业务预算等待 | `modem_deactivate_pdp()` 应用层断开 | 仅断开应用层网络连接，可能不去激活 PDP；需去激活 PDP 时用 `AT+CFUN=4` |

### `+MIPCALL` 拨号事件

| URC | 含义 | 映射建议 | 注意事项 |
|-----|------|----------|----------|
| `+MIPCALL: <cid>,1,"<ipv4>"[,"<ipv6>"]` | 应用层拨号成功 | `MODEM_EVENT_PDP_ACTIVATED`，缓存 IP | 可能是命令后的异步结果，也可能来自自动拨号 |
| `+MIPCALL:<cid>,0` / `+MIPCALL: <cid>,0` | 应用层网络连接断开 | `MODEM_EVENT_PDP_DEACTIVATED` 或网络恢复入口 | 冒号后空格格式可能随手册示例/固件输出变化；不等价于 PDP 已去激活，`AT+CFUN=4` 会逐路上报断开 |

## TCP/IP 连接层、DNS、PING 与 NTP

本节按 `TCP_IP用户手册_5.1.2-R - Pictures_Version` 图片版第 3 章校对。该手册同时覆盖 TCP/UDP Socket、DNS、PING、透传退出、TCP/IP URC、NTP 和 TCP/UDP 相关错误码；ML307R 与通用 4G/Cat1 描述不一致处以 ML307R 注脚为准。

实现注记：TCP client v1 uses `AT+MIPCFG="cid"`, `AT+MIPCFG="encoding",0,0,1`, `AT+MIPOPEN`, `AT+MIPSEND`, `AT+MIPRD`, and `AT+MIPCLOSE`. RX cache notifications use `+MIPURC: "rtcp"`; disconnect notifications use `+MIPURC: "disconn"`.

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 连接配置 | `AT+MIPCFG="cid",<connect_id>,<cid>` | `OK` 或 `+MIPCFG: "cid",...` | `connect_id=0..5`；`cid` 范围同 `CGDCONT`，需已激活 | 9s | socket 创建前绑定 PDP | 不指定时使用默认 PDP；配置仅当前启动周期有效 |
| 数据编码 | `AT+MIPCFG="encoding",<connect_id>,<send_format>,<recv_format>` | `OK` 或 `+MIPCFG: "encoding",...` | `send_format=0` ASCII/raw，`1` HEX，`2` 转义字符串；`recv_format=0` raw，`1` HEX | 9s | socket send/recv 编码配置 | `send_format` 影响命令内 `<data>`；`recv_format` 影响普通模式 `MIPURC` 和缓存模式 `MIPRD` 输出 |
| 发送超时 | `AT+MIPCFG="timeout",<connect_id>,<send_timeout>` | `OK` 或 `+MIPCFG: "timeout",...` | ML307R 范围 `1..120s`；默认 10s | 9s | prompt 输入超时 | 仅 `>` 数据输入模式相关 |
| 资源释放 | `AT+MIPCFG="autofree",<connect_id>,<free_mode>` | `OK` 或 `+MIPCFG: "autofree",...` | `0` 异常断开自动释放；`1` 手动释放 | 9s | socket close 策略 | `free_mode=1` 收到断链 URC 后需执行 `MIPCLOSE` 释放资源 |
| 发送缓存 | `AT+MIPCFG="sndbuf",<connect_id>,<send_buffer>` | `OK` 或 `+MIPCFG: "sndbuf",...` | 通用范围 `1..8192`，默认 1460 | 9s | 不建议映射 | TCP/IP 手册列出该项，但 ML307R 不支持设置发送缓存 |
| 接收缓存 | `AT+MIPCFG="rcvbuf",<connect_id>,<recv_buffer>` | `OK` 或 `+MIPCFG: "rcvbuf",...` | UDP 可配 `1460..65535`；TCP 接收窗口默认 64240 | 9s | UDP 缓存调优/诊断 | ML307R TCP 接收缓存配置无效；UDP 默认 64K |
| ACK 上报 | `AT+MIPCFG="ackmode",<connect_id>,<ack_mode>` | `OK` 或 `+MIPCFG: "ackmode",...` | `0` 不上报；`1` ACK 时上报 | 9s | 可选发送确认 | TCP 有效，UDP 无效 |
| SSL socket | `AT+MIPCFG="ssl",<connect_id>,<ssl_enable>,<ssl_id>` | `OK` 或 `+MIPCFG: "ssl",...` | `ssl_enable=0/1`；`ssl_id` 参见 SSL context | 9s | TLS socket 连接前配置 | 需先用 `AT+MSSLCFG` 配置证书/认证；ML307R SSL 流缓存模式需先读完当前缓存再接收后续数据 |
| TCP Keepalive | `AT+MIPTKA=<connect_id>[,<keepalive>[,<keepidle>[,<keepinterval>[,<keepcount>]]]]` | `+MIPTKA: <connect_id>,...` + `OK` 或 `OK` | `keepalive=0/1`；`keepidle=30..7200s`，默认 90；`keepinterval=30..600s`，默认 75；`keepcount=1..9`，默认 3 | 9s | 长连接保活配置 | UDP 不可设置 |
| 建立 TCP/UDP | `AT+MIPOPEN=<connect_id>,"<proto_type>","<address>",<remote_port>[,<timeout>[,<access_mode>[,<local_port>]]]` | 立即 `OK`，随后 `+MIPOPEN: <connect_id>,<result>`；透传成功为 `CONNECT` | `proto_type="TCP"/"UDP"`；`timeout=1..180s`，默认 60；`access_mode=0` 普通，`1` 透传，`2` TCP 流缓存，`3` UDP 包缓存；`local_port=0` 自动分配 | 命令 9s；连接结果按业务预算等待 | `modem_socket_open()` | `result=0` 成功；`local_port` 建议避开协议默认端口 |
| 关闭 TCP/UDP | `AT+MIPCLOSE=<connect_id>[,<mode>]` | `OK`，随后 `+MIPCLOSE: <connect_id>[,<ret_code>]` | ML307R 不支持配置 `mode`，等待缓存发送完再关闭 | 命令 9s；关闭结果按业务预算等待 | `modem_socket_close()` | `ret_code=0` 正常，`1` 服务器无响应/超时，`2` RST/传输超时等其他原因 |
| 发送数据 | `AT+MIPSEND=<connect_id>[,<send_length>[,<data>[,<rai>[,<seq>[,<pri_flag>]]]]]` | `>` 输入后 `OK`，或 `+MIPSEND: <connect_id>,<send_length>` + `OK` | 命令内数据 `0..1460`；数据模式 `1..8192`；`CTRL+Z` 发送，`ESC` 取消 | prompt 9s；发送完成按业务预算 | `modem_socket_send()` | `+MIPSEND` 表示进入协议栈缓存，不代表对端收到；4G/Cat1 不支持 RAI/SEQ，`pri_flag` 暂不支持；ML307R `send_length=0` 不是 UDP 空包发送 |
| 读取缓存 | `AT+MIPRD=<connect_id>[,<read_len>|<pack_count>]` | `+MIPRD: <connect_id>,<unread>,<data_len>,<data>` + `OK` 或仅 `OK` | TCP `read_len=0..4096`，`0` 或超过缓存表示读全部；UDP 用包数量；ML307R UDP 缓存 256 包 | 9s 或按长度预算 | `modem_socket_recv()` | 仅缓存模式使用；无可读数据时仅返回 `OK`；按长度解析数据，避免按行截断 |
| 切换数据模式 | `AT+MIPMODE=<connect_id>[,<access_mode>[,<packet_size>,<waittm>]]` | `+MIPMODE: <connect_id>,<access_mode>` + `OK`、`OK` 或 `CONNECT` | `access_mode=0` 普通；`1` 透传；`2` TCP 流缓存；`3` UDP 包缓存 | 9s | 透传/缓存模式控制 | 仅连接建立后使用；ML307R 不支持配置 `packet_size/waittm`；缓存切普通且有未读缓存时会自动按 `MIPRD` 格式上报缓存数据 |
| 查询连接状态 | `AT+MIPSTATE=<connect_id>` 或 `AT+MIPSTATE?` | `+MIPSTATE: <connect_id>,<service_type>,<address>,<remote_port>,<state>` + `OK` | `state="INITIAL"/"CONNECTING"/"CONNECTED"/"CLOSING"/"CLOSED"` | 9s | socket 状态诊断与恢复 | `INITIAL` 条目可能存在空的 service/address/port 字段 |
| 查询发送 ACK | `AT+MIPSACK=<connect_id>` | `+MIPSACK: <sent>,<acked>,<nack>,<received>` + `OK` | 已发、对端已确认、本地未确认、本地已接收字节数 | 9s | 可选发送可靠性诊断 | 仅已建立连接有效；UDP 无对端确认，`acked=0`，已发数据计入 `nack` |
| DNS 配置 | `AT+MDNSCFG="priority"[,<priority>]`；通用手册还列 `"ip"/"ipv6"/"cached"/"timeout"` | `+MDNSCFG: "priority",<priority>` + `OK` 或 `OK` | `priority=0` IPv4 优先；`1` IPv6 优先，默认 1 | 9s | DNS 优先级配置/诊断 | ML307R 仅支持 `priority` 设置且不保存 NV；不要把通用 `ip/ipv6/cached/timeout` 当作 ML307R 可用配置 |
| 域名解析 | `AT+MDNSGIP="<domainname>"[,<cid>]` | 立即 `OK`，随后 `+MDNSGIP: "<domainname>"[,"<ip>"[, ...]]` | 域名最大 255 字节；最多返回 4 个 IP；4G 默认 `cid=1` | 命令 9s；解析结果按业务预算 | `modem_dns_lookup()` | 结果异步上报；DNS 异常时可能仅返回原域名 |
| PING | `AT+MPING="<host>"[,<timeout>[,<ping_num>[,<packet_len>[,<cid>]]]]` | `OK`，随后 `+MPING: <result>[,"<ip>",<packet_len>,<time>,<ttl>]`；多包末尾 `+MPING: "statistics",<sent>,<lost>,<rtt_min>,<rtt_max>,<rtt_avg>` | `timeout=1..60s`，默认 10s；`ping_num=1..65535`，默认 4；`packet_len=1..1400`，默认 16；4G 默认 `cid=1`；`time/rtt_*` 单位 ms | 命令 9s；结果按 `timeout * ping_num` 预算 | `modem_ping()` / 联网自检 | `result=0` 成功；`1` DNS 失败；`2` DNS 超时；`3` 响应错误；`4` 响应超时；`5` 其他错误；执行前需 `MIPCALL` 成功 |
| 退出透传 | `+++` | `OK` | 仅透传模式有效 | 按透传 guard 时间 | data mode escape | `+++` 前后需与数据发送保持时间间隔并独立输入；失败时可能作为 payload 发出 |
| 网络时间同步 | `AT+MNTP[="<server>"[,<port>,[<sync>,[<timeout>]]]]` | 立即 `OK`，随后 `+MNTP: <result>[,"<time>"]` | `server` 默认 `ntp1.aliyun.com`；`port=0..65535` 默认 123；`sync=0/1` 默认 1；`timeout=1..300s`，ML307R 默认 30s；时间格式同 `CCLK` | 命令 9s；结果按 timeout 预算 | 可选网络校时/诊断 | `result=0` 成功；`1` DNS 错误；`2` 超时；`3` 同步失败 |

### TCP/IP URC 与错误

| URC/结果 | 含义 | 映射建议 | 注意事项 |
|----------|------|----------|----------|
| `+MIPOPEN: <connect_id>,0` | TCP/UDP 非透传连接成功 | socket connected | `MIPOPEN` 的最终异步结果 |
| `+MIPOPEN: <connect_id>,<err>` | 建链失败，`<err>` 为非 0 | `ESP_ERR_TIMEOUT`、`ESP_ERR_INVALID_ARG`、`ESP_ERR_INVALID_STATE` 或 `ESP_FAIL`；socket 状态置为 failed/closed | 错误码参考 TCP/IP 手册附录；实现应保留原始 `<err>` 进入日志和调试字段 |
| `CONNECT` | 透传模式连接成功 | data mode connected | 仅透传模式使用 |
| `+MIPCLOSE: <connect_id>[,<ret_code>]` | 连接关闭完成 | socket closed；释放本地连接对象 | `ret_code=0` 正常关闭；`1` 服务器端未响应、超时关闭；`2` RST/传输超时等其他原因关闭；无 `ret_code` 时按关闭完成处理 |
| `+MIPSEND: <connect_id>,<send_length>` | 数据进入协议栈缓存 | send accepted | 不代表远端 ACK |
| `+MIPURC: "rtcp",<connect_id>,<recv_length>,<data>` | 普通模式收到 TCP 数据 | socket RX event | `<data>` 受 `recv_format` 影响；按 `<recv_length>` 读取 payload |
| `+MIPURC: "rtcp",<connect_id>,<recv_length>,<total_length>` | TCP 流缓存模式收到数据提示 | 触发 `AT+MIPRD` | `<total_length>` 为当前缓存总字节数 |
| `+MIPURC: "rudp",<connect_id>,<recv_length>,<data>` | 普通模式收到 UDP 数据 | socket RX event | `<data>` 受 `recv_format` 影响；按 `<recv_length>` 读取 payload |
| `+MIPURC: "rudp",<connect_id>,<recv_count>` | UDP 包缓存模式收到数据提示 | 触发 `AT+MIPRD` | `<recv_count>` 为当前缓存包数量；ML307R/Cat1 最大 256 包 |
| `+MIPURC: "disconn",<connect_id>,<connect_state>` | 服务器关闭、连接异常或 PDP 去激活 | socket closed/recovery | `connect_state=1` 服务器关闭；`2` 连接异常；`3` PDP 去激活；`autofree=1` 时需 `MIPCLOSE`，`0` 时自动释放 |
| `+MIPURC: "ack",<connect_id>,<length>` | TCP ACK 包上报 | send ACK event | 需 `ackmode=1`；`length=1..1460` |
| `+MIPURC: "drop",<connect_id>,<drop_length>` | 接收缓存溢出 | RX overflow/drop event | 溢出数据被丢弃；缓存包数到上限后需先读取缓存才能继续接收新包 |
| `+MIPURC: "seq",<connect_id>,<seq>,<result>` | UDP 空口回传序号提示 | 第一版不要依赖 | ML307R 不支持 UDP 空口回传；手册 UDP 示例里的 `seq` 不适用于 ML307R |
| `+MDNSGIP: "<domainname>"[,"<ip>"[, ...]]` | DNS 解析完成 | DNS result | `MDNSGIP` 命令先返回 `OK`，结果随后上报；最多 4 个 IP；异常时可能仅返回域名 |
| `+MPING: ...` / `+MPING: "statistics",...` | PING 单包结果和统计 | ping result | `result=0..5`；手册示例对 `3/4` 的超时说明存在不一致，按参数表 `4` 为响应超时 |
| `+MNTP: <result>[,"<time>"]` | NTP 校时完成 | time sync result | `result=0` 成功；`1` DNS 错误；`2` 超时；`3` 同步失败 |

### TCP/UDP `+CME ERROR` 码

| 错误码 | 含义 | 映射建议 |
|--------|------|----------|
| `550` | TCP/IP 未知错误 | `ESP_FAIL`，保留原始码 |
| `551` | TCP/IP 未被使用 | `ESP_ERR_INVALID_STATE` |
| `552` | TCP/IP 已被使用 | `ESP_ERR_INVALID_STATE` |
| `553` | TCP/IP 未连接 | `ESP_ERR_INVALID_STATE` |
| `554` | SOCKET 创建失败 | `ESP_FAIL` |
| `555` | SOCKET 绑定失败 | `ESP_FAIL` 或端口/本地地址错误 |
| `556` | SOCKET 监听失败 | `ESP_FAIL` |
| `557` | SOCKET 连接被拒绝 | 连接失败，可重试或上报对端拒绝 |
| `558` | SOCKET 连接超时 | `ESP_ERR_TIMEOUT` |
| `559` | SOCKET 连接失败，其他异常 | `ESP_FAIL` |
| `560` | SOCKET 写入异常 | send failed |
| `561` | SOCKET 读取异常 | recv failed |
| `562` | SOCKET 接受异常 | server/accept 相关，客户端主路径通常只记录 |
| `570` | PDP 未激活 | 重新检查 `MIPCALL`/PDP 状态 |
| `571` | PDP 激活失败 | PDP activate failed |
| `572` | PDP 去激活失败 | PDP deactivate failed |
| `575` | APN 未配置 | 检查 `CGDCONT` |
| `576` | 端口忙碌 | 换端口或释放连接实例 |
| `577` | 不支持的 IPv4/IPv6 | 检查 PDP type、DNS 和地址族 |
| `580` | DNS 解析失败或 IP 格式错误 | DNS/address error |
| `581` | DNS 忙碌 | DNS busy，稍后重试 |
| `582` | PING 忙碌 | ping busy，稍后重试 |

## HTTP/HTTPS 相关指令

HTTP/HTTPS 使用 ML307R `MHTTP*` 命令族。HTTPS 通过 `AT+MHTTPCFG="ssl",<httpid>,1,<ssl_id>` 关联 SSL context，证书、认证方式、TLS 版本等由 `AT+MSSLCFG` 和证书写入命令配置。

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| HTTP 参数配置 | `AT+MHTTPCFG="<cmd>"[,<httpid>[,...]]` | `+MHTTPCFG: "<cmd>",...` + `OK` 或 `OK` | `header`、`chunked`、`cached`、`timeout`、`encoding`、`ssl`、`cid`、`fragment`、`urlencode` | 9s | HTTP client 参数配置 | `httpid=0..3`，由 `MHTTPCREATE` 创建；配置本次实例有效 |
| 创建 HTTP 实例 | `AT+MHTTPCREATE="<host>"` | `+MHTTPCREATE: <httpid>` + `OK` | host 包含 scheme、域名/IP、端口 | 9s | `http_client_init()` / create | HTTP/HTTPS 生命周期起点 |
| 设置特定报头 | `AT+MHTTPHEADER=<httpid>[,<eof>,<length>,<header>]` | `+MHTTPHEADER: <httpid>,<length>,<header>` + `OK` 或 `OK` | `eof` 结束标志；header 长度 | 9s | 设置请求头 | 通用报头也可用 `MHTTPCFG="header"` |
| 设置请求内容 | `AT+MHTTPCONTENT=<httpid>[,<eof>,<length>,<data>]` | `+MHTTPCONTENT: <httpid>,<length>,<data>` + `OK` 或 `OK` | POST/PUT body；编码受 `MHTTPCFG="encoding"` 影响 | 9s 或按长度预算 | HTTP body 写入 | 普通模式先写 content 再 request；chunked 模式先 request，收到 `ind` 后写 content |
| 发送 HTTP 请求 | `AT+MHTTPREQUEST=<httpid>,<method>[,<length>[,"<path>"]]` | `OK`，随后 `+MHTTPURC: "rsp",...` 或相关 URC | method 为 GET/POST/PUT/DELETE/HEAD 等手册枚举 | 命令 9s；响应按超时配置等待 | `http_client_perform()` | 请求完成由 URC 表示 |
| 读取 HTTP 数据 | `AT+MHTTPREAD=<httpid>[,<read_len>]` | `+MHTTPREAD: <httpid>,...` + 数据 + `OK` | 响应体/缓存数据长度 | 9s 或按长度预算 | `http_client_read()` | 按长度读取，payload 可能是二进制 |
| 删除 HTTP 实例 | `AT+MHTTPDEL=<httpid>` | `OK` | httpid | 9s | `http_client_cleanup()` | 释放指定实例 |
| 终止 HTTP 传输 | `AT+MHTTPTERM=<httpid>` | `OK` | httpid | 9s | 中止当前请求/传输 | 用于错误恢复 |
| HTTP 下载文件 | `AT+MHTTPDLFILE=<httpid>,...` | `OK`，随后 `+MHTTPURC: ...` | 文件路径、下载结果 | 命令 9s；下载按业务预算 | OTA/文件下载后续扩展 | 当前 Modem Adapter 可先不开放 |
| SSL context 配置 | `AT+MSSLCFG="<cmd>"[,<ssl_id>[,...]]` | `+MSSLCFG: "<cmd>",...` + `OK` 或 `OK` | `auth`、`cert`、`psk`、`encoding`、`negotime`、`version`、`ciphersuite`、`session`、`ignorestamp`、`ignoreverify` | 9s | TLS 参数配置 | `ssl_id` 供 HTTPS、MQTT、SSL socket 引用 |
| 写入 SSL 证书 | `AT+MSSLCERTWR=<name>,<length>` | `>` 输入证书，随后 `OK` | CA/客户端证书内容 | 按长度预算 | 证书管理 | 证书名称再由 `MSSLCFG="cert"` 引用 |
| 写入 SSL 私钥 | `AT+MSSLKEYWR=<name>,<length>` | `>` 输入私钥，随后 `OK` | 客户端私钥 | 按长度预算 | 双向认证配置 | 注意输入长度和编码 |
| 读取/列出/删除证书 | `AT+MSSLCERTRD`，`AT+MSSLLIST`，`AT+MSSLRM` | 证书信息 + `OK` 或 `OK` | 证书/密钥名称 | 9s | 诊断和证书维护 | 非每次启动路径 |
| 证书检查和套件查询 | `AT+MSSLCHECK`，`AT+MSSLCIPHER` | 检查/套件结果 + `OK` | 证书合法性、cipher suite | 9s | TLS 诊断 | TLS 问题定位使用 |

### HTTP/HTTPS URC 与错误

| URC/结果 | 含义 | 映射建议 | 注意事项 |
|----------|------|----------|----------|
| `+MHTTPURC: "rsp",<httpid>,<rsp_code>,<data_len>` | HTTP 响应完成 | `rsp_code=100..399` 可按成功/重定向处理；`400..599` 返回 HTTP 服务器错误；随后按 `data_len` 决定是否 `MHTTPREAD` | 部分手册版本缓存模式使用 `recv`，非缓存流式模式使用 `header/content`；实现应保留 `rsp_code` |
| `+MHTTPURC: "header",<httpid>,<code>,<header_len>,<data>` | 响应头输出 | header event；缓存 HTTP 状态码和 header | 输出格式受 `MHTTPCFG="encoding"/"fragment"` 影响；`<code>` 是 HTTP 请求结果码 |
| `+MHTTPURC: "content",<httpid>,<content_len>,<sum_len>,<cur_len>,<data>` | 响应内容输出 | streaming data event；当普通模式 `sum_len == content_len` 或 chunked 模式 `cur_len=0` 时认为 body 完成 | 按 `cur_len` 读取 payload，不能按行解析 |
| `+MHTTPURC: "ind",<httpid>,...` | chunked/content 输入提示 | content writable | chunked 流程中收到后再发 `MHTTPCONTENT` |
| `+MHTTPURC: "recv",<httpid>,<code>,<recv_header_len>,<recv_content_len>` | 缓存模式请求完成 | response ready；按 header/content 长度分段读取 | `<code>` 为 HTTP 请求结果码，非 2xx/3xx 不等于 AT 失败 |
| `+MHTTPURC: "err",<httpid>,<error_code>` | 请求错误 | HTTP request failed；清理或重建实例 | `error_code=1` DNS 失败；`2` 连接服务器失败；`3` 连接超时；`4` SSL 握手失败；`5` 连接异常断开；`6` 响应超时；`7` 接收解析失败；其他码记录原始值 |
| `+MHTTPURC: "closed",<httpid>,...` | HTTP 连接关闭 | HTTP disconnected / recovery | 若请求未完成则按连接错误处理，清理实例或重试 |
| `+MHTTPURC: "download",<downloaded>,<content_length>[,<entity_length>]` | 文件下载进度或完成 | download event | `downloaded == content_length` 可视为下载完成；文件下载后续扩展处理 |

## MQTT 相关指令

MQTT 使用 ML307R `MQTT*` 命令族。`connect_id` 范围 `0..5`；MQTT 协议版本当前仅支持 `4`，即 MQTT v3.1.1；配置掉电不保存。MQTTS 通过 `AT+MQTTCFG="ssl",<connect_id>,1,<ssl_id>` 关联 SSL context。

`conn_state` 取值：`0` 连接成功；`1` 正在重连；`2` 客户端主动断开；`3` 服务器拒绝；`4` 服务器断开；`5` ping 包超时断开；`6` 网络异常断开；`7..254` 保留；`255` 未知错误。`modem_ml307r_t` 只有收到 `conn_state=0` 才应进入 MQTT connected；`2/3/4/5/6/255` 均应进入 disconnected/error/recovery 分支。

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| MQTT 协议版本 | `AT+MQTTCFG="version",<connect_id>[,<version>]` | `+MQTTCFG: "version",<version>` + `OK` 或 `OK` | `version=4`，默认 4 | 9s | `mqtt_client_configure()` | 仅连接未创建时可配置 |
| MQTT PDP 绑定 | `AT+MQTTCFG="cid",<connect_id>[,<cid>]` | `+MQTTCFG: "cid"[,<cid>]` + `OK` 或 `OK` | ML307R `cid=1..15`，默认 1 | 9s | MQTT transport 绑定数据面 | 需先完成 `MIPCALL`；仅连接未创建时可配置 |
| MQTT SSL 配置 | `AT+MQTTCFG="ssl",<connect_id>[,<ssl_enable>[,<ssl_id>]]` | `+MQTTCFG: "ssl",<ssl_enable>[,<ssl_id>]` + `OK` 或 `OK` | `ssl_enable=0/1`，默认 0；`ssl_id` 指 SSL context | 9s | MQTTS 配置 | 证书和认证先用 `MSSLCFG` 配置 |
| Keepalive | `AT+MQTTCFG="keepalive",<connect_id>[,<keepalive_time>]` | `+MQTTCFG: "keepalive",<keepalive_time>` + `OK` 或 `OK` | `0` 永不断开；`60..65535s`，默认 120s | 9s | MQTT keepalive | 与 TCP keepalive 不同；1.5 倍超时后服务器断开 |
| Clean session | `AT+MQTTCFG="clean",<connect_id>[,<clean_session>]` | `+MQTTCFG: "clean",<clean_session>` + `OK` 或 `OK` | `0` 保留会话；`1` 清除会话，默认 0 | 9s | MQTT connect options | 连接前配置 |
| 重传参数 | `AT+MQTTCFG="retrans",<connect_id>[,<retrans_interval>[,<retry_times>]]` | `+MQTTCFG: "retrans",...` + `OK` 或 `OK` | `retrans_interval=20..60s`，默认 20；`retry_times=0..3`，默认 0；间隔随次数加倍 | 9s | QoS 重传策略 | 重传失败上报 `timeout`；仅连接未创建时可配置 |
| Will 参数 | `AT+MQTTCFG="willoption",<connect_id>[,<will_flag>[,<will_qos>[,<will_retain>]]]` | `+MQTTCFG: "willoption",...` + `OK` 或 `OK` | `will_qos=0..2`；retain `0/1` | 9s | 遗嘱配置 | 连接前配置 |
| Will payload | `AT+MQTTCFG="willpayload",<connect_id>[,"<will_topic>","<will_msg>"]` | `+MQTTCFG: "willpayload",...` + `OK` 或 `OK` | `will_topic`、`will_msg` 最长各 256 字节；输入格式受 encoding 影响 | 9s | 遗嘱 topic/payload | 连接前配置 |
| Ping 请求间隔 | `AT+MQTTCFG="pingreq",<connect_id>[,<ping_interval>]` | `+MQTTCFG: "pingreq",<ping_interval>` + `OK` 或 `OK` | `60..86400s`，默认 120 | 9s | 心跳诊断/调优 | 通常保留默认 |
| Ping 回显 | `AT+MQTTCFG="pingresp",<connect_id>[,<pingack>]` | `+MQTTCFG: "pingresp",<pingack>` + `OK` 或 `OK` | 是否上报 ping 响应 | 9s | MQTT ping URC 调试 | 按业务需要开启 |
| Payload 编码 | `AT+MQTTCFG="encoding",<connect_id>[,<input_format>[,<output_format>]]` | `+MQTTCFG: "encoding",<input_format>,<output_format>` + `OK` 或 `OK` | `<input_format>`：`0` ASCII / `1` HEX / `2` 转义；`<output_format>`：`0` 原始 / `1` HEX（不支持 `2`） | 9s | MQTT payload 编码 | 设置后**立即生效**；input 影响 `MQTTPUB` 的 `<message>` 和 `MQTTCFG` 的 `<will_msg>` |
| 缓存模式 | `AT+MQTTCFG="cached",<connect_id>[,<cached_mode>]` | `+MQTTCFG: "cached",<cached_mode>` + `OK` 或 `OK` | `0` 直接上报；`1` 缓存后读取 | 9s | MQTT RX 模式配置 | 二进制/长消息建议缓存模式 |
| 自动重连 | `AT+MQTTCFG="reconn",<connect_id>[,<reconn_times>[,<reconn_interval>[,<mode>]]]` | `+MQTTCFG: "reconn",...` + `OK` 或 `OK` | `reconn_times=0..3`，默认 3；`reconn_interval=20..60s`，默认 20；`mode=0` 固定间隔 / `1` 递增间隔 | 9s | 断线恢复策略 | 仍需 Core 侧状态机兜底 |
| 查询全部配置 | `AT+MQTTCFG="query",<connect_id>` | 13 行 `+MQTTCFG: "<key>",...` + `OK` | 返回该连接的全部配置项 | 9s | 配置诊断/调试 | ML307R 支持；MN316/MN316A/MN318/MN319/MN326 不支持 |
| 建立 MQTT 连接 | `AT+MQTTCONN=<connect_id>,"<host>"[,<port>[,"<clientID>"[,"<user>","<passwd>"]]]` | `OK`，随后 `+MQTTURC: "conn",<connect_id>,<conn_state>` | `host`/`clientID`/`user` 最长 128；`passwd` 最长 256；`port=0..65535`，默认 1883；`conn_state=0` 才表示连接成功 | 命令 9s；连接按业务预算 | `mqtt_client_connect()` | `host` 之后参数均可省略；只等待并接受 `conn_state=0`；重复连接同一 `connect_id` 返回错误 |
| 订阅主题 | `AT+MQTTSUB=<connect_id>,"<topic>",<qos>[,"<topic1>",<qos1>...]` | `+MQTTSUB: <connect_id>,<mid>` + `OK`，随后 `+MQTTURC: "suback",...` | `qos=0..2`；`topic` 最长 256 字节；最多同时 3 个主题 | 9s 或按 ACK 预算 | `mqtt_client_subscribe()` | 查询订阅用 `AT+MQTTSUB=<connect_id>`；多主题 suback code 顺序对应 |
| 取消订阅 | `AT+MQTTUNSUB=<connect_id>,"<topic>"[,"<topic1>"...]` | `+MQTTUNSUB: <connect_id>,<mid>` + `OK`，随后 `+MQTTURC: "unsuback",...` | `topic` 最长 256 字节；最多同时 3 个主题 | 9s 或按 ACK 预算 | `mqtt_client_unsubscribe()` | 失败/超时由 URC 表示 |
| 发布消息 | `AT+MQTTPUB=<connect_id>,"<topic>",<qos>,<retain>,<dup>,<msg_len>[,"<message>"]` | `+MQTTPUB: <connect_id>,<mid>,<length>` + `OK`，随后 QoS URC | `qos=0..2`；retain `0/1`；`dup=0/1` 重发标志；`msg_len=0..1024`（ML307R）；省略 `<message>` 进入数据模式 `>` | 9s 或按 QoS 预算 | `mqtt_client_publish()` | QoS1/2 需等待 `puback/pubrec/pubcomp` |
| 读取缓存消息 | `AT+MQTTREAD=<connect_id>` 或 `AT+MQTTREAD=<connect_id>,<count>` | `+MQTTREAD: <connect_id>,...` + `OK` | 形式一返回 `<store_msgs>,<total_len>`；形式二按 `<count>` 条返回 `<mid>,"<topic>",<payload_len>,<payload>` | 9s 或按长度预算 | MQTT RX 读取 | `cached=1` 时收到 `pubnmi` 后读取；4G/5G 缓存上限 8KB |
| 查询状态 | `AT+MQTTSTATE=<connect_id>` | `+MQTTSTATE: <state>` + `OK` | `state`：`1` 连接/重连中；`2` 连接成功；`3` 断开；`4..255` 保留 | 9s | `mqtt_client_get_state()` | 响应**不含** connect_id；状态值与 URC `conn_state` 不同 |
| 主动断开 | `AT+MQTTDISC=<connect_id>` | `OK`，随后 `+MQTTURC: "conn",<connect_id>,2` | `conn_state=2` 表示客户端主动断开 | 9s | `mqtt_client_disconnect()` | `2` 不是 connected，收到后清理本地 MQTT 状态 |

### MQTT URC

| URC/ACK | 含义 | 映射建议 | 注意事项 |
|---------|------|----------|----------|
| `+MQTTURC: "conn",<connect_id>,<conn_state>` | MQTT 连接状态变化 | `0` -> MQTT connected；`1` -> reconnecting；`2` -> user disconnected；`3` -> auth/protocol rejected；`4` -> server disconnected；`5` -> keepalive timeout；`6` -> network error；`255` -> unknown error | `MQTTCONN` 后只接受 `conn_state=0` 为成功；`MQTTDISC` 后期望 `conn_state=2` |
| `+MQTTURC: "pubnmi",<connect_id>,<mid>,<data_len>` | 缓存模式收到消息提示 | 触发 `AT+MQTTREAD` | 避免 payload 混入 AT 流 |
| `+MQTTURC: "publish",<connect_id>,<mid>,<topic>,<total_len>,<payload_len>,<payload>` | 直接模式收到发布消息 | MQTT RX event | 按长度解析 payload；ML307R 的 topic+msg 总长超 512 字节会分包 |
| `+MQTTURC: "drop",<connect_id>,<dropped_length>` | 接收数据被丢弃 | RX overflow/drop event | 记录日志并考虑增大读取频率 |
| `+MQTTURC: "pingresp",<connect_id>,<ping_ret>` | MQTT ping 响应 | keepalive event | 需配置上报 |
| `+MQTTURC: "timeout",<connect_id>,<mid>` | 订阅、取消订阅或发布最终超时 | 对应 pending operation 返回 `ESP_ERR_TIMEOUT`；必要时查询 `MQTTSTATE` | 重传包中间超时不上报，只有最终超时上报 |
| `+MQTTURC: "suback",<connect_id>,<mid>,<code>[,<code1>,...]` | 订阅确认 | `code=0/1/2` 分别表示订阅成功且授予 QoS0/1/2；`code=128` 表示订阅失败 | 多主题订阅时 code 顺序对应主题顺序，任一 128 应标记该主题失败 |
| `+MQTTURC: "unsuback",<connect_id>,<mid>` | 取消订阅确认 | unsubscribe complete | 与 `MQTTUNSUB` 对应 |
| `+MQTTURC: "puback",<connect_id>,<mid>,<dup>` | QoS1 发布确认 | QoS1 publish complete | `dup=1` 表示重发数据的 ACK |
| `+MQTTURC: "pubrec",<connect_id>,<mid>,<dup>` | QoS2 第一阶段确认 | QoS2 publish progress | 继续等待同一 `mid` 的 `pubcomp` |
| `+MQTTURC: "pubcomp",<connect_id>,<mid>,<dup>` | QoS2 发布完成 | QoS2 publish complete | QoS2 最终完成 |

### MQTT 错误码

MQTT 命令错误以 `+CME ERROR:<err>` 上报（需 `AT+CMEE=1`），手册定义的 MQTT 专用错误码如下。

| 错误码 | 含义 | 映射建议 |
|--------|------|----------|
| `600` | 未知错误 | `ESP_FAIL` 并记录原始码 |
| `601` | 无效参数 | `ESP_ERR_INVALID_ARG` |
| `602` | 未连接或连接失败 | `ESP_ERR_INVALID_STATE` |
| `603` | 正在连接 | `ESP_ERR_INVALID_STATE`，稍后重试 |
| `604` | 已经连接 | `ESP_ERR_INVALID_STATE` |
| `605` | 网络错误 | 网络错误，触发恢复流程 |
| `606` | 存储错误 | `ESP_ERR_NO_MEM` |
| `607` | 状态错误 | `ESP_ERR_INVALID_STATE` |
| `608` | DNS 错误 | DNS 错误 |
| `609..649` | 保留 | 记录原始码 |

## 休眠与低功耗

| 能力 | AT 指令 | 响应格式 | 关键参数/数据 | 默认超时 | 映射建议 | 注意事项 |
|------|---------|----------|----------------|----------|----------|----------|
| 睡眠 URC 配置 | `AT+MLPMCFG="sleepind"[,<sleepind_report>]` | `+MLPMCFG: "sleepind",<sleepind_report>` + `OK` 或 `OK` | `0` 关闭；`1/2` 按睡眠等级上报 | 9s | 低功耗事件配置 | 配置后进入/退出睡眠会上报 `+MLPMENTER/+MLPMEXIT` |
| 睡眠模式 | `AT+MLPMCFG="sleepmode"[,<sleep_mode>[,<permanent>]]` | `+MLPMCFG: "sleepmode",<sleep_mode>` + `OK` 或 `OK` | `0` 关闭睡眠；`1` 浅睡眠；`2` 浅睡眠和深睡眠 | 9s | 低功耗策略 | 通信流程示例用 `sleepmode,2,0` 打开浅睡眠和深睡眠 |
| 延迟休眠 | `AT+MLPMCFG="delaysleep"[,<delay_sleep>]` | `+MLPMCFG: "delaysleep",<delay_sleep>` + `OK` 或 `OK` | 延迟进入深睡眠秒数 | 9s | 唤醒后停留时间配置 | 示例 `AT+MLPMCFG="delaysleep",30` |
| 协议栈低功耗 URC | `AT+MLPMCFG="psind"[,<psind_report>]` | `+MLPMCFG: "psind",<psind_report>` + `OK` 或 `OK` | 协议栈低功耗状态上报开关 | 9s | 低功耗诊断 | 上报 `+MLPMPSTA:<protocol_status>` |
| 进入睡眠 URC | `+MLPMENTER: <sleep_level>` | URC | 睡眠等级 | URC | sleep event | 与系统 URC 注册清单保持一致 |
| 退出睡眠 URC | `+MLPMEXIT: <sleep_level>` | URC | 睡眠等级 | URC | wake event | DTR 低电平可永久唤醒，来电/短信/服务器数据/AT 输入可临时唤醒 |
| 协议栈低功耗状态 | `+MLPMPSTA: <protocol_status>` | URC | 协议栈低功耗状态 | URC | low-power state event | 仅关心模块睡眠时可不启用 `psind` |

## 系统 URC 注册清单

以下前缀是基础系统层建议优先注册并翻译为 `modem_event_t` 的 URC。TCP、HTTP、MQTT 等连接层 URC 在下一小节列出，建议由对应连接层对象处理，避免系统层和业务层重复消费同一行。

| 前缀/完整行 | 来源 | 触发条件 | 映射建议 | 注意事项 |
|-------------|------|----------|----------|----------|
| `+MATREADY` | 模块启动 | 模块 AT 初始化完成 | 释放初始化 ready 等待；AT 初始化完成后投递 `MODEM_EVENT_READY` | 自适应波特率模式可能不上报，需 `AT` 探测兜底 |
| `+CPIN:` | SIM | SIM/PIN 状态变化 | 更新 SIM 状态，必要时触发重新注册流程 | 同时也是 `AT+CPIN?` 查询响应前缀 |
| `+CREG:` | 网络注册 | CREG URC 开启后注册状态变化 | 更新通用注册状态 | 同时也是查询响应前缀 |
| `+CEREG:` | EPS 注册 | CEREG URC 开启后注册状态变化 | 优先用于 LTE 注册状态 | 同时也是查询响应前缀 |
| `+CGREG:` | GPRS 注册 | CGREG URC 开启后分组域注册状态变化 | 更新分组域注册状态 | 同时也是查询响应前缀 |
| `+MIPCALL:` | 应用层拨号 | 自动拨号、手动拨号或断开 | PDP/application networking activated/deactivated | 与命令结果同前缀，命令期间应由响应解析处理 |
| `+MLPMENTER:` | 低功耗 | 模块进入睡眠 | sleep event | 需 `MLPMCFG="sleepind"` 启用 |
| `+MLPMEXIT:` | 低功耗 | 模块退出睡眠 | wake event | 需 `MLPMCFG="sleepind"` 启用 |
| `+MLPMPSTA:` | 协议栈低功耗 | 协议栈低功耗状态变化 | low-power diagnostic event | 需 `MLPMCFG="psind"` 启用 |

### 后续连接层 URC

以下 URC/ACK 属于 TCP、HTTP、MQTT 连接层或数据路径。系统层不应直接翻译为通用网络事件，除非它们表示全局网络恢复入口。

| 前缀/完整行 | 来源 | 触发条件 | 后续映射建议 | 注意事项 |
|-------------|------|----------|--------------|----------|
| `+MIPOPEN:` | TCP/UDP socket | `MIPOPEN` 连接完成或失败 | socket connected/connect failed | `result=0` 成功 |
| `CONNECT` | TCP/UDP socket | 透传模式连接成功 | data mode connected | 第一版尽量避免透传主路径 |
| `+MIPCLOSE:` | TCP/UDP socket | 连接关闭完成 | socket closed | 可能含关闭返回码 |
| `+MIPSEND:` | TCP/UDP socket | 数据进入协议栈缓存 | send accepted | 不代表对端收到 |
| `+MIPURC:` | TCP/UDP socket | 收到数据、ACK、缓存溢出、异常断开等 | socket RX/cache/ACK/drop/disconnected | 按第一个字符串参数区分 `rtcp/rudp/disconn/ack/drop/seq`；ML307R 不支持 UDP `seq` |
| `+MDNSGIP:` | DNS | 域名解析完成 | DNS result | 异步结果 |
| `+MPING:` | PING | 单包结果和统计结果 | ping result/statistics | 按 `result` 区分成功、DNS 错误、超时等 |
| `+MNTP:` | NTP | 网络时间同步完成 | time sync result | `result=0` 成功 |
| `+MHTTPURC:` | HTTP/HTTPS | 请求完成、响应头/体、chunked 输入、下载或关闭 | HTTP response/data/closed/download | 具体事件由字符串参数区分 |
| `+MQTTURC:` | MQTT | 连接状态、消息、ACK、超时、drop、ping | MQTT event | MQTT 连接层集中处理 |

## 推荐初始化与联网流程

启动与 AT 口同步：

1. 固定波特率场景等待 `+MATREADY`；若超时且板级配置允许自适应波特率，则发送 `AT` 直到返回 `OK`。
2. 若收到 `+MATREADY` 后需要执行 `AT+CFUN=0` 或 `AT+CFUN=1`，先等待至少 2 秒。
3. `ATE0` 关闭回显。
4. `AT+CMEE=1` 启用数字错误码。
5. 可选 `ATI`、`AT+CGMM`、`AT+CGMR`、`AT+CGSN`、`AT+CIMI`、`AT+MCCID` 填充模块信息。

注册与 SIM 检查：

1. `AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2` 启用注册状态 URC。
2. 轮询 `AT+CPIN?`，要求 `+CPIN: READY`。
3. `AT+CFUN?`，确认驻网前功能模式为 `1`。
4. `AT+CSQ` 或 `AT+CESQ` 检查信号。
5. 轮询 `AT+CEREG?` 或 `AT+CGREG?`，要求 `stat=1` 或 `stat=5`。
6. `AT+CGATT?`，要求 `+CGATT: 1`。
7. 可选 `AT+MUESTATS="radio"` 或 `AT+MUESTATS="cell"` 打印诊断。

PDP 与应用层拨号：

1. `AT+MIPCALL?` 查询 `cid=1` 是否已由自动拨号激活。
2. 若未激活且 APN 需要显式配置，执行 `AT+CGDCONT=1,"IPV4V6","<apn>"`。
3. 专网卡需要鉴权时执行 `AT+CGAUTH=1,<auth>,"<user>","<password>"`。
4. `AT+MIPCALL=1,1`，等待 `+MIPCALL: 1,1,"<ipv4>"[...]`。
5. 可选 `AT+MPING="<host>",10,4,32,1` 执行基础连通性检查，解析每行 `+MPING: <result>,"<ip>",<packet_len>,<time>,<ttl>` 和末尾 `+MPING: "statistics",<sent>,<lost>,<rtt_min>,<rtt_max>,<rtt_avg>`。
6. 仅断开应用层网络时执行 `AT+MIPCALL=0,1`；需要去激活 PDP 时执行 `AT+CFUN=4`。

TCP Socket 非透传推荐流程（TCP client v1）：

1. 完成 `MIPCALL` 应用层拨号并缓存本地 IP。
2. `AT+MIPCFG="cid",0,1` 绑定连接实例到 `cid=1`。
3. `AT+MIPCFG="encoding",0,0,1` 使用 raw 输入 + HEX 缓存输出，保证 RX 二进制安全且避免 payload 直接混入 AT 流。
4. 可选 `AT+MIPCFG="autofree",0,1`，异常断开后由 Core 显式释放资源。
5. `AT+MIPOPEN=0,"TCP","<host>",<port>,60,2`，等待 `+MIPOPEN: 0,0`。
6. `AT+MIPSEND=0,<len>`，等待 `>` 后输入精确长度数据；`+MIPSEND: 0,<len>` 表示进入协议栈缓存，随后仍需等待尾随 `OK`，避免 `OK` 残留污染下一条命令。
7. 收到 `+MIPURC: "rtcp",0,<recv_length>,<total_length>` 后执行 `AT+MIPRD=0,<len>`，按 HEX 长度解码 payload；仅返回 `OK` 表示 stale readable/no cached data，不按错误处理。
8. 可选 `AT+MIPSACK=0` 查询未 ACK 字节数。
9. 收到 `+MIPURC: "disconn",0,<connect_state>` 或主动关闭时执行 `AT+MIPCLOSE=0` 释放连接，等待 `+MIPCLOSE: 0`。

HTTP/HTTPS 推荐流程：

1. 完成 `MIPCALL` 应用层拨号。
2. HTTPS 场景先写入证书/私钥，并配置 `AT+MSSLCFG="auth"`、`AT+MSSLCFG="cert"`、`AT+MSSLCFG="version"` 等 SSL context。
3. `AT+MHTTPCREATE="http://<host>:<port>"` 或 `AT+MHTTPCREATE="https://<host>:<port>"`，记录 `<httpid>`。
4. `AT+MHTTPCFG="cid",<httpid>,1` 绑定 PDP。
5. HTTPS 执行 `AT+MHTTPCFG="ssl",<httpid>,1,<ssl_id>`。
6. 可选 `AT+MHTTPCFG="timeout",<httpid>,<conn_timeout>,<rsp_timeout>,<input_timeout>`。
7. 可选 `AT+MHTTPHEADER=<httpid>,...` 设置请求头。
8. POST/PUT 普通模式先 `AT+MHTTPCONTENT=<httpid>,...` 写入 body。
9. `AT+MHTTPREQUEST=<httpid>,<method>[,<length>,"<path>"]`，等待 `+MHTTPURC: "rsp",<httpid>,<status>,<len>`。
10. `AT+MHTTPREAD=<httpid>[,<read_len>]` 按长度读取响应。
11. `AT+MHTTPDEL=<httpid>` 删除实例；错误中断时可先 `AT+MHTTPTERM=<httpid>`。

MQTT 推荐流程：

1. 完成 `MIPCALL` 应用层拨号。
2. TLS 场景先配置 SSL context，再执行 `AT+MQTTCFG="ssl",0,1,<ssl_id>`。
3. `AT+MQTTCFG="version",0,4`。
4. `AT+MQTTCFG="cid",0,1`。
5. `AT+MQTTCFG="keepalive",0,<keepalive>`，默认可使用 120s；按业务需要调整。
6. `AT+MQTTCFG="clean",0,<clean_session>`。
7. 二进制或长下行建议 `AT+MQTTCFG="cached",0,1`。
8. `AT+MQTTCONN=0,"<host>",<port>,"<client_id>","<user>","<password>"`，等待 `+MQTTURC: "conn",0,0` 表示连接成功；`conn_state` 为其他值时按连接失败或断链处理。
9. 发布使用 `AT+MQTTPUB=0,"<topic>",<qos>,<retain>,<dup>,<msg_len>,"<message>"`，按 QoS 等待对应 `+MQTTURC`。
10. 订阅使用 `AT+MQTTSUB=0,"<topic>",<qos>`，等待 `+MQTTURC: "suback",0,...`。
11. 缓存模式收到 `+MQTTURC: "pubnmi",0,...` 后执行 `AT+MQTTREAD=0` 或 `AT+MQTTREAD=0,<count>`。
12. 断开时执行 `AT+MQTTDISC=0`，等待 `+MQTTURC: "conn",0,2` 表示客户端主动断开完成。

错误恢复建议：

- `AT` 无响应时先按板级 reset/powerkey 策略恢复，再等待 `+MATREADY` 或用 `AT` 同步。
- 注册长时间失败时可执行 `AT+CFUN=0`、等待、再 `AT+CFUN=1` 重新驻网；若刚收到 `+MATREADY`，必须先满足 2s 等待。
- 因 SIM 欠费导致业务中断时，通信流程手册说明充值后需要复位模块，或执行 `AT+CFUN=0`、`AT+CFUN=1` 重新驻网。
- `+MIPCALL:<cid>,0` 或 `+MIPCALL: <cid>,0`、socket/MQTT/HTTP 断链后，先查询对应状态，再释放连接实例并重新走 `MIPCALL` 与连接层流程。
- 多路拨号时第 1 路为默认承载；当前实现可先只支持 `cid=1`，其他 CID 返回不支持或保留后续扩展。
