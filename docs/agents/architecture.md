# 架构概览

esp-lwlte 采用**用户门面 + 内部分层服务架构**。业务 App 代码只通过 `lwlte_handle_t` 和 `lwlte_*` 用户 API 操作 LTE；板级初始化或 App 自有配置代码可以通过公共 `lwlte_air780ep_config_t` 填写 UART/GPIO 参数。门面负责把用户 API 转成内部 service 调用，并在模块 factory 中完成依赖装配。内部层通过包装 API + 事件队列/回调注册实现单向依赖与数据上行。

本组件直接构建在 ESP-IDF 之上，所有层都可以直接使用 ESP-IDF / FreeRTOS API（`xTaskCreate`、`xQueueCreate`、`vTaskDelay`、`uart_write_bytes` 等），不做平台抽象封装。这与 Espressif 官方组件（esp-mqtt、button、esp-sr 等）的设计哲学一致。

---

## 1. 分层架构

```
App
  ↓ 只依赖 src/include/lwlte.h
LWLTE Facade
  ↓ 调用 Core/MQTT/TCP/HTTP 等 service API，并在模块 factory 中完成装配
Service Layer: MQTT → Core；Core 是访问 Modem 的 service；future TCP/HTTP 边界待设计
  ↓ Core 调用
Modem Adapter
  ↓
AT Engine
```

| 层 | 职责 |
|----|------|
| App | 用户业务逻辑只操作 `lwlte_handle_t`；板级初始化代码 include `lwlte.h` 并填写 `lwlte_air780ep_config_t` 等模块配置 |
| LWLTE Facade | `lwlte_handle_t` 用户门面、模块 factory、资源生命周期组合根、用户事件适配 |
| Service Layer | Core 负责网络状态机、PDP 管理、连接/重连和命令串行化；MQTT 是依赖 Core 的上层 service；TCP/HTTP 边界留待后续设计 |
| Modem Adapter | `modem_handle_t` 抽象、具体模块 factory 与 AT 指令/URC 语义翻译 |
| AT Engine | 通用 AT 协议引擎 + UART 硬件操作，只做命令响应和 URC 前缀分发 |

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

- 业务 App 代码只应 include `lwlte.h` 并调用 `lwlte_*` 操作；板级初始化或 App 自有配置代码也 include `lwlte.h`，并填写其中声明的模块配置如 `lwlte_air780ep_config_t`。
- Facade 的通用文件只应调用 service 层 API。
- Facade 的模块 factory 文件是 composition root，允许认识 AT Engine、Modem、具体 Modem factory 和 Core，用于创建并持有完整依赖树。
- Core 可以调用紧邻的 Modem Adapter；MQTT 通过 Core command queue 投递模块命令，不直接调用 Modem 或 AT Engine；TCP/HTTP 边界尚未承诺。

| 类别 | 规则 | 示例 |
|------|------|------|
| **函数调用** | 除 Facade 模块 factory 的装配例外外，只能调紧邻的下一层 | Core 不能调 `uart_write_bytes()` 或 `at_engine_send_cmd()` |
| **ESP-IDF 引用** | 任何层都可以直接 `#include` ESP-IDF/FreeRTOS 头文件 | AT Engine 直接 include `driver/uart.h`、`freertos/task.h` |
| **配置数据** | 自上而下单向传递 | App config → Facade factory → Core / Modem / AT Engine |
| **事件/URC** | 自下而上通过事件队列/回调逐层上传 | AT Engine URC → Modem 事件 → Core 状态 → Facade 用户事件 → App 通知 |

### 3.2 跨层调用仍然禁止

虽然所有层都可以使用 ESP-IDF API，但**层间函数调用仍然只能向下调紧邻的下一层**。Facade 模块 factory 只在初始化/销毁装配阶段例外；运行期 service 代码不能跨层调用。Core 不能直接调 AT Engine 的函数，必须通过 Modem Adapter 间接调用。

不封装 FreeRTOS API 不等于可以跨层调业务接口。

### 3.3 每层的能力边界

| 层 | 可以调用的层 | 可以使用的 ESP-IDF API | 可以知道的符号 |
|----|------------|----------------------|-------------|
| App 业务代码 | LWLTE Facade 用户操作 API | 不限（但不应掺入 LTE 硬件装配逻辑） | `lwlte.h`、`lwlte_handle_t`、`lwlte_start()` 等 |
| 板级初始化 / App 配置代码 | LWLTE Facade 模块 factory | 不限（用于准备公开配置） | `lwlte_air780ep_config_t` 中的 UART/GPIO 字段 |
| Facade 通用文件 | Core/MQTT/TCP/HTTP 等 service API | 不限 | `lwlte_handle_t`、`core_handle_t`、service 层间类型 |
| Facade 模块 factory | AT Engine、Modem、具体 Modem factory、Core | 不限（driver/gpio.h、driver/uart.h 等用于配置装配） | 完整装配 API，但不 include 任意 `_priv.h` |
| Service: Core | Modem `modem_*` 统一 API | 不限（FreeRTOS task/queue/timer、esp_event 等） | `modem_handle_t`、Core 自身定义的类型 |
| Service: MQTT | Core 层间 API、Core command queue（`core_submit_cmd()`） | 不限（FreeRTOS task/queue/timer、esp_event 等） | `core_handle_t`、MQTT 自身定义的类型 |
| Service: future TCP / future HTTP | 后续边界设计 | 不限 | 后续边界设计 |
| Modem | AT Engine API | 不限（driver/gpio.h 用于模块复位/电源控制等） | AT Engine 层间头文件、Modem 自身定义的类型 |
| AT Engine | 无下层（最底层） | 不限（driver/uart.h、FreeRTOS task/queue 等，直接操作硬件） | AT Engine 自身类型 |

---

## 4. 数据流：AT 指令下行与 URC 上行

### 4.1 AT 指令下行（调用链）

```
App:    lwlte_start(lte)，不需要知道 AT 指令存在
          │
Facade: core_start(core)
          │  用户门面只调用 service API，异步提交启动请求
          ▼
Core:   modem_start(modem) → modem_set_apn(modem, 1, "cmnet") → modem_activate_pdp(modem, 1)
          │  通过 modem_* 包装 API 调用（不知道下面是哪个模块）
          ▼
Modem:  at_engine_send_cmd(at, "AT+CGDCONT=1,\"IP\",\"cmnet\"", &resp, 3000)
          │  将语义操作翻译为具体 AT 指令字符串
          ▼
AT Eng: uart_write_bytes(uart_num, buf, len)    ← 直接调 ESP-IDF UART API
         │  加上 \r\n，处理单次命令超时
         ▼
        硬件 TX 引脚
```

### 4.2 URC 上行（事件队列 + 回调）

```
        硬件 RX 引脚
         │
AT Eng: uart_read_bytes() → 行拼接 → 有活动命令时写入命令响应；空闲期匹配 URC 前缀并分发
         │  "AT Engine 只做命令响应收集和空闲期 URC 模式匹配，不知道模块含义"
         │  持有：urc_handler_t *handlers[]  (按前缀字符串注册)
         │  回调：urc_handler(prefix, line)
         ▼
Modem:  "翻译"：原始 URC 字符串 → 模块级语义事件
         │  "+CGEV: ME PDN DEACT 1" → MODEM_EVENT_PDP_DEACTIVATED
         │  生成 modem_event_t → event_queue
         ▼
Modem:  event_task 从 event_queue 取出事件后调用 Core 回调
         ▼
Core:   处理事件 → 更新网络状态机 → 通知 Facade
          │  MODEM_EVENT_PDP_DEACTIVATED → LWLTE_EVENT_NET_OFFLINE
          ▼
Facade: 翻译为 LWLTE 用户事件并调用用户回调
          │  LWLTE_EVENT_NET_OFFLINE → LWLTE_EVENT_NET_OFFLINE
          ▼
App:    业务响应（重连、告警、降级等）
```

### 4.3 回调注册时序

回调在初始化阶段由上层向下层注册，保证运行时调用方向始终是单向的：

```c
/* Facade factory：具体模块工厂保存 AT Engine 句柄，Core 不看到具体类型。 */
modem_air780ep_config_t modem_cfg = {
    .default_cmd_timeout_ms = 3000,
    .event_queue_size       = 8,
};
modem_handle_t *modem = modem_air780ep_create(at, &modem_cfg);

esp_err_t core_init(core_handle_t *me, modem_handle_t *modem)
{
    me->modem = modem;
    return modem_register_event_callback(modem, core_event_handler, me);  /* Core → Modem */
}
```

空闲期 URC 先进入 Modem handler，再经 `event_queue` / `event_task` 通知 Core；命令期间收到的行优先归入当前命令响应，由发起命令的方法解析。

---

## 5. 各层职责详述

### 5.1 AT Engine 层（AT 引擎层）

AT Engine 是内部最底层，直接操作 UART 硬件。

| 维度 | 说明 |
|------|------|
| **职责** | 通用 AT 协议引擎 + UART 硬件操作：发命令、收响应、行解析、单次命令超时处理、空闲期 URC 检测与分发 |
| **知道什么** | AT 协议格式（`AT+XXX\r\n`、`\r\nOK\r\n`、`\r\nERROR\r\n`）、超时机制、URC 前缀识别、UART 引脚与波特率 |
| **不知道什么** | 具体 AT 指令的含义、模块型号、网络状态 |
| **对外接口** | `at_engine_send_cmd(at, cmd, response, timeout)` 返回 `esp_err_t` 并填充调用方提供的 `at_response_t`；`at_engine_register_urc(at, prefix, handler)` |
| **内部实现** | 创建 UART RX 接收线程（`xTaskCreate`），直接调用 `uart_write_bytes` / `uart_read_bytes`，用 `xQueue` 传递接收数据 |
| **OOP 角色** | 协议栈 + 硬件驱动 — 所有 AT 模块共用的基础设施 |

AT Engine 是一个"聪明的邮差"：会按地址送信、收信，并在没有当前命令时识别加急件（URC），但从不拆信看内容。同时它也负责维护邮路（UART 硬件）本身。

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

这一层由两部分组成：**公共 `modem_handle_t` / `modem_*` 抽象** + **模块实现（子类）**。

#### 5.2.1 Modem Public API（模块抽象接口）

| 维度 | 说明 |
|------|------|
| **职责** | 定义 `modem_handle_t` opaque 句柄和 `modem_*` 包装 API；内部使用 `modem_ops` 虚函数表分发到具体模块 |
| **知道什么** | 模块需要有 start、get_info、get_signal、set_apn、activate_pdp 等操作 |
| **不知道什么** | 每个操作的具体 AT 指令格式 |
| **对外接口** | `modem_start()`、`modem_get_info()`、`modem_get_signal()`、`modem_set_apn()`、`modem_activate_pdp()` 等 `modem_*` 包装 API |

#### 5.2.2 Modem Impl（具体模块实现）

| 维度 | 说明 |
|------|------|
| **职责** | 实现 `modem_ops` 表中每个方法，将语义操作翻译为具体 AT 指令 |
| **知道什么** | 该模块的初始化序列、AT 指令格式、URC 字符串的语义含义、模块硬件控制（GPIO 复位、电源） |
| **不知道什么** | 网络状态机逻辑、上层协议、应用业务 |
| **OOP 角色** | 子类 — 内嵌 `modem_handle_t`，实现内部 `modem_ops` 虚函数 |

Modem Impl 可以直接使用 `driver/gpio.h` 控制模块的 EN 引脚。

**Modem 层的核心价值**：不同 LTE 模块最大的差异就是 AT 指令格式和 URC 格式。把差异封装在 modem_impl 中，通过统一的 `modem_*` API 暴露给 Core，上层零感知。
内部再由 `modem_ops` 多态转发到具体模块。

```c
/* 不同模块实现同一个接口的不同行为 */
/* Air780EP: */
static esp_err_t air780ep_get_signal(modem_handle_t *me, modem_signal_t *signal)
{
    char *lines[4];
    at_response_t resp = {
        .max_lines = 4,
        .lines     = lines,
    };
    esp_err_t ret = at_engine_send_cmd(me->at, "AT+CSQ", &resp, 3000);
    if (ret != ESP_OK) {
        return ret;
    }
    /* 解析 resp.lines 中的 +CSQ: <rssi>,<ber> 并填充 signal */
    return ESP_OK;
}

/* SIM800: 同一个方法，不同 AT 指令 */
static esp_err_t sim800_get_signal(modem_handle_t *me, modem_signal_t *signal)
{
    char *lines[4];
    at_response_t resp = {
        .max_lines = 4,
        .lines     = lines,
    };
    esp_err_t ret = at_engine_send_cmd(me->at, "AT+CSQ", &resp, 5000);  /* 超时不同 */
    if (ret != ESP_OK) {
        return ret;
    }
    /* 解析格式可能不同，并填充 signal */
    return ESP_OK;
}
```

### 5.3 Core Service 层（核心服务层）

| 维度 | 说明 |
|------|------|
| **职责** | 网络状态机、PDP 激活/去激活管理、连接建立/重连/保活，并通过 `core_submit_cmd()` 串行化上层 service 协议命令 |
| **知道什么** | 网络状态迁移规则、重试策略、保活机制、Modem 语义 API |
| **不知道什么** | 用户门面配置、具体 AT 指令格式、模块型号、底层硬件 |
| **层间接口** | `core_create()`、`core_start()`、`core_connect()`、`core_register_protocol_callback()`、`core_submit_cmd()`、`LWLTE_EVENT` |
| **OOP 角色** | 内部 service — 调用 modem 接口，不关心实现 |

Core 层可以使用 FreeRTOS 任务/队列/定时器来实现 FSM 线程和保活定时器，直接调 `xTaskCreate` 等 API。Core API 是给 Facade 和上层内部 service 使用的层间 API，不是 App 用户 API；App 只通过 `lwlte_*` 门面函数操作 LTE。

### 5.4 MQTT Client Service 层（MQTT 客户端服务层）

| 维度 | 说明 |
|------|------|
| **职责** | MQTT 连接、订阅、取消订阅、发布和下行数据事件适配 |
| **知道什么** | Core 层间 API、Core 网络事件、Core protocol event、MQTT 自身状态机 |
| **不知道什么** | Modem Adapter、AT Engine、具体 AT 指令格式、模块型号 |
| **层间接口** | `mqtt_client_create()`、`mqtt_client_start()`、`mqtt_client_destroy()`、`LWLTE_MQTT_EVENT` |
| **OOP 角色** | 依赖 Core 的上层 service — 拥有 MQTT FSM，不直接调用 Modem/AT Engine |

MQTT Client Service 通过 Core event handler 接收网络和协议数据事件，通过 `core_submit_cmd()` 提交 MQTT 模块命令。它不 include Modem/AT Engine 头文件，也不直接调用 `modem_*`。

### 5.5 App 层（应用层）

| 维度 | 说明 |
|------|------|
| **职责** | 用户业务逻辑 |
| **知道什么** | 业务代码知道 `lwlte_handle_t` 和 `lwlte_*` 操作；板级初始化代码可知道 `lwlte_air780ep_config_t` 中公开的 UART/GPIO 字段 |
| **不知道什么** | 业务代码不认识模块内部类型、AT 指令、协议细节；板级初始化也不 include 内部层头文件 |
| **规则** | 业务逻辑与硬件配置分开；内部层头文件、AT 指令字符串和具体装配 API 不出现在业务逻辑中 |

---

## 6. 初始化与装配

### 6.1 初始化链

板级初始化或 App 自有配置代码只调用用户门面 factory，不直接创建 AT Engine、Modem 或 Core。这里填写的是公共 `lwlte_air780ep_config_t`（只含 Core/Modem/AT 字段），MQTT 配置通过独立的 `lwlte_mqtt_config_t` 传入：

```c
ESP_ERROR_CHECK(esp_event_loop_create_default());

lwlte_mqtt_config_t mqtt_config = {
    .host      = CONFIG_LWLTE_MQTT_HOST,
    .port      = CONFIG_LWLTE_MQTT_PORT,
    .client_id = CONFIG_LWLTE_MQTT_CLIENT_ID,
};

lwlte_air780ep_config_t config = {
    .uart_num       = UART_NUM_1,
    .uart_tx_pin    = GPIO_NUM_17,
    .uart_rx_pin    = GPIO_NUM_16,
    .uart_baud_rate = 115200,
    .apn            = CONFIG_LWLTE_APN,
    .primary_cid    = 1,
    .event_loop     = NULL,
};

lwlte_handle_t *lte = NULL;
ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                           app_event_handler, NULL));
ESP_ERROR_CHECK(lwlte_mqtt_init(lte, &mqtt_config));
ESP_ERROR_CHECK(lwlte_start(lte));
```

### 6.2 Facade Factory 模式

Facade 模块 factory 是 composition root，是唯一认识所有装配 API 和具体模块 factory 的文件：

生命周期职责边界：

- `lwlte_air780ep_init()` 只负责创建和装配 `lwlte_handle_t`、AT Engine、Modem、Core、Ping service，不启动模块、不等待 AT 通道 ready、不激活 PDP。MQTT 客户端由独立的 `lwlte_mqtt_init()` 创建。
- `lwlte_start()` 是用户显式启动入口，异步提交启动请求；最终 online 结果通过 `LWLTE_EVENT_NET_ONLINE` 上报。
- `lwlte_stop()` 是与 `lwlte_start()` 对称的用户停机入口，异步提交停止请求，去激活网络；配置 EN GPIO 时拉低 EN 断电，`en_pin == GPIO_NUM_NC` 时降级为逻辑停机。
- `core_start()` 成功投递 `CORE_SIG_START` 时会同步标记 `CORE_STATE_STARTING`；Core FSM 随后在 `CORE_SIG_START` 中调用阻塞式 `modem_start()`。`modem_start()` 完成硬复位、`AT OK` 和基础 AT 初始化后返回 `ESP_OK`，Core 随后执行 SIM、注册、附着、APN、PDP 激活和 IP 查询流程。
- `modem_start()` 表示模块动态开机到基础 AT ready：硬复位/等待 `AT OK`/基础 AT 初始化，并注册运行期 URC；不负责 APN/PDP/IP。

Air780EP modem 的动态开机由 Core 启动流程触发。Facade factory 只装配依赖并注册事件桥接，不在 init 中等待模块 ready。

```
Facade factory
    │
    ├─ 1. at_engine_create(&at_cfg)             → 创建 AT Engine
    │       └─ 内部创建 UART RX 线程，初始化 UART 硬件
    │
    ├─ 2. modem_air780ep_create(at, &modem_cfg) → 创建 Modem 实例
    │       └─ 传入 at 句柄和模块硬件配置，工厂保存依赖
    │
    ├─ 3. core_create(&core_cfg, modem)         → 创建 Core Service
    │       ├─ 传入 modem 句柄，Core 通过 modem_* API 操作模块
    │       └─ core 内部注册事件回调到 modem
    │
    ├─ 4. esp_event_handler_register(LWLTE_EVENT, LWLTE_EVENT_READY, facade_ready_handler, lte)
    │       └─ Facade 注册内部 handler 驱动 lwlte_wait_ready 同步
    │
    ├─ 5. ping_client_create(core)              → 创建 Ping Service
    │
    └─ 6. 返回 lwlte_handle_t，等待用户调用 lwlte_start(lte)
```

```c
/* src/lwlte/lwlte_air780ep.c — Air780EP 门面 factory */

struct lwlte_handle {
    at_engine_handle_t *at;
    modem_handle_t     *modem;
    core_handle_t      *core;
    mqtt_client_handle_t *mqtt; /* 由 lwlte_mqtt_init() 独立创建 */
    esp_event_loop_handle_t event_loop;
};

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_handle_t **out_lte)
{
    /* 1. 底：创建 AT Engine（直接传入 UART 硬件配置） */
    at_engine_config_t at_cfg = {
        .uart_num  = config->uart_num,
        .tx_pin    = config->uart_tx_pin,
        .rx_pin    = config->uart_rx_pin,
        .baud_rate = config->uart_baud_rate,
    };
    at_engine_handle_t *at = at_engine_create(&at_cfg);
    if (!at) return ESP_FAIL;

    /* 2. 模块适配（换模块只需换这一组配置和工厂） */
    modem_air780ep_config_t modem_cfg = {
        .en_pin = config->en_pin,
    };
    modem_handle_t *modem = modem_air780ep_create(at, &modem_cfg);
    if (!modem) goto err_at;

    /* 3. 核心服务：启动请求由 lwlte_start() 异步提交给 Core。 */
    core_config_t core_cfg = {
        .apn          = config->apn,
        .primary_cid  = config->primary_cid,
    };
    core_handle_t *core = core_create(&core_cfg, modem);
    if (!core) goto err_modem;

    lwlte_handle_t *lte = calloc(1, sizeof(*lte));
    if (!lte) goto err_core;

    lte->at = at;
    lte->modem = modem;
    lte->core = core;
    esp_event_handler_register(LWLTE_EVENT, LWLTE_EVENT_READY,
                               facade_ready_handler, lte);

    /* MQTT 客户端不在 factory 中创建；由 lwlte_mqtt_init() 独立创建。 */

    *out_lte = lte;
    return ESP_OK;

err_core:
    core_destroy(core);

err_modem:
    modem_destroy(modem);
err_at:
    at_engine_destroy(at);
    return ESP_FAIL;
}
```

**换模块时只新增/替换对应的门面 factory 和具体 Modem factory。** 通用 Facade、Core 和 App 代码零改动。

---

## 7. Event Bus

lwlte uses ESP-IDF's event loop library as its event bus. The bus is shared across
all layers (core, MQTT client) and exposed to the application.

### Bases

| Base | Producer | Events |
|------|----------|--------|
| LWLTE_EVENT | Core FSM | STARTED, READY, NET_CONNECTING, NET_ONLINE, NET_OFFLINE, NET_ERROR, STOPPED, ERROR |
| LWLTE_MQTT_EVENT | MQTT client FSM | STARTED, STOPPED, CONNECTING, CONNECTED, DISCONNECTED, SUBSCRIBED, UNSUBSCRIBED, PUBLISHED, DATA, ERROR |

### Registration

Application registers handlers via standard ESP-IDF APIs:

```c
esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID, handler, ctx);
esp_event_handler_register(LWLTE_MQTT_EVENT, ESP_EVENT_ANY_ID, handler, ctx);
```

### Protocol data (private)

Protocol data (modem → MQTT client) flows over a private synchronous callback
(core_register_protocol_callback), NOT over the event bus. This keeps the
high-volume data stream off the shared queue and hides transport-layer details
from the application.

---

## 8. OOP 模式在架构中的落地

| OOP 概念 | 架构落点 | 具体表现 |
|----------|---------|---------|
| **封装** | 全部层 | 每层对外只暴露句柄 + 操作函数，内部结构体不公开 |
| **继承** | Modem 层 | `modem_handle_t`（基类）→ `modem_air780ep_t`（子类），struct 嵌套 + container_of |
| **多态** | Modem 层 | 内部 `modem_ops` 表 → 不同模块同一 `modem_*` API 不同行为 |
| **抽象类** | Modem public API | 对外只暴露 `modem_handle_t` + `modem_*` 包装函数，内部 ops 表由子类填充 |
| **vptr 注入** | Modem 层内部 init 函数 | `me->ops = ops` 在具体模块 init/create 时完成，`static const` 存于 `.rodata` |
| **资源装配** | Facade 模块 factory | 唯一认识所有装配 API 和具体模块 factory，组装依赖树 |
| **业务/装配分离** | App 层 + Facade factory | 业务代码只操作 `lwlte_handle_t`；板级初始化可填 UART/GPIO config；内部装配集中在 Facade factory |

---

## 8. 与旧架构的关键区别

| 旧架构 | 新架构 |
|--------|--------|
| 五层（Port → AT Engine → Modem → Core → App） | App → LWLTE Facade → Service → Modem → AT Engine |
| Port 层封装 ESP-IDF API（lwlte_thread_create 等 wrapper） | 所有层直接使用 ESP-IDF/FreeRTOS API |
| UART 操作在 Port 层，通过 ops 表间接调用 | UART 直接内置在 AT Engine，直接调 driver/uart.h |
| port_ops 多态用于"换 RTOS" | 无 port_ops，不抽象平台 |
| `port->ops->uart_write(port, buf, len)` | `uart_write_bytes(uart_num, buf, len)` |
| Port 层同时处理 UART + 线程 + 互斥锁 + GPIO | AT Engine 处理 UART，Modem 处理 GPIO（模块复位），各层各自直接调 IDF API |
| 三层（中间件 + Port + App） | 用户 Facade + 内部 Service/Modem/AT 分层 |
| 模块差异散落在各处 | 模块差异集中在 Modem Adapter 层，通过 `modem_handle_t` + `modem_*` API 统一，内部用 ops 表多态 |
| 全局单例 `s_ctx` | create 返回句柄，支持多实例 |
| URC 处理嵌入在 FSM 中 | URC 经 Modem `event_queue` / `event_task` 解耦后回调上传 |
| core 同时认识 AT 和平台 | Core service 只认识 `modem_handle_t` + `modem_*` API，不跨层；App 不直接 include Core |

---

## 9. 已明确约定

- **状态机归属**：Core Service 持有网络状态机；`core_fsm_t`、`net_mgr_t`、`pdp_mgr_t` 是 `core_handle_t` 的组合成员。Modem 只提供模块语义能力和事件翻译。
- **线程模型**：AT Engine 拥有 UART RX task；Modem 拥有 event task；Core 拥有 FSM task 和 esp_event loop task。各线程通过队列/回调边界通信。
- **错误传播**：公共和层间 API 使用 ESP-IDF 标准 `esp_err_t`；AT 响应细节先由 Modem 转换为模块语义，再由 Core/Facade 向上传播。
- **生命周期**：Facade 模块 factory 创建并持有 AT Engine、Modem、Core 依赖树；销毁按反向顺序释放，失败路径使用统一 cleanup/goto 风格。
- **事件数据**：AT Engine 使用 `at_response_t`；Modem 上报 `modem_event_t`；Core 直接投递 `lwlte_event_data_t` 到 `LWLTE_EVENT`。
- **重试策略**：AT Engine 只处理单次命令超时；Core `net_mgr_t` 负责网络激活步骤、断线事件和重连策略。
- **并发边界**：跨层不共享私有结构；每层用自己的锁、队列和任务保护内部状态。Service 层不跨层直接调用 AT Engine。
