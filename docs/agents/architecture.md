# 架构概览

esp-lwlte 采用**用户门面 + 内部分层服务架构**。业务 App 代码只通过 `lwlte_t` 和 `lwlte_*` 用户 API 操作 LTE；板级初始化或 App 自有配置代码可以通过公共 `lwlte_air780ep_config_t` 填写 UART/GPIO 参数。门面负责把用户 API 转成内部 service 调用，并在模块 factory 中完成依赖装配。内部层通过包装 API + 事件队列/回调注册实现单向依赖与数据上行。

本组件直接构建在 ESP-IDF 之上，所有层都可以直接使用 ESP-IDF / FreeRTOS API（`xTaskCreate`、`xQueueCreate`、`vTaskDelay`、`uart_write_bytes` 等），不做平台抽象封装。这与 Espressif 官方组件（esp-mqtt、button、esp-sr 等）的设计哲学一致。

---

## 1. 分层架构

```
App
  ↓ 只依赖 src/include/lwlte*.h
LWLTE Facade
  ↓ 调用 Core/MQTT/TCP/HTTP 等 service API，并在模块 factory 中完成装配
Service Layer: Core, future MQTT, future TCP, future HTTP
  ↓
Modem Adapter
  ↓
AT Engine
```

| 层 | 职责 |
|----|------|
| App | 用户业务逻辑只操作 `lwlte_t`；板级初始化代码可 include `lwlte_air780ep.h` 并填写 UART/GPIO 配置 |
| LWLTE Facade | `lwlte_t` 用户门面、模块 factory、资源生命周期组合根、用户事件适配 |
| Service Layer | Core 负责网络状态机、PDP 管理、连接/重连；MQTT/TCP/HTTP 后续作为并列 service 扩展 |
| Modem Adapter | `modem_t` 抽象、具体模块 factory 与 AT 指令/URC 语义翻译 |
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

- 业务 App 代码只应 include `lwlte.h` 并调用 `lwlte_*` 操作；板级初始化或 App 自有配置代码可 include `lwlte_air780ep.h` 填写公开的 UART/GPIO 配置。
- Facade 的通用文件只应调用 service 层 API。
- Facade 的模块 factory 文件是 composition root，允许认识 AT Engine、Modem、具体 Modem factory 和 Core，用于创建并持有完整依赖树。
- Service 层仍只能向下调用紧邻的 Modem Adapter，不能直接调用 AT Engine。

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
| App 业务代码 | LWLTE Facade 用户操作 API | 不限（但不应掺入 LTE 硬件装配逻辑） | `lwlte.h`、`lwlte_t`、`lwlte_connect()` 等 |
| 板级初始化 / App 配置代码 | LWLTE Facade 模块 factory | 不限（用于准备公开配置） | `lwlte_air780ep_config_t` 中的 UART/GPIO 字段 |
| Facade 通用文件 | Core/MQTT/TCP/HTTP 等 service API | 不限 | `lwlte_t`、`core_t`、service 层间类型 |
| Facade 模块 factory | AT Engine、Modem、具体 Modem factory、Core | 不限（driver/gpio.h、driver/uart.h 等用于配置装配） | 完整装配 API，但不 include 任意 `_priv.h` |
| Service: Core / future MQTT / future TCP / future HTTP | Modem `modem_*` 统一 API | 不限（FreeRTOS task/queue/timer、esp_event 等） | `modem_t`、service 自身定义的类型 |
| Modem | AT Engine API | 不限（driver/gpio.h 用于模块复位/电源控制等） | AT Engine 层间头文件、Modem 自身定义的类型 |
| AT Engine | 无下层（最底层） | 不限（driver/uart.h、FreeRTOS task/queue 等，直接操作硬件） | AT Engine 自身类型 |

---

## 4. 数据流：AT 指令下行与 URC 上行

### 4.1 AT 指令下行（调用链）

```
App:    lwlte_connect(lte)，不需要知道 AT 指令存在
          │
Facade: core_connect(core)
          │  用户门面只调用 service API
          ▼
Core:   modem_set_apn(modem, 1, "cmnet")
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
AT Eng: uart_read_bytes() → 行拼接 → 匹配 URC 前缀 → 分发给已注册的 handler
         │  "AT Engine 只做模式匹配，不知道 URC 的模块含义"
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
          │  MODEM_EVENT_PDP_DEACTIVATED → CORE_EVENT_NET_OFFLINE
          ▼
Facade: 翻译为 LWLTE 用户事件并调用用户回调
          │  CORE_EVENT_NET_OFFLINE → LWLTE_EVENT_NET_OFFLINE
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
modem_t *modem = modem_air780ep_create(at, &modem_cfg);

/* Facade factory：通用 API 初始化具体模块，Air780EP 内部注册 URC handler。 */
modem_init(modem);  /* 内部调用 at_engine_register_urc(me->at, "+CGEV:", ...) */

esp_err_t core_init(core_t *me, modem_t *modem)
{
    me->modem = modem;
    return modem_register_event_callback(modem, core_event_handler, me);  /* Core → Modem */
}
```

运行时 URC 先进入 Modem handler，再经 `event_queue` / `event_task` 通知 Core，不再有向下的回调引用。

---

## 5. 各层职责详述

### 5.1 AT Engine 层（AT 引擎层）

AT Engine 是内部最底层，直接操作 UART 硬件。

| 维度 | 说明 |
|------|------|
| **职责** | 通用 AT 协议引擎 + UART 硬件操作：发命令、收响应、行解析、单次命令超时处理、URC 检测与分发 |
| **知道什么** | AT 协议格式（`AT+XXX\r\n`、`\r\nOK\r\n`、`\r\nERROR\r\n`）、超时机制、URC 前缀识别、UART 引脚与波特率 |
| **不知道什么** | 具体 AT 指令的含义、模块型号、网络状态 |
| **对外接口** | `at_engine_send_cmd(at, cmd, response, timeout)` 返回 `esp_err_t` 并填充调用方提供的 `at_response_t`；`at_engine_register_urc(at, prefix, handler)` |
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

这一层由两部分组成：**公共 `modem_t` / `modem_*` 抽象** + **模块实现（子类）**。

#### 5.2.1 Modem Public API（模块抽象接口）

| 维度 | 说明 |
|------|------|
| **职责** | 定义 `modem_t` opaque 句柄和 `modem_*` 包装 API；内部使用 `modem_ops` 虚函数表分发到具体模块 |
| **知道什么** | 模块需要有 init、get_info、get_signal、set_apn、activate_pdp 等操作 |
| **不知道什么** | 每个操作的具体 AT 指令格式 |
| **对外接口** | `modem_init()`、`modem_get_info()`、`modem_get_signal()`、`modem_set_apn()`、`modem_activate_pdp()` 等 `modem_*` 包装 API |

#### 5.2.2 Modem Impl（具体模块实现）

| 维度 | 说明 |
|------|------|
| **职责** | 实现 `modem_ops` 表中每个方法，将语义操作翻译为具体 AT 指令 |
| **知道什么** | 该模块的初始化序列、AT 指令格式、URC 字符串的语义含义、模块硬件控制（GPIO 复位、电源） |
| **不知道什么** | 网络状态机逻辑、上层协议、应用业务 |
| **OOP 角色** | 子类 — 内嵌 `modem_t`，实现内部 `modem_ops` 虚函数 |

Modem Impl 可以直接使用 `driver/gpio.h` 控制模块的 RESET/PWRKEY 引脚。

**Modem 层的核心价值**：不同 LTE 模块最大的差异就是 AT 指令格式和 URC 格式。把差异封装在 modem_impl 中，通过统一的 `modem_*` API 暴露给 Core，上层零感知。
内部再由 `modem_ops` 多态转发到具体模块。

```c
/* 不同模块实现同一个接口的不同行为 */
/* Air780EP: */
static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal)
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
static esp_err_t sim800_get_signal(modem_t *me, modem_signal_t *signal)
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
| **职责** | 网络状态机、PDP 激活/去激活管理、连接建立/重连/保活 |
| **知道什么** | 网络状态迁移规则、重试策略、保活机制、Modem 语义 API |
| **不知道什么** | 用户门面配置、具体 AT 指令格式、模块型号、底层硬件 |
| **层间接口** | `core_create()`、`core_start()`、`core_connect()`、`core_register_event_callback()`、`CORE_EVENT` |
| **OOP 角色** | 内部 service — 调用 modem 接口，不关心实现 |

Core 层可以使用 FreeRTOS 任务/队列/定时器来实现 FSM 线程和保活定时器，直接调 `xTaskCreate` 等 API。Core API 是给 Facade 和未来内部 service 使用的层间 API，不是 App 用户 API；App 只通过 `lwlte_*` 门面函数操作 LTE。

### 5.4 App 层（应用层）

| 维度 | 说明 |
|------|------|
| **职责** | 用户业务逻辑 |
| **知道什么** | 业务代码知道 `lwlte_t` 和 `lwlte_*` 操作；板级初始化代码可知道 `lwlte_air780ep_config_t` 中公开的 UART/GPIO 字段 |
| **不知道什么** | 业务代码不认识模块内部类型、AT 指令、协议细节；板级初始化也不 include 内部层头文件 |
| **规则** | 业务逻辑与硬件配置分开；内部层头文件、AT 指令字符串和具体装配 API 不出现在业务逻辑中 |

---

## 6. 初始化与装配

### 6.1 初始化链

板级初始化或 App 自有配置代码只调用用户门面 factory，不直接创建 AT Engine、Modem 或 Core。这里填写的是公共 `lwlte_air780ep_config_t`，不是访问内部层：

```c
lwlte_air780ep_config_t config = {
    .uart_num       = UART_NUM_1,
    .uart_tx_pin    = GPIO_NUM_17,
    .uart_rx_pin    = GPIO_NUM_16,
    .uart_baud_rate = 115200,
    .apn            = CONFIG_LWLTE_APN,
    .primary_cid    = 1,
    .auto_connect   = true,
};

lwlte_t *lte = NULL;
ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
ESP_ERROR_CHECK(lwlte_register_event_callback(lte, app_event_handler, NULL));
```

### 6.2 Facade Factory 模式

Facade 模块 factory 是 composition root，是唯一认识所有装配 API 和具体模块 factory 的文件：

```
Facade factory
    │
    ├─ 1. at_engine_create(&at_cfg)             → 创建 AT Engine
    │       └─ 内部创建 UART RX 线程，初始化 UART 硬件
    │
    ├─ 2. modem_air780ep_create(at, &modem_cfg) → 创建 Modem 实例
    │       └─ 传入 at 句柄和模块硬件配置，工厂保存依赖
    │
    ├─ 3. modem_init(modem)                     → 初始化模块并注册 URC
    │       └─ modem 内部注册 URC handler 到 at engine
    │
    ├─ 4. core_create(&core_cfg, modem)         → 创建 Core Service
    │       ├─ 传入 modem 句柄，Core 通过 modem_* API 操作模块
    │       └─ core 内部注册事件回调到 modem
    │
    ├─ 5. core_register_event_callback(core, facade_core_event_handler, lte)
    │       └─ Facade 把 CORE_EVENT 翻译为 LWLTE 用户事件
    │
    └─ 6. core_start(core)                      → 启动内部 service
```

```c
/* src/lwlte/lwlte_air780ep.c — Air780EP 门面 factory */

typedef struct lwlte {
    at_engine_t *at;
    modem_t     *modem;
    core_t      *core;
    lwlte_event_callback_t event_callback;
    void        *event_user_ctx;
} lwlte_t;

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte)
{
    /* 1. 底：创建 AT Engine（直接传入 UART 硬件配置） */
    at_engine_config_t at_cfg = {
        .uart_num  = config->uart_num,
        .tx_pin    = config->uart_tx_pin,
        .rx_pin    = config->uart_rx_pin,
        .baud_rate = config->uart_baud_rate,
    };
    at_engine_t *at = at_engine_create(&at_cfg);
    if (!at) return ESP_FAIL;

    /* 2. 模块适配（换模块只需换这一组配置和工厂） */
    modem_air780ep_config_t modem_cfg = {
        .pwrkey_pin = config->pwrkey_pin,
        .reset_pin  = config->reset_pin,
        .status_pin = config->status_pin,
    };
    modem_t *modem = modem_air780ep_create(at, &modem_cfg);
    if (!modem) goto err_at;

    if (modem_init(modem) != ESP_OK) goto err_modem;

    /* 3. 核心服务：auto_connect 由 Facade 在 ready 后显式触发。 */
    core_config_t core_cfg = {
        .apn          = config->apn,
        .primary_cid  = config->primary_cid,
        .auto_connect = false,
    };
    core_t *core = core_create(&core_cfg, modem);
    if (!core) goto err_modem;

    lwlte_t *lte = calloc(1, sizeof(*lte));
    if (!lte) goto err_core;

    lte->at = at;
    lte->modem = modem;
    lte->core = core;
    core_register_event_callback(core, facade_core_event_handler, lte);

    if (core_start(core) != ESP_OK) goto err_core;
    if (lwlte_wait_ready(lte, config->init_ready_timeout_ms) != ESP_OK) goto err_core;
    if (config->auto_connect && lwlte_connect(lte) != ESP_OK) goto err_core;

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

## 7. OOP 模式在架构中的落地

| OOP 概念 | 架构落点 | 具体表现 |
|----------|---------|---------|
| **封装** | 全部层 | 每层对外只暴露句柄 + 操作函数，内部结构体不公开 |
| **继承** | Modem 层 | `modem_t`（基类）→ `modem_air780ep_t`（子类），struct 嵌套 + container_of |
| **多态** | Modem 层 | 内部 `modem_ops` 表 → 不同模块同一 `modem_*` API 不同行为 |
| **抽象类** | Modem public API | 对外只暴露 `modem_t` + `modem_*` 包装函数，内部 ops 表由子类填充 |
| **vptr 注入** | Modem 层内部 init 函数 | `me->ops = ops` 在具体模块 init/create 时完成，`static const` 存于 `.rodata` |
| **资源装配** | Facade 模块 factory | 唯一认识所有装配 API 和具体模块 factory，组装依赖树 |
| **业务/装配分离** | App 层 + Facade factory | 业务代码只操作 `lwlte_t`；板级初始化可填 UART/GPIO config；内部装配集中在 Facade factory |

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
| 模块差异散落在各处 | 模块差异集中在 Modem Adapter 层，通过 `modem_t` + `modem_*` API 统一，内部用 ops 表多态 |
| 全局单例 `s_ctx` | create 返回句柄，支持多实例 |
| URC 处理嵌入在 FSM 中 | URC 经 Modem `event_queue` / `event_task` 解耦后回调上传 |
| core 同时认识 AT 和平台 | Core service 只认识 `modem_t` + `modem_*` API，不跨层；App 不直接 include Core |

---

## 9. 已明确约定

- **状态机归属**：Core Service 持有网络状态机；`core_fsm_t`、`net_mgr_t`、`pdp_mgr_t` 是 `core_t` 的组合成员。Modem 只提供模块语义能力和事件翻译。
- **线程模型**：AT Engine 拥有 UART RX task；Modem 拥有 event task；Core 拥有 FSM task 和 esp_event loop task。各线程通过队列/回调边界通信。
- **错误传播**：公共和层间 API 使用 ESP-IDF 标准 `esp_err_t`；AT 响应细节先由 Modem 转换为模块语义，再由 Core/Facade 向上传播。
- **生命周期**：Facade 模块 factory 创建并持有 AT Engine、Modem、Core 依赖树；销毁按反向顺序释放，失败路径使用统一 cleanup/goto 风格。
- **事件数据**：AT Engine 使用 `at_response_t`；Modem 上报 `modem_event_t`；Core 上报 `core_event_data_t`；Facade 翻译为 `lwlte_event_data_t`。
- **重试策略**：AT Engine 只处理单次命令超时；Core `net_mgr_t` 负责网络激活步骤、断线事件和重连策略。
- **并发边界**：跨层不共享私有结构；每层用自己的锁、队列和任务保护内部状态。Service 层不跨层直接调用 AT Engine。
