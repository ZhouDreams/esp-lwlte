# lwlte start/stop 生命周期对称化设计

- **日期**: 2026-06-15
- **作者**: JovisDreams（设计协作）
- **状态**: 待评审（Draft）
- **关联**: `docs/superpowers/specs/2026-06-04-lwlte-start-lifecycle-design.md`（前序：拆分 init 与 start）

## 1. 背景与现状

当前 `lwlte.h` 门面暴露 `lwlte_start()`，但没有对称的 `lwlte_stop()`，取而代之的是 `lwlte_disconnect()`。经代码核对，二者在 Core 层映射到**不同**的 FSM 信号与终态：

- `lwlte_disconnect` → `core_disconnect` → `CORE_SIG_NET_DEACTIVATE` → 去激活 PDP，Core 回到 `READY`（仅网络层动作，模块仍上电）。
- `core_stop`（已实现但未在门面暴露）→ `CORE_SIG_STOP` → `handle_stop`：关重连 + 去激活网络 + Core 回到 `STOPPED` + 发 `LWLTE_EVENT_STOPPED`。

更关键的现状：**四个后台线程全部在 init/create 阶段创建，在 destroy 阶段销毁**，并非在 start 时创建：

| 线程 | 创建于 | 销毁于 |
|---|---|---|
| `at_engine_rx` | `at_engine_create`（`at_engine.c:516`） | `at_engine_destroy` |
| `modem_evt` | `modem_base_init`（`modem.c:154`，create 内） | `modem_destroy` |
| `lwlte_fsm`（core） | `core_create`（`core_fsm.c:152`） | `core_destroy` |
| `mqtt_fsm` | `mqtt_client_create`（`mqtt_client.c:967`） | `mqtt_client_destroy` |

现有 `core_start`/`core_stop`、`mqtt_client_start`/`mqtt_client_stop` 是**逻辑**的「运行/停机」信号，FSM 任务始终常驻；`modem_start` 做的是 EN 硬复位 + AT 初始化，不涉及线程。

EN 引脚语义（`hardware_reset`，`modem_air780ep.c:2253`）：持 AT 命令路径独占 → EN 拉低 → 等 `reset_pulse_ms` → EN 拉高。**EN 低 = 断电/复位，EN 高 = 上电**。当前没有「拉低并保持（关机）」的路径。

## 2. 目标 / 非目标

### 目标
1. 门面提供对称的 `lwlte_start()` / `lwlte_stop()`，符合项目生命周期哲学：`init`/`destroy` 管资源，`start`/`stop` 管运行。
2. `lwlte_stop()` 执行**硬件关机**：把模块 EN 拉低并保持，使模块断电；同时让 Core/MQTT 状态机进入停机态、网络去激活、停机后静默。
3. `lwlte_start()` 可在 `stop` 之后**可靠地再次启动**：EN 拉低→拉高重新上电、重跑 AT 初始化、重连网络。
4. 保证 `init → start → stop → start → … → destroy` 往复循环可靠（重点）。
5. 移除 `lwlte_disconnect()`（无联网的 LTE 模块无意义，不需要「断网但保模块」的中间态）。

### 非目标
- 不改变线程归属模型：线程仍由 `init`/`create` 创建、`destroy` 销毁（**架构 A**，已确认）。`start`/`stop` 不创建/删除线程。
- 不实现深睡眠/低功耗电源管理（仅做 EN 关机）。
- 不重新暴露 `lwlte_connect()`（保持 Core 内部化；联网由 `start` 一次性完成）。
- 不引入「停机回收任务栈 RAM」能力（架构 B/C 已否决）。

## 3. 已确认的设计决策

1. **架构 A**：线程常驻（init 建 / destroy 销）。`stop` = 断电 + 静默；`start` = 上电 + 联网。
2. **MQTT 收尾**：`lwlte_stop` 强制把 MQTT 停到 `STOPPED`；重启后**不自动**拉起 MQTT，由 App 自行再调 `lwlte_mqtt_start`。
3. **stop 响应性**：联网循环（`run_activation_loop`）响应「stop pending」提前退出（协作式取消），避免 stop 干等最长 120s。
4. **modem 关机态**：新增 `MODEM_STATE_OFF` 表示「已断电、可重启」，不复用 `CREATED`。
5. **无 EN 脚降级**：`en_pin == GPIO_NUM_NC` 时无法物理断电，`stop` 降级为「逻辑停机」——Core 进 `STOPPED`、modem 进 `OFF`（逻辑）、模块仍上电但不再被驱动。

## 4. 生命周期模型（四层）

```
                 init/create        start            stop             destroy
at_engine    │  建 UART+RX 线程   │  (不变)         │  (不变)         │  停 RX+卸 UART
modem        │  建事件线程        │  EN 低→高+AT初始化│ EN 低(保持)+OFF │  停事件线程+ops.destroy
core         │  建 FSM 线程       │  STARTING→ONLINE │ deactivate+STOPPED│ 停 FSM+net_mgr 反初始化
mqtt(可选)   │  建 FSM 线程       │  (App 决定)      │  强制 STOPPED   │  停 FSM
```

要点：
- 线程在 `init` 已全部就绪；`start`/`stop` 只切换「上电联网 / 断电静默」与各 FSM 的逻辑状态。
- `at_engine` 没有生命周期状态机（只有命令级 `AT_STATE_IDLE/BUSY`）。`start`/`stop` 不动它，RX 线程常驻；模块断电后它阻塞在 UART 读，无数据。
- 阻塞型操作（`modem_start` 上电初始化、`modem_stop` 断电、网络去激活）一律在 **Core FSM 任务**上执行，与现有 `handle_start` 调 `modem_start` 的模式一致。

## 5. 各层详细设计

### 5.1 Facade（`src/include/lwlte.h` / `src/lwlte/lwlte.c`）

**移除**：
- `esp_err_t lwlte_disconnect(lwlte_handle_t *me);`（声明 `lwlte.h:375`，实现 `lwlte.c:271`）。

**新增**：
```c
/**
 * @brief 停止 LTE 并对模块断电（硬件关机）
 * @details 异步提交停机请求：去激活网络、停止 MQTT、对模块 EN 断电，Core 回到 STOPPED。
 *          ESP_OK 仅表示请求已提交；完成通过 LWLTE_EVENT_STOPPED 上报或用 lwlte_get_state 查询。
 *          停机后可再次 lwlte_start() 重新上电联网。
 */
esp_err_t lwlte_stop(lwlte_handle_t *me);
```

`lwlte_stop` 实现（异步、薄封装）：
1. `begin_api_call(me, true, &core)`。
2. 取锁读 `mqtt = me->mqtt`；若非 NULL，调用 `mqtt_client_stop(mqtt)`（best-effort，异步）。
3. `core_stop(core)`（异步，入队 `CORE_SIG_STOP`；重活在 FSM 任务上做）。
4. `end_api_call(me)`，返回 `core_stop` 的结果。

`lwlte_start`：签名不变；文档明确「可在 `STOPPED` 后再次调用」，仅当 Core 处于 `STOPPED` 时被接受。`core_start()` 成功投递 START 信号时同步把 Core 标记为 `STARTING`，因此 `lwlte_start()` 返回后立即调用 `lwlte_stop()` 不会被误判为仍处于 `STOPPED`。

**重启契约**（写入头文件注释）：`lwlte_stop` 为异步；App 若要紧接着重启，应等待 `LWLTE_EVENT_STOPPED`（或轮询 `lwlte_get_state()==LWLTE_STATE_STOPPED`）后再调用 `lwlte_start`。在 `STOPPED` 之前调用 `lwlte_start` 返回 `ESP_ERR_INVALID_STATE`。这与现有「`start` 异步、等 `LWLTE_EVENT_NET_ONLINE`」的模型对称。

### 5.2 Core（`src/core/`）

**(a) `handle_stop` 扩展**（`core_fsm.c:427`），新顺序：
1. `net_mgr_set_reconnect_enabled(me, false)` + `net_mgr_cancel_reconnect(me)`（已有）。
2. `net_mgr_deactivate(me)`（已有；模块此时仍上电，PDP 去激活 AT 可正常下发）。
3. **`modem_stop(me->modem)`（新增）**：在 FSM 任务上阻塞执行 EN 断电（见 5.3）。
4. `core_set_state(me, CORE_STATE_STOPPED)`（已有）。
5. `post_event_checked(me, LWLTE_EVENT_STOPPED, NULL)`（已有）。
6. 清除 `stop_pending`（见下）。

**(b) 协作式取消（stop 响应性）**：
- `core_handle` 新增 `bool stop_pending`（受 `me->lock` 保护）。
- `core_stop()`：在同一把 Core lock 下投递 `CORE_SIG_STOP`；仅当队列投递成功时置 `stop_pending = true`，避免入队失败后留下 stale pending。
- `net_mgr` 的 `check_activation_continue()`（`net_mgr.c:566`）在 `core_is_destroying` 之外，**追加检查 `stop_pending`**，命中即返回一个「被取消」的 sentinel（建议 `ESP_ERR_INVALID_STATE`）。
- `net_mgr_start_activation()` 尾部（`net_mgr.c:405-413`）：当返回是「被 stop 取消」时，**比照 destroying 分支直接返回，不调用 `fail_activation`**（即不置 `CORE_NET_STATE_ERROR`、不发 `NET_ERROR`）。这样激活循环迅速让出，FSM 任务回到队列处理 `CORE_SIG_STOP`。
- `core_start()` 成功投递 START 时清除 stale `stop_pending` 并标记 `STARTING`；`handle_start()` 在调用 `modem_start()` 前检查 `stop_pending`，若 stop 已排队则跳过开机；`modem_start()` 返回后再次检查 `stop_pending`，若 stop 已排队则直接执行 `handle_stop()`，不进入网络激活。`handle_stop()` 结束清除 `stop_pending`，保证下一轮干净。

**(c) `SERVICE_CMD` 停机门控**：
- `handle_service_cmd()`（`core_fsm.c:544`）当前不校验 Core 状态，停机后会拿 AT 打已断电模块、白等超时。
- 新增：进入时若 Core 状态为 `STOPPED`（模块已断电）或 `ERROR`，直接 `finish_service_cmd(..., CORE_CMD_RESULT_ERROR, &ESP_ERR_INVALID_STATE)`，不下发任何 modem AT。`Ping` 已校验 net online（`core_fsm.c:625`），此处补齐 MQTT 类命令。

**(d) 移除 disconnect 死代码**：
- 删除 `core_disconnect()`（`core.c:335`、`core.h:357`）与 `CORE_SIG_NET_DEACTIVATE` 的派发分支（`core_fsm.c:349`）、`api_state_allows`/`send_simple_signal` 中的对应分支。
- 保留 `net_mgr_deactivate()`（`handle_stop` 仍使用）。
- `core_connect()` / `CORE_SIG_NET_ACTIVATE` 当前也未被门面使用（`handle_start` 直接调 `net_mgr_start_activation`）。本次**可选**一并清理；如清理需确认无其它内部引用，否则留作后续。

### 5.3 Modem（`src/modem/modem.c` + 子类）

**(a) 基类新增 `modem_stop`**：
```c
/** @brief 停止并对模块断电（EN 拉低保持）；从任意非 DESTROYING 状态可调用 */
esp_err_t modem_stop(modem_handle_t *me);   // 调 ops->stop，类比 modem_start 调 ops->start
```
- 与 `modem_start` 不同，`modem_stop` 不走 `check_ready` 的窄门；断电应在任意非 `DESTROYING` 状态都可执行（含 `ERROR`），以支持失败恢复。

**(b) `modem_ops_t` 新增 `stop`**（`modem_priv.h:144`）：在 `start`、`reset` 旁加 `modem_no_arg_fn stop;`。

**(c) 新增 `MODEM_STATE_OFF`**（`modem.h` 枚举）：
- 语义：模块已断电、可重启。
- 需同步更新所有范围检查：`modem_set_state`（`modem.c:269` 的 `>= CREATED && <= DESTROYING`）、`modem_destroy` 的 allowed 集合（`modem.c:292`，加入 `OFF`）、`check_ready`。
- 枚举插入位置需保证范围检查连续（建议放在 `CREATED` 之后、其余运行态之前，或显式改写范围判断为白名单）。

**(d) `check_ready` 放行 OFF**（`modem.c:723`）：`modem_start`（`allow_created=true`）需允许从 `OFF` 启动——把 `OFF` 与 `CREATED` 同列于 `allow_created` 放行分支。

**(e) air780ep 新增静态助手 `hardware_power_off` + 实现 `air780ep_stop`**（`modem_air780ep.c`，`ops.stop`）：

新增静态函数，与 `hardware_reset`（`:2253`）并列、形如 `static esp_err_t hardware_power_off(modem_air780ep_t *self)`，职责单一——把 EN 拉低并保持：
1. `at_engine_begin_exclusive(self->base.at)`（与 `hardware_reset` 一致，序列化 AT 命令路径，确保切电时无命令在飞）。
2. 若 `en_pin == GPIO_NUM_NC`：`at_engine_end_exclusive` 后返回 `ESP_OK`（降级：无脚可断电）。
3. `gpio_config` EN 输出 → `gpio_set_level(en_pin, 0)` →（可选 `at_engine_flush_rx_exclusive` 清掉断电瞬态噪声）→ **不再拉高，保持低**。
4. `at_engine_end_exclusive`。
5. （EN 的 `gpio_config` 与 `hardware_reset` 重复，可抽一个 `static esp_err_t set_en_level(self, level)` 共用——可选。）

`air780ep_stop` 编排（对称版，逆 `air780ep_start` 入口）：
1. 取 `base.lock`，复位 MQTT 运行标志（`mqtt_data_enabled/session_connected/tcp_connected = false`），放锁；`set_initialized(self, false)`（逆 start 步 1/8）。
2. 若 `urc_registered` 则 `unregister_urcs(self)`（逆 start 步 7）。
3. `hardware_power_off(self)`（逆 start 步 4 的上电）。
4. `modem_set_state(me, MODEM_STATE_OFF)`——**即使步 3 返回错误也尽量落 OFF**，返回首个错误（断电幂等，下次 start 的 `hardware_reset` 会再拉低→拉高）。
5. **EN=NC 降级**：步 3 不动 GPIO，仅逻辑落 OFF，日志提示「无 EN 脚，模块未物理断电」。

**(f) ml307r 同样实现 `ml307r_stop`**（`lwlte_ml307r_config_t` 同样有 `en_pin`），保持多模组一致。

**(g) 复位流程不变**：`air780ep_start`（`modem_air780ep.c:2426`）已具备可重入能力（按 `urc_registered` 先注销再注册、复位 MQTT 标志、`hardware_reset` 前后 flush RX）。从 `OFF` 再 `start` 时，`hardware_reset` 的 EN 低（本已低）→高即重新上电，链路天然闭合。

### 5.4 AT Engine（`src/at_engine/`）

无 API 改动。RX 线程随 `create`/`destroy`，`start`/`stop` 不动。理由：UART 驱动的反复 install/delete 是 ESP-IDF 中最易出竞态之处，架构 A 刻意避免。断电期间无 UART 数据，RX 线程阻塞空转；`hardware_reset` 已在 EN 切换前后 flush RX，重启时输入干净。URC 的注销/重注册由 modem 子类负责。

### 5.5 MQTT / Ping

- **MQTT**：`lwlte_stop` 调 `mqtt_client_stop`（异步 → MQTT FSM 收 `MQTT_SIG_STOP` → 断开 → `STOPPED`）。即使在 modem 断电时 MQTT 的断开 AT 失败，MQTT FSM 也必须最终落到 `STOPPED`（验证点）。重启后由 App 决定何时再 `lwlte_mqtt_start`（`mqtt_client_start` 允许从 `STOPPED` 启动）。MQTT 对象的 init/destroy 生命周期保持独立，不受 start/stop 影响。
- **Ping**：同步、命令驱动，无线程。停机态下 `lwlte_ping` 因 net 非 online 返回 `ESP_ERR_INVALID_STATE`（已有）。

## 6. 状态机与迁移

**Core**：
```
STOPPED --START queued--> STARTING --(modem_start ok)--> READY --(net activate)--> NET_ACTIVATING --(PDP+IP)--> ONLINE
  ^                                                                                                       |
  └──────────────────────────── STOP（任意非 STOPPED：deactivate + modem_stop + STOPPED）────────────────┘
ERROR --STOP--> STOPPED --START--> …（失败恢复路径）
```

**Modem**：
```
CREATED --start--> INITIALIZING --> READY --> REGISTERING/REGISTERED/PDP_ACTIVE
   ^                                   |
   │                                   └── stop ──> OFF
  OFF --start(EN低→高 重新上电)--> INITIALIZING --> READY
任意态 --stop--> OFF（断电）；任意非 DESTROYING 态可 stop
```

**MQTT**：`STOPPED ↔ WAITING_NET → CONNECTING → CONNECTED`；`stop` 任意态 → `STOPPED`。

## 7. 停机后事件处理

**核心原则**：`STOPPED` 是权威态。每个 FSM 在停机态下丢弃一切异步/运行类事件，只认 `START`；定时器在 `stop` 时取消；阻塞操作绝不打到已断电的模块。

| 事件来源 | 现状 | 处理 |
|---|---|---|
| 队列残留 modem 事件 | `handle_modem_event` 已 `state==STOPPED → return`（`core_fsm.c:449`） | ✅ 已安全 |
| `NET_ACTIVATE`/`NET_DEACTIVATE`/`RECONNECT` 信号 | 均有状态门控，`STOPPED` 下 no-op（`core_fsm.c:342/349/367`） | ✅ 已安全（`NET_DEACTIVATE` 随 disconnect 一并移除） |
| 重连定时器回调 | `stop` 时已取消 + `reconnect_enabled=false`；`reconnect_timer_cb` 再校验 enabled | ✅ 已安全 |
| 模块 URC / 线噪 | 模块断电无真实 URC；`air780ep_stop` 注销 URC；即便命中也只 post modem 事件 → 被 core 丢弃 | ✅ 多重防护 |
| `SERVICE_CMD`（MQTT/Ping） | `handle_service_cmd` **未校验状态** | ⚠️ 新增 `STOPPED/ERROR` 门控（5.2c） |
| 用户 API 再调用 | `lwlte_start` 放行；`lwlte_ping` 已查 online | ⚠️ MQTT 类 service cmd 经 5.2c 拒绝；`lwlte_start` 仅 `STOPPED` 放行 |

## 8. start/stop 往复可靠性（验证点）

**重启时必须复位/重建的状态**：
- modem：`urc_registered` 先注销再注册（`air780ep_start` 已处理）；MQTT 运行标志清零；`MODEM_STATE_OFF → INITIALIZING → READY`。
- net_mgr：`net_mgr_start_activation` 重置 `retry_count`、`current_step`；`reconnect_timer` stop 时取消、start 时按需重启。
- core：`core_start()` 成功排队后标记 `STARTING` 并清 stale `stop_pending`；`handle_start()` 在 `modem_start()` 前后检查 `stop_pending`；`handle_stop()` 清 `stop_pending`。FSM 队列中的残留信号被状态门控丢弃。
- at_engine：`hardware_reset` 前后 flush RX，输入干净。
- mqtt：`stop` 落 `STOPPED`，`start` 从 `STOPPED` 重新发起。

**必测场景**：
1. `init → start → ONLINE → stop →（STOPPED + modem OFF + EN 低）→ start → ONLINE`，重复 N 次稳定。
2. 在 `STARTING` / `NET_ACTIVATING` 期间 `stop`：若 `modem_start()` 尚未进入，则跳过开机并处理 STOP；若已在阻塞式 `modem_start()` 中，则在该有界操作返回后立即停机且不进入网络激活；若在网络激活中，则协作式取消并落 `STOPPED`，不误报 `NET_ERROR`。
3. start 失败（modem/网络）→ Core `ERROR` → `stop`（从 ERROR 放行）→ `STOPPED + OFF` → `start` 恢复。
4. `stop` 时 MQTT 处于 `CONNECTED`：MQTT 落 `STOPPED`，网络 `OFFLINE`，modem 断电；其断开 AT 失败也不影响落 `STOPPED`。
5. 停机后注入残留/伪事件：被各状态门控丢弃，无副作用。
6. `destroy` 从 `STOPPED`、从 `ONLINE`、从 `OFF` 三种态均成功。
7. `en_pin == NC` 降级：逻辑停机成立，重启走逻辑路径（模块物理常上电）。

## 9. 错误处理与恢复

- `lwlte_stop` 异步返回 `ESP_OK` 仅表示请求已提交；真实结果以 `LWLTE_EVENT_STOPPED` / 状态查询为准。
- 失败恢复闭环：任何运行期错误（Core `ERROR`）→ `lwlte_stop`（断电、落 `STOPPED`）→ `lwlte_start`（重新上电）。`modem_stop` 必须从 `ERROR` 可调用。
- `modem_stop` 内部若 EN 操作失败：记录日志，仍尽量落 `OFF`（断电是「尽力而为且幂等」的操作，下次 `hardware_reset` 会再次拉低→拉高）。

## 10. 测试计划

**Host 静态契约测试**（`tests/host/`，沿用现有 grep/AST 风格）：
- 更新 `test_lwlte_start_lifecycle.py`：断言 `lwlte.h` 含 `lwlte_stop`、不含 `lwlte_disconnect`；`lwlte_stop` 实现体含 `core_stop` 且对 `me->mqtt` 调 `mqtt_client_stop`；`lwlte_start` 体不变（仍 `core_start`，不出现 `modem_*`）。
- 更新 `test_mqtt_end_to_end_contract.py`：把对 `lwlte_disconnect` 的引用（`:863-864`）改为 `lwlte_stop`。
- 新增断言：`handle_stop` 调用 `modem_stop`；存在静态 `hardware_power_off` 且含 `gpio_set_level(.., 0)`（无后续拉高）；`air780ep_stop` 调用 `hardware_power_off`、`unregister_urcs` 并设置 `MODEM_STATE_OFF`；`handle_service_cmd` 含 `STOPPED` 门控；`MODEM_STATE_OFF` 在 `check_ready` 放行。

**硬件人工验证**（无法 host 化）：
- 示波器/逻辑分析仪量 EN：`stop` 后 EN 维持低、模块断电；`start` 后 EN 低→高、模块重启。
- 反复 `start/stop` ≥ 20 次仍能稳定联网。

**构建**：按 `docs/agents/build-and-debug.md` 执行 ESP-IDF 构建（目标 esp32c3）确保编译通过。

## 11. 文档更新清单

- `src/include/lwlte.h`：移除 `lwlte_disconnect` 注释、新增 `lwlte_stop` 注释、更新 `lwlte_start` 生命周期说明与重启契约。
- `docs/agents/classes.md`：生命周期章节补 `lwlte_start`/`lwlte_stop` 对，modem `OFF` 态，移除 disconnect 叙述。
- `docs/agents/architecture.md`：用户操作 API 列表更新。
- `docs/agents/err.md`：API 列表把 `lwlte_disconnect` 换成 `lwlte_stop`。
- `docs/agents/oop-design.md`：如涉及 connect/disconnect 叙述则同步。
- `AGENTS.md` / `AGENTS_ZH.md`：若索引结构不变则无需改；如有内容性变更需双语同步。

## 12. 未决 / 未来

- `core_connect()` / `CORE_SIG_NET_ACTIVATE` 死代码清理（与本设计弱相关，留作可选）。
- 是否提供同步版 `lwlte_stop`（阻塞至 `STOPPED`）——当前定为异步 + 事件，契约已对称，暂不做。
- 低功耗/深睡眠协同（超出本设计范围）。
