# 架构概览

esp-lwlte 采用**四层分层架构**，层间通过 ops 表 + 回调注册实现单向依赖与数据上行。

本组件直接构建在 ESP-IDF 之上，所有层都可以直接使用 ESP-IDF / FreeRTOS API（`xTaskCreate`、`xQueueCreate`、`vTaskDelay`、`uart_write_bytes` 等），不做平台抽象封装。这与 Espressif 官方组件（esp-mqtt、button、esp-sr 等）的设计哲学一致。

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
│  AT 引擎  │  │  通用 AT 协议引擎 + UART 硬件操作       │     │
│    AT    │  │  发命令、收响应、行解析、超时重试          │     │
│  Engine  │  │  UART 中断/轮询接收、TX 写入              │     │
└──────────┘  │  URC 模式匹配与回调分发                   │     │
              │  不关心具体指令含义                        │     │
              └─────────────────────────────────────┘     │
                                                          │
                     回调方向（向上）                        │
    ┌──────────────────────────────────────────────────┐   │
    │  URC 事件、状态变更通知、数据到达                     │   │
    │  每层通过注册回调与上层通信                           │   │
    └──────────────────────────────────────────────────┘   │
```

---

## 2. 设计哲学：直接建在 ESP-IDF 上

### 2.1 不做的事

- **不封装 FreeRTOS API**：没有 `lwlte_thread_create()`，直接用 `xTaskCreate()`
- **不封装 UART API**：没有 `lwlte_uart_write()`，直接用 `uart_write_bytes()`
- **不封装互斥锁/队列**：没有 `lwlte_mutex_create()`，直接用 `xSemaphoreCreateMutex()`
- **不抽象平台层**：没有 `port_ops` 表，没有"换 RTOS 只需换 port 实现"的幻想

### 2.2 这样做是对的，因为

Espressif 官方组件（esp-mqtt、button、esp-sr、LCD 驱动）无一例外直接在 `.c` 文件里 `#include "freertos/task.h"` 然后裸调 `xTaskCreate`、`vTaskDelay`。它们认为"组件就是跑在 ESP-IDF 上的，没必要抽象"。

esp-lwlte 是一个 ESP-IDF 组件，不是通用嵌入式库。目标平台只有一个，目标 RTOS 只有一个。

---

## 3. 调用规则

### 3.1 核心规则

| 类别 | 规则 | 示例 |
|------|------|------|
| **函数调用** | 只能调紧邻的下一层 | Core 不能调 `uart_write_bytes()` |
| **ESP-IDF 引用** | 任何层都可以直接 `#include` ESP-IDF/FreeRTOS 头文件 | AT Engine 直接 include `driver/uart.h`、`freertos/task.h` |
| **配置数据** | 自上而下单向传递 | App config → Core → Modem → AT Engine |
| **事件/URC** | 自下而上通过回调冒泡 | AT Engine URC → Modem 事件 → Core 状态 → App 通知 |

### 3.2 跨层调用仍然禁止

虽然所有层都可以使用 ESP-IDF API，但**层间函数调用仍然只能向下调紧邻的下一层**。Core 不能直接调 AT Engine 的函数，必须通过 Modem Adapter 间接调用。

不封装 FreeRTOS API 不等于可以跨层调业务接口。

### 3.3 每层的能力边界

| 层 | 可以调用的层 | 可以使用的 ESP-IDF API | 可以知道的符号 |
|----|------------|----------------------|-------------|
| App | Core public API | 不限（但实践中只用于业务逻辑无关的初始化） | `lwlte_core.h`、`lwlte_mqtt.h` 等公共头文件 |
| Core | Modem ops 统一接口 | 不限（FreeRTOS task/queue/timer、esp_event 等） | modem_ops 表、Core 自身定义的类型 |
| Modem | AT Engine API | 不限（driver/gpio.h 用于模块复位/电源控制等） | AT Engine 公共头文件、Modem 自身定义的类型 |
| AT Engine | 无下层（最底层） | 不限（driver/uart.h、FreeRTOS task/queue 等，直接操作硬件） | AT Engine 自身类型 |

---

## 4. 数据流：AT 指令下行与 URC 上行

### 4.1 AT 指令下行（调用链）

```
App:  不需要知道 AT 指令存在
         │
Core:  modem->ops->set_apn(modem, "cmnet")
         │  通过 modem_ops 表调用（不知道下面是哪个模块）
         ▼
Modem: at_engine_send_cmd(at, "AT+CGDCONT=1,\"IP\",\"cmnet\"", 3000)
         │  将语义操作翻译为具体 AT 指令字符串
         ▼
AT Eng: uart_write_bytes(uart_num, buf, len)    ← 直接调 ESP-IDF UART API
         │  加上 \r\n，处理超时重试
         ▼
        硬件 TX 引脚
```

### 4.2 URC 上行（回调链）

```
        硬件 RX 引脚
         │
AT Eng: uart_read_bytes() → 行拼接 → 匹配 URC 前缀 → 分发给已注册的 handler
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

### 4.3 回调注册时序

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

## 5. 各层职责详述

### 5.1 AT Engine 层（AT 引擎层）

AT Engine 是四层架构的最底层，直接操作 UART 硬件。

| 维度 | 说明 |
|------|------|
| **职责** | 通用 AT 协议引擎 + UART 硬件操作：发命令、收响应、行解析、超时重试、URC 检测与分发 |
| **知道什么** | AT 协议格式（`AT+XXX\r\n`、`\r\nOK\r\n`、`\r\nERROR\r\n`）、超时机制、URC 前缀识别、UART 引脚与波特率 |
| **不知道什么** | 具体 AT 指令的含义、模块型号、网络状态 |
| **对外接口** | `at_engine_send_cmd(at, cmd, timeout)` → `at_response_t`；`at_engine_register_urc(at, prefix, handler)` |
| **内部实现** | 创建 UART RX 接收线程（`xTaskCreate`），直接调用 `uart_write_bytes` / `uart_read_bytes`，用 `xQueue` 传递接收数据 |
| **OOP 角色** | 协议栈 + 硬件驱动 — 所有 AT 模块共用的基础设施 |

AT Engine 是一个"聪明的邮差"：会按地址送信、收信、识别加急件（URC），但从不拆信看内容。同时它也负责维护邮路（UART 硬件）本身。

AT Engine 的配置直接包含 UART 硬件参数：

```c
typedef struct {
    uart_port_t uart_num;        // UART 端口号（如 UART_NUM_1）
    int tx_pin;                  // TX GPIO
    int rx_pin;                  // RX GPIO
    int baud_rate;               // 波特率（如 115200）
    int rx_buf_size;             // 接收缓冲区大小
    int rx_task_stack;           // 接收任务栈大小
    int rx_task_priority;        // 接收任务优先级
} at_engine_config_t;
```

### 5.2 Modem Adapter 层（模块适配层）

这一层由两部分组成：**接口定义（基类）** + **模块实现（子类）**。

#### 5.2.1 Modem Interface（模块抽象接口）

| 维度 | 说明 |
|------|------|
| **职责** | 定义 `modem_ops` 虚函数表，规定 Core 层可以调用的操作 |
| **知道什么** | 模块需要有 init、get_signal、get_imei、connect 等操作 |
| **不知道什么** | 每个操作的具体 AT 指令格式 |
| **对外接口** | `struct modem_ops { .init, .get_signal, .get_imei, .connect, .set_apn, ... }` |

#### 5.2.2 Modem Impl（具体模块实现）

| 维度 | 说明 |
|------|------|
| **职责** | 实现 `modem_ops` 表中每个方法，将语义操作翻译为具体 AT 指令 |
| **知道什么** | 该模块的初始化序列、AT 指令格式、URC 字符串的语义含义、模块硬件控制（GPIO 复位、电源） |
| **不知道什么** | 网络状态机逻辑、上层协议、应用业务 |
| **OOP 角色** | 子类 — 继承 modem_interface，实现虚函数 |

Modem Impl 可以直接使用 `driver/gpio.h` 控制模块的 RESET/PWRKEY 引脚。

**Modem 层的核心价值**：不同 LTE 模块最大的差异就是 AT 指令格式和 URC 格式。把差异封装在 modem_impl 中，通过统一的 modem_ops 暴露给 Core，上层零感知。

```c
/* 不同模块实现同一个接口的不同行为 */
/* Air780EP: */
static esp_err_t air780ep_get_signal(modem_t *me, int *rssi, int *ber) {
    at_response_t *resp = at_engine_send_cmd(me->at, "AT+CSQ", 3000);
    /* 解析 +CSQ: <rssi>,<ber> */
}

/* SIM800: 同一个方法，不同 AT 指令 */
static esp_err_t sim800_get_signal(modem_t *me, int *rssi, int *ber) {
    at_response_t *resp = at_engine_send_cmd(me->at, "AT+CSQ", 5000);  /* 超时不同 */
    /* 解析格式可能不同 */
}
```

### 5.3 Core Service 层（核心服务层）

| 维度 | 说明 |
|------|------|
| **职责** | 网络状态机、PDP 激活/去激活管理、连接建立/重连/保活、MQTT/HTTP 客户端 |
| **知道什么** | 网络协议、状态迁移规则、重试策略、保活机制、MQTT 协议 |
| **不知道什么** | 具体 AT 指令格式、模块型号、底层硬件 |
| **对外接口** | `lwlte_core_create()`、`lwlte_core_start()`、`lwlte_mqtt_publish()`、事件回调注册 |
| **OOP 角色** | 业务逻辑 — 调用 modem 接口，不关心实现 |

Core 层可以使用 FreeRTOS 任务/队列/定时器来实现 FSM 线程和保活定时器，直接调 `xTaskCreate` 等 API。

### 5.4 App 层（应用层）

| 维度 | 说明 |
|------|------|
| **职责** | 用户业务逻辑 |
| **知道什么** | Core Service 提供的公共 API |
| **不知道什么** | 硬件、模块、AT 指令、协议细节 |
| **规则** | 代码中不出现 AT 指令字符串、模块型号 |

---

## 6. 初始化与装配

### 6.1 初始化链

初始化从底向上，回调从顶向下注册：

```
Board Init
    │
    ├─ 1. at_engine_create(&uart_config)        → 创建 AT Engine
    │       └─ 内部创建 UART RX 线程，初始化 UART 硬件
    │
    ├─ 2. modem_air780ep_create(at)             → 创建 Modem 实例
    │       ├─ 传入 at 句柄，Modem 通过 at_engine_send_cmd() 发 AT 指令
    │       └─ modem 内部注册 URC handler 到 at engine
    │
    ├─ 3. lwlte_core_create(&config, modem)     → 创建 Core 实例
    │       ├─ 传入 modem 句柄，Core 通过 modem->ops 操作模块
    │       └─ core 内部注册事件回调到 modem
    │
    └─ 4. app_init(core)                        → App 持有 Core 句柄
            └─ app 注册业务事件回调到 core
```

### 6.2 Board Init 模式

Board Init 是唯一认识所有具体类型的文件，负责装配整个依赖树：

```c
/* lwlte_board_init.c — 唯一认识硬件和模块型号的文件 */

lwlte_core_t *g_core;   /* 全局句柄（仅此一个） */

int lwlte_board_init(void)
{
    /* 1. 底：创建 AT Engine（直接传入 UART 硬件配置） */
    at_engine_config_t at_cfg = {
        .uart_num         = UART_NUM_1,
        .tx_pin           = GPIO_NUM_17,
        .rx_pin           = GPIO_NUM_16,
        .baud_rate        = 115200,
        .rx_buf_size      = 2048,
        .rx_task_stack    = 4096,
        .rx_task_priority = 10,
    };
    at_engine_t *at = at_engine_create(&at_cfg);
    if (!at) return -1;

    /* 2. 模块适配（换模块只需换这一行） */
    modem_t *modem = modem_air780ep_create(at);
    if (!modem) goto err_at;

    /* 3. 核心服务 */
    lwlte_core_config_t config = {
        .apn       = CONFIG_LWLTE_APN,
        .auto_conn = true,
        .keepalive = 60,
    };
    g_core = lwlte_core_create(&config, modem);
    if (!g_core) goto err_modem;

    /* 4. 注册 App 事件回调 */
    lwlte_core_register_event(g_core, LWLTE_EVENT_ALL, app_event_handler, NULL);
    return 0;

err_modem:
    modem_destroy(modem);
err_at:
    at_engine_destroy(at);
    return -1;
}
```

**换模块时只改第 2 步的一行代码。** 其余代码零改动。

---

## 7. OOP 模式在架构中的落地

| OOP 概念 | 架构落点 | 具体表现 |
|----------|---------|---------|
| **封装** | 全部四层 | 每层对外只暴露句柄 + 操作函数，内部结构体不公开 |
| **继承** | Modem 层 | modem_interface（基类）→ modem_air780ep（子类），struct 嵌套 + container_of |
| **多态** | Modem 层 | modem_ops 表 → 不同模块同一接口不同行为 |
| **抽象类** | Modem Interface | 定义 ops 表但不实现，子类必须填充 |
| **vptr 注入** | 各层 init 函数 | `me->ops = ops` 在 init 时完成，`static const` 存于 `.rodata` |
| **板级装配** | Board Init | 唯一认识所有具体类型的文件，组装依赖树 |
| **零硬件引用** | App 层 + Core 层 | 不 include 模块私有头文件 |

---

## 8. 与旧架构的关键区别

| 旧架构 | 新架构 |
|--------|--------|
| 五层（Port → AT Engine → Modem → Core → App） | 四层（AT Engine → Modem → Core → App） |
| Port 层封装 ESP-IDF API（lwlte_thread_create 等 wrapper） | 所有层直接使用 ESP-IDF/FreeRTOS API |
| UART 操作在 Port 层，通过 ops 表间接调用 | UART 直接内置在 AT Engine，直接调 driver/uart.h |
| port_ops 多态用于"换 RTOS" | 无 port_ops，不抽象平台 |
| `port->ops->uart_write(port, buf, len)` | `uart_write_bytes(uart_num, buf, len)` |
| Port 层同时处理 UART + 线程 + 互斥锁 + GPIO | AT Engine 处理 UART，Modem 处理 GPIO（模块复位），各层各自直接调 IDF API |
| 三层（中间件 + Port + App） | 四层（AT Engine → Modem → Core → App） |
| 模块差异散落在各处 | 模块差异集中在 Modem Adapter 层，通过 ops 表统一 |
| 全局单例 `s_ctx` | create 返回句柄，支持多实例 |
| URC 处理嵌入在 FSM 中 | URC 沿回调链逐层翻译上传 |
| core 同时认识 AT 和平台 | core 只认识 modem_ops，不跨层 |

---

## 9. 待细化问题

以下问题值得在进入编码前进一步讨论：

1. **FSM 放置**：网络状态机放在 Core 层是一整块，还是拆分为 Core（高层状态）+ Modem Impl（AT 级状态）两层 FSM？两者如何同步？

2. **线程模型**：哪些层拥有自己的线程？AT Engine 有一个 UART RX 线程？Core 有一个 FSM 线程？线程数是否有硬约束？

3. **错误传播**：AT Engine 返回 `TIMEOUT` 和 Modem 返回 `NO_RESPONSE` 和 Core 返回 `NETWORK_ERROR` 之间的错误码映射规则是什么？

4. **资源生命周期**：deinit/destroy 的顺序如何保证反向释放？失败回滚（goto err_xxx）模式是否作为项目统一规范？

5. **跨层数据结构**：AT Engine → Modem 的 `at_response_t`、Modem → Core 的 `modem_event_t`、Core → App 的 `app_event_t` 各包含什么字段？

6. **重试策略归属**：AT 指令发出去没有响应——是 AT Engine 层自动重试，还是 Modem 层决定要不要重试，还是 Core 层判断后下指令重试？

7. **测试策略**：每层通过注入 mock ops 表做单元测试，但跨层集成测试（如 Modem + AT Engine 联调）要测到什么粒度？

8. **并发模型**：AT Engine 接收线程和 Core FSM 线程之间的数据竞争由各自内部处理？是否需要一个明确的线程安全约定？
