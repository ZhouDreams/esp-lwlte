# Air780EP 引脚简化设计

**日期**: 2026-05-25
**状态**: 已被 RDY 初始化流程修订取代
**后续修订**: RDY 等待流程以后续设计 `docs/superpowers/specs/2026-05-25-air780ep-rdy-init-flow-design.md` 为准；该后续设计移除了 `boot_wait_ms`，并要求先注册 URC、硬复位、等待 `RDY` 后再发送 AT 初始化命令。

## 目标

将 modem_air780ep 的硬件控制从多引脚信号模型简化为单 EN 引脚控制模型。

## 变更概要

| 变更项 | 旧 | 新 |
|--------|----|----|
| 配置引脚 | pwrkey_pin + reset_pin + status_pin | en_pin |
| 配置参数 | power_on_pulse_ms | 移除 |
| 上电方式 | PWRKEY 脉冲 + boot_wait | URC 注册后通过 EN 硬复位并等待 RDY |
| 复位方式 | AT+RESET 命令 | 拉低 EN 引脚，等 reset_pulse_ms，拉高，等待 RDY URC |
| init 入口 | maybe_power_on() | 先注册 URC，再 hardware_reset()，等待 RDY 后做 AT 初始化 |

## 文件变更清单

### 1. `src/modem/modem_air780ep.h`

```c
typedef struct {
    gpio_num_t en_pin;                  // 新增，替代 pwrkey_pin + reset_pin + status_pin
    uint32_t reset_pulse_ms;            // 保留
    uint32_t ready_timeout_ms;          // 等待 RDY URC 超时
    uint32_t default_cmd_timeout_ms;    // 保留
    int event_queue_size;               // 保留
    int event_task_stack;               // 保留
    int event_task_priority;            // 保留
} modem_air780ep_config_t;
```

移除字段：pwrkey_pin, reset_pin, status_pin, power_on_pulse_ms
新增字段：en_pin

### 2. `src/modem/modem_air780ep.c`

- 移除 `ms_to_ticks_round_up()`, `pulse_gpio()`, `maybe_power_on()` 三个函数
- 新增 `hardware_reset()`：拉低 en_pin → 等 reset_pulse_ms → 清空 AT RX → 准备 RDY 等待 → 拉高 en_pin
- `air780ep_init()`：先注册 URC，然后调用 `hardware_reset()`，等待 RDY 后发 AT init 命令
- `air780ep_reset()`：改为调用 `hardware_reset()`，不发送 AT+RESET，等待 RDY 后重新发 AT init 命令，回到 READY 状态

### 3. `src/include/lwlte_air780ep.h`

移除字段：pwrkey_pin, reset_pin, status_pin, module_power_stable_ms, modem_power_on_pulse_ms
保留字段：en_pin（透传至 modem config 的 en_pin）

### 4. `src/lwlte/lwlte_air780ep.c`

- 移除 `enable_module_if_needed()` 函数
- 移除 `validate_config()` 中对 pwrkey_pin/reset_pin/status_pin 的校验
- `lwlte_air780ep_init()` 中移除对 `enable_module_if_needed()` 的调用
- modem_config 映射改为透传 en_pin

### 5. `examples/basic_connect/main/main.c`

- 移除 `.pwrkey_pin`, `.reset_pin`, `.status_pin`, `.module_power_stable_ms` 等字段
- 移除 `EXAMPLE_MODULE_POWER_STABLE_MS` define

### 6. `docs/agents/classes.md`

同步更新 `modem_air780ep_config_t` 和 `lwlte_air780ep_config_t` 结构体定义。

### 7. `examples/basic_connect/README.md`

更新硬件接线说明，移除对 PWRKEY/RESET/STATUS 引脚的描述。

## 验证

- 构建 example：`idf.py -C examples/basic_connect build`
