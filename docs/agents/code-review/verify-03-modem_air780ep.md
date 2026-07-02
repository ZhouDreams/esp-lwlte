# Verification: Modem Air780EP Impl（#3）

对应报告：`report-03-modem_air780ep.md`

验证方法：`read` 重读关键路径上下文 + `rg` 交叉验证调用点 + AT 参考文档对照模块能力。

---

## ✅ 确认的问题

### 1. 🟡 HTTP 响应体静默截断（`modem_air780ep.c:5536-5596`）

**验证结论**：确认成立。

控制流追踪（`air780ep_http_request`）：
1. `:5514-5516` 从 `+HTTPACTION: <method>,<status>,<len>` 解析出 `data_len`（模块报告的**完整**响应体长度）。
2. `:5537-5540` `read_len = data_len; if (read_len > 3356) read_len = 3356;` —— 截断到 `AIR780EP_HTTPREAD_BODY_MAX`。
3. `:5543` `AT+HTTPREAD=0,%u` 只读 `read_len` 字节（offset 0，单次）。
4. `:5594-5595` `response->body = body_buf; response->body_len = copied;`（copied ≤ read_len ≤ 3356）。
5. 函数继续到 `:5604` `return ESP_OK;` —— **无论 data_len 多大，均返回成功，body_len 仅反映截断后长度，无任何标志**。

**模块能力对照**（`docs/agents/at_cmd_air780ep.md:209`）：`AT+HTTPREAD=<start_address>,<byte_size>`，`start_address=0..3356`、`byte_size=1..3356`。说明 Air780EP HTTP body 缓冲本身 ≈ 3356 上限，分片读取超出该窗口的能力不确定（不应依赖）。

**结论**：截断有"模块缓冲上限"的客观成分，但代码**掌握 `data_len` 却未用于截断检测**——这才是缺陷。与 POST 路径不对称（POST body 超 3356 返回 `ESP_ERR_INVALID_SIZE`，`:5448-5451`）。

**修复方向（需告知用户，因影响 HTTP API 对 App 的可见行为）**：
- 方案 A（推荐，与 POST 一致）：`data_len > AIR780EP_HTTPREAD_BODY_MAX` 时返回 `ESP_ERR_INVALID_RESPONSE`（或专用错误码），不返回截断 body。
- 方案 B：读满 3356 + 在日志/新增字段标记截断（需扩 `modem_http_response_t` 公共结构，影响面更大）。

---

## ✅ 确认（低 / 注记，非代码修复）

### 2. 🟢 `air780ep_cmd_ctx_t` 栈占用（`modem_air780ep.c:97`）

确认：`char *lines[101]` = 404 B + `at_response_t` ≈ 32 B，每命令帧 ≈ 436 B 栈。`AIR780EP_MAX_RESPONSE_LINES=101` 由 `_Static_assert`（`:85`）锁定（CIPPING 100 reply + 终态），不可调小。非缺陷，仅资源注记。

### 3. 🟢 HTTP body 重组 `+HTTPREAD:` 前缀跳过（`modem_air780ep.c:5580`）

确认：body 数据行若以 `"+HTTPREAD:"` 开头会被 `strncmp` 跳过。HTTP body 出现该前缀概率极低，理论边界风险，不单独修。

---

## 误报防范自检

- **destroy 泄漏疑虑（自查驳回）**：初判 `air780ep_destroy` 仅 `clear_mqtt_state` 可能不释放 mqtt 堆串。实际追踪：`clear_mqtt_state`→`air780ep_clear_ssl_state`（`:2995`）→`:3008 free_mqtt_config(&self->mqtt_config)` 释放 client_id/username/password/host。**无泄漏，驳回为缺陷**。
- 报告引用行号均经 `read`/`rg` 实核。
- 未把模块缓冲上限当纯代码缺陷夸大：已据 AT 文档区分"模块能力限制"与"代码未检测截断"。

---

## 修复记录

- **`src/modem/modem_air780ep.c:5536`** — HTTP 响应体超限改为显式失败
  - 改动：`air780ep_http_request` 在 `data_len > AIR780EP_HTTPREAD_BODY_MAX`（3356）时，原先静默截断并返回 `ESP_OK`；现改为 `ESP_LOGW` + `HTTP_CLEANUP()`（发 `AT+HTTPTERM`）+ 返回 `ESP_ERR_INVALID_RESPONSE`。删除已不可达的 `read_len` 截断块。
  - 行为影响（App 可见）：HTTP 响应体 > ~3.3 KB 时 `modem_http_request` 返回错误而非截断 body，与 POST 路径（`ESP_ERR_INVALID_SIZE`）对称。`modem_http_response_t.body` 失败时保持 NULL（base wrapper 已 `memset` 清零 + 本函数失败路径不写 body）。
  - 依据：Air780EP HTTPREAD 模块缓冲上限 ≈ 3356B（`docs/agents/at_cmd_air780ep.md:209`），分片超出能力不可靠，故选择显式失败而非循环读取。
  - 构建验证：✅ `idf.py build` 通过。

---

## 模块交付清单

- **Change summary**：1 处改动——`air780ep_http_request` 在模块报告的响应体长度超过 HTTPREAD 缓冲上限（3356B）时，由静默截断+`ESP_OK` 改为显式返回 `ESP_ERR_INVALID_RESPONSE` 并清理 HTTP 会话。
- **Resource budget**（Air780EP 子类单实例，叠加基类）：
  - 子类堆：`calloc(1, sizeof(modem_air780ep_t))` ≈ **3–4 KB**（`ssl_auth_modes[256]`=1024B、`pdp[4]`≈488B、`ssl_provisioned_bitmap[8]`=32B + base 字段）。
  - 命令栈：每命令帧 `air780ep_cmd_ctx_t` ≈ **436 B**（`lines[101]`=404B，CIPPING 所需，`_Static_assert` 锁定）+ AT Engine `send_cmd` 栈。Core FSM task 栈需预留 > 512B。
  - URC/数据路径：MQTT topic/payload、TCP hex payload、HTTP body 均**按实际长度 malloc**，无大块常驻池。
  - 配置项（`default_cmd_timeout_ms`/`ready_timeout_ms`/event 组）未被乘以更大上限，无爆裂。
- **Lifecycle / ownership notes**：
  - `self->base.at`：借用（Facade 拥有）。
  - `mqtt_config` 字符串：heap cloned（`copy_mqtt_config`），destroy 经 `clear_mqtt_state`→`air780ep_clear_ssl_state`→`free_mqtt_config` 释放。
  - MQTT/TCP 下行 topic/payload：URC handler heap 拷贝 → `modem_post_event` 成功转 Modem 拥有（event_task 回调后释放），失败/未启用时调用方释放。
  - HTTP body / socket recv payload：成功时调用方拥有（`modem.h` 契约），失败时 NULL。
- **Failure-path review**：`HTTP_CLEANUP` 宏在所有失败分支发 `AT+HTTPTERM`；命令串 malloc 即用即 free；`copy_mqtt_config` build-new-swap 部分失败回滚；`parse_msub_direct`/`decode_hex_payload` malloc 失败互释。
- **Cross-module contract review**：未破坏 Facade/Core/Modem/AT Engine 契约。HTTP body 超限行为变更（错误而非截断）为 App 可见，已按用户决策采纳；`modem_http_response_t` 公共结构未改。
- **Residual risks**：
  - HTTP 响应体硬上限 ~3356B（模块缓冲限制）；更大响应需上层分片或换传输方案。
  - `air780ep_cmd_ctx_t` 每帧 ~436B 栈占用，依赖 Core FSM task 栈足够（建议 `core.fsm.task_stack` 默认值体现该需求）。
  - HTTP body 重组跳过 `+HTTPREAD:` 前缀行的理论边界（概率极低，未修）。
  - 本次审查聚焦高风险路径，未逐行穷举全部 6000+ 行（查询型 ops 解析已抽样确认 bounds 安全）。

---

## 回归 Review（2026-07-02，针对 `4a0ae18` 新增代码）

本次 review 闭环后，作者提交 `4a0ae18 fix(modem): resolve HTTP request failures on ML307R and Air780EP` 给 Air780EP 新增了 `air780ep_http_ensure_bearer`（SAPBR GPRS 承载激活，`:5337-5407`），并在 `air780ep_http_request` 开头调用（`:5421-5425`）。该模块被改动，触发回归 review。

**回归审查结论**：新增代码 clean，无需修复。

- `air780ep_http_ensure_bearer`（`:5337`）：纯栈 `air780ep_cmd_ctx_t`，无堆分配/无泄漏。
- 流程幂等且自校验：设 CONTYPE/APN → `SAPBR=2,1` 查询，status==1 直接返回；未连接则 `SAPBR=1,1` 打开，**容忍"已激活"CME 错误**（`:5379-5381` 仅 DEBUG 日志）后**再用 `SAPBR=2,1` 验证 status==1**（`:5389-5404`），未盲信打开命令。超时用 `AIR780EP_SAPBR_OPEN_TIMEOUT_MS`(60s)。
- `+SAPBR: %d,%d` 双字段解析，无越界。

**低优先注记（非 bug，实机已验证）**：`:5354 AT+SAPBR=3,1,"APN",""` 设空 APN，HTTP 承载用网络默认/继承 APN，而非 Core 配置的 APN。用户网络（`ip.3322.net` 实测 200）可用；其他运营商若需显式 APN 可能需调整。归入残留观察项。

模块 #3 维持 ✅ Done（回归无新问题）。
