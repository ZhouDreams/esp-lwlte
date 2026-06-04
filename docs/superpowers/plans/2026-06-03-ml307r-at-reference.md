# ML307R AT Reference Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a full ML307R AT/URC reference document with coverage parallel to the Air780EP reference and index it from both root agent instruction files.

**Architecture:** This is a documentation-only change. The new `docs/agents/at_cmd_ml307r.md` will mirror the Air780EP reference structure while replacing Air780EP-specific command families with ML307R-specific startup, PDP, TCP/IP, HTTP/HTTPS, MQTT, low-power, and URC details. Root indexes will link the new document so future agents can find it.

**Tech Stack:** Markdown documentation, vendor PDF text extraction via `pdftotext`, repository search via `rg` through the available search tools, no ESP-IDF build required.

---

## File Structure

- Create: `docs/agents/at_cmd_ml307r.md`
- Modify: `AGENTS.md` only if the ML307R index entry is missing
- Modify: `AGENTS_ZH.md` to add the ML307R index entry
- Reference only: `docs/agents/at_cmd_air780ep.md`
- Reference only: `reference/中移物联ML307R/AT_Commands_Reference_Guide_4G_Series_V2.0.5.pdf`
- Reference only: `reference/中移物联ML307R/ML307R_通信流程示例-V1.1.2.pdf`
- Reference only: `reference/中移物联ML307R/TCP_IP用户手册_5.1.2-R.pdf`
- Reference only: `reference/中移物联ML307R/HTTP_HTTPS用户手_V6.1.1.pdf`
- Reference only: `reference/中移物联ML307R/SSL用户手册_V5.4.2-R.pdf`
- Reference only: `reference/中移物联ML307R/MQTT用户手册_V6.8.3.pdf`
- Reference only: `reference/中移物联ML307R/扩展AT用户手册_4G系列-1.7.0.pdf`

### Task 1: Prepare Vendor Text References

**Files:**
- Read: `reference/中移物联ML307R/*.pdf`
- Temporary output: `/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_*.txt`

- [ ] **Step 1: Verify temporary directory exists**

Run: `ls "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode"`

Expected: command exits with status 0 and lists existing temporary files or directories.

- [ ] **Step 2: Extract key PDFs to text**

Run:

```bash
pdftotext -layout "reference/中移物联ML307R/AT_Commands_Reference_Guide_4G_Series_V2.0.5.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_at_reference.txt" && pdftotext -layout "reference/中移物联ML307R/ML307R_通信流程示例-V1.1.2.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_flow_reference.txt" && pdftotext -layout "reference/中移物联ML307R/TCP_IP用户手册_5.1.2-R.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_tcpip_reference.txt" && pdftotext -layout "reference/中移物联ML307R/HTTP_HTTPS用户手_V6.1.1.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_http_reference.txt" && pdftotext -layout "reference/中移物联ML307R/SSL用户手册_V5.4.2-R.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_ssl_reference.txt" && pdftotext -layout "reference/中移物联ML307R/MQTT用户手册_V6.8.3.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_mqtt_reference.txt" && pdftotext -layout "reference/中移物联ML307R/扩展AT用户手册_4G系列-1.7.0.pdf" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode/ml307r_ext_reference.txt"
```

Expected: no stdout and exit status 0.

- [ ] **Step 3: Confirm extracted text contains required command families**

Run: `rg "MATREADY|MIPCALL|MIPOPEN|MIPSEND|MIPURC|MQTTCFG|MQTTCONN|MQTTURC|HTTP|SSL|MLPMCFG" "/var/folders/pd/ntx6_vj17ts99tv5g69rz70c0000gn/T/opencode"`

Expected: matches across the extracted ML307R text files.

### Task 2: Create ML307R AT/URC Reference

**Files:**
- Create: `docs/agents/at_cmd_ml307r.md`
- Read: `docs/agents/at_cmd_air780ep.md`

- [ ] **Step 1: Draft the document skeleton**

Create `docs/agents/at_cmd_ml307r.md` with these sections in this order:

```markdown
# ML307R AT 指令摘录

本文从 `reference/中移物联ML307R/` 下的 ML307R/4G 系列 AT、通信流程、TCP/IP、HTTP/HTTPS、SSL、MQTT 和扩展 AT 用户手册中摘录 `modem_ml307r_t` 需要优先整合的 AT 指令与 URC。

本文不是完整 AT 手册，只覆盖后续 Modem Adapter 层实现所需的基础能力：模块识别、AT 口初始化、SIM/信号/网络注册、PDP 与应用层拨号、TCP/UDP Socket、DNS、PING、HTTP/HTTPS、MQTT、休眠低功耗和相关 URC。

## 使用边界

## 实现注意事项

## 文档字段说明

## 模块身份与基础控制

## 串口与结果码配置

## SIM 与网络状态

## 分组域/PDP 与应用层拨号

### `+MIPCALL` 拨号事件

## TCPIP 连接层与 PING

### TCP/IP URC 与错误

## HTTP/HTTPS 相关指令

### HTTP/HTTPS URC 与错误

## MQTT 相关指令

### MQTT URC

## 休眠与低功耗

## 系统 URC 注册清单

### 后续连接层 URC

## 推荐初始化与联网流程
```

- [ ] **Step 2: Fill usage boundary and implementation notes**

Add the included/excluded bullet lists matching Air780EP coverage. Include the ML307R-specific notes: `+MATREADY` startup, autobaud no-URC behavior, one AT command at a time, wait at least 2 seconds after `+MATREADY` before `AT+CFUN=0/1`, and AT Engine query-response versus URC overlap.

- [ ] **Step 3: Fill identity, UART, SIM, and registration tables**

Add tables using the same columns as Air780EP: `能力`, `AT 指令`, `响应格式`, `关键参数/数据`, `默认超时`, `映射建议`, `注意事项`. Include at least `ATI`, `AT+CGMM`, `AT+CGMR`, `AT+CGSN`, `AT+CIMI`, `AT+CFUN`, `ATE0`, `AT+CMEE`, `AT+IPR`, `AT+IFC`, `AT&W`, `AT+CPIN?`, ICCID query, `AT+CSQ`, `AT+CESQ`, `AT+CREG`, `AT+CEREG`, `AT+CGREG`, `AT+COPS?`, and ML307R system-info diagnostics if present in the manuals.

- [ ] **Step 4: Fill PDP and application dialing section**

Document standard PDP commands `AT+CGATT?`, `AT+CGDCONT`, `AT+CGAUTH`, `AT+CGACT`, `AT+CGPADDR` plus ML307R application dialing `AT+MIPCALL?`, `AT+MIPCALL=1,<cid>`, and `AT+MIPCALL=0,<cid>`. Explicitly state that `MIPCALL=0,<cid>` disconnects application networking and the vendor guide warns it does not necessarily deactivate PDP; document `AT+CFUN=4` as the full PDP deactivation path described by the communication-flow guide.

- [ ] **Step 5: Fill TCP/IP, DNS, ping, and socket section**

Document `AT+MIPCFG`, `AT+MIPTKA`, `AT+MIPOPEN`, `AT+MIPCLOSE`, `AT+MIPSEND`, `AT+MIPRD`, `AT+MIPMODE`, `AT+MIPSTATE`, `AT+MIPSACK`, `AT+MDNSCFG`, `AT+MDNSGIP`, `AT+MPING`, and `+MIPURC`. Keep the mapping suggestions focused on future socket APIs and Modem Adapter helpers.

- [ ] **Step 6: Fill HTTP/HTTPS and SSL section**

Use the ML307R HTTP/HTTPS and SSL manuals to document the HTTP lifecycle commands, HTTPS enablement, certificate/SSL context configuration, request action, response read, termination, and the related HTTP/HTTPS URCs. Do not copy Air780EP assumptions unless the ML307R manuals use the same command and response forms.

- [ ] **Step 7: Fill MQTT section**

Document `AT+MQTTCFG`, `AT+MQTTCONN`, `AT+MQTTSUB`, `AT+MQTTUNSUB`, `AT+MQTTPUB`, `AT+MQTTREAD`, `AT+MQTTSTATE`, `AT+MQTTDISC`, and `+MQTTURC`. Include these parameter limits and defaults from the MQTT manual: `connect_id=0..5`, MQTT version 4, `keepalive` default/range, `clean_session`, SSL enable/context, payload encoding, cache mode, and reconnect settings.

- [ ] **Step 8: Fill low-power and URC registry sections**

Document available ML307R sleep/low-power and wakeup commands from the base or extension manuals. Add system URC rows for `+MATREADY`, `+CPIN:`, `+CREG:`, `+CEREG:`, `+CGREG:`, and `+MIPCALL:`. Add connection-layer URC rows for `+MIPOPEN:`, `+MIPCLOSE:`, `+MIPSEND:`, `+MIPURC:`, `CONNECT`, `+MDNSGIP:`, `+MQTTURC:`, and HTTP/HTTPS completion/data indications found in the manual.

- [ ] **Step 9: Fill recommended flows**

Add ML307R-specific flow lists for startup, registration, PDP/application dialing, TCP socket, HTTP/HTTPS, MQTT, and recovery. The flows must use `+MATREADY`/autobaud startup, `AT+MIPCALL` for data-plane setup, `AT+MIPOPEN`/`AT+MIPSEND`/`AT+MIPCLOSE` for sockets, and `AT+MQTTCFG`/`AT+MQTTCONN`/`AT+MQTTPUB`/`AT+MQTTSUB`/`AT+MQTTDISC` for MQTT.

### Task 3: Update Root Agent Indexes

**Files:**
- Modify: `AGENTS.md`
- Modify: `AGENTS_ZH.md`

- [ ] **Step 1: Check English index entry**

Run: `rg "ML307R AT Commands & URCs" AGENTS.md`

Expected: one match. If missing, add this row after the Air780EP AT row:

```markdown
| ML307R AT Commands & URCs | [docs/agents/at_cmd_ml307r.md](docs/agents/at_cmd_ml307r.md) |
```

- [ ] **Step 2: Add Chinese index entries**

In `AGENTS_ZH.md`, add these rows after the general AT command reference row:

```markdown
| Air780EP AT 指令与 URC | [docs/agents/at_cmd_air780ep.md](docs/agents/at_cmd_air780ep.md) |
| ML307R AT 指令与 URC | [docs/agents/at_cmd_ml307r.md](docs/agents/at_cmd_ml307r.md) |
| Air780EP CME ERROR 码 | [docs/agents/cme_error_air780ep.md](docs/agents/cme_error_air780ep.md) |
```

Rationale: `AGENTS_ZH.md` currently lacks the Air780EP AT/CME rows already present in `AGENTS.md`, so adding all three keeps both root indexes aligned.

### Task 4: Verify Documentation Consistency

**Files:**
- Verify: `docs/agents/at_cmd_ml307r.md`
- Verify: `AGENTS.md`
- Verify: `AGENTS_ZH.md`

- [ ] **Step 1: Verify required ML307R sections exist**

Run: `rg "^## (使用边界|实现注意事项|模块身份与基础控制|串口与结果码配置|SIM 与网络状态|分组域/PDP 与应用层拨号|TCPIP 连接层与 PING|HTTP/HTTPS 相关指令|MQTT 相关指令|休眠与低功耗|系统 URC 注册清单|推荐初始化与联网流程)$" docs/agents/at_cmd_ml307r.md`

Expected: matches for all listed headings.

- [ ] **Step 2: Verify ML307R command families are present**

Run: `rg "\+MATREADY|AT\+MIPCALL|AT\+MIPOPEN|AT\+MIPSEND|\+MIPURC|AT\+MQTTCFG|AT\+MQTTCONN|\+MQTTURC" docs/agents/at_cmd_ml307r.md`

Expected: matches for each ML307R-specific startup, PDP, TCP/IP, and MQTT family.

- [ ] **Step 3: Verify Air780EP-only command families were not copied into ML307R recommended flows**

Run: `rg "AT\+CSTT|AT\+CIICR|AT\+CIFSR|AT\+MCONFIG|AT\+MCONNECT|AT\+MSUB|CONNACK OK" docs/agents/at_cmd_ml307r.md`

Expected: no matches, unless a line explicitly says these are Air780EP commands not used by ML307R.

- [ ] **Step 4: Verify root indexes contain ML307R link**

Run: `rg "at_cmd_ml307r.md" AGENTS.md AGENTS_ZH.md`

Expected: one match in each root index file.

- [ ] **Step 5: Review git diff for unintended edits**

Run: `git diff -- docs/agents/at_cmd_ml307r.md AGENTS.md AGENTS_ZH.md`

Expected: diff only creates the ML307R reference and adds/keeps root index rows.

### Task 5: Final Review

**Files:**
- Review: `docs/agents/at_cmd_ml307r.md`
- Review: `AGENTS.md`
- Review: `AGENTS_ZH.md`

- [ ] **Step 1: Read the new document top to bottom**

Check for incomplete sections, unresolved notes, contradictory flow recommendations, and table rows that still mention `modem_air780ep_t` where they should mention `modem_ml307r_t`.

- [ ] **Step 2: Fix discovered documentation issues**

Use `apply_patch` for any manual edits. Keep fixes limited to the three intended files.

- [ ] **Step 3: Re-run verification commands from Task 4**

Expected: all Task 4 checks still pass.

- [ ] **Step 4: Report final status**

Summarize created/modified files and note that no ESP-IDF build was run because this is documentation-only.
