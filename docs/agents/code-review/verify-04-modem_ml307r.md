# Verification: Modem ML307R Impl（#4）

对应报告：`report-04-modem_ml307r.md`

> **重新评估（2026-07-02）**：本次 review 启动后，`4a0ae18 fix(modem): resolve HTTP request failures on ML307R and Air780EP` 重写了 `ml307r_http_request` 并经实机验证（`http://ip.3322.net/` 返回 HTTP 200 + 真实内容）。下方原 🟡 HTTP 发现基于**修复前代码**，现已全部解决，归类为 **❌ 误报（针对当前代码）/ 已解决**。其余结论不变。

验证方法：`read` 重读 HTTP 路径 + `rg` 交叉验证 SSL 索引校验 + AT 参考文档逐条对照 + 对照 `4a0ae18` 重写后的当前代码。

---

## ❌ 误报（针对当前代码）/ 已解决

### 原 🟡 HTTP 响应路径命令/响应格式不一致（`modem_ml307r.c` HTTP 路径）

**重新评估结论**：**已解决，当前代码正确且实机验证通过**。本发现基于 review 启动时的修复前代码，`4a0ae18` 已重写。

逐条对照当前代码（HEAD = `4a0ae18`）：

| 原报告问题（修复前） | 当前代码 | 状态 |
|---|---|---|
| `AT+MHTTPREAD=<id>,1024`（1024 当 type） | `:5519` `AT+MHTTPREAD=%d,1,%u`（type=1 content，再 len） | ✅ 解决 |
| 等待 `+MHTTPURC: "rsp"`（文档称 V6.1.1 未定义） | `:5454` 等待 `+MHTTPURC: "recv",%d` | ✅ 解决 |
| `+MHTTPREAD: %d` 单段解析 | `:5556-5563` 跳过 4 个逗号定位 data（5 段） | ✅ 解决 |
| realloc 无上限 | `:5569` 单次 `malloc(want)`，`want` 上限 `ML307R_HTTP_BODY_MAX`(4096)（`:5515-5516`） | ✅ 解决 |
| body 重组丢失 `\r\n` | `:5588-5594` 行间还原 `\r\n` | ✅ 解决 |
| `buf_cap=4096` 字面量 | 已无 realloc，单次 `malloc(want)` 用宏 | ✅ 解决 |

额外亮点：`:5525-5539` 当 body 可能超过标准 `ctx.lines[101]` 上限时（如 robots.txt ~140 行），单独堆分配更大 `body_lines` 数组，避免栈 ctx 溢出——设计周全。

**自检**：原 review 的"需上机验证、不盲改"定级本身是恰当的（未误判为确定 bug），但发现的对象（修复前代码）已被作者并行实机验证后重写。当前 HTTP 路径 clean，无残留问题。

---

## ✅ 确认（clean，驳回报告备注疑虑）

### 报告备注：SSL `ssl_auth_modes[index]` 越界 —— 驳回为缺陷

**验证结论**：SSL context_id 越界防护**完备**，非问题。

`rg` 全量核对，所有 `ssl_auth_modes[context_id]` / bitmap 访问前均校验 `context_id < ML307R_SSL_MAX_CONTEXTS`（6）：`:2632/:2666/:2822/:2896/:3072/:4285/:4529/:4665/:4675/:4704/:4718`。`ml307r_clear_ssl_state`（`:2688`）用 `sizeof/sizeof` 边界遍历。无越界可能。报告备注疑虑解除。

---

## ✅ 确认（低 / 注记）

### 🟢 `ml307r_cmd_ctx_t` 栈占用（`modem_ml307r.c:81`）

确认：每命令帧 `lines[101]` ≈ 404B + `at_response_t` ≈ 436B。`ML307R_MAX_RESPONSE_LINES=101` 由 `_Static_assert`（`:69`，MPING 所需）锁定，不可调小。HTTP body 读取已在 `:5525-5539` 用堆 `body_lines` 规避该上限。非缺陷，资源注记。

---

## 误报防范自检

- 未把"文档 vs 代码不一致"直接判为确定 bug：原定级为"需上机验证"，作者实机验证后重写（`4a0ae18`），证实修复前确有问题（commit message 自述"wrong command sequence"），但当前代码已解决。
- SSL 越界备注经全量 `rg` 核验后**主动驳回**为缺陷。
- create/destroy/copy_mqtt_config/URC ownership 等同 #3 已验证 clean 的模式，未重复造报告。

---

## 修复记录

- 本模块**无代码修复**：唯一 🟡 发现已被作者 `4a0ae18` 重写解决（实机验证）；低优先项（realloc/`\r\n`/宏统一）亦随重写一并解决。

---

## 模块交付清单

- **Change summary**：本模块审查未产生代码改动。原 HTTP 🟡 发现基于修复前代码，作者已通过 `4a0ae18`（实机验证）重写解决；SSL 越界备注驳回；其余维度 clean。
- **Resource budget**（ML307R 子类单实例）：
  - 子类堆：`calloc(sizeof(modem_ml307r_t))`，含 `ssl_auth_modes[6]`=24B、`pdp[1]`≈122B、`ssl_provisioned_bitmap`=1B + base，整体 < Air780EP。
  - 命令栈：每帧 `ml307r_cmd_ctx_t` ≈ 436B（`lines[101]`，MPING 所需）；HTTP body 读取用堆 `body_lines` 规避栈上限。
  - HTTP body：单次 `malloc(want)`，`want` 上限 `ML307R_HTTP_BODY_MAX`(4096)；大 body 时额外堆 `body_lines`（按 want/2+16 估算，用完即 free）。
- **Lifecycle / ownership notes**：同 #3——`at` 借用；`mqtt_config` heap cloned，destroy 经 `clear_mqtt_state`→`ml307r_clear_ssl_state`→`free_mqtt_config` 释放；HTTP body 成功时调用方拥有、失败 NULL。
- **Failure-path review**：`HTTP_CLEANUP_ML` 各分支发 `AT+MHTTPDEL`；命令串即用即 free；POST 超 4096 返回 `ESP_ERR_INVALID_SIZE`；`body_lines`/`body_buf` 失败路径均 free。
- **Cross-module contract review**：未破坏分层契约。
- **Residual risks**：
  - 无重大残留。`body_line_cap = want/2+16`（`:5526`）为启发式，极端全 1 字节行 body 可能略欠容量导致尾部截断（病理场景，实机正常内容已验证），不修。