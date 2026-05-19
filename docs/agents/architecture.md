# 架构概览

esp-lwlte 采用严格的**五层分层架构**，层间通过 ops 表 + 回调注册实现单向依赖与数据上行。

---

## 1. 分层架构

```
                     调用方向（向下）
    ┌──────────────────────────────────────────────────────┐
    │                                                    │
    ▼                                                    │
┌──────────┐  ┌─────────────────────────────────────┐     │
│   应用层  │  │  业务逻辑，零硬件引用，零模块引用          │     │
│   App    │  │  只依赖 Core Service 公共接口            │     │
└────┬─────┘  └─────────────────────────────────────┘     │
     │                                                    │
     ▼                                                    │
┌──────────┐  ┌─────────────────────────────────────┐     │
│ 核心服务  │  │  网络状态机、PDP 管理、连接/重连/保活    │     │
│  Core    │  │  MQTT/HTTP 等上层协议                   │     │
│ Service  │  │  通过 modem_ops 表操作模块，不知道下面是 │     │
└────┬─────┘  │  哪个具体模块                            │     │
     │        └─────────────────────────────────────┘     │
     ▼                                                    │
┌──────────┐  ┌─────────────────────────────────────┐     │
│ 模块适配  │  │  定义 modem_ops 抽象接口（虚函数表）     │     │
│  Modem   │  │  ├─ modem_interface  (基类)             │     │
│ Adapter  │  │  └─ modem_air780ep  (子类实现)          │     │
└────┬─────┘  │  封装具体 AT 指令差异，翻译 URC           │     │
     │        └─────────────────────────────────────┘     │
     ▼                                                    │
┌──────────┐  ┌─────────────────────────────────────┐     │
│  AT 引擎  │  │  通用 AT 协议引擎                      │     │
│    AT    │  │  发命令、收响应、行解析、超时重试          │     │
│  Engine  │  │  URC 模式匹配与回调分发                   │     │
└────┬─────┘  │  不关心具体指令含义                        │     │
     │        └─────────────────────────────────────┘     │
     ▼                                                    │
┌──────────┐  ┌─────────────────────────────────────┐     │
│  移植层  │  │  封装 ESP-IDF 平台 API                 │     │
│   Port   │  │  UART、线程、互斥锁、队列、定时器、GPIO   │     │
└──────────┘  │  不涉及 AT 协议或模块逻辑                │     │
              └─────────────────────────────────────┘     │
                                                          │
                     回调方向（向上）                        │
    ┌──────────────────────────────────────────────────┐   │
    │  URC 事件、状态变更通知、数据到达                     │   │
    │  每层通过注册回调与上层通信                           │   │
    └──────────────────────────────────────────────────┘   │
```

---

## 2. 调用规则

### 2.1 核心规则

| 类别 | 规则 | 示例 |
|------|------|------|
| **函数调用** | 只能调紧邻的下一层 | Core 不能调 `port_uart_write()` |
| **类型/句柄引用** | 可以持有 port 层的类型 | Core 可以持有 `mutex_t`（通过初始化链传下来的） |
| **配置数据** | 自上而下单向传递 | App config → Core → Modem → AT Engine → Port |
| **事件/URC** | 自下而上通过回调冒泡 | Port 字节流 → AT Engine URC → Modem 事件 → Core 状态 → App 通知 |

### 2.2 为什么禁止跨层调用

允许 Core 直接调 port，很快 port 层会变成所有人的工具库，层间边界形同虚设。port 提供的基础类型（mutex、queue、thread）确实是全局需要的——解决方案是让它们通过 **初始化链往下传**，调用者只持有产物，不直接调 port 的创建函数。

**一句话口诀：可以持有 port 的产物，不能调用 port 的函数。**

### 2.3 每层的能力边界

| 层 | 可以调用的层 | 可以持有的类型 | 可以知道的符号 |
|----|------------|-------------|-------------|
| App | Core public API | Core 句柄、App 自身定义的类型 | `lwlte_core.h`、`lwlte_mqtt.h` 等公共头文件 |
| Core | Modem ops 统一接口 | Port 产物（mutex/queue/thread）、Core 自身定义的类型 | modem_ops 表、port 类型前向声明 |
| Modem | AT Engine API | AT Engine 句柄、Modem 自身定义的类型 | AT Engine 公共头文件 |
| AT Engine | Port ops 统一接口 | Port 产物（UART 句柄、mutex、timer）、AT Engine 自身类型 | Port ops 表 |
| Port | ESP-IDF API | FreeRTOS / UART / GPIO 等原生类型 | ESP-IDF 头文件 |

---

## 3. 数据流：AT 指令下行与 URC 上行

### 3.1 AT 指令下行（调用链）

```
App:  不需要知道 AT 指令存在
         │
Core:  modem->ops->set_apn(modem, "cmnet")
         │  通过 modem_ops 表调用（不知道下面是哪个模块）
         ▼
Modem: at_engine->send_cmd(at, "AT+CGDCONT=1,\"IP\",\"cmnet\"")
         │  将语义操作翻译为具体 AT 指令字符串
         ▼
AT Eng: port->ops->uart_write(port, buf, len)
         │  加上 \r\n，处理超时重试
         ▼
Port:  uart_write_bytes(uart_num, buf, len)
         │  直接调 ESP-IDF UART API
         ▼
       硬件 TX 引脚
```

### 3.2 URC 上行（回调链）

```
        硬件 RX 引脚
         │
Port:  uart_read_bytes() → 原始字节流
         │  回调：data_handler(uint8_t *data, size_t len)
         ▼
AT Eng: 行拼接 → 匹配 URC 前缀 → 分发给已注册的 handler
         │  "AT Engine 只做模式匹配，不知道 URC 的模块含义"
         │  持有：urc_handler_t *handlers[]  (按前缀字符串注册)
         │  回调：urc_handler(prefix, line)
         ▼
Modem:  "翻译"：原始 URC 字符串 → 模块级语义事件
         │  "+CGEV: ME PDN DEACT 1" → MODEM_EVENT_PDP_DEACT
         │  回调：modem_event_handler(event_id, data)
         ▼
Core:   处理事件 → 更新网络状态机 → 通知 App
         │  MODEM_EVENT_PDP_DEACT → APP_EVENT_NETWORK_LOST
         │  回调：app_event_cb(event_id, data)
         ▼
App:    业务响应（重连、告警、降级等）
```

### 3.3 回调注册时序

回调在初始化阶段由上层向下层注册，保证运行时调用方向始终是单向的：

```c
/* 初始化时：逐层注册回调 */
int modem_init(modem_t *me, at_engine_t *at) {
    me->at = at;
    at_engine_register_urc(at, "+CGEV:", modem_urc_handler, me);   /* Modem → AT Engine */
    at_engine_register_urc(at, "+CEREG:", modem_urc_handler, me);
    return 0;
}

int core_init(core_t *me, modem_t *modem) {
    me->modem = modem;
    modem_register_event(modem, MODEM_EVENT_ALL, core_event_handler, me);  /* Core → Modem */
    return 0;
}
```

运行时 URC 沿已注册路径冒泡，不再有向下的回调引用。

---

## 4. 各层职责详述

### 4.1 Port 层（移植层）

| 维度 | 说明 |
|------|------|
| **职责** | 封装 ESP-IDF 平台 API，向上层提供统一的操作接口 |
| **知道什么** | UART 引脚、波特率、FreeRTOS API、GPIO 复位、定时器 |
| **不知道什么** | AT 协议、LTE 模块型号、网络状态、业务逻辑 |
| **对外接口** | `struct port_ops { uart_write, uart_read, mutex_create, thread_create, ... }` |
| **OOP 角色** | 平台抽象接口 — 换 RTOS 时只换这一层的 ops 实现 |

Port 层是唯一 `#include "freertos/FreeRTOS.h"`、`#include "driver/uart.h"` 的地方。任何 ESP-IDF 头文件不应出现在 Port 层以上的文件中。

### 4.2 AT Engine 层（AT 引擎层）

| 维度 | 说明 |
|------|------|
| **职责** | 通用 AT 协议引擎：发命令、收响应、行解析、超时重试、URC 检测与分发 |
| **知道什么** | AT 协议格式（`AT+XXX\r\n`、`\r\nOK\r\n`、`\r\nERROR\r\n`）、超时机制、URC 前缀识别 |
| **不知道什么** | 具体 AT 指令的含义、模块型号、网络状态 |
| **对外接口** | `at_send_cmd(at, cmd, timeout)` → `at_response_t`；`at_register_urc(at, prefix, handler)` |
| **OOP 角色** | 协议栈 — 所有 AT 模块共用的基础设施 |

AT Engine 是一个"聪明的邮差"：会按地址送信、收信、识别加急件（URC），但从不拆信看内容。

### 4.3 Modem Adapter 层（模块适配层）

这一层由两部分组成：**接口定义（基类）** + **模块实现（子类）**。

#### 4.3.1 Modem Interface（模块抽象接口）

| 维度 | 说明 |
|------|------|
| **职责** | 定义 `modem_ops` 虚函数表，规定 Core 层可以调用的操作 |
| **知道什么** | 模块需要有 init、get_signal、get_imei、connect 等操作 |
| **不知道什么** | 每个操作的具体 AT 指令格式 |
| **对外接口** | `struct modem_ops { .init, .get_signal, .get_imei, .connect, .set_apn, ... }` |

#### 4.3.2 Modem Impl（具体模块实现）

| 维度 | 说明 |
|------|------|
| **职责** | 实现 `modem_ops` 表中每个方法，将语义操作翻译为具体 AT 指令 |
| **知道什么** | 该模块的初始化序列、AT 指令格式、URC 字符串的语义含义 |
| **不知道什么** | 网络状态机逻辑、上层协议、应用业务 |
| **OOP 角色** | 子类 — 继承 modem_interface，实现虚函数 |

**Modem 层的核心价值**：不同 LTE 模块最大的差异就是 AT 指令格式和 URC 格式。把差异封装在 modem_impl 中，通过统一的 modem_ops 暴露给 Core，上层零感知。

```c
/* 不同模块实现同一个接口的不同行为 */
/* Air780EP: */
static esp_err_t air780ep_get_signal(modem_t *me, int *rssi, int *ber) {
    at_response_t *resp = me->at->ops->send_cmd(me->at, "AT+CSQ", 3000);
    /* 解析 +CSQ: <rssi>,<ber> */
}

/* SIM800: 同一个方法，不同 AT 指令 */
static esp_err_t sim800_get_signal(modem_t *me, int *rssi, int *ber) {
    at_response_t *resp = me->at->ops->send_cmd(me->at, "AT+CSQ", 5000);  /* 超时不同 */
    /* 解析格式可能不同 */
}
```

### 4.4 Core Service 层（核心服务层）

| 维度 | 说明 |
|------|------|
| **职责** | 网络状态机、PDP 激活/去激活管理、连接建立/重连/保活、MQTT/HTTP 客户端 |
| **知道什么** | 网络协议、状态迁移规则、重试策略、保活机制、MQTT 协议 |
| **不知道什么** | 具体 AT 指令格式、模块型号、底层硬件 |
| **对外接口** | `lwlte_core_create()`、`lwlte_core_start()`、`lwlte_mqtt_publish()`、事件回调注册 |
| **OOP 角色** | 业务逻辑 — 调用 modem 接口，不关心实现 |

### 4.5 App 层（应用层）

| 维度 | 说明 |
|------|------|
| **职责** | 用户业务逻辑 |
| **知道什么** | Core Service 提供的公共 API |
| **不知道什么** | 硬件、模块、AT 指令、协议细节 |
| **规则** | 代码中不出现任何 ESP-IDF 头文件、AT 指令字符串、模块型号 |

---

## 5. 初始化与装配

### 5.1 初始化链

初始化从底向上，回调从顶向下注册：

```
Board Init
    │
    ├─ 1. lwlte_port_espidf_create()          → 创建 Port 实例
    │       └─ 注入 espidf_ops（static const）
    │
    ├─ 2. lwlte_at_engine_create(port)        → 创建 AT Engine
    │       └─ 传入 port 句柄，AT Engine 通过 port->ops 操作硬件
    │
    ├─ 3. lwlte_modem_air780ep_create(at)     → 创建 Modem 实例
    │       ├─ 传入 at 句柄，Modem 通过 at->ops 发 AT 指令
    │       └─ modem 内部注册 URC handler 到 at engine
    │
    ├─ 4. lwlte_core_create(config, modem)    → 创建 Core 实例
    │       ├─ 传入 modem 句柄，Core 通过 modem->ops 操作模块
    │       └─ core 内部注册事件回调到 modem
    │
    └─ 5. app_init(core)                      → App 持有 Core 句柄
            └─ app 注册业务事件回调到 core
```

### 5.2 Board Init 模式

Board Init 是唯一认识所有具体类型的文件，负责装配整个依赖树：

```c
/* lwlte_board_init.c — 唯一认识硬件的文件 */

lwlte_core_t *g_core;   /* 全局句柄（仅此一个） */

int lwlte_board_init(void)
{
    /* 1. 底：创建平台实现 */
    lwlte_port_t *port = lwlte_port_espidf_create();
    if (!port) return -1;

    /* 2. AT 引擎 */
    lwlte_at_engine_t *at = lwlte_at_engine_create(port);
    if (!at) goto err_port;

    /* 3. 模块适配（换模块只需换这一行） */
    lwlte_modem_t *modem = lwlte_modem_air780ep_create(at);
    if (!modem) goto err_at;

    /* 4. 核心服务 */
    lwlte_core_config_t config = {
        .apn     = CONFIG_LWLTE_APN,
        .auto_conn = true,
        .keepalive = 60,
    };
    g_core = lwlte_core_create(&config, modem);
    if (!g_core) goto err_modem;

    /* 5. 注册 App 事件回调 */
    lwlte_core_register_event(g_core, LWLTE_EVENT_ALL, app_event_handler, NULL);
    return 0;

err_modem:
    lwlte_modem_destroy(modem);
err_at:
    lwlte_at_engine_destroy(at);
err_port:
    lwlte_port_destroy(port);
    return -1;
}
```

**换模块时只改第 3 步的一行代码。** 换平台时只改第 1 步。其余代码零改动。

---

## 6. OOP 模式在架构中的落地

| OOP 概念 | 架构落点 | 具体表现 |
|----------|---------|---------|
| **封装** | 全部五层 | 每层对外只暴露句柄 + 操作函数，内部结构体不公开 |
| **继承** | Modem 层 | modem_interface（基类）→ modem_air780ep（子类），struct 嵌套 + container_of |
| **多态** | Modem 层 + Port 层 | modem_ops 表 → 不同模块同一接口不同行为；port_ops 表 → 不同平台 |
| **抽象类** | Modem Interface | 定义 ops 表但不实现，子类必须填充 |
| **vptr 注入** | 各层 init 函数 | `me->ops = ops` 在 init 时完成，`static const` 存于 `.rodata` |
| **板级装配** | Board Init | 唯一认识所有具体类型的文件，组装依赖树 |
| **零硬件引用** | App 层 + Core 层 | 不 include 任何 ESP-IDF 或模块私有头文件 |

---

## 7. 与旧架构的关键区别

| 旧架构 | 新架构 |
|--------|--------|
| 三层（中间件 + Port + App） | 五层（Port → AT Engine → Modem → Core → App） |
| Port 层混杂了 AT 命令生成 | AT 协议引擎独立成层 |
| 模块差异散落在各处 | 模块差异集中在 Modem Adapter 层，通过 ops 表统一 |
| 直接调用 `lwlte_sys_xxx()` | 通过 `platform->ops->xxx()` 间接调用 |
| 全局单例 `s_ctx` | create 返回句柄，支持多实例 |
| URC 处理嵌入在 FSM 中 | URC 沿回调链逐层翻译上传 |
| core 同时认识 AT 和平台 | core 只认识 modem_ops，不跨层 |

---

## 8. 待细化问题

以下问题值得在进入编码前进一步讨论：

1. **FSM 放置**：网络状态机放在 Core 层是一整块，还是拆分为 Core（高层状态）+ Modem Impl（AT 级状态）两层 FSM？两者如何同步？

2. **线程模型**：哪些层拥有自己的线程？AT Engine 需要一个接收线程？Core 需要一个 FSM 线程？线程数是否有硬约束？

3. **错误传播**：AT Engine 返回 `TIMEOUT` 和 Modem 返回 `NO_RESPONSE` 和 Core 返回 `NETWORK_ERROR` 之间的错误码映射规则是什么？

4. **资源生命周期**：deinit/destroy 的顺序如何保证反向释放？失败回滚（goto err_xxx）模式是否作为项目统一规范？

5. **跨层数据结构**：AT Engine → Modem 的 `at_response_t`、Modem → Core 的 `modem_event_t`、Core → App 的 `app_event_t` 各包含什么字段？

6. **重试策略归属**：AT 指令发出去没有响应——是 AT Engine 层自动重试，还是 Modem 层决定要不要重试，还是 Core 层判断后下指令重试？

7. **测试策略**：每层通过注入 mock ops 表做单元测试，但跨层集成测试（如 Modem + AT Engine 联调）要测到什么粒度？

8. **并发模型**：Port 层提供的队列/互斥锁是否假设为线程安全？AT Engine 接收线程和 Core FSM 线程之间的数据竞争谁来负责？
