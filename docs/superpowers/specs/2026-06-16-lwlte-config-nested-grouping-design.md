# lwlte 模块配置嵌套分组设计

- 日期：2026-06-16
- 状态：设计已确认，待评审
- 主题：把 `lwlte_air780ep_config_t` / `lwlte_ml307r_config_t` 从平铺字段重构为「公共基础 + 嵌套分组」结构

## 1. 背景与问题

`src/include/lwlte.h` 中的 `lwlte_air780ep_config_t`（lwlte.h:225）和 `lwlte_ml307r_config_t`（lwlte.h:263）当前各有 25 个平铺成员，靠 `uart_`/`at_`/`modem_`/`core_` 前缀人工区分语义。两个问题：

1. **可读性差**：25 个字段糊在一个结构体里，填配置时看不出哪些属于同一子系统。
2. **重复严重**：两个结构体逐字段完全相同（字段名、类型、顺序一致），连下层 `modem_air780ep_config_t`（modem_air780ep.h:36）与 `modem_ml307r_config_t`（modem_ml307r.h:36）也完全相同。当前**在任何一层都不存在模块特有字段**。

ESP-IDF v5.x 的 `esp_mqtt_client_config_t` 用「结构体套结构体」分组（broker/credentials/session/network/task/buffer/outbox）解决了同类问题，本设计参照该思路。

## 2. 目标与非目标

### 目标

- 把公共字段按子系统拆成 5 个嵌套子结构体：`uart` / `at_engine` / `modem` / `core` / `event`。
- 抽出公共基础聚合体 `lwlte_base_config_t`，两个模块配置 = `base` + 模块特有字段。
- 嵌套后去掉冗余前缀（`at_rx_buf_size` → `at_engine.rx_buf_size`）。
- 工厂映射按子系统分块，来源一目了然。
- **行为零变化**：校验逻辑、默认值、启动语义完全保持。

### 非目标

- 不改 `lwlte_mqtt_config_t`（lwlte.h:199），MQTT 配置不在本次范围。
- 不改下层 `at_engine_config_t` / `modem_*_config_t` / `core_config_t` 的定义。
- 不把 facade 公共类型与下层内部类型合并复用（见 §5）。
- 不合并两个 `lwlte_*_init()` 函数，保持每模块独立工厂。
- 不新增任何真实的模块特有字段（当前为空预留位）。

## 3. 设计

### 3.1 五个公共子结构体

字段顺手去前缀，注释沿用原结构体的双语 Doxygen 文案。

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
```

### 3.2 公共基础与模块包装（方案 A）

```c
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
 * @note Air780EP 启动在硬复位后通过 AT OK 探测就绪；base.modem.ready_timeout_ms 为该阶段总超时。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* Air780EP 特有字段：暂无，预留； Air780EP-specific fields: none yet, reserved */
} lwlte_air780ep_config_t;

/**
 * @brief ML307R LTE 初始化配置
 * @details ML307R LTE initialization configuration
 * @note ML307R 启动不等待 +MATREADY，硬复位后重复发送 AT 并等待 OK；base.modem.ready_timeout_ms 为该阶段总超时。
 */
typedef struct {
    lwlte_base_config_t base;  /**< 公共基础配置； Common base configuration */
    /* ML307R 特有字段：暂无，预留； ML307R-specific fields: none yet, reserved */
} lwlte_ml307r_config_t;
```

访问形如 `config.base.uart.tx_pin`、`config.base.core.apn`、`config.base.modem.ready_timeout_ms`。

模块特有的启动语义差异（Air780EP vs ML307R 的就绪探测）以前分散在字段级 `@note`，现在收敛到各模块包装结构体的结构级 `@note` 与对应 `lwlte_*_init()` 函数文档，字段级注释保持模块无关的通用描述。

### 3.3 完整字段映射（25 个）

| 旧（平铺） | 新（嵌套） |
|---|---|
| `uart_num` | `base.uart.num` |
| `uart_tx_pin` | `base.uart.tx_pin` |
| `uart_rx_pin` | `base.uart.rx_pin` |
| `uart_baud_rate` | `base.uart.baud_rate` |
| `at_rx_buf_size` | `base.at_engine.rx_buf_size` |
| `at_rx_task_stack` | `base.at_engine.rx_task_stack` |
| `at_rx_task_priority` | `base.at_engine.rx_task_priority` |
| `at_rx_line_buf_size` | `base.at_engine.rx_line_buf_size` |
| `at_cmd_default_timeout_ms` | `base.at_engine.cmd_default_timeout_ms` |
| `at_max_response_lines` | `base.at_engine.max_response_lines` |
| `en_pin` | `base.modem.en_pin` |
| `modem_reset_pulse_ms` | `base.modem.reset_pulse_ms` |
| `init_ready_timeout_ms` | `base.modem.ready_timeout_ms`（重命名，见 §3.4） |
| `modem_default_cmd_timeout_ms` | `base.modem.default_cmd_timeout_ms` |
| `modem_event_queue_size` | `base.modem.event_queue_size` |
| `modem_event_task_stack` | `base.modem.event_task_stack` |
| `modem_event_task_priority` | `base.modem.event_task_priority` |
| `apn` | `base.core.apn` |
| `primary_cid` | `base.core.primary_cid` |
| `net_activate_timeout_ms` | `base.core.net_activate_timeout_ms` |
| `reconnect_delay_ms` | `base.core.reconnect_delay_ms` |
| `core_fsm_queue_size` | `base.core.fsm_queue_size` |
| `core_fsm_task_stack` | `base.core.fsm_task_stack` |
| `core_fsm_task_priority` | `base.core.fsm_task_priority` |
| `event_loop` | `base.event.loop` |

### 3.4 `init_ready_timeout_ms` 重命名

该字段在两个工厂里都喂给下层 `modem` 的 `ready_timeout_ms`（lwlte_air780ep.c:144、lwlte_ml307r.c:80）。归入 `modem` 组并改名为 `ready_timeout_ms`，与下层字段名一致。"init/ready" 的启动语义通过模块包装结构体的 `@note` 表达。

## 4. 工厂映射改动

`lwlte_air780ep.c` 与 `lwlte_ml307r.c` 的 `lwlte_*_init()` 和 `validate_config()` 把所有 `config->xxx` 访问改为嵌套路径，并按子系统分块组织，例如：

```c
const at_engine_config_t at_config = {
    /* uart 组 */
    .uart_num   = config->base.uart.num,
    .tx_pin     = config->base.uart.tx_pin,
    .rx_pin     = config->base.uart.rx_pin,
    .baud_rate  = config->base.uart.baud_rate,
    /* at_engine 组 */
    .rx_buf_size            = config->base.at_engine.rx_buf_size,
    .rx_task_stack          = config->base.at_engine.rx_task_stack,
    .rx_task_priority       = config->base.at_engine.rx_task_priority,
    .rx_line_buf_size       = config->base.at_engine.rx_line_buf_size, /* ML307R 仍保留 ?: 2048 兜底 */
    .cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms,
    .max_response_lines     = config->base.at_engine.max_response_lines,
};
```

保持不变的行为约束：

- `at_engine_config_t` 由 `uart` + `at_engine` 两组共同填充（非 1:1）。
- `core_config_t.event_loop` 取自 `base.event.loop`（经 `me->event_loop`），`.apn` 保留 `?: ""` 兜底。
- ML307R 的 `rx_line_buf_size` 保留 `config->base.at_engine.rx_line_buf_size ? : LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE` 逻辑。
- `validate_config()` 校验项与原逻辑逐条等价，仅访问路径变化。

## 5. 为什么不复用下层类型

`lwlte_modem_config_t` 与 `modem_air780ep_config_t` 字段完全一致，看似可直接复用，但**不可**：`lwlte.h` 是公共门面头，按架构分层规则（architecture.md、oop-design.md），App/板级代码不得 include 下层 Core/Modem/AT Engine 头。因此 facade 子结构体必须是独立的公共类型，工厂内保持逐字段拷贝。这是分层约束，不是冗余。

## 6. 影响面

### 源码（必改）

- `src/include/lwlte.h`：删除两个平铺结构体，新增 5 个子结构体 + `lwlte_base_config_t` + 2 个模块包装；重分配 `@note`。
- `src/lwlte/lwlte_air780ep.c`：`lwlte_air780ep_init()` 与 `validate_config()` 改嵌套访问。
- `src/lwlte/lwlte_ml307r.c`：同上，保留 2048 兜底逻辑。

### 示例（必改，config 字面量）

- `example/air780ep_basic_connect.c:103`、`example/air780ep_mqtt_client.c:140`
- `example/ml307r_basic_connect.c:103`、`example/ml307r_mqtt_client.c:140`

### Agent 文档（按 AGENTS 同步原则）

- `docs/agents/architecture.md`：config 字面量（约 :301）与映射片段（约 :370）。
- `docs/agents/oop-design.md`：config 字面量（约 :677）。
- `docs/agents/classes.md:1936` 仅函数签名，不变。
- 历史 `docs/superpowers/specs|plans/` 为存档，不回改。

## 7. 契约测试改动清单（精确）

以下 host 契约测试把旧字段拼写钉死在断言里，必须同步更新：

1. **`tests/host/test_mqtt_end_to_end_contract.py`**（`test_air780ep_mqtt_config_validation_and_factory_wiring`，832-840）：
   - `config->uart_num` → `config->base.uart.num`
   - `config->uart_tx_pin` → `config->base.uart.tx_pin`
   - `config->uart_rx_pin` → `config->base.uart.rx_pin`
   - `config->uart_baud_rate > 0` → `config->base.uart.baud_rate > 0`
   - `config->primary_cid == LWLTE_AIR780EP_PRIMARY_CID` → `config->base.core.primary_cid == LWLTE_AIR780EP_PRIMARY_CID`
   - `non_negative_int(config->at_rx_buf_size)` → `non_negative_int(config->base.at_engine.rx_buf_size)`
   - `non_negative_int(config->at_rx_task_stack)` → `...base.at_engine.rx_task_stack`
   - `non_negative_int(config->at_rx_task_priority)` → `...base.at_engine.rx_task_priority`
   - init_body 的下层变量名断言（`at_engine_create(&at_config)` 等，848-854）**不变**。
   - 718-723 是 at_engine 内部（`me->config.uart_num`），**不变**。

2. **`tests/host/test_lwlte_start_lifecycle.py:89`**：锚点 `"uart_port_t uart_num;"` → `"uart_port_t num;"`（现位于 `lwlte_uart_config_t`）。`assert_not_contains(config_body, "auto_connect")` 的提取区间锚点需相应调整。

3. **`tests/host/test_air780ep_command_gated_init.py:143-152`**（`test_public_config_comments_use_at_ready_not_rdy_wait`）：`init_ready_timeout_ms` → `ready_timeout_ms`；`@note` 锚点改到新字段/包装结构体；结束锚点 `uint32_t net_activate_timeout_ms`（现位于 `lwlte_core_config_t`，名称不变）；保留 "AT OK" 文案校验。

4. **`tests/host/test_ml307r_contract.py:146`**：`.rx_line_buf_size = config->at_rx_line_buf_size ?` → `.rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?`。该文件 74-83 校验的是 `modem_ml307r.h`（下层），**不变**。

5. **`tests/host/test_mqtt_end_to_end_contract.py:1095-1108`**（`test_event_loop_field_in_configs`）：原先检查 `event_loop` 是两个模块配置结构体的直接字段。嵌套后改为检查 `lwlte_event_config_t` 含 `esp_event_loop_handle_t loop`、`lwlte_base_config_t` 含 `lwlte_event_config_t event`、两个模块包装结构体含 `lwlte_base_config_t base`。

未受影响（仅引用结构体名/init 签名，名称不变）：`test_ml307r_examples_contract.py`。

## 8. 验证

1. **静态/契约**：运行 `tests/host` 全部契约测试，确认上述更新后全绿。
2. **编译**：按 `docs/agents/build-and-debug.md` 初始化环境（`source ~/.espressif/v6.0/esp-idf/export.sh`）后 `idf.py build`，分别覆盖 Air780EP 与 ML307R 示例配置，确认无编译错误。
3. 本次为纯结构重构、行为不变，不要求实机串口验证；如用户需要可烧录基础连接示例确认联网行为未回归。

> 精确的测试运行命令在实现计划（writing-plans 阶段）中确认。

## 9. 风险与取舍

- **访问层级加深**：`config.base.uart.tx_pin` 比原来多一层。已与用户确认接受，换取公共基础单一来源、零成员重复。
- **空模块包装**：`lwlte_air780ep_config_t` 当前仅含 `base` 一个成员，是合法 C；作为未来模块特有字段的预留位，符合"先定公共基础、再加模块特有"的演进路径。
- **字段重命名与 event 嵌套**（`init_ready_timeout_ms` → `ready_timeout_ms`、`event_loop` → `base.event.loop`）会触达契约测试，已在 §7 列明。

## 10. 开放问题

无。所有设计分叉（分组粒度、组合方式、字段重命名）已与用户确认。
