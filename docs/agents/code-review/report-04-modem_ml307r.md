# Code Review: Modem ML307R Impl（#4）

**日期**: 2026-07-01（2026-07-02 重新评估）
**文件**: `src/modem/modem_ml307r.c`（6739 行）、`src/modem/modem_ml307r.h`

> **重新评估（2026-07-02）**：本次 review 启动后，作者提交 `4a0ae18`（实机验证）重写了 `ml307r_http_request`。下方 🟡 HTTP 发现基于**修复前代码**，现已被重写解决（详见 `verify-04-modem_ml307r.md` 重新评估节）。当前 HTTP 路径 clean。
**审查范围**: ML307R 子类——生命周期、URC 翻译、MQTT/socket/HTTP/ping 命令、SSL（`MSSLCFG`，6 context）、AT 解析。结构与 Air780EP（#3）同源，重点核验差异点与 ML307R 专属命令族（`MIP*`/`MHTTP*`/`MQTT*`/`MPING`）。
**审查聚焦**：内存 ownership/泄漏、资源账本、HTTP body 路径、命令/响应格式与 AT 参考文档一致性。

---

## 🔴 高严重度

无。

---

## 🟡 中严重度

- **`src/modem/modem_ml307r.c:5410-5432`（+ `:5343-5397`）** — HTTP 响应路径命令/响应格式与 AT 参考文档不一致，**需上机验证**。
  - **命令格式**：`:5412` 构造 `AT+MHTTPREAD=%d,%u`（→ `AT+MHTTPREAD=<httpid>,1024`）。但 `docs/agents/at_cmd_ml307r.md:202` 规定 `AT+MHTTPREAD=<httpid>,<type>[,<read_len>]`，第二参数为 `<type>`（0=header / 1=content），`<read_len>` 是第三参数。代码把 1024 放在 type 槽（type=1024 非法），疑似漏写 type。
  - **响应解析**：`:5429` `sscanf(read_hdr, "+MHTTPREAD: %d", &read_len)` 只取首个数字；文档规定带 read_len 的响应为 `+MHTTPREAD: <httpid>,<type>,<unread_length>,<data_len>,<data>`（5 段），首数为 `<httpid>` 而非 `<data_len>`。
  - **请求完成 URC**：`:5353-5354` 等待 `+MHTTPURC: "rsp",<id>`；但文档 `:201` 明确"V6.1.1 未定义 "rsp" URC"，缓存模式应为 `+MHTTPURC: "recv",<id>,<code>,<recv_header_len>,<recv_content_len>`（`:222`）。
  - **不确定性的客观背景**：AT 文档自身多处标注"以实测为准"（`:215`）、版本差异（`:201` V6.1.1 未定义）。因此无法仅凭文档断定代码必然错误——也可能代码匹配实际固件、文档描述另一版本。
  - **结论与建议**：不臆断为确定 bug（遵循误报防范）。但若固件遵循文档，则 ML307R HTTP body 读取逻辑（命令 type、响应解析、完成 URC）整体不可用。HTTP 是最后合入的功能（`429e8d1 feat: add HTTP/HTTPS request service`），很可能尚未上机验证。**强烈建议上机实测**：GET 一个已知 body 的 URL，确认 `response->body`/`body_len` 正确；若失败，按文档修正 type 参数（`AT+MHTTPREAD=<id>,1,<len>`）、5 段响应解析、完成 URC（`"recv"`）。

---

## 🟢 低严重度

- **`src/modem/modem_ml307r.c:5404-5453`** — HTTP body 读取用 `realloc` 倍增，**无显式上限**。
  - `buf_cap` 起始 4096，循环读取时 `realloc` 翻倍增长（`:5441-5452`）。ML307R 与 Air780EP 不同：ML307R **循环分片读取**（无截断，优于 Air780EP），但 body 越大 heap 占用越高。OOM 由 `realloc` 失败路径兜底（`:5446-5449` free+返回 NO_MEM），不会内存破坏，但在受限 heap 上可能先吃掉大量 RAM。建议加显式上限（如 `ML307R_HTTP_BODY_MAX` 或独立 READ 上限）。
  - 附加小瑕疵：`:5404 buf_cap = 4096` 用字面量而非 `ML307R_HTTP_BODY_MAX` 宏（宏在 `:5313` POST 路径已用），建议统一。

- **`src/modem/modem_ml307r.c:5434-5456`** — HTTP body 重组直接拼接数据行，**未在行间补 `\r\n`**（与 Air780EP `:5586-5589` 插入 `\r\n` 不同）。若 ML307R MHTTPREAD 输出会被 AT 行解析器按 `\r\n` 拆行，则 body 内的 `\r\n` 分隔符会丢失。是否为缺陷取决于模块返回格式（hex/raw/按长度）——与上述 HTTP 路径一并需上机确认。

- **`src/modem/modem_ml307r.c:81`** — `ml307r_cmd_ctx_t` 每命令帧栈占用 ≈ **404 B**（`lines[101]`，MPING 100 reply 所需，`_Static_assert :69` 锁定）。同 #3，资源注记非 bug。

---

## 无问题维度

- **A 资源账本**：单实例 `calloc(sizeof(modem_ml307r_t))`（`ssl_auth_modes[6]`=24B、`pdp[1]`≈122B、`ssl_provisioned_bitmap`=1B + base），整体远小于 Air780EP；栈 ctx 同 #3。配置项未被乘以更大上限，无爆裂。
- **B 内存安全与生命周期**：
  - `copy_mqtt_config`（`:2594` 区域）build-new-swap，部分克隆失败回滚（`:2599`）。
  - **destroy 无泄漏**：`ml307r_destroy`→`clear_mqtt_state`→`ml307r_clear_ssl_state`（`:2686`）→`free_mqtt_config` 释放 mqtt 堆串。已逐链验证。
  - MQTT URC heap topic/payload 经 `post_mqtt_data_event`→`modem_post_event`：成功转 ownership，失败/未启用时调用方释放（`handle_mqtturc`/free at `:6194-6203`）。同 #3 模式，无 double-free。
  - `"IPV4V6"`（6+NUL）写入 `pdp_type[MODEM_PDP_TYPE_MAX_LEN=8]`（`:1538`）—— 安全（Air780EP 用 "IP"，均不溢出）。
- **C 并发**：URC 在 AT RX task 做有界解析+非阻塞 post；命令 ops 在 Core FSM task；共享标志经 base lock。同 #3，无死锁/持锁回调。
- **D 失败路径**：`HTTP_CLEANUP_ML` 宏（`:5235`）在各失败分支发 `AT+MHTTPDEL`/`AT+MHTTPTERM`；命令串即用即 free；POST body 超 4096 返回 `ESP_ERR_INVALID_SIZE`（`:5313`，与 Air780EP POST 对称）。
- **E AT/Modem**：`ML307R_SSL_MAX_CONTEXTS=6`、`MQTT_MAX_PAYLOAD_LEN=1024` 等常量与模块能力自洽；MPING 存储容量 `_Static_assert` 锁定。**唯一契约疑点即上述 HTTP 命令/响应格式**（需实测）。
- **G 类型与边界**：`snprintf` 截断逐处检查；SSL context_id 经 `ssl_auth_modes[6]` 越界防护（需确认 index<6 校验，见备注）；`realloc` 失败兜底。

---

## 备注

- 本模块为 Air780EP（#3）的同源兄弟，已验证 clean 的模式（build-new-swap、destroy 释放链、URC ownership、cmd_ctx、bounds 校验）在此同样成立，未重复展开。
- **HTTP 路径是本模块最大不确定性**：代码与 AT 参考文档在命令格式/响应解析/完成 URC 三处不一致，且文档自身标注版本差异与"以实测为准"。建议优先上机验证 HTTP body 读取，再决定是否修复（修复方案取决于实际固件行为）。
- SSL context_id → `ssl_auth_modes[index]` 的越界校验未在本次抽样中逐一确认（`ML307R_SSL_MAX_CONTEXTS=6`，若调用方传入 context_id≥6 而无校验会越界）——建议在修复 HTTP 时一并复核 SSL 索引校验。
