# Modem 模块主框架分析

分析依据：`src/modem/modem.h`、`src/modem/modem_priv.h`、`src/modem/modem.c`、`src/modem/modem_air780ep.h`、`src/modem/modem_air780ep.c`、`docs/agents/classes.md`。

结论优先：本文以当前源码为准。`classes.md` 对 Modem Adapter 的分层、基类/子类、ops 多态、事件队列和 Air780EP AT 指令映射描述总体正确，但当前源码已经多出 MQTT 下行 `+MSUB`、SIM busy 定时轮询、payload 所有权释放、内部 stop event task 等实现细节，也存在若干状态机和销毁错误路径需要代码走查时直接指出。

## 0. 当前源码与 classes.md 的主要出入

| 项目 | classes.md 描述 | 当前源码事实 | 影响 |
|------|-----------------|--------------|------|
| Air780EP 子类字段 | `modem_air780ep_t` 包含 CPIN/CREG/CEREG/CGREG/CGEV/PDP deact handler、缓存、`urc_registered`、`initialized` | 额外已有 `msub_handler`、MQTT 配置/连接状态和 `mqtt_data_enabled` | 当前实现已经支持 MQTT 下行数据 URC；初始化不再使用 `RDY` 作为 gate |
| URC 列表 | 注册 `+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:`、`+CGEV:`、`+PDP DEACT`、`+PDP:DEACT` | 额外注册 `+MSUB:` | MQTT 下行数据会被解析成 `MODEM_EVENT_PROTOCOL_DATA`，不是只支持网络/SIM/PDP 事件 |
| 初始化命令 | 文档列出 `ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2` | 源码额外执行 `AT*I`，且每条基础初始化命令最多重试 3 次 | 面试讲 init 时要补充 retry 和 `AT*I` |
| `GPIO_NUM_NC` 语义 | 配置注释写 EN GPIO 未使用时为 `GPIO_NUM_NC` | `hardware_reset()` 在 `GPIO_NUM_NC` 时不会拉 EN，但仍 flush RX；后续 `wait_at_ready()` 轮询 `AT` 直到 `OK` | 如果外部没有保证模块 AT 通道可用，`start/reset` 仍可能因 `AT OK` 超时；这不是“跳过复位直接初始化” |
| `modem_start()` 状态 | 文档隐含从 CREATED 启动到 READY | `check_ready(me, true)` 允许 CREATED，也允许 READY/REGISTERING/REGISTERED/PDP_ACTIVE 后再次调用 `modem_start()` | `start` 实际可作为“重新启动/硬复位”入口使用，但语义不够清晰 |
| `modem_reset()` | reset 通过 EN 复位并恢复基础 AT 环境 | 源码中 `air780ep_reset()` 与 `air780ep_start()` 主干几乎相同 | 可讲为显式软 API 触发的模块级硬复位；实现上存在重复代码 |
| 事件枚举 | 文档列出 READY、SIM、REG、PDP、SIGNAL、ERROR、PROTOCOL_DATA、PROTOCOL_CLOSED | 当前源码实际投递 READY、SIM_CHANGED、REG_CHANGED、PDP_ACTIVATED、PDP_DEACTIVATED、PROTOCOL_DATA；未看到 SIGNAL_CHANGED、ERROR、PROTOCOL_CLOSED 的生产者 | 不能把未投递的事件讲成当前已实现能力 |
| 事件 payload 所有权 | 文档强调 `MODEM_EVENT_PROTOCOL_DATA` 的 topic/payload 由 event task 释放 | 源码通过 `release_event_payload()` 和 `drain_event_queue_payloads()` 落地；`modem_post_event()` 失败时调用者仍释放 | 这是当前资源所有权比较清楚的一点 |
| destroy 错误路径 | 文档只说 destroy 注销 URC 并释放资源 | `modem_destroy()` 先停止 event task，再调用子类 destroy；如果 `air780ep_unregister_urcs()` 失败，状态会被回滚，但 event task 已经停掉 | 存在半销毁风险：句柄状态可能恢复为 READY/REGISTERED 等，但事件上报通道已不可用 |
| 内部 base API | 文档只概述 base 资源 | 当前 `modem_priv.h` 额外暴露 `modem_base_stop_event_task()`，并有 `event_cb_active/event_cb_done_sema` 防止注销回调时悬空 | event callback 的并发释放比文档更完整，但也增加 destroy/stop 的状态复杂度 |

直接指出：当前 Modem 实现的主框架是清楚的，基类负责事件队列、event task、状态锁和 ops 分发，Air780EP 子类负责 AT 指令和 URC 翻译。但也有三个代码走查风险点：`modem_start()` 可重复调用且会触发硬复位，`GPIO_NUM_NC` 不拉 EN 但仍会进入 `AT OK` 轮询，`modem_destroy()` 在子类 destroy 失败后会留下已停止 event task 的半销毁对象。

## 1. 模块一句话职责

Modem 模块负责把 Core 的模块语义 API 转换为具体 Air780EP AT 命令，把 AT Engine 分发的 URC 转换成 `modem_event_t` 并通过 `event_queue + event_task` 上报给 Core，同时维护 Modem 本地状态、PDP/SIM/注册/MQTT 数据开关缓存，以及 Air780EP EN 复位、`AT OK` ready 轮询和事件 payload 所有权。

## 2. 对外 API 总览

这里按“`.h` 文件或非 static 函数”列出。`modem.h` 是 Core 可见的层间 API；`modem_air780ep.h` 是 Facade factory 可见的具体模块工厂；`modem_priv.h` 是 Modem 模块内部 API，只给通用实现和具体子类使用。

### Core/Facade 可见 API

| API | 类型 | 作用 |
|-----|------|------|
| `modem_air780ep_create(at, config)` | create | 分配 Air780EP 子类对象，初始化 PDP/信号/SIM/注册/MQTT 缓存，调用 `modem_base_init()` 创建事件队列和 event task，返回 `modem_t *` |
| `modem_start(me)` | start | 通用 wrapper，检查状态和 ops 后调用 `air780ep_start()`；Air780EP 会硬复位、轮询 `AT` 直到 `OK`、执行基础 AT 初始化命令、注册运行期 URC、进入 READY |
| `modem_reset(me)` | reset/recover | 通用 wrapper，检查状态后调用 `air780ep_reset()`；主干与 init 相同，用于运行期硬复位恢复基础 AT 环境 |
| `modem_destroy(me)` | stop/deinit/destroy | 设置 DESTROYING，停止 event task，调用子类 destroy 注销 URC，再释放 base 资源和对象内存 |
| `modem_register_event_callback(me, callback, user_ctx)` | handle/register | Core 注册或清除 Modem 上行事件回调；清除回调时会等待正在执行的 callback 结束 |
| `modem_get_state(me, state)` | process/query | 读取 Modem 当前本地状态 |
| `modem_get_info(me, info)` | process/query | 查询 IMEI/IMSI/ICCID/型号/固件版本，并更新 `cached_info` |
| `modem_get_sim_status(me, status)` | process/query | 查询 `AT+CPIN?`，处理 READY/PIN/PUK/未插卡；SIM busy 时按固定间隔轮询重试 |
| `modem_get_signal(me, signal)` | process/query | 查询 `AT+CSQ`，解析 RSSI/BER 和 dBm，更新 `last_signal` |
| `modem_get_registration(me, status)` | process/query | 按 `CEREG -> CGREG -> CREG` fallback 查询注册状态，更新 `last_reg_status` 和 `modem_state_t` |
| `modem_get_packet_attach_status(me, attached)` | process/query | 查询 `AT+CGATT?` 判断分组域是否附着 |
| `modem_set_apn(me, cid, apn)` | send/config | 发送 `AT+CGDCONT=<cid>,"IP","<apn>"` 并更新 PDP cache |
| `modem_activate_pdp(me, cid)` | start/enable | 对 Air780EP TCPIP 场景执行 `CSTT -> CIICR -> CIFSR`，缓存 IP，置 `PDP_ACTIVE`，投递 PDP 激活事件 |
| `modem_deactivate_pdp(me, cid)` | stop/disable | 发送 `AT+CIPSHUT`，清 PDP cache，置 READY，投递 PDP 去激活事件 |
| `modem_get_pdp_context(me, cid, pdp)` | process/query | 读取本地 PDP cache，并用 `AT+CGACT?`、`AT+CGPADDR=<cid>` 校准 active/IP |
| `modem_mqtt_configure(me, config)` | send/config | 发送 `AT+MCONFIG` 设置 MQTT client id/user/password，并缓存 host/port、clean_session、keepalive_s |
| `modem_mqtt_tcp_connect(me)` | start/enable | 使用缓存 host/port 发送 `AT+MIPSTART` 建立 MQTT TCP 通道，接受 `CONNECT OK` 或 `ALREADY CONNECT` |
| `modem_mqtt_connect(me)` | start/enable | 使用缓存 clean_session/keepalive_s 发送 `AT+MCONNECT`，成功 `CONNACK OK` 后打开 `mqtt_data_enabled` |
| `modem_mqtt_disconnect(me)` | stop/disable | 关闭 `mqtt_data_enabled` 并发送 `AT+MDISCONNECT`，只断开 MQTT broker 会话 |
| `modem_mqtt_tcp_disconnect(me)` | stop/disable | 发送 `AT+MIPCLOSE` 关闭 MQTT TCP 通道，通常在 `modem_mqtt_disconnect()` 后执行 |
| `modem_mqtt_subscribe(me, topic)` | send/process | 发送 `AT+MSUB` 并等待 `SUBACK` |
| `modem_mqtt_unsubscribe(me, topic)` | send/process | 发送 `AT+MUNSUB` 并等待 `UNSUBACK` |
| `modem_mqtt_publish(me, publish)` | send/process | 发送 `AT+MPUBEX`，等待 `>` prompt 后写 raw payload；QoS1 等待 `PUBACK` |
| `modem_ping(me, request, replies, max_replies, summary)` | send/process | 发送 `AT+CIPPING`，解析每条 `+CIPPING:` reply 并计算 summary |

### Modem 内部非 static API

| API | 类型 | 作用 |
|-----|------|------|
| `modem_base_init()` | create/base init | 初始化 `modem_t` 基类字段，创建 lock、event queue、event task done sema、callback done sema 和 event task |
| `modem_base_deinit()` | base deinit | 停 event task，清队列 payload，删除 queue/semaphore/lock，清空基类字段 |
| `modem_base_stop_event_task()` | stop | 请求 event task 停止并等待 `event_task_done_sema` |
| `modem_post_event()` | handle/process | URC handler 或 ops 方法向 base event queue 投递 `modem_event_t` |
| `modem_set_state()` | process/state | 加锁更新 `modem_state_t`，destroying 时拒绝非 DESTROYING 状态 |

推荐调用顺序：

```text
at_engine_create
-> modem_air780ep_create
-> modem_start
-> modem_register_event_callback
-> modem_get_* / modem_set_apn / modem_activate_pdp / modem_mqtt_* / modem_ping
-> modem_reset 可选恢复
-> modem_deactivate_pdp / modem_mqtt_disconnect + modem_mqtt_tcp_disconnect 可选
-> modem_destroy
-> at_engine_destroy
```

模块有独立 `modem_start()` 启动入口，但没有独立 `stop()` API。`modem_air780ep_create()` 成功后 event task 已经运行；`modem_start()` 是硬件和 AT 工作环境启动；`modem_destroy()` 同时承担 stop 和 deinit。

## 3. 生命周期主流程

显式状态来自 `modem_state_t`，实际生命周期还包含资源创建和 task 启停：

```text
UNINIT
-> modem_air780ep_create 成功
-> CREATED: 对象/base/event task 已创建
-> modem_start
-> INITIALIZING: 执行硬件 reset，轮询 AT OK，执行基础 AT 初始化命令
-> READY: AT OK 与基础 AT 命令成功，运行期 URC 已注册，投递 MODEM_EVENT_READY
-> REGISTERING: 查询或 URC 发现网络搜索中
-> REGISTERED: 查询或 URC 发现已注册
-> PDP_ACTIVE: modem_activate_pdp 成功，或 PDP cache 被置 active
-> READY: PDP deactive/CIPSHUT/PDP DEACT URC
-> ERROR: start/reset 失败，按条件回滚 URC
-> DESTROYING: modem_destroy 设置 destroying 并停止 event task
-> UNINIT: 子类 destroy 成功，base deinit，free(me)
```

异常路径：

```text
create 任一步失败
-> 删除已创建 sema/task/queue/lock
-> free(self)
-> 返回 NULL

start/reset 失败
-> 如果本轮新注册了 URC，则 unregister_urcs
-> initialized=false
-> state=MODEM_STATE_ERROR
-> 返回失败码

destroy 子类注销 URC 失败
-> event task 已经停止
-> restoring destroying=false/state=旧状态/event_task_stop_requested=false
-> 返回错误码
-> 句柄未 free，但事件上报通道已停，属于半销毁风险
```

运行期状态机不是完整网络状态机。Modem 只维护低层观察状态；真正的联网重试、退避和业务状态迁移归 Core。

## 4. 核心数据结构分析

### `struct modem`

| 类别 | 成员 | 含义 |
|------|------|------|
| 多态 | `const modem_ops_t *ops` | 具体模块虚函数表，通用 `modem_*` wrapper 通过它分发到 Air780EP |
| 依赖 | `at_engine_t *at` | 下层 AT Engine 句柄，Modem 借用，不拥有生命周期 |
| 资源 | `lock` | 保护 state、callback、destroying、event task 标志和 cache 访问 |
| 资源 | `event_queue` | URC/ops 生成的 `modem_event_t` 队列 |
| 资源 | `event_task` | 从 event queue 取事件并调用 Core callback 的后台 task |
| 资源 | `event_task_done_sema` | event task 退出同步 |
| 资源 | `event_cb_done_sema` | 清除 callback 时等待正在执行的 callback 结束 |
| 回调 | `event_cb`、`event_user_ctx` | Core 注册的上行事件回调槽位 |
| 运行数据 | `event_cb_active` | 当前正在执行的 callback 数量 |
| 状态 | `state` | Modem 本地生命周期和低层连接状态 |
| 状态 | `destroying` | destroy 已开始，拒绝新事件和部分状态修改 |
| 状态 | `event_task_stop_requested` | event task 协作退出标志 |
| 配置/诊断 | `name` | 具体模块名，当前为 `air780ep` |

`struct modem` 是模块的“躯干”：它不懂 Air780EP AT 指令，但拥有共同的状态锁、事件传输通道、上层 callback 和 ops 多态入口。

### `modem_ops_t`

`modem_ops_t` 是内部虚函数表，按 Core 需要的语义能力划分，不按 AT 指令逐条暴露。当前 Air780EP ops 完整实现了 start/reset/info/SIM/signal/registration/attach/APN/PDP/MQTT/ping。Core 只看 `modem_*` wrapper，不直接看 ops。

### `modem_air780ep_t`

| 类别 | 成员 | 含义 |
|------|------|------|
| 继承 | `modem_t base` | 必须是第一个字段，用 `MODEM_CONTAINER_OF` 从基类回到子类 |
| 配置 | `modem_air780ep_config_t config` | EN GPIO、reset pulse、AT OK ready timeout、默认命令超时、event task 参数快照 |
| 回调节点 | `cpin_handler`、`creg_handler`、`cereg_handler`、`cgreg_handler`、`cgev_handler`、`pdp_deact_handler`、`pdp_colon_deact_handler`、`msub_handler` | Air780EP 拥有的 AT Engine URC handler 节点，注册期间保持有效 |
| 运行缓存 | `cached_info` | 最近一次模块/SIM 静态信息查询结果 |
| 运行缓存 | `last_sim_status`、`last_reg_status`、`last_signal` | 最近一次 SIM/注册/信号状态 |
| 运行缓存 | `pdp[AIR780EP_MAX_PDP_CONTEXTS]` | PDP context cache，当前最多 4 个，TCPIP 激活实际只支持 cid 1 |
| 状态 | `urc_registered` | URC handler 是否已注册到 AT Engine |
| 状态 | `initialized` | Air780EP 基础 AT 工作环境是否完成 |
| 状态 | `mqtt_configured`、`mqtt_tcp_connected`、`mqtt_session_connected` | MQTT 配置、TCP 通道和会话连接状态 |
| 状态 | `mqtt_data_enabled` | MQTT 登录后才接收并上报 `+MSUB:` 下行数据 |

这个结构体代表 Air780EP 子类的“身体部件”：基类躯干、硬件配置、运行期 URC 触角、SIM/注册/信号/PDP/MQTT 运行缓存，以及 Air780EP 特有 AT 指令能力。

### `air780ep_cmd_ctx_t`

`air780ep_cmd_ctx_t` 是单次 AT 命令的栈上临时上下文，只包含 `char *lines[AIR780EP_MAX_RESPONSE_LINES]` 和 `at_response_t response`。它不跨命令保存，也不拥有 AT Engine response 字符串内存。

## 5. 资源所有权

| 资源 | 谁创建 | 谁使用 | 谁释放 | 创建失败回滚 | stop/deinit 关系 | 风险 |
|------|--------|--------|--------|--------------|------------------|------|
| `modem_air780ep_t` 对象 | `modem_air780ep_create()` 的 `calloc` | 所有 `modem_*` API 和 URC handler | `modem_destroy()` 成功路径 `free(me)` | create 失败直接 `free(self)` | 只有 destroy 释放 | destroy 子类失败时对象不释放且 event task 已停 |
| `at_engine_t *at` | Facade/AT Engine 创建后传入 | Air780EP 发送命令、注册 URC | Modem 不释放，由上层反向顺序释放 | create 失败不接管 | destroy 只注销 URC，不 destroy AT | 调用方必须保证 at 生命周期覆盖 modem |
| `base.lock` | `modem_base_init()` | 状态、callback、event task 标志、cache 保护 | `modem_base_deinit()` | `modem_base_deinit()` 判空删除 | deinit 删除 | URC handler 中部分路径用 0 tick 获取，锁忙会丢事件 |
| `event_queue` | `modem_base_init()` 的 `xQueueCreate` | `modem_post_event()` 和 `event_task` | `modem_base_deinit()` 的 `vQueueDelete` | `modem_base_deinit()` | destroy 先停 task 再删 queue | queue 满时事件丢弃，协议数据失败时调用者必须释放 |
| `event_task` | `modem_base_init()` 的 `xTaskCreate` | 消费 `event_queue` 并调用 Core callback | task 自己 `vTaskDelete(NULL)`，destroy 等 done sema | base init 失败时 `modem_base_deinit()` 处理 | 没有单独 public stop | 若 stop 后子类 destroy 失败，任务不会自动恢复 |
| `event_task_done_sema` | `modem_base_init()` | stop/destroy 等待 event task 退出 | `modem_base_deinit()` | 同上 | deinit 删除 | 如果 task 卡住，stop 会永久等待 |
| `event_cb_done_sema` | `modem_base_init()` | 清除 callback 时等待 active callback 归零 | `modem_base_deinit()` | 同上 | deinit 删除 | 防止 callback 悬空；从 event task 内清 callback 会返回 `ESP_ERR_INVALID_STATE` 防死锁 |
| `event_cb/user_ctx` | `modem_register_event_callback()` 写入 | `event_task` 调用 | 清 callback 或 base deinit 清空 | 不涉及 | destroy 清空 | 上层清 callback 后等待 active callback 结束，设计较稳 |
| URC handler 节点 | Air780EP 对象内嵌 | AT Engine URC 链表 | `air780ep_unregister_urcs()` 清零节点 | `register_urcs()` 部分失败会注销已注册 prefix | destroy 注销 | unregister 失败会阻止 free，产生半销毁风险 |
| MQTT command 字符串 | `escape_at_string()` 和 `malloc(cmd)` | MQTT ops 发送 AT 命令 | 各 ops 返回前 `free` | 每个失败分支手动 free 已分配项 | 不跨 API | 当前路径基本对称 |
| `+MSUB` topic/payload | `parse_msub_direct()` 的 `malloc` | `MODEM_EVENT_PROTOCOL_DATA` callback | `event_task` 的 `release_event_payload()`，或 post 失败/disabled 时 handler 释放 | payload 分配失败会释放 topic | event task/drain 时释放 | 所有权转移规则清楚；消费者若保留必须自己复制 |
| PDP/SIM/REG/signal cache | 对象内静态字段 | 查询 API 和 URC handler | 随对象 free | create 初始化默认值 | destroy 随对象释放 | 多数用 lock 保护，但部分 URC 锁忙直接丢更新 |
| GPIO EN 配置 | `hardware_reset()` 调 `gpio_config()` | reset/init 拉低/拉高 EN | 无显式 deinit | GPIO 配置失败直接返回 | destroy 不释放 GPIO | ESP-IDF GPIO 通常无“释放”；`GPIO_NUM_NC` 不拉 EN，但后续仍会轮询 `AT OK` |

释放对称性结论：create 失败回滚总体完整，MQTT 临时 heap 和协议事件 heap 释放也比较对称。主要风险集中在 destroy：`modem_destroy()` 先停 base event task，再执行子类 URC 注销；如果注销失败，句柄没有释放但 event task 不会恢复，后续状态看似回滚，实际上事件通道已经失效。

## 6. 运行时入口

| 入口 | 谁注册 | 何时调用 | 输入事件 | 修改状态 | 主要处理 | 是否上报 |
|------|--------|----------|----------|----------|----------|----------|
| `event_task(void *arg)` | `modem_base_init()` 通过 `xTaskCreate` | `event_queue` 有事件或 100 ms 超时检查 stop | `modem_event_t` | `event_cb_active` 增减；停止时 drain queue payload | 取事件，检查 destroying，调用 `event_cb`，释放 protocol payload | 调用 Core 注册的 `modem_event_callback_t` |
| `cpin_urc_handler()` | `register_urcs()` 注册 `+CPIN:` | SIM 状态 URC | `+CPIN: ...` | 更新 `last_sim_status` | 解析 SIM 状态 | 投递 `MODEM_EVENT_SIM_CHANGED` |
| `reg_urc_handler()` | 注册 `+CREG:`、`+CEREG:`、`+CGREG:` | 注册状态 URC | 注册状态行 | 更新 `last_reg_status`，非阻塞设置 READY/REGISTERING/REGISTERED | 解析注册状态 | 投递 `MODEM_EVENT_REG_CHANGED` |
| `cgev_urc_handler()` | 注册 `+CGEV:` | PDN ACT/DEACT URC | `+CGEV: ... PDN ACT/DEACT ...` | 更新对应 PDP active；DEACT 时清 IP、关闭 MQTT data、置 READY | 解析 CID 和 active/deactive | 投递 PDP 激活/去激活事件 |
| `pdp_deact_urc_handler()` | 注册 `+PDP DEACT`、`+PDP:DEACT` | 模块报告 PDP 去激活 | PDP deact 行 | 关闭 MQTT data，清所有 active PDP cache，置 READY | 批量清 cache | 对原 active 的 context 投递去激活事件 |
| `handle_msub_urc()` | 注册 `+MSUB:` | MQTT 下行消息 URC | `+MSUB:<topic>,<len>,<payload>` | 不改主状态，只检查 `mqtt_data_enabled` | heap 复制 topic/payload | 投递 `MODEM_EVENT_PROTOCOL_DATA`；失败或未 enabled 时释放 heap |
| `modem_event_callback_t` | Core 调用 `modem_register_event_callback()` | event task 取到事件后 | `modem_event_t` | 由 Core 决定 | Core 处理网络状态或协议数据 | 向 Core 上报 |

没有 esp_timer callback、esp_event handler、ISR handler、socket callback。AT Engine 的 UART RX task 是更底层入口；Modem 看到的是 AT Engine 分发出来的 URC callback。

## 7. 主要方法的主干伪代码

### `modem_air780ep_create()`

1. 检查 `at` 和 `config`。
2. `calloc` 分配 `modem_air780ep_t`。
3. 保存 config，填默认 `default_cmd_timeout_ms` 和 `ready_timeout_ms`。
4. 初始化 `last_sim_status/last_reg_status/last_signal` 和 4 个 PDP cache。
5. 调用 `modem_base_init()` 创建 lock、event queue、event task 等基类资源。
6. 返回 `&self->base`。

错误路径：

| 失败步骤 | 已创建资源 | 回滚 |
|----------|------------|------|
| 参数无效 | 无 | 返回 NULL |
| `calloc` 失败 | 无 | 返回 NULL |
| `modem_base_init` 失败 | `self` | `free(self)` |

### `modem_base_init()`

1. 检查 `me/name/at/ops`。
2. 清空 base 资源字段和 event 状态。
3. 填默认 event queue/task 参数。
4. 设置 `ops/at/name/state=CREATED/destroying=false/callback=NULL`。
5. 创建 `lock`。
6. 创建 `event_queue`。
7. 创建 `event_task_done_sema`。
8. 创建 `event_cb_done_sema`。
9. `xTaskCreate(event_task, "modem_evt", ...)`。

错误路径：任一步失败都跳到 `err`，调用 `modem_base_deinit(me)` 判空释放已创建资源。

### `modem_start()` / `air780ep_start()`

1. wrapper 检查 `me`。
2. `check_ready(me, true)`，允许 CREATED，也允许 READY/REGISTERING/REGISTERED/PDP_ACTIVE。
3. 检查 `ops->start`。
4. Air780EP 清 `mqtt_data_enabled` 和 `initialized`。
5. `modem_set_state(INITIALIZING)`。
6. `hardware_reset()` 获取 AT exclusive，flush RX，必要时拉低/拉高 EN。
7. `wait_at_ready()` 在 ready 总超时内轮询 `AT`，直到返回 `OK`。
8. `run_basic_init_cmds()` 逐条执行 `ATE0`、`AT+CMEE=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`、`AT*I`，每条最多 3 次。
9. `register_urcs()` 注册 CPIN/REG/CGEV/PDP/MSUB 等运行期 URC；初始化不使用 `RDY` 作为 gate。
10. `finish_modem_ready()` 设置 READY、`initialized=true`、投递 READY 事件。

错误路径：

| 失败步骤 | 回滚 |
|----------|------|
| 设置 INITIALIZING 失败 | 必要时注销本轮新注册 URC，`initialized=false`，state=ERROR |
| 注册 URC 失败 | `register_urcs()` 内部回滚已注册 prefix，外层 state=ERROR |
| hardware reset / AT OK polling / init command 失败 | 如果 URC 是本轮注册的则注销；`initialized=false`；state=ERROR |
| READY 事件投递失败 | 只打 warning，不使 init 失败 |

### `modem_reset()` / `air780ep_reset()`

1. wrapper 检查 `me`。
2. `check_ready(me, false)`，不允许 CREATED。
3. 检查 `ops->reset`。
4. Air780EP 执行与 init 相同的序列：清标志、置 INITIALIZING、hardware reset、轮询 `AT` 到 `OK`、基础命令、注册运行期 URC、finish ready。

错误路径与 `air780ep_start()` 一致。实现上 start/reset 代码重复，可后续抽成一个 ready sequence helper。

### `modem_destroy()`

1. 检查 `me` 和 `me->lock`。
2. 如果当前 task 是 `event_task`，返回 `ESP_ERR_INVALID_STATE` 防止自销毁。
3. 加锁检查 state 是否允许销毁：CREATED/READY/REGISTERING/REGISTERED/PDP_ACTIVE/ERROR。
4. 设置 `state=DESTROYING`、`destroying=true`。
5. `modem_base_stop_event_task()` 请求 event task 停止并等待。
6. 调用 `ops->destroy()`，Air780EP 注销 URC、清 initialized。
7. `modem_base_deinit()` 删除 event queue、done sema、callback sema、lock，清字段。
8. `free(me)`。

错误路径：

| 失败步骤 | 结果 |
|----------|------|
| 参数无效 | 返回 `ESP_ERR_INVALID_ARG` |
| 当前正在 INITIALIZING/DESTROYING 或已 destroying | 返回 `ESP_ERR_INVALID_STATE` |
| 从 event task 自己调用 destroy | 返回 `ESP_ERR_INVALID_STATE` |
| stop event task 失败 | 返回错误；通常资源未释放 |
| 子类 destroy/URC 注销失败 | 恢复 old state、`destroying=false`、`event_task_stop_requested=false`，但 event task 已停止，存在半销毁风险 |

### `modem_register_event_callback()`

1. 检查 `me` 和 `lock`。
2. 加锁；如果清 callback 且当前在 event task，返回 `ESP_ERR_INVALID_STATE` 防止死锁。
3. 如果设置 callback 且 destroying，返回 `ESP_ERR_INVALID_STATE`。
4. 写入 `event_cb/event_user_ctx`。
5. 如果是设置 callback，直接返回。
6. 如果是清 callback，循环等待 `event_cb_active` 归零，通过 `event_cb_done_sema` 同步。

### 查询类 API 主干

适用于 `get_info/get_signal/get_registration/get_packet_attach_status/get_pdp_context/get_sim_status`：

1. wrapper 检查参数。
2. `check_ready(me, false)`，要求不是 CREATED/INITIALIZING/ERROR/DESTROYING。
3. 检查对应 ops 存在。
4. Air780EP 发送一个或多个 AT 命令。
5. `ensure_at_ok()` 检查最终响应。
6. 解析响应行，更新 cache 和必要状态。
7. 输出值对象给调用方。

特殊点：

| API | 特殊行为 |
|-----|----------|
| `get_sim_status` | `+CME ERROR: SIM busy` 时最多等 10 秒，按 1 秒轮询间隔后重试 `AT+CPIN?` |
| `get_registration` | 按 CEREG/CGREG/CREG fallback；未知状态不立即失败，会尝试下一路 |
| `get_pdp_context` | 先用 cache，再查询 `CGACT` 和 `CGPADDR` 校准 active/IP |

### PDP/MQTT/Ping 发送类 API 主干

1. wrapper 检查参数、状态和 ops。
2. Air780EP 检查 CID、QoS、字符串安全或对字符串做 AT 转义。
3. 构造 AT 命令字符串，必要时 malloc。
4. 使用 `send_cmd()`、`send_cmd_with_options()` 或 `at_engine_send_cmd_with_payload()`。
5. `ensure_at_ok()` 映射 AT 失败。
6. 更新 cache/状态/事件。
7. 释放临时 heap。

典型错误路径：

| 场景 | 回滚 |
|------|------|
| 参数无效/QoS/CID 不支持 | 不分配或释放已分配字符串，直接返回 |
| malloc/escape 失败 | 释放已分配项，返回 `ESP_ERR_NO_MEM` |
| AT send 或响应失败 | 释放临时命令字符串；通常不自动恢复 |
| PDP 激活中间步骤失败 | 不置 PDP_ACTIVE，不投递激活事件；已执行过的模块侧动作不自动 CIPSHUT 回滚 |
| MQTT 会话连接成功 | `mqtt_data_enabled=true` |
| MQTT disconnect/PDP deact | `mqtt_data_enabled=false` |

## 8. static helper 分类

| 函数名 | 类型 | 是否主干 | 一句话作用 |
|--------|------|----------|------------|
| `event_task` | D task/callback 入口 | 是 | Modem 事件队列消费循环，调用 Core callback 并释放协议 payload |
| `event_task_should_stop` | D task/callback 入口 | 是 | event task 检查停止/destroy 标志 |
| `check_ready` | B 叶子 helper | 是 | 统一检查 API 是否允许在当前 state 调用 |
| `call_no_arg` | B 叶子 helper | 否 | 调用无参数 ops 的薄包装 |
| `release_event_payload` | C 清理/回滚函数 | 是 | 释放 `MODEM_EVENT_PROTOCOL_DATA` 的 heap topic/payload |
| `drain_event_queue_payloads` | C 清理/回滚函数 | 是 | deinit/task exit 前清空队列中未处理协议 payload |
| `air780ep_destroy` | C 清理/回滚函数 | 是 | 注销 URC、清 Air780EP 子类状态 |
| `air780ep_start` | A 阶段函数 | 是 | Air780EP 启动主流程 |
| `air780ep_reset` | A 阶段函数 | 是 | Air780EP 运行期复位恢复主流程 |
| `air780ep_get_info` | A 阶段函数 | 是 | 查询并缓存模块/SIM 静态信息 |
| `air780ep_get_sim_status` | A 阶段函数 | 是 | 查询 SIM 状态，并处理 SIM busy 定时轮询 |
| `air780ep_get_signal` | A 阶段函数 | 是 | 查询 CSQ 并换算 RSSI dBm |
| `air780ep_get_registration` | A 阶段函数 | 是 | fallback 查询注册状态并更新 modem state |
| `air780ep_get_packet_attach_status` | A 阶段函数 | 是 | 查询 `CGATT` 附着状态 |
| `air780ep_set_apn` | A 阶段函数 | 是 | 设置 `CGDCONT` 并更新 APN cache |
| `air780ep_activate_pdp` | A 阶段函数 | 是 | 执行 CSTT/CIICR/CIFSR 激活数据面 |
| `air780ep_deactivate_pdp` | A 阶段函数 | 是 | 执行 CIPSHUT 并清 PDP/MQTT 状态 |
| `air780ep_get_pdp_context` | A 阶段函数 | 是 | 用 cache + CGACT/CGPADDR 输出 PDP 状态 |
| `air780ep_mqtt_configure/tcp_connect/connect/disconnect/subscribe/unsubscribe/publish` | A 阶段函数 | 是 | Air780EP MQTT 语义命令实现 |
| `air780ep_ping` | A 阶段函数 | 是 | 发送 CIPPING 并解析 reply/summary |
| `send_cmd` | A 阶段函数 | 是 | 使用默认超时构造 AT options 并发送命令 |
| `send_cmd_with_options` | A 阶段函数 | 是 | 初始化命令 ctx 并调用 AT Engine options API |
| `ensure_at_ok` | C 清理/回滚函数 | 是 | 把 AT response 非 OK 统一记录并映射为 `ESP_FAIL` |
| `now_ms`、`elapsed_at_least`、`delay_init_retry` | B 叶子 helper | 是 | 启动轮询和基础初始化命令重试的时间辅助 |
| `wait_at_ready` | A 阶段函数 | 是 | 在 ready 总超时内轮询 `AT`，直到返回 `OK` |
| `hardware_reset` | A 阶段函数 | 是 | 获取 AT exclusive，flush RX，控制 EN 完成硬复位 |
| `register_urcs` | A 阶段函数 | 是 | 初始化并注册所有 Air780EP URC handler，失败时回滚 |
| `unregister_urcs` | C 清理/回滚函数 | 是 | 注销 URC 的日志包装 |
| `air780ep_unregister_urcs` | C 清理/回滚函数 | 是 | 从 AT Engine 注销 URC prefix 并清 handler 节点 |
| `run_basic_init_cmds` | A 阶段函数 | 是 | 执行基础 AT 初始化命令并每条重试 3 次 |
| `finish_modem_ready` | A 阶段函数 | 是 | 设置 READY、initialized，并投递 READY 事件 |
| `cpin_urc_handler` | D task/callback 入口 | 是 | 处理 CPIN URC、更新 SIM cache、投递 SIM 事件 |
| `reg_urc_handler` | D task/callback 入口 | 是 | 处理注册 URC、更新状态、投递注册事件 |
| `cgev_urc_handler` | D task/callback 入口 | 是 | 处理 PDN ACT/DEACT URC 并投递 PDP 事件 |
| `pdp_deact_urc_handler` | D task/callback 入口 | 是 | 处理 PDP 去激活 URC，清全部 PDP cache |
| `handle_msub_urc` | D task/callback 入口 | 是 | 解析 MQTT 下行 URC 并投递协议数据事件 |
| `post_mqtt_data_event` | A 阶段函数 | 是 | 构造 `MODEM_EVENT_PROTOCOL_DATA` 并转移 heap payload 所有权 |
| `post_pdp_deactivated_events` | A 阶段函数 | 是 | 批量投递 PDP 去激活事件 |
| `clear_all_pdp_cache` | C 清理/回滚函数 | 是 | 清所有 PDP active/IP，并返回原 active context |
| `set_state_nonblocking` | B 叶子 helper | 是 | URC 场景中尝试 0 tick 更新 modem state |
| `set_initialized`、`set_mqtt_data_enabled`、`mqtt_data_is_enabled`、`cache_sim_status` | B 叶子 helper | 否 | 带锁读写若干 Air780EP 状态位/cache |
| `query_cgatt`、`query_cgact`、`query_cgpaddr` | A 阶段函数 | 是 | PDP/附着查询的子流程 |
| `init_cmd_ctx`、`to_air780ep` | B 叶子 helper | 否 | 初始化命令上下文、container_of 转换 |
| `find_line_with_prefix`、`first_data_line` | B 叶子 helper | 否 | 从 AT response 中找数据行 |
| `copy_str_field`、`copy_str_field_strip_quotes` | B 叶子 helper | 否 | 拷贝字符串并检查截断 |
| `cid_valid`、`pdp_by_cid` | B 叶子 helper | 否 | CID 范围检查和 PDP cache 索引 |
| `at_arg_safe`、`escape_at_string` | B 叶子 helper | 否 | AT 字符串参数安全检查或转义 |
| `skip_prefix_value`、`parse_int_after_prefix`、`parse_two_ints_after_prefix` | B 叶子 helper | 否 | 通用响应行解析 |
| `map_reg_status`、`parse_registration_line`、`parse_registration_urc_line`、`consume_registration_extra_fields` | B 叶子 helper | 否 | 注册状态响应/URC 解析 |
| `parse_sim_status_line`、`sim_status_from_cme_error` | B 叶子 helper | 否 | SIM 状态字符串/CME 错误映射 |
| `parse_cid_from_line`、`looks_like_ip_addr` | B 叶子 helper | 否 | PDP URC CID 和 IP 格式检查 |
| `parse_cipping_line`、`parse_cipping_uint`、`calculate_ping_summary`、`ping_cmd_timeout_ms` | B 叶子 helper | 否 | Ping 响应解析和超时/summary 计算 |
| `parse_msub_direct` | B 叶子 helper | 是 | 解析 `+MSUB:` 并分配 topic/payload heap |
| `timeout_ticks` | B 叶子 helper | 否 | 毫秒转 FreeRTOS tick，避免正超时变 0 tick |

可能过度封装或可合并点：

| 项目 | 判断 |
|------|------|
| `call_no_arg` | 只是 `return fn(me)`，可以直接合并到 `modem_start/reset` |
| `unregister_urcs` | 只是调用 `air780ep_unregister_urcs()` 并记录 warning，可合并到错误路径或保留作日志语义 |
| `first_data_line` | 用 `find_line_with_prefix(response, "")` 实现“首行”，技巧性偏强，直接写首个 line 更易读 |
| `air780ep_start` 和 `air780ep_reset` | 两者主干重复，可抽成 `air780ep_run_ready_sequence()`；当前重复不影响正确性，但维护成本高 |
| 多个带锁 setter | `set_initialized/set_mqtt_data_enabled/cache_sim_status` 很薄，数量增多后会让状态修改分散；如果继续扩展，建议明确状态所有权规则 |

## 9. 错误码分类

| 类别 | 错误码 | 发生条件 | 性质 | 清理/恢复 | 返回给谁 |
|------|--------|----------|------|-----------|----------|
| 参数错误 | `ESP_ERR_INVALID_ARG` | NULL handle/output、非法 CID、APN/topic/payload 为空或不安全、QoS 不符合当前实现、命令字符串截断、ping 参数越界 | 调用者用错 | 通常不修改状态；若已分配临时 heap 则释放 | Core/Facade 调用者 |
| 状态错误 | `ESP_ERR_INVALID_STATE` | destroying 中调用、未 init 就调用业务 API、destroy 状态不允许、从 event task 自销毁/清 callback、event task 已停仍 post event | 调用顺序或并发错误 | 不自动恢复；destroy 部分错误会恢复 state 但不能恢复 event task | Core/Facade 或内部调用者 |
| 不支持 | `ESP_ERR_NOT_SUPPORTED` | ops 缺失、Air780EP TCPIP PDP 只支持 cid 1、MQTT QoS 2 当前不支持 | 能力边界 | 不恢复 | 调用者 |
| 资源错误 | `ESP_ERR_NO_MEM` | `calloc/malloc` 失败、queue/semaphore/task 创建失败、AT 字符串转义分配失败 | 运行时资源不足 | create/base init 会释放已创建资源；MQTT/Ping 临时分配失败会释放已分配项 | create 返回 NULL 或 API 返回错误 |
| 响应格式错误 | `ESP_ERR_INVALID_RESPONSE` | AT 返回缺少期望行、数值越界、IP 格式不合法、字符串截断、ping reply 数量不足 | 模块响应异常或解析不兼容 | 不重试；通常不改状态，部分 cache 可能保持旧值 | 调用者 |
| 超时错误 | `ESP_ERR_TIMEOUT` | event queue 满、AT OK 轮询总超时、SIM busy 轮询超时、AT Engine 命令超时 | 运行时故障或模块无响应 | start/reset 失败进入 ERROR；SIM busy 返回 UNKNOWN；AT Engine 自身做 RX flush | 调用者；event queue 满只日志/返回给 post 调用点 |
| 通信/外设/AT 业务错误 | `ESP_FAIL` 或下层返回码 | AT Engine 写失败、模块返回 ERROR/CME/CMS 后 `ensure_at_ok()` 映射失败、GPIO 配置/电平失败 | 运行时故障或模块拒绝命令 | 基础初始化命令有 3 次 retry；其他大多不重试 | 调用者 |
| 查找错误 | `ESP_ERR_NOT_FOUND` | `query_cgact/query_cgpaddr` 未找到指定 CID，或注销 URC 时 prefix 不存在 | 状态/响应缺失 | PDP context 查询中部分 NOT_FOUND 会作为 inactive 处理；URC 注销时 NOT_FOUND 被忽略 | 内部处理或调用者 |
| 严重不可恢复 | 无 `ESP_ERROR_CHECK` | 当前 modem 模块没有 panic/restart 路径 | 不适用 | 不调用 `esp_restart()` | 不适用 |

注意：`ensure_at_ok()` 会把 `AT_RESP_ERROR`、`AT_RESP_CME_ERROR`、`AT_RESP_CMS_ERROR` 统一映射为 `ESP_FAIL`，只在日志里保留 CME/CMS code。唯一例外是 `get_sim_status()` 会专门把 SIM 相关 CME code 映射成 `MODEM_SIM_*`。

## 10. 故障恢复机制

### Level 1：初始化失败回滚

| 失败位置 | 已有资源 | 回滚方式 |
|----------|----------|----------|
| `modem_air780ep_create()` 参数/calloc 失败 | 无 | 返回 NULL |
| `modem_base_init()` 任一步失败 | self 以及部分 base 资源 | `modem_base_deinit()` 判空释放，外层 free self |
| `register_urcs()` 中途失败 | 已成功注册的若干 URC prefix | 循环 unregister 已注册 prefix；若 rollback 成功则清 handler 并 `urc_registered=false` |
| `air780ep_start/reset()` 失败 | 对象、base 资源、event task 仍存在，URC 可能已注册 | 如果 URC 是本轮新注册则注销；`initialized=false`；state=ERROR |

### Level 2：运行时软恢复

| 故障 | 恢复动作 | 是否 retry | 是否上报 |
|------|----------|------------|----------|
| 基础初始化 AT 命令失败 | 同一命令最多重试 3 次 | 是，仅 init cmds | 最终失败返回并置 ERROR |
| SIM busy | 1 秒轮询间隔后重试 `AT+CPIN?`，最多 10 秒 | 是 | 超时返回 `ESP_ERR_TIMEOUT`，SIM cache 置 UNKNOWN |
| 注册查询某一路失败 | 从 CEREG fallback 到 CGREG，再到 CREG | 是，换查询路径 | 最终返回最后错误或 UNKNOWN |
| AT 命令超时 | AT Engine 做 flush RX/queue/line buffer 和 epoch 隔离 | Modem 不重试 | 返回下层错误 |
| PDP 去激活 URC | 清 PDP cache，关闭 MQTT data，置 READY | 不重试 | 投递 PDP_DEACTIVATED |
| MQTT 下行 post 失败 | handler 释放 topic/payload | 不重试 | 仅 warning |
| event queue 满 | `modem_post_event()` 返回 `ESP_ERR_TIMEOUT`，调用点多为 warning | 不重试 | 事件丢弃 |

### Level 3：模块级重启

当前有显式 `modem_reset()`，它会执行：

```text
set INITIALIZING
-> hardware_reset
-> wait AT OK
-> run_basic_init_cmds
-> register_urcs
-> set READY + post READY
```

这属于 Modem 层的模块级硬复位恢复，但不是完整对象重建。完整重建仍需要上层按反向顺序执行：

```text
modem_destroy
-> modem_air780ep_create
-> modem_start
-> register callback
```

当前没有最大失败次数，没有 ERROR 状态下的自动退避循环，也不会调用 `esp_restart()`。网络断线后的重连策略应由 Core 决策。

## 11. 事件流 / 数据流

### 下行命令数据流

```text
Core 调用 modem_* API
-> modem.c wrapper 参数检查 + check_ready + ops 存在检查
-> Air780EP ops 构造 AT 命令和 at_cmd_options
-> at_engine_send_cmd / send_cmd_with_options / send_cmd_with_payload
-> AT Engine 写 UART 并等待响应
-> Air780EP 解析 at_response_t
-> 更新 cache/state
-> 必要时 modem_post_event
-> API 返回 esp_err_t 给 Core
```

### URC 上行事件流

```text
LTE 模块输出 URC
-> AT Engine RX task 组完整行
-> prefix 匹配到 Air780EP handler
-> Air780EP URC handler 解析语义
-> 更新 SIM/REG/PDP/MQTT 本地状态
-> modem_post_event 投递 modem_event_t 到 event_queue
-> modem event_task 取事件
-> 调用 Core 注册的 modem_event_callback_t
-> 如果是 PROTOCOL_DATA，callback 返回后释放 topic/payload heap
```

### MQTT 下行数据流

```text
Air780EP 输出 +MSUB:<topic>,<len>,<payload>
-> handle_msub_urc
-> parse_msub_direct 分配 topic/payload heap
-> mqtt_data_enabled 为 false: 直接 free 并丢弃
-> mqtt_data_enabled 为 true: post MODEM_EVENT_PROTOCOL_DATA
-> event_task 调用 Core callback
-> release_event_payload 释放 heap
```

模块类型判断：

| 类型 | 结论 |
|------|------|
| 主动轮询型 | 不是主模型；启动 AT ready 和 SIM busy 分支使用短期轮询，event task 用 100 ms timeout 检查 stop |
| 事件驱动型 | 是，URC 由 AT Engine RX task 驱动 |
| callback 驱动型 | 是，AT Engine URC callback 进入 Modem，Modem event callback 上报 Core |
| task + queue 驱动型 | 是，Modem 自己用 `event_queue + event_task` 把 AT RX task 与 Core callback 解耦 |
| 混合型 | 是，下行命令是阻塞 API，上行 URC 是 callback + queue + task |

## 12. 最终总结：模块卡片

模块名：Modem Adapter / Air780EP Modem

一句话职责：把 Core 的网络、PDP、MQTT、Ping 语义操作翻译成 Air780EP AT 命令，并把 AT Engine 的 URC 翻译成异步 `modem_event_t` 上报。

核心 API：

| API 类别 | 说明 |
|----------|------|
| create | `modem_air780ep_create()` 分配对象和事件资源，event task 已启动 |
| start | `modem_start()` 硬复位、轮询 `AT` 到 `OK`、跑基础 AT 命令、注册运行期 URC，成功后模块 READY |
| stop | 没有独立 stop；`modem_deactivate_pdp()` 停数据面，`modem_mqtt_disconnect()` 停协议会话，`modem_mqtt_tcp_disconnect()` 关闭 MQTT TCP 通道 |
| send/process | `get_*` 查询、`set_apn/activate_pdp/deactivate_pdp`、`mqtt_*`、`ping` 都是语义命令入口 |
| recover | `modem_reset()` 硬复位模块并恢复基础 AT 环境；基础初始化命令 retry、SIM busy 定时轮询是局部软恢复 |
| deinit | `modem_destroy()` 停 event task、注销 URC、删除 sema/queue/lock、free 对象 |

生命周期：

```text
UNINIT -> CREATED -> INITIALIZING -> READY -> REGISTERING/REGISTERED/PDP_ACTIVE -> READY/ERROR -> DESTROYING -> UNINIT
```

核心状态：

```text
MODEM_STATE_CREATED
MODEM_STATE_INITIALIZING
MODEM_STATE_READY
MODEM_STATE_REGISTERING
MODEM_STATE_REGISTERED
MODEM_STATE_PDP_ACTIVE
MODEM_STATE_ERROR
MODEM_STATE_DESTROYING
```

拥有资源：

```text
Air780EP 对象、base lock、event queue、event task、event task done sema、event callback done sema、内嵌 URC handler 节点、PDP/SIM/REG/signal/MQTT 状态缓存、临时 AT 命令 heap、MQTT 下行事件 heap payload
```

运行时入口：

```text
event_task
cpin_urc_handler
reg_urc_handler
cgev_urc_handler
pdp_deact_urc_handler
handle_msub_urc
modem_event_callback_t
```

正常主流程：

```text
Core 调 modem_* 语义 API
-> wrapper 检查状态
-> ops 分发到 Air780EP
-> 构造 AT 命令
-> AT Engine 执行并返回 response
-> Air780EP 解析、更新 cache/state
-> 必要时通过 event_queue/event_task 上报 Core
```

主要错误类型：

```text
参数错误、状态/并发错误、能力不支持、内存不足、AT 响应格式错误、AT ready/SIM/AT 命令超时、GPIO/AT 通信失败、URC/event queue 上报失败
```

故障恢复策略：

```text
create 失败按已创建资源回滚；URC 注册失败回滚已注册 prefix；start/reset 失败进入 ERROR；启动期硬复位后轮询 `AT` 到 `OK`，不使用 `RDY` 作为 gate；基础初始化命令重试 3 次；SIM busy 按固定间隔轮询重试；运行期可用 modem_reset 做模块硬复位；不做自动重连/退避/esp_restart，这些归 Core。
```

我面试时可以这样讲：

Modem Adapter 是我在 AT Engine 和 Core 之间做的一层模块适配，它用 `modem_t + modem_ops_t` 做 C 语言多态，让 Core 调的是 `modem_activate_pdp()`、`modem_mqtt_publish()` 这种语义 API，而不是直接拼 Air780EP AT 字符串。下行命令路径是阻塞式的：wrapper 做参数和状态检查，Air780EP ops 构造 AT 命令，AT Engine 返回响应后再解析成 SIM/注册/PDP/MQTT/Ping 的值对象和状态缓存。上行事件路径是事件驱动的：AT Engine RX task 只触发短小 URC handler，handler 把原始 URC 翻译成 `modem_event_t` 投递到 Modem 自己的 queue，再由 event task 调 Core callback，避免 Core 逻辑阻塞 AT RX task。故障恢复上，启动流程会硬复位后轮询 `AT` 到 `OK`，不使用 `RDY` 作为 gate；基础初始化命令有 retry，SIM busy 会按固定间隔轮询重试，运行期可以通过 `modem_reset()` 做模块硬复位；但自动重连和退避不放在 Modem，而交给 Core 网络状态机。代码走查时我会主动指出当前实现的风险：`GPIO_NUM_NC` 不拉 EN 但仍会进入 `AT OK` 轮询、`modem_start()` 可重复触发 reset 语义不够清楚、destroy 在 URC 注销失败后会留下 event task 已停的半销毁对象。
