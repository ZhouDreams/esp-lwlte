# Modem 模块实现设计

日期：2026-05-23

## 背景

本设计用于实现 esp-lwlte 的第二部分：Modem Adapter（模块适配层）。AT Engine 已提供 `at_engine_send_cmd()`、`at_engine_send_cmd_with_options()` 和 URC 注册能力，`docs/agents/classes.md` 已定义 Modem 层的类、值对象、事件模型和 Air780EP 子类边界。

本轮目标是在源码中落地该设计，使 Core 后续可以只通过 `modem_*` 语义 API 使用 LTE 模块，而不直接依赖 AT Engine 或 Air780EP 具体 AT 指令。

## 范围

本轮实现完整但收敛的 Modem Adapter：

- 新增通用 Modem 公共 API、内部基类和 ops 分发。
- 新增 Air780EP 工厂与基础系统/联网能力实现。
- 新增 Modem event queue 和 event task，将 URC 从 AT Engine RX task 解耦后上报。
- 接入 CMake，使 ESP-IDF build 覆盖新增源码。

本轮不实现：

- Core Service 网络状态机。
- MQTT、HTTP、FTP、SMS、GNSS、socket 数据收发等业务协议。
- 低功耗公开 API；低功耗 AT 指令只保留为后续扩展参考。
- 真实硬件联网验证；本轮默认完成编译验证。

## 方案选择

采用“完整但收敛”的实现路线：一次性落地 `modem.h`、`modem_air780ep.h`、`modem_priv.h`、`modem.c` 和 `modem_air780ep.c`，Air780EP 方法实际发送并解析基础 AT 指令。

该方案优于只写骨架，因为 Core 下一阶段可以直接对接 Modem 语义 API；也优于绕过基类直接写 Air780EP，因为它保持已有 OOP/分层设计，后续增加其他模块只需新增子类和 ops 表。

## 文件结构

新增文件：

- `src/include/modem.h`：层间 API。暴露 opaque `modem_t`、状态枚举、SIM/注册/信号/PDP/信息值对象、事件类型、回调类型和 `modem_*` 包装 API。
- `src/include/modem_air780ep.h`：Board Init 专用 API。暴露 `modem_air780ep_config_t` 和 `modem_air780ep_create()`。
- `src/modem/modem_priv.h`：Modem 层内部头文件。定义 `struct modem`、`modem_ops_t`、基类初始化/销毁辅助、事件投递辅助和 state setter。
- `src/modem/modem.c`：通用基类实现、公共 wrapper、event task、生命周期辅助。
- `src/modem/modem_air780ep.c`：Air780EP 子类、AT 指令发送/解析、URC handler。

修改文件：

- `src/CMakeLists.txt`：加入 `modem/modem.c`、`modem/modem_air780ep.c`，并把 GPIO 驱动依赖加入 `REQUIRES`。

## 公共 API

`src/include/modem.h` 保持 Core 可见但隐藏内部结构：

```c
typedef struct modem modem_t;

esp_err_t modem_destroy(modem_t *me);
esp_err_t modem_init(modem_t *me);
esp_err_t modem_reset(modem_t *me);

esp_err_t modem_register_event_callback(modem_t *me,
                                         modem_event_callback_t callback,
                                         void *user_ctx);

esp_err_t modem_get_state(modem_t *me, modem_state_t *state);
esp_err_t modem_get_info(modem_t *me, modem_info_t *info);
esp_err_t modem_get_sim_status(modem_t *me, modem_sim_status_t *status);
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal);
esp_err_t modem_get_registration(modem_t *me, modem_reg_status_t *status);

esp_err_t modem_set_apn(modem_t *me, uint8_t cid, const char *apn);
esp_err_t modem_activate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_deactivate_pdp(modem_t *me, uint8_t cid);
esp_err_t modem_get_pdp_context(modem_t *me, uint8_t cid,
                                 modem_pdp_context_t *pdp);
```

`src/include/modem_air780ep.h` 只给 Board Init 使用：

```c
typedef struct {
    gpio_num_t pwrkey_pin;
    gpio_num_t reset_pin;
    gpio_num_t status_pin;
    uint32_t power_on_pulse_ms;
    uint32_t reset_pulse_ms;
    uint32_t boot_wait_ms;
    uint32_t default_cmd_timeout_ms;
    int event_queue_size;
    int event_task_stack;
    int event_task_priority;
} modem_air780ep_config_t;

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config);
```

## 内部结构

`modem_t` 是内部基类，包含 ops 指针、AT Engine 句柄、事件资源、回调和状态：

```c
struct modem {
    const modem_ops_t *ops;
    at_engine_t *at;
    SemaphoreHandle_t lock;
    QueueHandle_t event_queue;
    TaskHandle_t event_task;
    SemaphoreHandle_t event_task_done_sema;
    modem_event_callback_t event_cb;
    void *event_user_ctx;
    modem_state_t state;
    bool destroying;
    bool event_task_stop_requested;
    const char *name;
};
```

`modem_ops_t` 只在 Modem 层内部可见，由 Air780EP 以 `static const modem_ops_t` 注入。通用 wrapper 负责参数、状态和方法检查，再调用 `me->ops->method(me, ...)`。

Air780EP 子类以 `modem_t base` 作为第一个字段，使用 `container_of(me, modem_air780ep_t, base)` 做向下转型，禁止裸强转。

## 生命周期

`modem_air780ep_create()`：

- 校验 `at` 和配置。
- 分配并清零 `modem_air780ep_t`。
- 调用基类初始化辅助，创建 `lock`、`event_queue`、`event_task_done_sema` 和 `event_task`。
- 保存配置快照、默认超时、AT Engine 句柄、初始 state 和缓存默认值。
- 返回 `&self->base`。

`modem_init()`：

- wrapper 检查对象未销毁并有 `ops->init`。
- Air780EP 注册 `RDY`、`+CPIN:`、`+CREG:`、`+CEREG:`、`+CGREG:`、`+CGEV:`、`+PDP DEACT`、`+PDP:DEACT` URC handler。
- 发送 `ATE0`、`AT+CMEE=1`、`AT+CGEREP=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`。
- 状态变为 `MODEM_STATE_READY`，投递 `MODEM_EVENT_READY`。

`modem_destroy()`：

- 设置 `destroying`，拒绝新的 wrapper 调用。
- 停止 event task 并等待 `event_task_done_sema`。
- 调用子类 destroy，注销已注册 URC handler；若子类 destroy 返回错误，则保留基类资源和对象内存并向上返回该错误，避免 AT Engine 中残留的 URC handler 指向已释放对象。
- 子类 destroy 成功后释放基类 FreeRTOS 资源和对象内存。
- 不销毁 AT Engine；AT Engine 生命周期由 Board Init 管理。

`modem_reset()`：

- 优先发送 `AT+RESET`。
- 复位后将状态回退到需要重新初始化的状态，由 Core 或 Board Init 决定是否再次 `modem_init()`。

## 事件流

AT Engine RX task 调用 Air780EP URC handler 时，handler 只做短小解析和非阻塞事件投递。本项目实际使用普通 `xQueueSend(event_queue, &event, 0)`，因为回调运行在 RX task 而不是硬中断。

事件队列满时记录 warning 并丢弃事件，不阻塞 AT Engine RX task。Modem event task 从队列取出事件，读取当前注册的 Core 回调并调用：

```c
callback(modem, &event, user_ctx);
```

硬约束：Air780EP URC handler 不得直接调用 `modem_event_callback_t`，也不得在同一 AT Engine 实例上调用会获取 AT Engine 锁的 API。

## Air780EP 方法

第一版实现以下 ops：

- `init`：注册 URC，配置 `ATE0`、`AT+CMEE=1`、`AT+CGEREP=1`、`AT+CEREG=2`、`AT+CGREG=2`、`AT+CREG=2`。
- `reset`：发送 `AT+RESET`。
- `get_info`：查询 `AT+CGSN`、`AT+CIMI`、`AT+ICCID`、`AT+CGMM`、`AT+CGMR`，填充 `modem_info_t` 和缓存。
- `get_sim_status`：发送 `AT+CPIN?`，解析 `READY`、`SIM PIN`、`SIM PUK`、`SIM REMOVED` 等状态。
- `get_signal`：发送 `AT+CSQ`，解析 `+CSQ: <rssi>,<ber>`，按 `-113 + 2 * rssi` 换算 dBm；`rssi=99` 标记为无效。
- `get_registration`：优先发送 `AT+CEREG?`，必要时降级到 `AT+CGREG?` 和 `AT+CREG?`，把 `stat` 映射到 `modem_reg_status_t`。
- `set_apn`：发送 `AT+CGDCONT=<cid>,"IP","<apn>"`，更新 PDP 缓存。
- `activate_pdp`：本实现仅支持 `cid=1` 的 Air780EP 全局 TCPIP 路径；检查 SIM、注册、附着状态后发送 `AT+CSTT`、`AT+CIICR`、`AT+CIFSR`，成功后更新 PDP 缓存和 state。有效但不支持的 `cid=2..4` 返回 `ESP_ERR_NOT_SUPPORTED`。
- `deactivate_pdp`：本实现仅支持 `cid=1`；发送全局 `AT+CIPSHUT`，成功后清空 PDP active 和 IP 缓存。有效但不支持的 `cid=2..4` 返回 `ESP_ERR_NOT_SUPPORTED`。
- `get_pdp_context`：优先返回缓存，并用 `AT+CGACT?`、`AT+CGPADDR=<cid>` 补充激活状态和 IP；第一版只校验并缓存 IPv4 地址，IPv6 地址后续扩展。

特殊响应使用 AT Engine options：

- `AT+CIFSR` 使用 `AT_CMD_SUCCESS_MATCH_ANY_LINE`，把纯 IP 行作为成功终止响应并保存到 `response.lines[0]`。
- `AT+CIPSHUT` 使用 exact match `SHUT OK` 作为成功终止响应。

## URC 翻译

Air780EP 注册并翻译以下系统级 URC：

- `RDY` → `MODEM_EVENT_READY`。
- `+CPIN:` → 更新 SIM 状态并投递 `MODEM_EVENT_SIM_CHANGED`。
- `+CREG:`、`+CEREG:`、`+CGREG:` → 解析注册状态并投递 `MODEM_EVENT_REG_CHANGED`。
- `+CGEV: ... PDN ACT <cid>...`、`+CGEV: ... PDN DEACT <cid>...` → 从 `PDN ACT`/`PDN DEACT` 后第一个十进制字段解析 CID；CID 缺失、格式错误或超出 `1..4` 时记录日志并丢弃，不更新缓存。
- `+CGEV: ... PDN ACT <cid>...` → 更新对应 PDP 缓存并投递 `MODEM_EVENT_PDP_ACTIVATED`。
- `+CGEV: ... PDN DEACT <cid>...` → 清空对应 PDP 缓存并投递 `MODEM_EVENT_PDP_DEACTIVATED`。
- `+PDP DEACT`、`+PDP:DEACT` → 全局去激活 URC；清空所有 active PDP 缓存，并为受影响 CID 投递 `MODEM_EVENT_PDP_DEACTIVATED`。

`CLOSED` 和 `+CIPRXGET:` 属于连接层或 socket 数据路径，本轮不注册，避免公开事件模型提前膨胀。

## 数据解析与缓存

Air780EP 实现使用小型 helper 处理常见解析：

- 查找指定响应前缀。
- 跳过前缀、冒号和空格。
- 解析逗号分隔的整数字段。
- 复制字符串到固定长度字段，始终 NUL 结尾。
- `AT+CGMM` 和 `AT+CGMR` 的字符串值复制前剥离一层外层引号。
- 注册查询响应按 `<n>,<stat>` 解析；注册 URC 按前缀后的第一个整数作为 `stat` 解析，并校验后续可选字段。
- 校验 `cid` 在缓存数组范围内。

PDP 缓存只保存第一版需要的 `cid`、`apn`、`pdp_type`、`active` 和 `ip_addr`。缓存是 Modem 层快照，不替代 Core 网络状态机。

## 错误处理

所有公开 API 和 ops 方法返回 `esp_err_t`。

- NULL 参数返回 `ESP_ERR_INVALID_ARG`。
- destroy 中、未初始化或非法状态返回 `ESP_ERR_INVALID_STATE`。
- 未支持能力返回 `ESP_ERR_NOT_SUPPORTED`。
- `at_engine_send_cmd()` / `at_engine_send_cmd_with_options()` 超时传播 `ESP_ERR_TIMEOUT`。
- AT Engine 返回 `ESP_OK` 但 `response.status` 为错误时，Air780EP helper 记录 status/error_code 并返回标准 ESP-IDF 错误码，默认使用 `ESP_FAIL`。
- 响应行缺失或格式无法解析时返回 `ESP_ERR_INVALID_RESPONSE`。

资源创建使用 `ESP_GOTO_ON_*` cleanup 模式，局部变量命名为 `ret`，标签统一为 `err`。

## 线程安全约定

通用 wrapper 在进入 ops 前检查 `destroying`。`state`、event callback、销毁标志、Air780EP 的 `pdp[]`、`cached_info`、`last_sim_status`、`last_reg_status` 和 `last_signal` 由 `modem_t.lock` 保护。AT 命令仍由 AT Engine 的 `cmd_mutex` 串行化，Modem 层不重复实现命令互斥。

普通 API 方法在读写缓存时使用阻塞锁，但不在持锁期间发送 AT 命令或投递事件。URC handler 在 AT Engine RX task 中只使用非阻塞锁；锁忙时记录 warning 并丢弃该次缓存更新和对应事件，避免阻塞 RX task。

调用方必须保证 `modem_destroy()` 不与同一句柄上的新 `modem_*` API 并发启动；这是与 AT Engine destroy 约束一致的上层生命周期要求。

## 验证计划

实现完成后执行：

1. `git diff --check`
2. ESP-IDF MCP build tool `esp-idf-eim_build_project`
3. 搜索确认新增 API 名称、事件名和 CMake 源文件注册一致。
4. 审阅 `git diff`，确认只修改本轮相关源码、头文件和必要构建文件。

本轮验证不声称完成真实模块联网。实机验证需要后续 Board Init、Core 接入、烧录和串口日志检查。

## 已批准的设计选择

- 本轮范围选择“完整实现”。
- 文件边界采用公共 `modem.h`、Board Init 专用 `modem_air780ep.h`、内部 `modem_priv.h`、通用 `modem.c`、Air780EP `modem_air780ep.c`。
- 生命周期由 Modem 创建/销毁自身资源，但不拥有 AT Engine 生命周期。
- URC 必须经 Modem event queue 和 event task 上报，禁止在 AT Engine RX task 中直接调用 Core。
- Air780EP 第一版实现基础系统/联网方法，特殊响应使用 `at_engine_send_cmd_with_options()`。
- 错误处理使用 ESP-IDF 标准 `esp_err_t`，不新增自定义错误码。
- 提交操作不在本轮自动执行，除非用户明确要求。
