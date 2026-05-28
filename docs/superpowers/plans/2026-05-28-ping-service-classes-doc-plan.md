# Ping Service Classes Doc Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update `docs/agents/classes.md` so it documents a lightweight user-callable Ping Service after MQTT, using Core command queue and Air780EP `AT+CIPPING` without adding a Ping FSM.

**Architecture:** App code calls `lwlte_ping()` through Facade. Facade delegates to internal `ping_client_ping()`, which synchronously submits `CORE_CMD_PING` and waits for Core FSM completion. Core remains the only service that calls `modem_ping()`, and Air780EP maps that operation to `AT+CIPPING`.

**Tech Stack:** Markdown documentation, ESP-IDF C naming conventions, FreeRTOS semaphore concepts, existing esp-lwlte C OOP documentation style, host-side Python static regression tests.

---

## File Structure

- Create: `tests/host/test_ping_classes_doc_contract.py` — static regression checks for the approved Ping Service `classes.md` contract.
- Modify: `docs/agents/classes.md` — canonical class design document; add Ping Service section, update Core command queue, Modem ops, visibility table, and App public API notes.
- Read-only source: `docs/superpowers/specs/2026-05-28-ping-service-design.md` — approved design source for this documentation update.
- Do not modify: `docs/agents/architecture.md` in this docs-only pass.
- Do not modify: `docs/agents/at_cmd_air780ep.md`; it already documents `AT+CIPPING` command semantics.

`docs/agents/classes.md` must keep its existing Chinese prose, table format, and section numbering style. This plan updates documentation only; it does not implement C source files.

---

### Task 1: Add Ping Classes Contract Test

**Files:**
- Create: `tests/host/test_ping_classes_doc_contract.py`

- [x] **Step 1: Write the failing static contract test**

Create `tests/host/test_ping_classes_doc_contract.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static regression checks for the Ping Service class documentation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
CLASSES_MD = ROOT / "docs/agents/classes.md"


class PingClassesDocContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.classes_md = CLASSES_MD.read_text(encoding="utf-8")

    def test_visibility_table_mentions_ping_service(self):
        for token in [
            "src/ping_client/ping_client.h",
            "ping_client_",
            "Ping Service",
            "AT Engine、Modem、Core、MQTT Client Service 和 Ping Service 都没有任何用户 API",
        ]:
            self.assertIn(token, self.classes_md)

    def test_modem_ping_contract_is_documented(self):
        for token in [
            "modem_ping_request_t",
            "modem_ping_reply_t",
            "modem_ping_summary_t",
            "esp_err_t modem_ping(modem_t *me,",
            "esp_err_t (*ping)(modem_t *me,",
            "| `ping` | 执行网络连通性诊断，不参与 Core online 条件 | `AT+CIPPING` |",
            "AT+CIPPING` 现在作为 `modem_ping()` 的 Air780EP 映射",
        ]:
            self.assertIn(token, self.classes_md)

    def test_core_ping_command_contract_is_documented(self):
        for token in [
            "core_ping_reply_t",
            "core_ping_summary_t",
            "CORE_CMD_PING",
            "core_ping_reply_t *replies;",
            "core_ping_summary_t *summary;",
            "`CORE_CMD_PING` 的 `host` 由 `core_submit_cmd()` 深拷贝",
            "`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer",
            "Core 网络未 online 时，`CORE_CMD_PING` 返回 `CORE_CMD_RESULT_ERROR`",
        ]:
            self.assertIn(token, self.classes_md)

    def test_ping_service_section_is_after_mqtt_before_app(self):
        mqtt_idx = self.classes_md.index("## 4. MQTT Client Service")
        ping_idx = self.classes_md.index("## 5. Ping Service")
        app_idx = self.classes_md.index("## 6. App")
        self.assertLess(mqtt_idx, ping_idx)
        self.assertLess(ping_idx, app_idx)

    def test_ping_service_is_lightweight_and_boundary_safe(self):
        for token in [
            "Ping Service 不创建自己的 FSM task、FSM queue 或 esp_event loop",
            "ping_client_create(core_t *core);",
            "ping_client_ping(ping_client_t *me,",
            "core_submit_cmd(CORE_CMD_PING)",
            "Ping Service 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h`",
            "不能把 `lwlte_ping_reply_t *` 强转成 `core_ping_reply_t *`",
        ]:
            self.assertIn(token, self.classes_md)

    def test_public_facade_ping_api_is_documented(self):
        for token in [
            "lwlte_ping_request_t",
            "lwlte_ping_reply_t",
            "lwlte_ping_summary_t",
            "esp_err_t lwlte_ping(lwlte_t *me,",
            "调用方负责传入 `lwlte_ping_reply_t` 数组",
            "第一版只实现同步阻塞 `lwlte_ping()`",
            "`lwlte_ping_async()`",
        ]:
            self.assertIn(token, self.classes_md)


if __name__ == "__main__":
    unittest.main()
```

- [x] **Step 2: Run the new test and verify it fails**

Run: `python3 -m unittest tests.host.test_ping_classes_doc_contract -v`

Expected: FAIL. The failures should mention missing `src/ping_client/ping_client.h`, missing `CORE_CMD_PING`, missing `## 5. Ping Service`, and missing public `lwlte_ping_*` documentation.

- [x] **Step 3: Check worktree diff for the test only**

Run: `git diff -- tests/host/test_ping_classes_doc_contract.py`

Expected: diff shows only the new Python test file above.

- [x] **Step 4: Commit only if explicitly approved for this execution**

If the user has explicitly approved commits for this execution, run:

```bash
git add tests/host/test_ping_classes_doc_contract.py
git commit -m "test: add ping classes doc contract"
```

Expected: commit succeeds and includes only `tests/host/test_ping_classes_doc_contract.py`.

If commits are not explicitly approved, skip this step and leave the file unstaged.

---

### Task 2: Update Visibility, Modem, And Core Ping Contract

**Files:**
- Modify: `docs/agents/classes.md:5-16`
- Modify: `docs/agents/classes.md:332-545`
- Modify: `docs/agents/classes.md:866-1105`

- [x] **Step 1: Update the visibility table row**

Replace the current layer API row in `docs/agents/classes.md` with this row:

```markdown
| 层间 API | `src/core/core.h`、`src/mqtt_client/mqtt_client.h`、`src/ping_client/ping_client.h`、`src/modem/modem.h`、`src/modem/modem_air780ep.h`、`src/at_engine/at_engine.h` | 组件内部相邻层；Facade factory 作为 composition root 可见全部装配 API | `core_`、`mqtt_client_`、`ping_client_`、`modem_`、`modem_air780ep_`、`at_engine_` |
```

- [x] **Step 2: Update the visibility explanation**

Replace the sentence that lists internal layers without user API with this sentence:

```markdown
**核心区别**：用户 API 是给 App 开发者用的，层间 API 是层与层之间、以及 Facade 模块 factory 装配时用的。AT Engine、Modem、Core、MQTT Client Service 和 Ping Service 都没有任何用户 API——它们被 LWLTE Facade 封装，最终用户看不到它们的存在。
```

- [x] **Step 3: Add Modem ping classes to the overview table**

In `### 2.1 类总览`, add these rows after `modem_mqtt_publish_t` and before `modem_event_id_t`:

```markdown
| `modem_ping_request_t` | 层间 API | Core + Modem 层 | 值对象 | Ping 请求参数，Core 执行 `CORE_CMD_PING` 时使用 |
| `modem_ping_reply_t` | 层间 API | Core + Modem 层 | 值对象 | 单次 ping reply 结果，包含序号、IP、耗时、TTL 和成功标志 |
| `modem_ping_summary_t` | 层间 API | Core + Modem 层 | 值对象 | Ping 汇总结果，包含 sent/received/lost/min/max/avg |
```

- [x] **Step 4: Add `modem_ping()` to the Modem method list**

In the `modem_t` layer method code block, add this prototype after `modem_mqtt_publish(...)`:

```c
esp_err_t modem_ping(modem_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary);
```

- [x] **Step 5: Add Modem ping value objects**

In `### 2.3 modem_ops_t`, replace the heading text `**MQTT 命令值对象**（`src/modem/modem.h`）：` with:

```markdown
**模块命令值对象**（`src/modem/modem.h`）：
```

Then add these definitions after `modem_mqtt_publish_t` in the code block:

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
} modem_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} modem_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} modem_ping_summary_t;
```

Replace the paragraph immediately after that code block with:

```markdown
这些值对象属于 Modem Adapter 的模块命令语义，不是上层 service API。Core 执行 `core_cmd_t` 时把 Core command 数据转换成这些值对象，再调用对应 `modem_*` 包装 API；MQTT Client Service 和 Ping Service 不 include `modem.h`，也不直接调用这些函数。
```

- [x] **Step 6: Add `ping` to `modem_ops_t`**

In the `modem_ops_t` code block, add this function pointer after `mqtt_publish`:

```c
    esp_err_t (*ping)(modem_t *me,
                      const modem_ping_request_t *request,
                      modem_ping_reply_t *replies,
                      size_t max_replies,
                      modem_ping_summary_t *summary);
```

- [x] **Step 7: Add Air780EP ping mapping to the ops table**

In the ops mapping table, add this row after `mqtt_publish`:

```markdown
| `ping` | 执行网络连通性诊断，不参与 Core online 条件 | `AT+CIPPING` |
```

Then replace the paragraph after the mapping table that currently includes `AT+CIPPING` as an internal helper with:

```markdown
MQTT command ops 和 ping ops 是 Modem Adapter 暴露给 Core 的模块语义能力，用于执行 Core command queue 中的上层 service 命令。它们不改变上层 service 的依赖方向：MQTT service 和 Ping Service 仍只调用 Core，不调用 Modem。

`AT+IPR`、`AT+IFC`、`AT&W` 属于板级串口/持久化配置，不进入 Modem ops。`AT+COPS?`、`AT^SYSINFO` 属于诊断或联网自检，第一版可作为 Air780EP 内部 helper，不先扩大 Core 可见 API。`AT+CIPPING` 现在作为 `modem_ping()` 的 Air780EP 映射暴露给 Core command queue，但它仍是用户触发的诊断命令，不参与 Core online 判定。`AT+CSCLK`、`AT+POWERMODE`、`AT+CFGRI` 等低功耗指令需要 Core 低功耗策略后再设计独立 ops。
```

- [x] **Step 8: Add Core ping classes to the Core overview table**

In `### 3.1 类总览`, add these rows after `core_cmd_result_t` and before `core_cmd_t`:

```markdown
| `core_ping_reply_t` | 层间 API | Ping Service + Core 内部 | 值对象 | `CORE_CMD_PING` 的单包结果，Core command callback 前写入 |
| `core_ping_summary_t` | 层间 API | Ping Service + Core 内部 | 值对象 | `CORE_CMD_PING` 的汇总结果，Core command callback 前写入 |
```

Also update the `core_cmd_type_t` through `core_cmd_done_callback_t` rows so their “被谁使用” column says:

```markdown
MQTT Client Service + Ping Service + Core 内部
```

- [x] **Step 9: Update the Core command queue introduction**

Replace the first paragraph under `### 3.5 Core command queue 类型` with:

```markdown
Core command queue 是本设计中上层 service 使用的 typed command 入口。MQTT Client Service 通过 `core_submit_cmd()` 投递 MQTT 模块命令；Ping Service 通过 `core_submit_cmd()` 投递轻量 `CORE_CMD_PING` 诊断命令。Core FSM 串行执行这些 command，执行时调用 `modem_*` API，并在命令完成后通过 callback 把结果交还给上层 service。MQTT 和 Ping 都不直接调用 Modem 或 AT Engine。TCP/HTTP 后续可以复用这个 command 边界，但本节不承诺它们的具体跨层边界。
```

- [x] **Step 10: Extend the Core command queue code block**

In the `core_cmd_type_t` snippet, add `CORE_CMD_PING` after `CORE_CMD_MQTT_PUBLISH`:

```c
    CORE_CMD_MQTT_PUBLISH,
    CORE_CMD_PING,
```

Add these Core ping result types after `core_cmd_result_t` and before `core_cmd_done_callback_t`:

```c
typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} core_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} core_ping_summary_t;
```

Add this `ping` union arm after `mqtt_publish`:

```c
        struct {
            const char *host;
            uint8_t count;
            uint16_t data_len;
            uint16_t timeout_100ms;
            uint8_t ttl;
            core_ping_reply_t *replies;
            size_t max_replies;
            core_ping_summary_t *summary;
        } ping;
```

- [x] **Step 11: Extend Core command queue decisions**

In the `关键设计决策` list under `### 3.5`, add these bullets after the existing deep-copy bullet:

```markdown
- `CORE_CMD_PING` 的 `host` 由 `core_submit_cmd()` 深拷贝；`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer，Core 只在 command 执行期间写入，不拥有其生命周期。
- `ping_client_ping()` 会等待 `CORE_CMD_PING` 完成后才返回，所以 `replies` 和 `summary` 在 `done_cb` 返回前有效。
- Core 网络未 online 时，`CORE_CMD_PING` 返回 `CORE_CMD_RESULT_ERROR`，上层 Ping Service 映射为 `ESP_ERR_INVALID_STATE`。
```

- [x] **Step 12: Update Core thread model to include Ping Service**

In `### 3.9 Core 线程模型`, add this block after the MQTT FSM task block:

```text
  Ping Service 同步调用
  └─ ping_client_ping()
       └─ core_submit_cmd(CORE_CMD_PING)
            └─ 深拷贝 host → CORE_SIG_SERVICE_CMD → fsm.queue
            └─ 等待一次性完成信号量
```

Replace the sentence after the diagram with:

```markdown
Core FSM 处理 `CORE_SIG_SERVICE_CMD` 时执行 Core-owned `core_cmd_t`，按命令调用对应 `modem_*` API，随后调用 `done_cb` 并释放 command。MQTT 的 `done_cb` 只投递 `MQTT_SIG_CORE_CMD_DONE`；Ping 的 `done_cb` 只写入同步等待上下文并释放一次性完成信号量。
```

- [x] **Step 13: Run the focused contract test and verify it still fails only on Ping Service/App section**

Run: `python3 -m unittest tests.host.test_ping_classes_doc_contract -v`

Expected: FAIL. The remaining failures should be for missing `## 5. Ping Service`, missing `## 6. App`, and missing public `lwlte_ping_*` documentation. The visibility, Modem, and Core token checks should pass.

- [x] **Step 14: Commit only if explicitly approved for this execution**

If the user has explicitly approved commits for this execution, run:

```bash
git add docs/agents/classes.md tests/host/test_ping_classes_doc_contract.py
git commit -m "docs: add ping command class boundaries"
```

Expected: commit succeeds and includes only `docs/agents/classes.md` plus the Ping contract test when Task 1 was not already committed.

If commits are not explicitly approved, skip this step and leave the files unstaged.

---

### Task 3: Insert Ping Service Section And Public API Notes

**Files:**
- Modify: `docs/agents/classes.md:1336-1716`

- [x] **Step 1: Insert the Ping Service section after MQTT Client Service**

Insert the following section immediately after the current MQTT boundary subsection and before the `---` separator that precedes App:

````markdown
---

## 5. Ping Service（Ping 诊断服务层）

Ping Service 是 Core 之上的轻量 service，负责把用户同步 `lwlte_ping()` 请求转换成 `CORE_CMD_PING`，并等待 Core command 完成后把详细 ping 结果返回给调用方。它不负责网络状态机、不参与 Core online 判定，也不维护长期连接状态。

Ping Service 不创建自己的 FSM task、FSM queue 或 esp_event loop。它只在一次 `ping_client_ping()` 调用期间创建短生命周期同步对象，提交 Core command，然后阻塞等待结果。Ping Service 运行期只能调用 Core 层间 API，不能 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。

### 5.1 类总览

| 类 | 可见性 | 被谁使用 | OOP 角色 | 说明 |
|----|--------|---------|---------|------|
| `ping_client_t` | 层间 API (opaque) | Facade | service 句柄 | Ping Service 实例，持有 Core 依赖和短生命周期同步逻辑 |
| `ping_client_request_t` | 层间 API | Facade + Ping Service | 值对象 | Ping 请求参数，Facade 从用户 `lwlte_ping_request_t` 映射而来 |
| `ping_wait_ctx_t` | 模块私有 API | Ping Service | 工作上下文 | 单次同步 ping 调用的完成信号量、结果码和 Core command result |

### 5.2 `ping_client_t` — Ping 服务句柄

**所属层**：Ping Service
**可见性**：层间 API (opaque) — Facade 持有句柄；struct 定义在 `src/ping_client/ping_client_priv.h` 或 `.c` 中
**OOP 角色**：轻量 service 对象，借用 Core 依赖

**层间方法**（`src/ping_client/ping_client.h`）：

```c
typedef struct ping_client ping_client_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} ping_client_request_t;

ping_client_t *ping_client_create(core_t *core);
esp_err_t ping_client_destroy(ping_client_t *me);
esp_err_t ping_client_ping(ping_client_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary);
```

**关键内部字段类别**：
- `core`：Facade factory 注入的 `core_t` 句柄，Ping Service 借用但不拥有生命周期。
- `lock`、`destroying`：保护短生命周期状态，销毁开始后拒绝新的 ping 调用。

**关键设计决策**：
- Ping Service 没有独立状态机；`ping_client_t` 不保存 connected/error 等长期状态。
- Ping Service 不保存 `modem_t *`、`at_engine_t *` 或具体模块句柄。
- `ping_client_ping()` 可以阻塞调用 task，直到 Core command 完成或总超时到达。
- `timeout_100ms == 0` 是无效参数；`total_timeout_ms == 0` 表示根据 `count * timeout_100ms * 100` 加命令开销派生默认总等待预算。

### 5.3 `ping_wait_ctx_t` — 同步等待上下文

**所属层**：Ping Service
**可见性**：模块私有 API
**OOP 角色**：单次调用工作上下文

```c
typedef struct {
    SemaphoreHandle_t done_sema;
    core_cmd_result_t core_result;
    esp_err_t esp_result;
    bool completed;
} ping_wait_ctx_t;
```

`ping_wait_ctx_t` 只在 `ping_client_ping()` 栈上或短生命周期堆对象中存在。Core command done callback 只写入该上下文并 `xSemaphoreGive(done_sema)`，不直接调用 Facade 或用户回调。

### 5.4 Core command queue 边界

Ping Service 只通过 `core_submit_cmd()` 投递 ping：

```text
Facade/App task
  └─ lwlte_ping()
       └─ ping_client_ping()
            ├─ 参数检查 + Ping/Core 类型映射
            ├─ 构造 CORE_CMD_PING
            ├─ core_submit_cmd(CORE_CMD_PING)
            ├─ xSemaphoreTake(done_sema, total_timeout)
            └─ Core done callback 写入结果后返回
```

`CORE_CMD_PING` 的 `host` 由 Core 深拷贝；`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer。Core 只在 command 执行期间写入这些 buffer，不释放它们。`ping_client_ping()` 必须等 Core command 完成后才返回，保证输出 buffer 在 `done_cb` 返回前有效。

Facade、Core 和 Modem 各自保留自己的层间值对象。实现时必须逐字段复制，不能把 `lwlte_ping_reply_t *` 强转成 `core_ping_reply_t *`，也不得把 `core_ping_reply_t *` 强转成 `modem_ping_reply_t *`。

### 5.5 Ping 操作流程

```text
lwlte_ping()
  └─ 检查 me/request/replies/max_replies
  └─ 映射 lwlte_ping_request_t -> ping_client_request_t
  └─ ping_client_ping()
       └─ 构造 core_cmd_t { .type = CORE_CMD_PING }
       └─ core_submit_cmd()
            └─ CORE_SIG_SERVICE_CMD 入 Core FSM queue
       └─ 等待 done_sema
            └─ Core FSM 调 modem_ping()
                 └─ Air780EP 发送 AT+CIPPING
                 └─ 解析多行 +CIPPING 结果
            └─ Core done callback 唤醒 Ping Service
  └─ 映射 core_ping_reply_t/core_ping_summary_t -> lwlte_ping_reply_t/lwlte_ping_summary_t
  └─ 返回 esp_err_t
```

`lwlte_ping()` 是同步阻塞 API，不应在时间敏感 callback 中调用。Ping command 与 MQTT command 共享 Core FSM 串行化，不能并发打断其他 AT 命令。

### 5.6 错误处理规则

- `host == NULL`、空字符串、`count == 0`、`count > 100`、`replies == NULL`、`max_replies < count` 返回 `ESP_ERR_INVALID_ARG`。
- `data_len > 1024`、`timeout_100ms` 不在 `1..600`、`ttl == 0` 返回 `ESP_ERR_INVALID_ARG`。
- Core 网络未 online 时返回 `ESP_ERR_INVALID_STATE`。
- `core_submit_cmd()` 入队失败返回 `ESP_FAIL`。
- 等待超过有效总超时返回 `ESP_ERR_TIMEOUT`；有效总超时是调用方非零 `total_timeout_ms` 或 `total_timeout_ms == 0` 时派生出的默认值。
- AT command 超时返回 `ESP_ERR_TIMEOUT`。
- AT 返回 `ERROR`、`+CME ERROR` 或 `+CMS ERROR` 时沿用 Modem 层标准错误映射。
- `+CIPPING:` 响应格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。
- 部分丢包不是 API 错误；丢包通过 `replies[].success == false` 表达，`summary` 非 NULL 时同时通过 `summary.lost` 表达。

### 5.7 与 Core / Modem / AT Engine 的边界

- Ping Service 可以调用 `core_submit_cmd()` 和必要的 Core 状态查询 API，因为 Core 是 Ping 的直接依赖。
- Ping Service 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h` 或其他模块的 `_priv.h`。
- Ping Service 不直接调用 `modem_ping()`、`at_engine_*` 或具体 Air780EP helper。
- Ping Service 不注册 AT Engine URC handler；`AT+CIPPING` 是命令响应，不是 Ping Service URC 数据路径。
- Core 仍只负责网络状态机、PDP、重连和命令串行化，不持有 Ping 业务状态机。
- Facade 是 composition root，负责创建 Ping Service，并把用户 `lwlte_ping()` 参数映射到内部 Ping Service。

### 5.8 后续异步预留

第一版只实现同步阻塞 `lwlte_ping()`。后续可以增加：

```c
esp_err_t lwlte_ping_async(lwlte_t *me,
                           const lwlte_ping_request_t *request,
                           void *user_ctx);
```

异步版本仍复用 `CORE_CMD_PING`，但需要 Ping Service 持有 heap-owned request/result，并定义取消和 destroy 语义。第一版不增加 `LWLTE_EVENT_PING_DONE` 或异步事件 ID。
````

- [x] **Step 2: Renumber App heading**

Change the App heading from:

```markdown
## 5. App（应用层）
```

to:

```markdown
## 6. App（应用层）
```

- [x] **Step 3: Add public Facade ping API notes to App section**

After the existing MQTT public API bullet list in the App section, add this paragraph and bullets:

````markdown
Ping 第一版会增加这些用户可见类型和函数：

- `lwlte_ping_request_t`：用户传入的 ping 请求，包含 host、count、data_len、timeout_100ms、ttl 和 total_timeout_ms。
- `lwlte_ping_reply_t`：单包 ping 结果，包含 seq、ip、time_ms、ttl 和 success。
- `lwlte_ping_summary_t`：ping 汇总结果，包含 sent、received、lost、min_time_ms、max_time_ms 和 avg_time_ms。
- `lwlte_ping()`：同步阻塞 Facade 用户 API，内部只调用 `ping_client_ping()`，不直接操作 Core command 或 Modem。

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} lwlte_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} lwlte_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} lwlte_ping_summary_t;

esp_err_t lwlte_ping(lwlte_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);
```

调用方负责传入 `lwlte_ping_reply_t` 数组，`max_replies` 必须大于等于 `request->count`；组件不分配也不释放该数组。`summary` 可以为 `NULL`。第一版只实现同步阻塞 `lwlte_ping()`；`lwlte_ping_async()` 只作为后续设计方向，不进入第一版用户 API。
````

- [x] **Step 4: Update the final App boundary sentence**

Replace the final sentence of the App section with:

```markdown
App 仍不 include `src/mqtt_client/mqtt_client.h`、`src/ping_client/ping_client.h`、`src/core/core.h`、`src/modem/modem.h` 或任何 `_priv.h`。
```

- [x] **Step 5: Run the focused contract test and verify it passes**

Run: `python3 -m unittest tests.host.test_ping_classes_doc_contract -v`

Expected: PASS. All six `PingClassesDocContractTest` tests pass.

- [x] **Step 6: Commit only if explicitly approved for this execution**

If the user has explicitly approved commits for this execution, run:

```bash
git add docs/agents/classes.md tests/host/test_ping_classes_doc_contract.py
git commit -m "docs: add lightweight ping service class design"
```

Expected: commit succeeds and includes only `docs/agents/classes.md` plus the Ping contract test when prior tasks were not already committed.

If commits are not explicitly approved, skip this step and leave the files unstaged.

---

### Task 4: Verify Documentation Contract And Scope

**Files:**
- Test: `tests/host/test_ping_classes_doc_contract.py`
- Verify: `docs/agents/classes.md`

- [x] **Step 1: Run the Ping documentation contract test**

Run: `python3 -m unittest tests.host.test_ping_classes_doc_contract -v`

Expected: PASS. Output includes `Ran 6 tests` and `OK`.

- [x] **Step 2: Run existing host contract tests that read `classes.md`**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract tests.host.test_net_mgr_activation_flow -v`

Expected: PASS. Existing MQTT and net manager contract tests still pass after App section renumbering and Core/Modem doc edits.

- [x] **Step 3: Verify required `classes.md` tokens with ripgrep**

Run: `rg "## 5\. Ping Service|CORE_CMD_PING|modem_ping\(|AT\+CIPPING|lwlte_ping\(" docs/agents/classes.md`

Expected: output contains all five patterns in `docs/agents/classes.md`.

- [x] **Step 4: Verify no incomplete markers were introduced**

Run: `rg "\x54\x42\x44|\x54\x4f\x44\x4f|\x3f\x3f" docs/agents/classes.md docs/superpowers/plans/2026-05-28-ping-service-classes-doc-plan.md tests/host/test_ping_classes_doc_contract.py`

Expected: no matches.

- [x] **Step 5: Review diff scope**

Run: `git diff -- docs/agents/classes.md tests/host/test_ping_classes_doc_contract.py`

Expected: diff only creates `tests/host/test_ping_classes_doc_contract.py` and updates `docs/agents/classes.md` with Ping Service, Core command, Modem ping, visibility, and App public API documentation.

- [x] **Step 6: Final commit only if explicitly approved for this execution**

If the user has explicitly approved commits for this execution and prior tasks were not committed, run:

```bash
git add docs/agents/classes.md tests/host/test_ping_classes_doc_contract.py
git commit -m "docs: document ping service class design"
```

Expected: commit succeeds and includes only the docs/test files from this plan.

If commits are not explicitly approved, skip this step and report the uncommitted files.
