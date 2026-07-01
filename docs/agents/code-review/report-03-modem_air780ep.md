# Code Review: Modem Air780EP Impl（#3）

**日期**: 2026-07-01
**文件**: `src/modem/modem_air780ep.c`（6111 行）、`src/modem/modem_air780ep.h`
**审查范围**: Air780EP 子类——生命周期（create/destroy/start/reset/stop）、URC 翻译与上行事件 ownership、MQTT/socket/HTTP/ping 命令实现、SSL context 配置、AT 响应解析。
**审查聚焦**：因单文件 6000+ 行，按本项目痛点优先审**内存 ownership/泄漏**与**资源账本**，覆盖全部维度的高风险路径（create/destroy、MSUB/TCP 数据路径、HTTP body、MQTT 配置深拷贝、SSL bitmap、解析边界、失败路径清理）。

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

- **`src/modem/modem_air780ep.c:5536-5596`** — HTTP 响应体被静默截断。
  - `AT+HTTPREAD=0,%u` 单次最多读 `AIR780EP_HTTPREAD_BODY_MAX`（3356）字节（`:5538-5540`）。模块返回的 `data_len`（来自 `+HTTPACTION:`，`:5514`）若大于 3356，则只取首个分片；函数仍返回 `ESP_OK`，`response->body_len = copied`（≤3356），**无任何截断标志/错误**。
  - 与 POST 路径不对称：POST body 超 3356 直接返回 `ESP_ERR_INVALID_SIZE`（`:5448-5451`），而 READ 路径静默截断。
  - 影响：HTTP 响应普遍 > 3.3 KB（JSON/HTML），调用方拿到"看似完整"的截断 body，导致隐蔽业务 bug。`modem_http_response_t` 契约（`modem.h:449-457`）未声明截断可能。
  - 可修复性：Air780EP `AT+HTTPREAD=<offset>,<length>` 支持分片，可循环读取至 `data_len`；或至少在 `data_len > read_len` 时置错误/标志位。
  - 建议修复：循环分片读取累计到 `data_len`，或返回 `ESP_ERR_INVALID_RESPONSE`/截断标志以告知调用方。

---

## 🟢 低严重度

- **`src/modem/modem_air780ep.c:97-99 / 全部命令函数`** — `air780ep_cmd_ctx_t` 每命令帧栈占用 ≈ **404 B**（`char *lines[101]`）。
  - `AIR780EP_MAX_RESPONSE_LINES=101` 是为 `+CIPPING`（最多 100 reply + 终态行，`_Static_assert` 锁定 `:85-86`）所必需，无法调小。
  - 非缺陷：命令函数运行在 Core FSM task，栈可配。但属显著的每帧成本，建议在 `core.fsm.task_stack` 文档/默认值中体现"命令路径需预留 > 512 B 栈 + AT Engine send_cmd 栈"。（资源注记，非 bug。）

- **`src/modem/modem_air780ep.c:5575-5593`** — HTTP body 重组按 AT 行解析结果在行间补 `\r\n`；若某 body 数据行恰好以 `"+HTTPREAD:"` 开头会被 `:5580` 跳过。HTTP body 出现该前缀概率极低，属解析边界理论风险。

- **跨模块交叉**：`modem_mqtt_publish` 零长度 payload 限制继承自 base（`modem.c:666`，模块 #2 已记）。Air780EP `AT+MPUBEX` 需显式长度，本层与之自洽，非新增问题。

---

## 无问题维度

- **A 资源账本**：单实例 `calloc(1, sizeof(modem_air780ep_t))`（含 `ssl_auth_modes[256]`=1024 B、`pdp[4]`≈488 B、`ssl_provisioned_bitmap[8]`=32 B，整体 ≈ 3–4 KB 堆），未被配置项乘以更大上限；无 AT Engine 式池爆裂。栈 ctx 见上条注记。
- **B 内存安全与生命周期**（本项目主线之一）：
  - `parse_msub_direct`（`:5682`）：`strlen(cursor) < parsed_payload_len` 前置校验（`:5753`）、`strtoul` errno/ERANGE + `>SIZE_MAX` 防溢出（`:5740`）；malloc 失败互释（`:5763-5765`）。
  - `decode_hex_payload`（`:1711`）：偶数长度校验 + 逐 nibble 校验，非法即 free 返回；索引 `i*2/i*2+1` 安全。
  - `copy_mqtt_config`（`:2594`）：build-new-then-swap——先克隆全部串到临时 `copy`，全成功才 `free_mqtt_config(dst)` 后 `*dst=copy`，**重配置无泄漏、无 use-after-free**。
  - **destroy 无泄漏**：`air780ep_destroy`→`clear_mqtt_state`→`air780ep_clear_ssl_state`（`:3008`）→`free_mqtt_config` 释放 mqtt 堆串；URC 注销在 destroy 中完成。已逐链验证。
  - URC heap topic/payload 经 `post_mqtt_data_event`→`modem_post_event`：post 成功 ownership 转 Modem（event_task 回调后释放），post 失败 / `!mqtt_data_enabled` 时调用方释放（`handle_msub_urc :6099-6110`）。无 double-free。
- **C 并发/死锁**：URC handler 在 AT RX task 运行，仅做有界解析 + malloc + 非阻塞 `modem_post_event`；命令 ops 在 Core FSM task 经 base lock 保护共享标志（`set_initialized`/`set_mqtt_data_enabled`/`mqtt_data_is_enabled` 均持锁）。无持锁回调、无跨状态机持锁迁移。
- **D 失败路径**：`HTTP_CLEANUP` 宏（`:5357`）在所有失败分支发 `AT+HTTPTERM`；`url_cmd`/`ct_cmd` 在 `send_cmd` 后立即 free（`:5409/:5435`）；命令串 malloc 失败返回 NO_MEM 并 cleanup；`copy_mqtt_config` 部分克隆失败回滚。
- **E AT/Modem**：特殊成功终止（`CLOSE OK`、`SHUT OK`、`+HTTPACTION:`、`CONNECT OK`）均经 `send_cmd_with_options` + `at_cmd_success_match_t`，**未硬编码进 AT Engine**（契约正确）；`+CIPPING` 存储容量 `_Static_assert` 锁定；`AT+CIPRXGET=3` hex 解析有界。
- **G 类型与边界**：`snprintf` 截断逐处检查（`:5444/:5457` 等）；`strtoul/strtol` errno/ERANGE + `INT_MIN/MAX` 范围校验（`:6073`）；SSL bitmap `index>=256` 越界防护（`:2644/:2662`）；`modem_error_code` 输出指针可选 NULL 均判断。

---

## 备注

- 本模块工程质量高：ownership 转移、build-new-swap 配置、bounds 校验、失败路径清理均系统化处理。唯一实质功能缺陷为 HTTP body 静默截断（🟡）。
- 审查聚焦高风险路径，未逐行覆盖全部 6000+ 行（如部分查询型 ops get_info/get_signal/get_registration 的解析已抽样确认 bounds 安全，但未穷举）。
