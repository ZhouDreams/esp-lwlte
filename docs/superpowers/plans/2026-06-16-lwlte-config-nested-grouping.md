# lwlte 模块配置嵌套分组 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `lwlte_air780ep_config_t` / `lwlte_ml307r_config_t` 从 25 个平铺字段重构为「公共基础 `lwlte_base_config_t`（含 uart/at_engine/modem/core/event 五个嵌套子结构体）+ 每模块包装」，行为零变化。

**Architecture:** 仅改公共配置结构的形状与读取路径。新增 5 个公共子结构体类型 + `lwlte_base_config_t` 聚合体；两个模块配置结构体变为 `{ lwlte_base_config_t base; /* 模块特有字段预留 */ }`。两个工厂 `lwlte_*_init()` / `validate_config()` 把 `config->xxx` 改为 `config->base.<group>.<field>` 的嵌套路径并按子系统分块。下层 `at_engine_config_t` / `modem_*_config_t` / `core_config_t` 定义不变；`lwlte_mqtt_config_t` 不动。

**Tech Stack:** C（ESP-IDF v6.0，纯 C，无 C++）；host 契约测试为 Python `unittest`（用 pytest 运行）；构建用 `idf.py`。

**Spec:** `docs/superpowers/specs/2026-06-16-lwlte-config-nested-grouping-design.md`

---

## ⚠️ 提交策略（必读）

- **在用户显式授权前，禁止运行任何 `git commit`。** 每个任务的"提交"步骤一律只 **`git add` 暂存** 并 **暂停等待授权**。
- 本重构是**原子性**的：改完 `lwlte.h` 后，到工厂/示例同步改完之前，C 代码**不能编译**。因此**不做逐任务提交**（会产生编译不过的中间提交）。计划只在 Task 6 全部静态契约 + 编译验证通过后，安排**一个**受授权门控的提交。
- host 契约测试是纯文本断言（grep 源码），不编译 C，因此 Task 1–3 可以逐任务跑对应测试文件并变绿，即使此刻整体还不能编译。

---

## File Structure

修改（无新增文件）：

- `src/include/lwlte.h` — 替换 212–289 行的两个平铺结构体为 5 子结构体 + `lwlte_base_config_t` + 2 个模块包装。
- `src/lwlte/lwlte_air780ep.c` — `lwlte_air780ep_init()` 的 at/modem/core 配置映射 + `me->event_loop` 赋值 + `validate_config()` 改嵌套路径。
- `src/lwlte/lwlte_ml307r.c` — 同上，保留 `rx_line_buf_size` 的 2048 兜底。
- `example/air780ep_basic_connect.c`、`example/air780ep_mqtt_client.c`、`example/ml307r_basic_connect.c`、`example/ml307r_mqtt_client.c` — config 字面量改嵌套。
- `tests/host/test_lwlte_start_lifecycle.py`、`tests/host/test_air780ep_command_gated_init.py`、`tests/host/test_mqtt_end_to_end_contract.py`、`tests/host/test_ml307r_contract.py` — 同步契约断言。
- `docs/agents/architecture.md`、`docs/agents/oop-design.md` — config 字面量与工厂示例片段改嵌套。

运行测试命令（全程）：

```bash
python3 -m pytest tests/host/ -q          # 全量
python3 -m pytest tests/host/<file>.py -q # 单文件
```

---

## Task 1: lwlte.h 嵌套配置类型

**Files:**
- Test: `tests/host/test_lwlte_start_lifecycle.py:85-92`、`tests/host/test_air780ep_command_gated_init.py:143-152`
- Modify: `src/include/lwlte.h:212-289`

- [ ] **Step 1: 更新契约测试（先失败）**

`tests/host/test_lwlte_start_lifecycle.py` — 把 `test_public_api_has_start_not_connect_or_auto_connect` 整个方法替换为：

```python
    def test_public_api_has_start_not_connect_or_auto_connect(self):
        assert_contains(self, self.lwlte_h, "esp_err_t lwlte_start(lwlte_handle_t *me);", "lwlte.h")
        assert_not_contains(self, self.lwlte_h, "esp_err_t lwlte_connect(lwlte_handle_t *me);", "lwlte.h")
        assert_not_contains(self, self.lwlte_h, "auto_connect", "lwlte.h")
        for token in [
            "} lwlte_uart_config_t;",
            "} lwlte_at_engine_config_t;",
            "} lwlte_modem_config_t;",
            "} lwlte_core_config_t;",
            "} lwlte_event_config_t;",
            "} lwlte_base_config_t;",
            "lwlte_base_config_t base;",
        ]:
            assert_contains(self, self.lwlte_h, token, "lwlte.h")
```

`tests/host/test_air780ep_command_gated_init.py` — 把 `test_public_config_comments_use_at_ready_not_rdy_wait` 整个方法替换为：

```python
    def test_public_config_comments_use_at_ready_not_rdy_wait(self):
        modem_end = self.lwlte_h.index("} lwlte_modem_config_t;")
        modem_start = self.lwlte_h.rindex("typedef struct {", 0, modem_end)
        modem_body = self.lwlte_h[modem_start:modem_end]

        self.assertIn("ready_timeout_ms;", modem_body)
        self.assertIn("AT OK", modem_body)
        self.assertNotIn("RDY" + " 等待超时", modem_body)
        self.assertNotIn("RDY " + "wait timeout", modem_body)
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m pytest tests/host/test_lwlte_start_lifecycle.py tests/host/test_air780ep_command_gated_init.py -q`
Expected: FAIL —`test_lwlte_start_lifecycle` 报缺少 `} lwlte_uart_config_t;` 等 token；`test_air780ep_command_gated_init` 报 `ValueError: substring not found`（旧头无 `} lwlte_modem_config_t;`）。

- [ ] **Step 3: 替换 lwlte.h:212-289 为嵌套定义**

把 `src/include/lwlte.h` 第 212 行（Air780EP 结构体的 `/**` 文档注释开头）到第 289 行（`} lwlte_ml307r_config_t;`）整段替换为以下内容（类型 `uart_port_t`/`gpio_num_t`/`esp_event_loop_handle_t` 已在本头文件现有 include 中，无需新增 include）：

```c
/**
 * @brief UART 硬件配置
 * @details UART hardware configuration
 */
typedef struct {
    uart_port_t num;        /**< 必填 UART 端口号； Required UART port number */
    gpio_num_t  tx_pin;     /**< 必填 UART TX GPIO，不能为 GPIO_NUM_NC； Required UART TX GPIO, not GPIO_NUM_NC */
    gpio_num_t  rx_pin;     /**< 必填 UART RX GPIO，不能为 GPIO_NUM_NC； Required UART RX GPIO, not GPIO_NUM_NC */
    int         baud_rate;  /**< 必填 UART 波特率，必须大于 0； Required UART baud rate, must be > 0 */
} lwlte_uart_config_t;

/**
 * @brief AT 引擎调优配置
 * @details AT engine tuning configuration
 * @note 所有字段为 0 时使用下层默认值，非 0 值必须大于 0。
 */
typedef struct {
    int rx_buf_size;            /**< AT RX 缓冲大小，0 使用默认值； AT RX buffer size, 0 uses default */
    int rx_task_stack;          /**< AT RX 任务栈大小，0 使用默认值； AT RX task stack, 0 uses default */
    int rx_task_priority;       /**< AT RX 任务优先级，0 使用默认值； AT RX task priority, 0 uses default */
    int rx_line_buf_size;       /**< AT 单行缓冲大小，0 使用默认值； AT line buffer size, 0 uses default */
    int cmd_default_timeout_ms; /**< AT 默认命令超时，0 使用默认值； AT default command timeout, 0 uses default */
    int max_response_lines;     /**< AT 最大响应行数，0 使用默认值； AT maximum response lines, 0 uses default */
} lwlte_at_engine_config_t;

/**
 * @brief 调制解调器配置
 * @details Modem configuration
 * @note en_pin 可设为 GPIO_NUM_NC 以禁用门面对 EN GPIO 的控制。
 * @note ready_timeout_ms 为 0 时使用下层默认值；该值为硬复位后等待 AT OK 的总超时。
 * @note 有符号的队列、任务字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
    gpio_num_t en_pin;                 /**< 可选模块 EN GPIO，GPIO_NUM_NC 表示不控制； Optional module EN GPIO, GPIO_NUM_NC disables control */
    uint32_t   reset_pulse_ms;         /**< Modem 复位脉冲(EN 拉低保持)时长，0 表示不额外等待； Modem reset pulse (EN low hold) length, 0 skips extra wait */
    uint32_t   ready_timeout_ms;       /**< 启动 AT OK 等待总超时，0 使用下层默认值； Startup AT OK wait timeout, 0 uses lower-layer default */
    uint32_t   default_cmd_timeout_ms; /**< Modem 默认命令超时，0 使用默认值； Modem default command timeout, 0 uses default */
    int        event_queue_size;       /**< Modem 事件队列长度，0 使用默认值； Modem event queue size, 0 uses default */
    int        event_task_stack;       /**< Modem 事件任务栈大小，0 使用默认值； Modem event task stack, 0 uses default */
    int        event_task_priority;    /**< Modem 事件任务优先级，0 使用默认值； Modem event task priority, 0 uses default */
} lwlte_modem_config_t;

/**
 * @brief Core 网络/PDP 与状态机配置
 * @details Core network/PDP and FSM configuration
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note primary_cid 当前仅支持 1。
 * @note 有符号的队列、任务字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
    const char *apn;                     /**< 可选 APN，NULL/空表示门面不配置； Optional APN, NULL/empty means facade does not configure it */
    uint8_t     primary_cid;             /**< 必填主 PDP 上下文 ID，当前仅支持 1； Required primary PDP context ID, currently supports 1 only */
    uint32_t    net_activate_timeout_ms; /**< 网络激活总超时，0 使用 Core 默认值； Network activation timeout, 0 uses Core default */
    uint32_t    reconnect_delay_ms;      /**< 重连延迟，0 使用 Core 默认值； Reconnect delay, 0 uses Core default */
    int         fsm_queue_size;          /**< Core FSM 队列长度，0 使用默认值； Core FSM queue size, 0 uses default */
    int         fsm_task_stack;          /**< Core FSM 任务栈大小，0 使用默认值； Core FSM task stack, 0 uses default */
    int         fsm_task_priority;       /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
} lwlte_core_config_t;

/**
 * @brief 事件总线配置
 * @details Event bus configuration
 */
typedef struct {
    esp_event_loop_handle_t loop;        /**< 可选事件总线，NULL 使用 default loop； Optional event loop, NULL uses default */
} lwlte_event_config_t;

/**
 * @brief LTE 公共基础配置
 * @details LTE common base configuration
 */
typedef struct {
    lwlte_uart_config_t      uart;      /**< UART 硬件； UART hardware */
    lwlte_at_engine_config_t at_engine; /**< AT 引擎调优； AT engine tuning */
    lwlte_modem_config_t     modem;     /**< 调制解调器； Modem */
    lwlte_core_config_t      core;      /**< Core 网络/状态机； Core network/FSM */
    lwlte_event_config_t     event;     /**< 事件总线； Event bus */
} lwlte_base_config_t;

/**
 * @brief Air780EP LTE 初始化配置
 * @details Air780EP LTE initialization configuration
 * @note base 为公共基础配置；UART 端口必须满足 UART_NUM_0 <= base.uart.num < UART_NUM_MAX，TX/RX 必须是有效 GPIO 且不能为 GPIO_NUM_NC，base.uart.baud_rate 必须大于 0。
 * @note Air780EP 启动在硬复位后通过 AT OK 探测就绪；base.modem.ready_timeout_ms 为该阶段总超时。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_air780ep_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* Air780EP 特有字段：暂无，预留； Air780EP-specific fields: none yet, reserved */
} lwlte_air780ep_config_t;

/**
 * @brief ML307R LTE 初始化配置
 * @details ML307R LTE initialization configuration
 * @note base 为公共基础配置；校验约束与 Air780EP 相同。
 * @note ML307R 启动不等待 +MATREADY，硬复位后重复发送 AT 并等待 OK；base.modem.ready_timeout_ms 为该阶段总超时。
 * @note MQTT 客户端不再在此配置中初始化；请在 lwlte_ml307r_init() 之后调用 lwlte_mqtt_init()。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* ML307R 特有字段：暂无，预留； ML307R-specific fields: none yet, reserved */
} lwlte_ml307r_config_t;
```

- [ ] **Step 4: 运行测试确认通过**

Run: `python3 -m pytest tests/host/test_lwlte_start_lifecycle.py tests/host/test_air780ep_command_gated_init.py -q`
Expected: PASS（两个文件全部用例通过）。

- [ ] **Step 5: 暂存（不提交）**

```bash
git add src/include/lwlte.h tests/host/test_lwlte_start_lifecycle.py tests/host/test_air780ep_command_gated_init.py
# 不要 commit；等待用户授权（见顶部提交策略）
```

---

## Task 2: lwlte_air780ep.c 工厂与校验改嵌套

**Files:**
- Test: `tests/host/test_mqtt_end_to_end_contract.py:832-841`
- Modify: `src/lwlte/lwlte_air780ep.c:120-171`、`src/lwlte/lwlte_air780ep.c:223-255`

- [ ] **Step 1: 更新契约测试（先失败）**

`tests/host/test_mqtt_end_to_end_contract.py` — 把 `test_air780ep_mqtt_config_validation_and_factory_wiring` 中 832–841 的 token 列表替换为：

```python
        for token in [
            "config->base.uart.num",
            "config->base.uart.tx_pin",
            "config->base.uart.rx_pin",
            "config->base.uart.baud_rate > 0",
            "config->base.core.primary_cid == LWLTE_AIR780EP_PRIMARY_CID",
            "non_negative_int(config->base.at_engine.rx_buf_size)",
            "non_negative_int(config->base.at_engine.rx_task_stack)",
            "non_negative_int(config->base.at_engine.rx_task_priority)",
        ]:
            self.assertIn(token, validate_body)
```

同时把 `test_event_loop_field_in_configs` 整个方法替换为：

```python
    def test_event_loop_field_in_configs(self):
        """Both lwlte config structs must carry typed event loop via base.event."""
        event_match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                                r"lwlte_event_config_t\s*;",
                                self.lwlte_h, re.DOTALL)
        self.assertIsNotNone(event_match, "lwlte_event_config_t typedef not found in lwlte.h")
        self.assertIn("esp_event_loop_handle_t loop", event_match.group("body"),
                      "lwlte_event_config_t missing loop field")

        base_match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                               r"lwlte_base_config_t\s*;",
                               self.lwlte_h, re.DOTALL)
        self.assertIsNotNone(base_match, "lwlte_base_config_t typedef not found in lwlte.h")
        self.assertRegex(base_match.group("body"), r"lwlte_event_config_t\s+event;",
                         "lwlte_base_config_t missing event config field")

        for config_name in ["lwlte_air780ep_config_t", "lwlte_ml307r_config_t"]:
            match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                              + config_name + r"\s*;",
                              self.lwlte_h, re.DOTALL)
            self.assertIsNotNone(match, f"{config_name} typedef not found in lwlte.h")
            self.assertRegex(match.group("body"), r"lwlte_base_config_t\s+base;",
                             f"{config_name} missing base config field")
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py::MqttEndToEndContract::test_air780ep_mqtt_config_validation_and_factory_wiring -q`
Expected: FAIL — `config->base.uart.num` 等不在 `validate_body` 中（当前仍是 `config->uart_num`）；`test_event_loop_field_in_configs` 在 Task 1 后若已为 nested 结构则可能已经通过。

> 若该 `-q` 节点路径因类名不同报收集错误，退用整文件：`python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`。

- [ ] **Step 3: 改 at_config / modem_config / event_loop / core_config 映射**

`src/lwlte/lwlte_air780ep.c` — 把 120–131 的 `at_config` 初始化替换为：

```c
    const at_engine_config_t at_config = {
        /* uart 组 */
        .uart_num = config->base.uart.num,
        .tx_pin = config->base.uart.tx_pin,
        .rx_pin = config->base.uart.rx_pin,
        .baud_rate = config->base.uart.baud_rate,
        /* at_engine 组 */
        .rx_buf_size = config->base.at_engine.rx_buf_size,
        .rx_task_stack = config->base.at_engine.rx_task_stack,
        .rx_task_priority = config->base.at_engine.rx_task_priority,
        .rx_line_buf_size = config->base.at_engine.rx_line_buf_size,
        .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
        .max_response_lines = config->base.at_engine.max_response_lines,
    };
```

把 141–149 的 `modem_config` 初始化替换为：

```c
    const modem_air780ep_config_t modem_config = {
        .en_pin = config->base.modem.en_pin,
        .reset_pulse_ms = config->base.modem.reset_pulse_ms,
        .ready_timeout_ms = config->base.modem.ready_timeout_ms,
        .default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms,
        .event_queue_size = config->base.modem.event_queue_size,
        .event_task_stack = config->base.modem.event_task_stack,
        .event_task_priority = config->base.modem.event_task_priority,
    };
```

把第 160 行的 event_loop 赋值替换为：

```c
    me->event_loop = config->base.event.loop;   /* NULL = use default loop */
```

把 162–171 的 `core_config` 初始化替换为：

```c
    const core_config_t core_config = {
        .apn = config->base.core.apn ? config->base.core.apn : "",
        .primary_cid = config->base.core.primary_cid,
        .net_activate_timeout_ms = config->base.core.net_activate_timeout_ms,
        .reconnect_delay_ms = config->base.core.reconnect_delay_ms,
        .fsm_queue_size = config->base.core.fsm_queue_size,
        .fsm_task_stack = config->base.core.fsm_task_stack,
        .fsm_task_priority = config->base.core.fsm_task_priority,
        .event_loop = me->event_loop,
    };
```

- [ ] **Step 4: 改 validate_config 校验路径**

把 `src/lwlte/lwlte_air780ep.c` 的 `validate_config` 函数体（223–255）替换为：

```c
static esp_err_t validate_config(const lwlte_air780ep_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->base.uart.num >= UART_NUM_0 &&
                        config->base.uart.num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(gpio_required_valid(config->base.uart.tx_pin) &&
                        gpio_required_valid(config->base.uart.rx_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(gpio_optional_valid(config->base.modem.en_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid en_pin GPIO");
    ESP_RETURN_ON_FALSE(config->base.uart.baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART baud rate");
    ESP_RETURN_ON_FALSE(config->base.core.primary_cid == LWLTE_AIR780EP_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG,
                        "primary CID must be 1");
    ESP_RETURN_ON_FALSE(non_negative_int(config->base.at_engine.rx_buf_size) &&
                        non_negative_int(config->base.at_engine.rx_task_stack) &&
                        non_negative_int(config->base.at_engine.rx_task_priority) &&
                        non_negative_int(config->base.at_engine.rx_line_buf_size) &&
                        non_negative_int(config->base.at_engine.cmd_default_timeout_ms) &&
                        non_negative_int(config->base.at_engine.max_response_lines) &&
                        non_negative_int(config->base.modem.event_queue_size) &&
                        non_negative_int(config->base.modem.event_task_stack) &&
                        non_negative_int(config->base.modem.event_task_priority) &&
                        non_negative_int(config->base.core.fsm_queue_size) &&
                        non_negative_int(config->base.core.fsm_task_stack) &&
                        non_negative_int(config->base.core.fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");

    return ESP_OK;
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `python3 -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`
Expected: PASS（整文件全部用例通过）。

- [ ] **Step 6: 暂存（不提交）**

```bash
git add src/lwlte/lwlte_air780ep.c tests/host/test_mqtt_end_to_end_contract.py
# 不要 commit；等待用户授权
```

---

## Task 3: lwlte_ml307r.c 工厂与校验改嵌套（保留 2048 兜底）

**Files:**
- Test: `tests/host/test_ml307r_contract.py:145-148`
- Modify: `src/lwlte/lwlte_ml307r.c:57-103`、`src/lwlte/lwlte_ml307r.c:151-182`

- [ ] **Step 1: 更新契约测试（先失败）**

`tests/host/test_ml307r_contract.py` — 把 `test_facade_defaults_ml307r_line_buffer_for_direct_mqtt_urcs` 中 145–148 的断言替换为：

```python
        self.assertIn(
            ".rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?",
            init_body,
        )
```

- [ ] **Step 2: 运行测试确认失败**

Run: `python3 -m pytest tests/host/test_ml307r_contract.py -q`
Expected: FAIL — `test_facade_defaults_ml307r_line_buffer_for_direct_mqtt_urcs` 找不到 `config->base.at_engine.rx_line_buf_size ?`（当前是 `config->at_rx_line_buf_size`）。

- [ ] **Step 3: 改 at_config / modem_config / event_loop / core_config 映射**

`src/lwlte/lwlte_ml307r.c` — 把 57–70 的 `at_config` 初始化替换为（注意 `rx_line_buf_size` 保留 2048 兜底）：

```c
    const at_engine_config_t at_config = {
        /* uart 组 */
        .uart_num = config->base.uart.num,
        .tx_pin = config->base.uart.tx_pin,
        .rx_pin = config->base.uart.rx_pin,
        .baud_rate = config->base.uart.baud_rate,
        /* at_engine 组 */
        .rx_buf_size = config->base.at_engine.rx_buf_size,
        .rx_task_stack = config->base.at_engine.rx_task_stack,
        .rx_task_priority = config->base.at_engine.rx_task_priority,
        .rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?
                             config->base.at_engine.rx_line_buf_size :
                             LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE,
        .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
        .max_response_lines = config->base.at_engine.max_response_lines,
    };
```

把 77–85 的 `modem_config` 初始化替换为：

```c
    const modem_ml307r_config_t modem_config = {
        .en_pin = config->base.modem.en_pin,
        .reset_pulse_ms = config->base.modem.reset_pulse_ms,
        .ready_timeout_ms = config->base.modem.ready_timeout_ms,
        .default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms,
        .event_queue_size = config->base.modem.event_queue_size,
        .event_task_stack = config->base.modem.event_task_stack,
        .event_task_priority = config->base.modem.event_task_priority,
    };
```

把第 92 行的 event_loop 赋值替换为：

```c
    me->event_loop = config->base.event.loop;   /* NULL = use default loop */
```

把 94–103 的 `core_config` 初始化替换为：

```c
    const core_config_t core_config = {
        .apn = config->base.core.apn ? config->base.core.apn : "",
        .primary_cid = config->base.core.primary_cid,
        .net_activate_timeout_ms = config->base.core.net_activate_timeout_ms,
        .reconnect_delay_ms = config->base.core.reconnect_delay_ms,
        .fsm_queue_size = config->base.core.fsm_queue_size,
        .fsm_task_stack = config->base.core.fsm_task_stack,
        .fsm_task_priority = config->base.core.fsm_task_priority,
        .event_loop = me->event_loop,
    };
```

- [ ] **Step 4: 改 validate_config 校验路径**

把 `src/lwlte/lwlte_ml307r.c` 的 `validate_config` 函数体（151–182）替换为：

```c
static esp_err_t validate_config(const lwlte_ml307r_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->base.uart.num >= UART_NUM_0 &&
                        config->base.uart.num < UART_NUM_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid uart_num");
    ESP_RETURN_ON_FALSE(gpio_required_valid(config->base.uart.tx_pin) &&
                        gpio_required_valid(config->base.uart.rx_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART pins");
    ESP_RETURN_ON_FALSE(gpio_optional_valid(config->base.modem.en_pin),
                        ESP_ERR_INVALID_ARG, TAG, "invalid en_pin GPIO");
    ESP_RETURN_ON_FALSE(config->base.uart.baud_rate > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid UART baud rate");
    ESP_RETURN_ON_FALSE(config->base.core.primary_cid == LWLTE_ML307R_PRIMARY_CID,
                        ESP_ERR_INVALID_ARG, TAG, "primary CID must be 1");
    ESP_RETURN_ON_FALSE(non_negative_int(config->base.at_engine.rx_buf_size) &&
                        non_negative_int(config->base.at_engine.rx_task_stack) &&
                        non_negative_int(config->base.at_engine.rx_task_priority) &&
                        non_negative_int(config->base.at_engine.rx_line_buf_size) &&
                        non_negative_int(config->base.at_engine.cmd_default_timeout_ms) &&
                        non_negative_int(config->base.at_engine.max_response_lines) &&
                        non_negative_int(config->base.modem.event_queue_size) &&
                        non_negative_int(config->base.modem.event_task_stack) &&
                        non_negative_int(config->base.modem.event_task_priority) &&
                        non_negative_int(config->base.core.fsm_queue_size) &&
                        non_negative_int(config->base.core.fsm_task_stack) &&
                        non_negative_int(config->base.core.fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG,
                        "defaultable integer fields must be non-negative");

    return ESP_OK;
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `python3 -m pytest tests/host/test_ml307r_contract.py -q`
Expected: PASS（整文件全部用例通过）。

- [ ] **Step 6: 暂存（不提交）**

```bash
git add src/lwlte/lwlte_ml307r.c tests/host/test_ml307r_contract.py
# 不要 commit；等待用户授权
```

---

## Task 4: 四个示例 config 字面量改嵌套

**Files:**
- Modify: `example/air780ep_basic_connect.c:103-114`、`example/air780ep_mqtt_client.c:140-151`、`example/ml307r_basic_connect.c:103-114`、`example/ml307r_mqtt_client.c:140-151`

> 四个文件的 config 字面量字段完全一致，仅结构体类型名不同（`lwlte_air780ep_config_t` / `lwlte_ml307r_config_t`）。无对应文本契约测试，编译验证在 Task 6。

- [ ] **Step 1: 改 air780ep_basic_connect.c**

把 `example/air780ep_basic_connect.c:103-114` 的 config 字面量替换为：

```c
    const lwlte_air780ep_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
            .event = {
                .loop = NULL,
            },
        },
    };
```

- [ ] **Step 2: 改 air780ep_mqtt_client.c**

把 `example/air780ep_mqtt_client.c:140-151` 的 config 字面量替换为与 Step 1 完全相同的内容（结构体类型同为 `lwlte_air780ep_config_t`）：

```c
    const lwlte_air780ep_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
            .event = {
                .loop = NULL,
            },
        },
    };
```

- [ ] **Step 3: 改 ml307r_basic_connect.c**

把 `example/ml307r_basic_connect.c:103-114` 的 config 字面量替换为（类型为 `lwlte_ml307r_config_t`）：

```c
    const lwlte_ml307r_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
            .event = {
                .loop = NULL,
            },
        },
    };
```

- [ ] **Step 4: 改 ml307r_mqtt_client.c**

把 `example/ml307r_mqtt_client.c:140-151` 的 config 字面量替换为与 Step 3 完全相同的内容（类型 `lwlte_ml307r_config_t`）：

```c
    const lwlte_ml307r_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
            .event = {
                .loop = NULL,
            },
        },
    };
```

- [ ] **Step 5: 静态确认无遗留平铺写法**

Run: `rg -n "\.uart_num\s*=|\.init_ready_timeout_ms\s*=|\.modem_reset_pulse_ms\s*=" example/`
Expected: 无输出（四个示例已无平铺字段写法）。

- [ ] **Step 6: 暂存（不提交）**

```bash
git add example/air780ep_basic_connect.c example/air780ep_mqtt_client.c example/ml307r_basic_connect.c example/ml307r_mqtt_client.c
# 不要 commit；等待用户授权
```

---

## Task 5: Agent 文档示例同步

**Files:**
- Modify: `docs/agents/architecture.md:290-451`、`docs/agents/oop-design.md:677-693`

- [ ] **Step 1: 改 architecture.md 的 config 字面量**

把 `docs/agents/architecture.md:301-309` 替换为：

```c
lwlte_air780ep_config_t config = {
    .base = {
        .uart = {
            .num       = UART_NUM_1,
            .tx_pin    = GPIO_NUM_17,
            .rx_pin    = GPIO_NUM_16,
            .baud_rate = 115200,
        },
        .modem = {
            .en_pin = GPIO_NUM_NC,
        },
        .core = {
            .apn         = CONFIG_LWLTE_APN,
            .primary_cid = 1,
        },
        .event = {
            .loop = NULL,
        },
    },
};
```

- [ ] **Step 2: 改 architecture.md 工厂示例片段的字段读取**

把 `docs/agents/architecture.md:290` 的说明文字改为描述 `lwlte_air780ep_config_t.base` 下的 `uart`、`at_engine`、`modem`、`core`、`event` 嵌套公开分组，并说明 MQTT 配置仍通过独立 `lwlte_mqtt_config_t` 传入。

把 `docs/agents/architecture.md:369-430` 的 factory 示例片段替换为能展示完整分发路径的版本：

```c
    /* 1. 底：创建 AT Engine（直接传入 UART 硬件和 AT 引擎调优配置） */
    at_engine_config_t at_cfg = {
        .uart_num               = config->base.uart.num,
        .tx_pin                 = config->base.uart.tx_pin,
        .rx_pin                 = config->base.uart.rx_pin,
        .baud_rate              = config->base.uart.baud_rate,
        .rx_buf_size            = config->base.at_engine.rx_buf_size,
        .rx_task_stack          = config->base.at_engine.rx_task_stack,
        .rx_task_priority       = config->base.at_engine.rx_task_priority,
        .rx_line_buf_size       = config->base.at_engine.rx_line_buf_size,
        .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
        .max_response_lines     = config->base.at_engine.max_response_lines,
    };
    at_engine_handle_t *at = at_engine_create(&at_cfg);
    if (!at) return ESP_FAIL;

    /* 2. 模块适配（换模块只需换这一组配置和工厂） */
    modem_air780ep_config_t modem_cfg = {
        .en_pin                 = config->base.modem.en_pin,
        .reset_pulse_ms         = config->base.modem.reset_pulse_ms,
        .ready_timeout_ms       = config->base.modem.ready_timeout_ms,
        .default_cmd_timeout_ms = config->base.modem.default_cmd_timeout_ms,
        .event_queue_size       = config->base.modem.event_queue_size,
        .event_task_stack       = config->base.modem.event_task_stack,
        .event_task_priority    = config->base.modem.event_task_priority,
    };
    modem_handle_t *modem = modem_air780ep_create(at, &modem_cfg);
    if (!modem) goto err_at;

    /* 3. 核心服务：启动请求由 lwlte_start() 异步提交给 Core。 */
    esp_event_loop_handle_t event_loop = config->base.event.loop;
    core_config_t core_cfg = {
        .apn                     = config->base.core.apn ? config->base.core.apn : "",
        .primary_cid             = config->base.core.primary_cid,
        .net_activate_timeout_ms = config->base.core.net_activate_timeout_ms,
        .reconnect_delay_ms      = config->base.core.reconnect_delay_ms,
        .fsm_queue_size          = config->base.core.fsm_queue_size,
        .fsm_task_stack          = config->base.core.fsm_task_stack,
        .fsm_task_priority       = config->base.core.fsm_task_priority,
        .event_loop              = event_loop,
    };
    core_handle_t *core = core_create(&core_cfg, modem);
    if (!core) goto err_modem;

    lwlte_handle_t *lte = calloc(1, sizeof(*lte));
    if (!lte) goto err_core;

    lte->at = at;
    lte->modem = modem;
    lte->core = core;
    lte->event_loop = event_loop;
    if (event_loop) {
        esp_event_handler_register_with(event_loop, LWLTE_EVENT, LWLTE_EVENT_READY,
                                        facade_ready_handler, lte);
    } else {
        esp_event_handler_register(LWLTE_EVENT, LWLTE_EVENT_READY,
                                   facade_ready_handler, lte);
    }
```

同时把 factory 流程图中事件注册步骤改为按 `event_loop` 选择 `esp_event_handler_register_with()` 或默认 loop 注册。

- [ ] **Step 3: 改 oop-design.md 的 config 字面量**

把 `docs/agents/oop-design.md:677-684` 替换为：

```c
    lwlte_air780ep_config_t config = {
        .base = {
            .uart = {
                .num       = CONFIG_LWLTE_UART_NUM,
                .tx_pin    = CONFIG_LWLTE_TX_GPIO,
                .rx_pin    = CONFIG_LWLTE_RX_GPIO,
                .baud_rate = 115200,
            },
            .modem = {
                .en_pin = GPIO_NUM_NC,
            },
            .core = {
                .apn         = CONFIG_LWLTE_APN,
                .primary_cid = 1,
            },
        },
    };
```

- [ ] **Step 4: 静态确认文档无遗留平铺写法**

Run: `rg -n "config->uart_num|config->uart_tx_pin|config->en_pin|config->apn|\.uart_num\s*=" docs/agents/architecture.md docs/agents/oop-design.md`
该命令过宽，会误报 `at_engine_config_t` 的合法下层字段 `.uart_num = config->base.uart.num`。实际执行使用以下 corrected stale-check：

```bash
rg -n "config->uart_num|config->uart_tx_pin|config->uart_rx_pin|config->uart_baud_rate|config->en_pin|config->apn|config->primary_cid|\.uart_num\s*=\s*(UART_NUM_|CONFIG_|EXAMPLE_)|\.uart_tx_pin\s*=|\.uart_rx_pin\s*=|\.uart_baud_rate\s*=" docs/agents/architecture.md docs/agents/oop-design.md
rg -n "只含 Core/Modem/AT 字段|UART/GPIO 字段|UART/GPIO 参数|UART/GPIO 配置" docs/agents/architecture.md docs/agents/oop-design.md
```

Expected: 两个命令均无输出。

- [ ] **Step 5: 暂存（不提交）**

```bash
git add docs/agents/architecture.md docs/agents/oop-design.md
# 不要 commit；等待用户授权
```

---

## Task 6: 全量验证 + 受授权门控的提交

**Files:** 无（仅验证）

- [ ] **Step 1: 全量 host 契约测试**

Run: `python3 -m pytest tests/host/ -q`
Expected: PASS — 全部用例通过，无 FAIL/ERROR。

- [ ] **Step 2: ESP-IDF 编译（一次覆盖全部 4 个示例）**

`example/CMakeLists.txt` 把四个示例 `.c` 全部列入 SRCS，故一次 `idf.py build` 即编译 src（头 + 两工厂）与全部 4 个示例字面量。

Run:
```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py build
```
Expected: 编译成功，结尾出现 `Project build complete.`；无报错（尤其无 `lwlte_*_config_t` 字段相关错误）。

- [ ] **Step 3: 失败处置**

若编译报某字段不存在/初始化器不匹配：核对该处是否仍用平铺路径（应为 `config->base.<group>.<field>`，示例为 `.base.<group>.<field>`）。修正后重跑 Step 1、Step 2。

- [ ] **Step 4: 暂存全部改动并请求提交授权（不自动提交）**

```bash
git add -A
git status   # 复核改动集
```

向用户报告：契约全绿 + 编译通过，列出改动文件，并**请求提交授权**。获授权后（且仅在此时）执行单次提交，建议信息：

```
refactor(config): group lwlte module configs into nested base sub-structs

Restructure lwlte_air780ep_config_t / lwlte_ml307r_config_t into a shared
lwlte_base_config_t with uart/at_engine/modem/core/event sub-structs plus a
reserved per-module slot. Rename init_ready_timeout_ms -> modem.ready_timeout_ms.
Update factories, validation, examples, docs, and host contract tests.
No behavior change.
```

---

## Self-Review（写计划后自查结果）

- **Spec 覆盖**：spec §3.1/3.2（5 子结构体+base+包装）→ Task 1；§3.3 字段映射 + §4 工厂分发 → Task 2/3；§6 示例 → Task 4；§6 文档 → Task 5；§7 契约测试逐条 → Task 1（lifecycle/gated_init）、Task 2（mqtt validate + event_loop nested contract）、Task 3（ml307r line buf）；§8 验证 → Task 6。`init_ready_timeout_ms → ready_timeout_ms` 重命名贯穿 Task 1（头+测试）、Task 2/3（工厂）、Task 4（示例）。无遗漏。
- **Placeholder 扫描**：每个改代码步骤均给出完整代码块与精确路径/行号；验证步骤给出可执行命令与预期输出。无 TBD/TODO/"类似上文"。
- **类型/名称一致性**：测试断言的 token（如 `config->base.at_engine.rx_buf_size`、`} lwlte_modem_config_t;`、`config->base.at_engine.rx_line_buf_size ?`）与对应源码改动逐字一致；字段名（`num`/`ready_timeout_ms`/`fsm_queue_size`/`loop` 等）跨 Task 统一。
- **编译耦合**：已在顶部说明中间态不可编译，故不做逐任务提交；唯一提交在 Task 6 验证后、且受用户授权门控。

---

## Execution Handoff

计划已保存到 `docs/superpowers/plans/2026-06-16-lwlte-config-nested-grouping.md`。两种执行方式：

1. **Subagent-Driven（推荐）** — 每个任务派发独立 subagent，任务间我来审查，迭代快。
2. **Inline Execution** — 在当前会话用 executing-plans 批量执行，带检查点复核。

> 无论哪种：依顶部提交策略，全程不提交，直到你在 Task 6 后显式授权。

选哪种？
