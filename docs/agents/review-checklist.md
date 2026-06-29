# 代码 Review 检查清单与流程

本文档规定 esp-lwlte 项目的日常代码 review 流程与检查维度。功能开发阶段结束后，项目进入"日常 review 找 bug、优化代码"阶段，所有 review 工作必须遵循本文档。

本文档内容源自：
- 旧项目 `reference/esp-lwlte-old/docs/agents/code-review/` 的实战经验
- AT Engine 内存爆掉事件（`response_pool = max_response_lines * rx_line_buf_size` 在 ESP32-C3 上撑爆 heap）的教训
- 嵌入式 review 的两条主线：**内存泄漏** 与 **死锁**

---

## 一、Review 流程（必须按顺序执行）

Review 不是"让 AI 直接改 bug"，而是一条**先理解、再列单、再验证、最后才动手**的流水线。任何阶段都不得跳步。

### 阶段 0：前置理解（每次会话开始时）

在找任何 bug 之前，reviewer 必须先建立对项目的准确理解：

1. 读 `AGENTS.md` 与 `docs/agents/` 下的架构、目录、类设计文档。
2. 搞清楚分层契约：**门面层（Facade）→ Core Service → Modem 适配层 → AT Engine**，以及 `tcp_client` 等内部 service 的边界。
3. 搞清楚每层数据的生命周期：response line、event payload、protocol data 是 borrowed 还是 owned。

> 不理解调用链就找 bug，等于在猜。

### 阶段 1：建立 / 更新审查清单

1. 扫描 `src/` 下所有源文件，按功能模块分组，每组的粒度是 **1–3 个密切相关的源文件**。
2. 维护 `docs/agents/code-review/review-list.md`（格式见下文"模板"一节），每个模块一行，标注状态：`⬜ Pending` / `✅ Done` / `⏭️ Skipped`。
3. 单次审查范围既不能过大（一整层），也不能过小（单个函数）。一个模块一次审完。
4. 已审过的模块在后续迭代中如果被改动，需重新标记为 `⬜ Pending`（回归 review）。

### 阶段 2：逐模块审查（只产出报告，不动代码）

对清单中第一个 `⬜ Pending` 的模块，按"二、Review 维度"逐条检查，把发现写入 `docs/agents/code-review/report-<编号>-<模块名>.md`：

1. 每条发现必须带 **文件:行号**。
2. 按严重度分三档：🔴 高 / 🟡 中 / 🟢 低。
3. **本阶段禁止修改源代码**——审查与修复必须分离，避免边找边改导致漏审。
4. 审完更新 `review-list.md` 状态为 `✅ Done`，并填入报告文件名。
5. 输出本次审查摘要（问题数 + 最严重的 1–2 个发现）。

### 阶段 3：验证（绝不盲信报告）

这是最容易省略、但最关键的一步。审查报告会出错，reviewer 也会编造调用点或误判语义。修复前必须对每条发现**独立验证**：

1. 打开对应源码，读上下文**至少前后 30 行**。
2. 追踪调用链、条件分支、已有防护逻辑，确认问题是否真实存在。
3. 按三类输出验证结论，写入 `docs/agents/code-review/verify-<对应报告名>.md`：
   - ✅ **确认的问题**：简述确认理由。
   - ❌ **误报**：说明为什么不是真问题（参见"三、误报防范"）。
   - ⚠️ **部分正确**：真正的问题是什么，修复方案应如何调整。
4. 验证完成前，不得开始修复。

### 阶段 4：修复（按严重度，验证一项修一项）

1. 只修复阶段 3 中 ✅ 确认的问题，**不修误报**。
2. 按严重度从高到低逐个修复，每个修复：
   - 先用 1–2 句说明修复思路；
   - 实施修改；
   - 运行 `idf.py build` 确认编译通过（构建/烧录必须遵循 [build-and-debug.md](build-and-debug.md)）。
3. 🟢 低严重度问题修复前先问用户，由用户决定是否修。
4. 某个修复若影响其他模块，**先告知风险再动手**。
5. 每个修复完成后，在对应 `verify-*.md` 末尾追加"修复记录"。

### 阶段 5：模块收尾交付清单

每个模块 review + 修复闭环后，必须能回答以下六项（写进 verify 文件末尾）。**写不出 Resource budget 就不算 review 完成**：

- **Change summary**：本次改动概述。
- **Resource budget**：启动 heap / 运行 heap / 峰值 heap / task stack / queue size / buffer size，所有 `count*size` 显式计算。
- **Lifecycle / ownership notes**：关键数据 borrowed vs owned 标注。
- **Failure-path review**：malloc / queue / event / AT timeout 的失败路径是否完备。
- **Cross-module contract review**：是否破坏门面/Core/Modem/AT Engine 之间的契约。
- **Residual risks**：已知但未解决的问题、上机才可能暴露的风险。

---

## 二、Review 维度（找什么问题）

### A. 资源账本与乘法型分配（最高优先级）

本项目踩过最深的坑：一个配置项被跨模块复用后，在底层被乘以一个上限值，启动时一次性撑爆 heap。所有发现都要优先往这条线上靠。

- **所有 `count * size` 的乘法型占用必须显式计算**：`max_response_lines * rx_line_buf_size`、`queue_len * payload_size`、`max_lines * line_size`、`conn_count * buffer_size`。
- **一次性预分配审查**：`calloc(max_count, max_size)`、二维静态数组、大 payload 队列——问"所有槽位真的都需要按最大值常驻吗？"优先采用"指针槽位常驻 + 内容按需 malloc"。
- **配置传播半径**：新增或调大配置项时，追踪它在哪些模块被使用——是单个 buffer 大小，还是被当成池中每个元素的大小？默认值变化是否改变启动 footprint？
- **超过几十 KB 的一次性分配必须解释理由**。

### B. 内存安全与生命周期（嵌入式主线之一）

- **指针偏移前的长度校验**：`remaining = len - N` 类计算，当 `len < N` 时 uint 下溢成巨大值，使后续 `<` 检查失效，导致越界读。
- **VLA / 大块栈分配**：`char buf[config_value]` 在受限任务栈上极易栈溢出，且 VLA 无错误恢复机制。改用堆或固定小 buffer。
- **失败路径的内存泄漏**：`fsm_post_sig` / queue send / event post 失败后，已分配的 `sig` / payload 是否被释放。对照同模块其他路径（如 blocking vs async）看是否一致释放。
- **ownership 与 borrowed/owned**：调用方是否可能在下一次命令后仍持有旧指针？
- **半初始化失败的反序销毁**：init 链路中途失败，已初始化的子系统是否按反序清理。

### C. 并发、竞态与死锁（嵌入式主线之二）

- **死锁**：锁/信号量的获取顺序、跨状态机迁移时持锁、回调查调持锁。
- **竞态**：共享状态（如 core context）的读写保护、URC 任务与主任务的访问顺序。
- **重入安全**：callback 是否可能在持锁状态下被递归触发。
- **被忽略的返回值**：`fsm_post_sig` 失败仅记日志就 `return OK`，会欺骗调用方并泄漏资源。

### D. 失败路径完整性

- `malloc` 失败后对象是否仍可继续使用、是否设 `ESP_ERR_NO_MEM` 并 flush RX。
- AT 命令失败/超时后 RX 是否清理，旧数据是否污染下一条命令。
- queue/event send 失败后 payload 的 ownership 归属。
- **`abort()` 类宏是否被用在可恢复的 init 路径里**（旧项目真实案例：URC 表填满后 `LWLTE_ERROR_CHECK` 直接 abort 整个程序）。

### E. AT / Modem 特有风险

- AT 命令只能在哪些状态下发送；状态机迁移与时序。
- **URC 与同步响应交错**；timeout 后旧响应污染下一条命令。
- 特殊成功行是否需要 custom success match；模块私有响应（TCPIP/MQTT/SSL）不要硬塞通用 AT Engine。
- 配置顺序依赖（如 CIPMUX/CIPMODE 必须在 CSTT/CIICR 之前）。

### F. 跨模块契约

- 门面层 / Core / Modem / AT Engine 之间的数据生命周期是否清楚、是否有 host contract 锁定关键契约。
- 配置项被多个模块复用时的语义是否一致（A 模块改默认值会不会撑爆 B 模块的池）。

### G. 类型与边界

- 类型不一致导致的静默截断（`uint32_t` → `uint16_t`、`int` vs `uint32_t` 的 size 字段）。
- 死代码、不可达分支、重复赋值。
- 魔数应提为命名常量（尤其长度偏移这类与协议耦合的值）。

### H. 代码质量与一致性（低严重度但低成本）

- 命名一致性、误导性函数名（如 `_act` 后缀实际处理全部类型）。
- 与 [coding-style.md](coding-style.md) 的一致性：双语注释格式、section 组织（DEFINES vs MACROS）、include 风格。
- 传递性头文件依赖（宏用了 `LOGE` 但头文件未 include，靠间接依赖才编译过）。
- 废弃别名有无编译器级 deprecation 警告。

---

## 三、误报防范（什么不该报）

验证阶段驳回的典型误报，提示 reviewer 守住技术严谨：

- **unsigned 回绕运算被误判为 bug**：`elapsed = now - start` 在 uint 模运算下对回绕是正确的，是特性不是缺陷。
- **把设计意图当缺陷**：如 fire-and-forget 语义的"首条响应即完成"被误读为漏洞。先确认接口语义文档，再下结论。
- **报告里编造调用点**：声称某宏在某行使用，实际该行用的是另一个宏。验证时必须实际搜索调用点。
- **"用户可能移除 include"等不现实场景**不应作为设计考量。
- **空壳函数**（如无堆分配时仍加空 `deinit`）属风格偏好，非缺陷。

---

## 四、模板

### 4.1 review-list.md

```markdown
# Code Review List

| # | Module | Path | Status | Report |
|---|--------|------|--------|--------|
| 1 | AT Engine | src/at_engine/*.c, *.h | ⬜ Pending | — |
| 2 | Core Service | src/core/*.c, *.h | ⬜ Pending | — |
```

### 4.2 report-<编号>-<模块名>.md

```markdown
# Code Review: <模块名>

**日期**: <今天>
**文件**: <文件列表>

## 🔴 高严重度
- **文件:行号** — 问题描述
  - 建议修复：...

## 🟡 中严重度
...

## 🟢 低严重度
...

## 无问题维度
- 列出无显著问题的维度
```

### 4.3 verify-<对应报告名>.md

```markdown
# Verification: <对应报告的模块名>

## ✅ 确认的问题
- **原报告条目**: ...
  - 验证结论: 简述确认理由

## ❌ 误报
- **原报告条目**: ...
  - 驳回理由: 说明为什么这不是真正的问题

## ⚠️ 部分正确（需调整修复方案）
- **原报告条目**: ...
  - 调整说明: 真正的问题是什么，应如何修正

## 修复记录
- **文件:行号** — 修复内容简述
  - 改动: ...
  - 构建验证: ✅ / ❌

## 模块交付清单
- Change summary: ...
- Resource budget: ...
- Lifecycle / ownership notes: ...
- Failure-path review: ...
- Cross-module contract review: ...
- Residual risks: ...
```
