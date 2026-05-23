# Air780EP 系统 AT 指令文档设计

## 背景

项目下一步要实现 `modem_air780ep_t`，该类负责把 Core 层的语义操作翻译为 Air780EP 模块的具体 AT 指令，并把系统级 URC 翻译为模块事件。

完整 AT 手册是 PDF，检索和复用不方便。因此需要在 `docs/agents/` 下新增 Markdown 文档，提取 `上海合宙 Cat.1 模组(移芯 EC618&EC716&EC718 平台系列)AT 命令手册 V1.6.7.pdf` 中系统/联网基础相关的指令，作为后续实现 `modem_air780ep_t` 的直接参考。

## 目标文档

新增文件：`docs/agents/at_cmd_air780ep.md`

该文档不是完整 AT 手册，也不覆盖全部业务指令。它只收录后续 modem 层需要优先整合的系统能力指令。

## 范围

本轮收录范围包含 TCPIP 激活所需的基础命令：

- 模块身份与基础控制
- 串口、回显、错误结果码和保存配置
- SIM 状态、信号质量、网络注册状态
- 分组域、PDP、TCPIP 激活和基础连通性检查
- 休眠、低功耗和 RI 唤醒相关配置
- 系统/联网相关 URC 注册清单

本轮不收录：

- MQTT、HTTP、FTP 等上层业务协议指令
- SMS、语音、GNSS、文件系统等业务功能指令
- 固件升级、GPIO/ADC、VSIM 等非基础联网流程指令，除非后续 `modem_air780ep_t` 明确需要

## 推荐结构

`docs/agents/at_cmd_air780ep.md` 按 modem 能力分组，而不是按 PDF 章节顺序摘抄。

推荐章节：

1. 文档目的与来源
2. 实现注意事项
3. 模块身份与基础控制
4. 串口与结果码配置
5. SIM 与网络状态
6. 分组域/PDP 与 TCPIP 激活
7. 休眠与低功耗
8. URC 注册清单
9. 推荐初始化/联网流程

## 每条指令字段

每条 AT 指令使用统一格式，便于后续实现时直接映射到 `modem_air780ep_t`：

- 能力
- AT 指令
- 用途
- 响应格式
- 关键参数
- 默认超时
- 需要解析的数据
- 相关 URC
- `modem_air780ep_t` 映射建议
- 注意事项

## 指令分组

### 模块身份与基础控制

候选指令：

- `ATI`
- `AT+VER`
- `AT+CGMM`
- `AT+CGMR`
- `AT+CGSN`
- `AT+CIMI`
- `AT+RESET`
- `AT+CFUN`

用途：提供模块识别、固件版本、IMEI/IMSI 查询、重启和功能模式控制。

### 串口与结果码配置

候选指令：

- `ATE0`
- `AT+CMEE`
- `AT+IPR`
- `AT+IFC`
- `AT&W`

用途：初始化 AT 口行为，关闭回显，启用可解析的错误码，配置波特率、流控和持久化配置。

### SIM 与网络状态

候选指令：

- `AT+CPIN?`
- `AT+CCID` / `AT+ICCID`
- `AT+CSQ`
- `AT+CESQ`
- `AT+CREG`
- `AT+CEREG`
- `AT+CGREG`
- `AT+COPS?`
- `AT^SYSINFO`

用途：判断 SIM 是否 ready、读取卡号、查询信号质量、判断网络注册状态和当前服务状态。

### 分组域/PDP 与 TCPIP 激活

候选指令：

- `AT+CGATT?`
- `AT+CGDCONT`
- `AT+CGAUTH`
- `AT+CGACT`
- `AT+CGPADDR`
- `AT+CGEREP`
- `+CGEV`
- `AT+CSTT`
- `AT+CIICR`
- `AT+CIFSR`
- `AT+CIPSTATUS`
- `AT+CIPSHUT`
- `AT+CIPPING`

用途：建立 PDP/数据承载，获取 IP，检测 TCPIP 栈状态，处理 PDN 激活/去激活 URC，执行基础 ping 测试。

### 休眠与低功耗

候选指令：

- `AT+CSCLK`
- `AT+WAKETIM`
- `AT*RTIME`
- `AT+POWERMODE`
- `AT+CFGRI`
- `AT+CFGRISAVE`
- `AT^WAKEUPHEX`

用途：配置普通睡眠、超低功耗模式、数据模式休眠等待时间、RI 唤醒和指定 URC 唤醒过滤。

### URC 注册清单

候选 URC：

- `RDY`
- `+CPIN:`
- `+CREG:`
- `+CEREG:`
- `+CGREG:`
- `+CGEV:`
- `+PDP DEACT` 或手册示例中的 `+PDP:DEACT`
- `CLOSED`
- `+CIPRXGET:`

用途：给 `modem_air780ep_t` 注册系统级 URC 前缀，并转译为 Core 可消费的模块事件。

## AT Engine 约束

当前 `at_engine` 第一版行为：

- 有当前命令时，非最终响应行会优先进入当前 `at_response_t`。
- 无当前命令时，才按 URC 前缀分发给已注册 handler。
- 因此第一版不保证命令等待期间的自发 URC 能独立分发。

`at_cmd_air780ep.md` 中必须明确标注这一点，尤其是 `+CREG:`、`+CEREG:`、`+CGREG:` 这类既可能作为查询响应、也可能作为 URC 的前缀。

后续如果 `modem_air780ep_t` 需要在命令等待期间接收关键 URC，应先扩展 AT Engine 的 URC 判定策略，再把该策略写入文档。

## 推荐联网流程

文档应给出一个后续实现可直接参考的基础流程：

1. `ATE0`
2. `AT+CMEE=1` 或 `AT+CMEE=2`
3. `AT+CPIN?`
4. `AT+CSQ` 或 `AT+CESQ`
5. `AT+CEREG?` / `AT+CGREG?`
6. `AT+CGATT?`
7. `AT+CGEREP=1`
8. `AT+CSTT`
9. `AT+CIICR`
10. `AT+CIFSR`
11. 可选 `AT+CIPPING`

同时说明 Air780EP 手册中也存在 `AT+CGDCONT`、`AT+CGACT`、`AT+CGPADDR` 这一组 PDP 标准指令；本项目第一阶段可优先采用旧实现已有的 `CSTT/CIICR/CIFSR` TCPIP 激活流程，标准 PDP 指令作为显式 APN、鉴权或故障诊断路径。

## 成功标准

`docs/agents/at_cmd_air780ep.md` 完成后应满足：

- 能让实现者不再反复翻 PDF 即可编写 `modem_air780ep_t` 的系统能力方法。
- 每条指令都有命令格式、响应格式、超时建议和解析重点。
- URC 前缀和命令响应同前缀风险被明确记录。
- 范围聚焦系统/联网基础，不混入 MQTT/HTTP/FTP/SMS/GNSS 等业务指令。

## 非目标

- 不实现 `modem_air780ep_t`。
- 不修改 `AGENTS.md` 索引，除非后续明确要求把该文档加入索引。
- 不重构 AT Engine。
- 不验证实机行为。
