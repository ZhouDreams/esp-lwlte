# Air780EP 系统 AT 指令摘录

本文从 `reference/上海合宙Cat.1模组(移芯EC618&EC716&EC718平台系列)AT命令手册V1.6.7.pdf` 中摘录 Air780EP 在 `modem_air780ep_t` 中需要优先整合的系统级 AT 指令。

本文不是完整 AT 手册，只覆盖后续 Modem Adapter 层实现所需的基础能力：模块识别、AT 口初始化、SIM/信号/网络注册、PDP 与 TCPIP 激活、基础连通性检查、休眠低功耗和系统 URC。

## 使用边界

本轮包含：

- 模块身份与基础控制
- 串口、回显、错误结果码和配置保存
- SIM 状态、信号质量、网络注册状态
- 分组域、PDP、TCPIP 激活和基础连通性检查
- 休眠、低功耗和 RI 唤醒相关配置
- 系统/联网相关 URC 注册清单

本轮不包含：

- MQTT、HTTP、FTP 等上层业务协议指令
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
| 查询 PIN/SIM 状态 | `AT+CPIN?` | `+CPIN: <code>` + `OK` | `READY` 可用；`SIM PIN` 等待 PIN；`SIM REMOVED` 未检出 | 9s | SIM-ready 初始化内部步骤 | 单次查询保持 9s；SIM-ready 等待应由上层轮询/重试总预算实现，而不是一次 `AT+CPIN?` 等待 180s；也存在 URC `+CPIN:<code>` |
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

以下前缀是本轮 Modem 系统层会注册并翻译为 `modem_event_t` 的 URC。

| 前缀/完整行 | 来源 | 触发条件 | 映射建议 | 注意事项 |
|-------------|------|----------|----------|----------|
| `RDY` | 模块启动 | 模块重启完成 | 释放初始化 RDY 等待；AT 初始化完成后再投递 `MODEM_EVENT_READY` | PDF 片段未系统列出，但旧实现已注册，实机常见 |
| `+CPIN:` | SIM | SIM 状态变化 | 更新 SIM 状态，必要时触发重新注册流程 | 同时也是 `AT+CPIN?` 查询响应前缀 |
| `+CREG:` | 网络注册 | CREG URC 开启后注册状态变化 | 更新通用注册状态 | 同时也是 `AT+CREG?` 查询响应前缀 |
| `+CEREG:` | EPS 注册 | CEREG URC 开启后 LTE 注册状态变化 | 优先用于 LTE 注册状态 | 同时也是 `AT+CEREG?` 查询响应前缀 |
| `+CGREG:` | GPRS 注册 | CGREG URC 开启后分组域注册状态变化 | 更新分组域注册状态 | 同时也是 `AT+CGREG?` 查询响应前缀 |
| `+CGEV:` | 分组域事件 | PDP/PDN 激活、去激活或修改 | 触发 `MODEM_EVENT_PDP_ACTIVATED` / `MODEM_EVENT_PDP_DEACTIVATED` | 当前实现不发送 `AT+CGEREP`；若模块默认上报则处理 |
| `+PDP DEACT` / `+PDP:DEACT` | TCPIP 示例 | PDP 上下文被网络释放 | 网络离线，执行恢复流程 | 手册 TCPIP 章节写法不完全一致，实现时可兼容两个文本 |

### 后续连接层 URC

以下 URC 属于连接层或 socket 数据路径，本轮 Modem 系统层不注册，只保留为后续实现参考。

| 前缀/完整行 | 来源 | 触发条件 | 后续映射建议 | 注意事项 |
|-------------|------|----------|--------------|----------|
| `CLOSED` | TCPIP 示例 | TCP 断链 | socket closed 或网络离线事件 | 连接层/socket 后续处理 |
| `+CIPRXGET:` | TCPIP 手动取数 | 手动接收模式收到数据 | 数据到达事件 | socket 数据路径后续处理 |

## 推荐初始化与联网流程

第一阶段建议 `modem_air780ep_t` 使用旧实现验证过的 TCPIP 激活路径，标准 PDP 指令作为显式 APN、鉴权和诊断路径。

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

错误恢复建议：

- 收到 `+CGEV: NW PDN DEACT` 或 `+CGEV: ME PDN DEACT` 后，Core 应进入网络恢复流程。
- 收到 `+PDP DEACT` 或 `+PDP:DEACT` 后，先执行 `AT+CIPSHUT`，再重新走联网流程。
- `AT+CIPSTATUS` 显示 `PDP DEACT` 时，也应执行 `AT+CIPSHUT` 回到 `IP INITIAL`。
- `AT+CSTT/AT+CIICR/AT+CIFSR` 失败时，不要盲目重复同一步；先 `AT+CIPSHUT` 清理移动场景。
