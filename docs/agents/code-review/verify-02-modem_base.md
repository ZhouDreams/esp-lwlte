# Verification: Modem Base / Wrapper（#2）

对应报告：`report-02-modem_base.md`

验证方法：重读 `modem.c` 全文上下文 + `rg` 交叉验证调用点 + 子类实现可达性确认。绝不盲信报告推理。

---

## ✅ 确认的问题

### 1. 🟡 destroy 回滚后状态恢复为 READY，但 event_task 已死（`modem.c:326-337`）

**验证结论**：确认成立。

逐行追踪控制流（`modem.c:295-347`）：
1. `:319` `modem_base_stop_event_task(me)` 返回 OK，其内部 `:250` 已执行 `me->event_task = NULL` 并通过 done_sema 等待 task 真正退出。此时 event_task 恒为 NULL。
2. `:326` 调用子类 `ops->destroy(me)`；若返回非 OK 进入 `:327` 分支。
3. `:330-334` 回滚：`destroying=false`；`:331-332` 三元把 `state` 恢复为原值（若 destroy 从 READY 发起 → 恢复为 READY，因 `READY(2) <= ERROR(6)` 条件成立）；`event_task_stop_requested=false`。
4. `event_task` 复活路径排查：`rg "event_task\s*=" src/modem/modem.c` 仅命中 `:126`（init 初值）、`:225`（deinit 清零）、`:250`（stop 清零）。**唯一的非 NULL 赋值是 `modem_base_init:167` 的 `xTaskCreate`，回滚后不会再被调用**。故 event_task 永久为 NULL。

降级态后果（已逐条对源码确认）：
- `modem_post_event`（`:262-266`）：`!me->event_task` 为真 → 所有事件返回 `ESP_ERR_INVALID_STATE`，URC/协议数据无法上报 Core。
- `check_ready`（`:905-913`）：READY 在允许集合内 → 返回 OK，`modem_*` 命令路径可继续调用 ops。
- 净效果：对象"看起来可用、命令能发、事件哑火"，且无法自愈。

**可达性确认**：
- `air780ep_destroy`（`modem_air780ep.c:3551-3556`）：`self->urc_registered` 时调 `air780ep_unregister_urcs`，失败即 `return ret`。
- `air780ep_unregister_urcs`（`modem_air780ep.c:5781+`）循环调 `at_engine_unregister_urc`（`:5806`），任一失败则返回错误。
- 触发概率：destroy 期间 AT Engine 仍存活（AT Engine 由 Facade 拥有、独立释放），故正常情况下注销成功；仅在 AT Engine 异常态下命中，属非常规但可达的错误路径。
- 影响面：`modem_destroy` 唯一调用点 `src/lwlte/lwlte.c:1367`（Facade 销毁链）。

**修复方向**：回滚时不应恢复到"活跃"状态。将 `:331-332` 改为无条件 `me->state = MODEM_STATE_ERROR;`（ERROR 仍被 destroy 白名单 `:310` 允许以支持重试；但被 `check_ready` `:908-913` 拒绝，防止误用）。`destroying=false` 保留以便重试 destroy。

---

## ⚠️ 部分正确（需调整修复方案）

### 2. 🟢 `modem_mqtt_publish` 拒绝零长度 payload（`modem.c:666-667`）

**验证结论**：限制客观存在（`publish->payload && publish->payload_len > 0`），但**是否为缺陷取决于模块能力与项目意图**，不能直接定论为 bug。

- MQTT 协议层：零长度 payload 合法（retain-only、遗嘱、心跳消息）。
- 模块层：Air780EP `AT+MPUBEX` 需要显式长度，零长度是否被模块接受需在模块 #3/#4 review 时确认 `air780ep_mqtt_publish` 的实际 AT 构造。
- 调整建议：暂记为低优先功能限制，待模块 #3 review 交叉确认后再决定是否放宽校验。**修复前问用户**（checklist 阶段 4 第 3 条）。

### 3. 🟢 `modem_destroy` 白名单排除 INITIALIZING（`modem.c:304-310`）——非 bug，契约注记

**验证结论**：当前**不是 bug**（驳回为缺陷），但作为隐式跨模块契约风险保留。

- 子类 `start()` 在所有路径均退出 INITIALIZING：
  - 成功：`air780ep.c:3633` / `ml307r.c:3347` → READY。
  - 失败（err 标签）：`air780ep.c:3646` / `ml307r.c:3637` → ERROR（`(void)` 忽略 set_state 返回，但此时 destroying=false 且 state 合法，set_state 不会失败）。
- 故运行期不会出现"卡在 INITIALIZING 的 modem"，destroy 不会因此返 INVALID_STATE。
- 残留风险：这是**隐式契约**——未来任何子类若在 start 失败时停留在 INITIALIZING，该 modem 将无法 destroy（泄漏）。建议在 `modem_state_t` 或 `modem_destroy` 文档显式声明"start() 必须在所有退出路径上离开 INITIALIZING"，或将 INITIALIZING 纳入 destroy 白名单作防御。

---

## ✅ 确认的问题（低 / 质量）

### 4. 🟢 包装函数 lock 校验风格不一致（`modem.c:351` vs `:363` 等）

**验证结论**：确认，纯风格问题，行为一致。

- 一类：`modem_destroy:297` / `modem_stop:363` / `modem_get_state:433` / `modem_post_event:258` / `modem_set_state:280` / `modem_register_event_callback:392` 用 `ESP_RETURN_ON_FALSE(me && me->lock, ...)`。
- 另一类：`modem_start:351` / `modem_reset:378` / `modem_activate_pdp:517` / `modem_deactivate_pdp:529` / `modem_mqtt_*` / `modem_socket_*` 等仅 `ESP_RETURN_ON_FALSE(me, ...)`，依赖后续 `check_ready():895` 的 `me && me->lock` 兜底。
- 两类结果等价（第二类最终也会校验 lock），仅为写法不统一。

---

## 误报防范自检

- 报告中未编造调用点：所有引用行号均经 `read`/`rg` 实际核对。
- 未把设计意图当缺陷：回调注销同步、PROTOCOL_DATA ownership、stop 顺序均经详细追踪确认为正确实现，已列入报告"无问题维度"。
- INITIALIZING 排除经子类代码确认为当前非 bug，仅保留契约注记，未夸大为缺陷。

---

## 修复记录

- **`src/modem/modem.c:331-332`** — destroy 回滚状态恢复改为无条件 `MODEM_STATE_ERROR`
  - 改动：子类 `ops->destroy` 失败的回滚分支原先把 `state` 恢复为原值（可能是 READY/PDP_ACTIVE 等活跃态），但此时 event_task 已死、事件无法上报。改为强制置 ERROR：被 `check_ready`（`:908-913`）拒绝以防止误用，仍被 `modem_destroy` 白名单（`:310`）允许以支持重试 destroy。`destroying=false` 保留。
  - 跨模块影响：`modem_destroy` 唯一调用点 `lwlte.c:1366-1373` 在 destroy 失败时仅记日志并 return 错误，不继续使用 modem，故无负面影响。
  - 构建验证：✅ `idf.py build` 通过。

- **`src/modem/modem.c` 全部公共包装 API** — 统一入口 lock 校验风格（🟢 低）
  - 改动：原先部分包装函数用 `ESP_RETURN_ON_FALSE(me && me->lock, ...)`、部分仅 `ESP_RETURN_ON_FALSE(me, ...)` 再依赖 `check_ready()` 兜底。统一为所有 `modem_*` 公共包装在入口即校验 `me && me->lock`（与直管 lock 的 `modem_stop`/`modem_get_state` 等一致）。`modem_base_init`（lock 尚未创建）与 static helper `call_no_arg`/`check_ready` 保持原样。
  - 行为等价（check_ready 本就校验 lock），纯风格统一，无功能变化。
  - 构建验证：✅ `idf.py build` 通过。

- **`src/modem/modem.h`** — INITIALIZING 销毁契约防御文档（🟢 低）
  - 改动：`modem_destroy` 文档增 `@note` 声明允许/不允许的销毁状态，并显式约定"子类 `start()` 必须在所有退出路径离开 INITIALIZING（成功→READY，失败→ERROR），否则 destroy 返回 `ESP_ERR_INVALID_STATE` 致对象无法销毁"；`MODEM_STATE_INITIALIZING` 枚举注释标注为瞬态。
  - 当前非 bug（两子类均合规），属面向未来子类的防御性契约文档。
  - 构建验证：✅ 注释改动，`idf.py build` 通过。

---

## 模块交付清单

- **Change summary**：3 处改动——(1) `modem_destroy` 子类 destroy 失败回滚时强制置 `MODEM_STATE_ERROR`（避免 event_task 已死后对象仍呈现活跃态）；(2) 全部 `modem_*` 公共包装统一入口 `me && me->lock` 校验；(3) `modem.h` 补 INITIALIZING 销毁契约防御文档。
- **Resource budget**（基类单实例，默认配置）：
  - event queue：`event_queue_size(8) × sizeof(modem_event_t)(≈128B)` ≈ **1 KB**（唯一乘法型分配，无跨模块放大）。
  - event_task 栈：**4096 B**（`MODEM_DEFAULT_EVENT_TASK_STACK`，可配）。
  - 同步原语：lock + event_task_done_sema + event_cb_done_sema，3 个 FreeRTOS 对象 ≈ **240 B**。
  - `struct modem_t` ≈ **60 B**。
  - 启动 heap footprint ≈ **5.4 KB**；无大块一次性分配；配置项 `event_queue_size`/`event_task_stack` 仅本层使用，未被跨模块乘以上限。
- **Lifecycle / ownership notes**：
  - `me->at`：借用（Facade 拥有，Modem 不 free，destroy 前注销自己注册的 URC）。
  - PROTOCOL_DATA 的 `topic`/`payload`：heap-owned，`modem_post_event` 成功→Modem 拥有（event_task 回调返回后 / drain 时由 `release_event_payload` 释放）；失败→调用方拥有。
  - `modem_event_t` 经 `xQueueSend` 按值拷贝入队，队中拷贝由 event_task 消费后释放，无 borrowed 残留。
- **Failure-path review**：
  - `modem_base_init` 半初始化失败 → `err:` 调 `modem_base_deinit` 反序清理（NULL 防御完备）。
  - `modem_post_event` 队列满 → `ESP_ERR_TIMEOUT`，调用方保留 payload ownership。
  - destroy 子类失败 → 本次修复后置 ERROR（防误用）+ `destroying=false`（允许重试）。
  - 各输出结构体（result/status/response）ops 前置 `memset` 清零。
- **Cross-module contract review**：
  - 未破坏门面/Core/Modem/AT Engine 契约；改动仅影响 Modem 内部 destroy 错误路径的状态语义与入口校验风格。
  - 隐式契约已文档化：子类 `start()` 必须在所有退出路径离开 INITIALIZING，否则 destroy 返 INVALID_STATE 泄漏（见 `modem.h` @note）。
- **Residual risks**：
  - `modem_mqtt_publish` 拒绝零长度 payload（`modem.c:666`）：是否放宽待模块 #3/#4 交叉确认模块 AT 能力。
  - destroy 回滚后 event_task 不可恢复：本次修复以 ERROR 状态规避误用，但对象仍只能重试 destroy、无法恢复事件能力（设计上 destroy 失败即不可恢复，可接受）。
