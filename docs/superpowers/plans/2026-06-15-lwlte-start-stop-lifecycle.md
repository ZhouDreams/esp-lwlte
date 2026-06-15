# lwlte start/stop 生命周期对称化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 lwlte 门面补上与 `lwlte_start()` 对称的 `lwlte_stop()`（断电 + 静默），移除 `lwlte_disconnect()`，并保证 `init → start → stop → start` 往复可靠。

**Architecture:** 架构 A——线程随 `init` 建、`destroy` 销，常驻不动；`start`/`stop` 只切换「上电联网 / 断电静默」与各 FSM 逻辑状态。`stop` 在 Core FSM 任务上去激活网络并对 modem EN 断电；新增 `MODEM_STATE_OFF`，`modem_start` 可从 OFF 重新上电。

**Tech Stack:** ESP-IDF (C, FreeRTOS), esp_event；host 静态契约测试用 Python `unittest`。

**关联 spec:** `docs/superpowers/specs/2026-06-15-lwlte-start-stop-lifecycle-design.md`

---

## ⚠️ 提交策略（用户硬性要求，覆盖技能默认）

**整个计划执行期间禁止任何 `git commit`。** 用户要求全部改完后由其本人 review 再自行提交。因此：
- 本计划所有任务的收尾步骤是 **Checkpoint（构建 + 运行 host 契约测试）**，**不是** commit。
- 允许 `git add`/`git status`/`git diff` 用于查看，但**不得** `git commit`。
- 最终 Task 10 只做整体验证与交接，不提交。

## 命令约定（全程复用）

- **构建（优先 MCP）**：优先用 MCP `build_project` 工具；命令行兜底：
  ```bash
  source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
  ```
- **跑单个 host 契约测试文件**：
  ```bash
  python3 tests/host/test_lwlte_start_stop_lifecycle.py -v
  ```
- **跑单个测试方法**：
  ```bash
  python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.<method> -v
  ```
- **跑全部 host 测试**：
  ```bash
  python3 -m pytest tests/host -q
  ```

## 文件结构（改动地图）

| 文件 | 职责 | 改动 |
|---|---|---|
| `src/modem/modem.h` | modem 基类公共接口 | 加 `MODEM_STATE_OFF`、`modem_stop()` 原型 |
| `src/modem/modem_priv.h` | modem 基类内部 | `modem_ops_t` 加 `stop` |
| `src/modem/modem.c` | modem 基类实现 | `modem_stop()`、`check_ready` 放行 OFF、`modem_destroy` allowed 集合 |
| `src/modem/modem_air780ep.c` | Air780EP 子类 | `hardware_power_off()` + `air780ep_stop()` + ops 接线 |
| `src/modem/modem_ml307r.c` | ML307R 子类 | `hardware_power_off()` + `ml307r_stop()` + ops 接线 |
| `src/core/core_priv.h` | Core 内部 | 句柄加 `stop_pending`；删 `CORE_SIG_NET_DEACTIVATE` |
| `src/core/core.c` | Core 门面 | `core_stop` 置 `stop_pending`；加 `core_stop_pending()`；删 `core_disconnect` |
| `src/core/core.h` | Core 层间接口 | 删 `core_disconnect` 原型 |
| `src/core/core_fsm.c` | Core FSM | `handle_stop` 调 `modem_stop`+清 pending；`handle_service_cmd` 门控；删 DEACTIVATE 分支 |
| `src/core/net_mgr.c` | 网络管理 | `check_activation_continue` 响应 `stop_pending` |
| `src/lwlte/lwlte.c` | 门面实现 | 删 `lwlte_disconnect`、加 `lwlte_stop` |
| `src/include/lwlte.h` | 门面公共接口 | 删 `lwlte_disconnect`、加 `lwlte_stop` + 文档 |
| `tests/host/test_lwlte_start_stop_lifecycle.py` | 新契约测试 | 新建，累积 Task 1–7 断言 |
| `tests/host/test_mqtt_end_to_end_contract.py` | 既有契约测试 | 改切片锚点 `lwlte_disconnect`→`lwlte_stop` |
| `docs/agents/{classes,architecture,err,oop-design}.md` | 设计文档 | 同步 start/stop 与 OFF 态 |

执行顺序自底向上（下层 API 先就位）：modem 基类 → 子类 → core → 门面 → 测试/文档。

---

### Task 1: Modem 基类——OFF 态 + ops.stop + modem_stop()

**Files:**
- Modify: `src/modem/modem.h`（枚举 `:54-63`；原型区 `:302` 后）
- Modify: `src/modem/modem_priv.h`（`modem_ops_t` `:145-148`）
- Modify: `src/modem/modem.c`（`modem_stop` 置于 `:346` 后；`check_ready` `:735`；`modem_destroy` `:292`）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`（新建）

- [ ] **Step 1: 新建 host 契约测试文件（先红）**

创建 `tests/host/test_lwlte_start_stop_lifecycle.py`：

```python
#!/usr/bin/env python3
"""Static contract checks for the lwlte start/stop power-cycle lifecycle."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV_H = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP_C = ROOT / "src/modem/modem_air780ep.c"
ML307R_C = ROOT / "src/modem/modem_ml307r.c"
CORE_H = ROOT / "src/core/core.h"
CORE_PRIV_H = ROOT / "src/core/core_priv.h"
CORE_C = ROOT / "src/core/core.c"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"
NET_MGR_C = ROOT / "src/core/net_mgr.c"
LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"


def function_body(source: str, signature: str) -> str:
    # 跳过前置声明（`... );`），只在真正的定义（签名后先遇到 `{`）处展开。
    search_from = 0
    while True:
        start = source.find(signature, search_from)
        if start < 0:
            raise AssertionError(f"missing function definition: {signature}")
        after = start + len(signature)
        brace = source.find("{", after)
        semicolon = source.find(";", after)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        search_from = after
    depth = 0
    for idx in range(brace, len(source)):
        if source[idx] == "{":
            depth += 1
        elif source[idx] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:idx]
    raise AssertionError(f"function body not closed for {signature}")


def contains(tc, hay, needle, label):
    if needle not in hay:
        tc.fail(f"missing {needle!r} in {label}")


def absent(tc, hay, needle, label):
    if needle in hay:
        tc.fail(f"unexpected {needle!r} in {label}")


class LwlteStartStopContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv_h = MODEM_PRIV_H.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep_c = AIR780EP_C.read_text(encoding="utf-8")
        cls.ml307r_c = ML307R_C.read_text(encoding="utf-8")
        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_priv_h = CORE_PRIV_H.read_text(encoding="utf-8")
        cls.core_c = CORE_C.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")
        cls.net_mgr_c = NET_MGR_C.read_text(encoding="utf-8")
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")

    # ---- Task 1: modem base ----
    def test_modem_has_off_state(self):
        contains(self, self.modem_h, "MODEM_STATE_OFF", "modem.h")

    def test_modem_ops_has_stop(self):
        contains(self, self.modem_priv_h, "modem_no_arg_fn stop;", "modem_priv.h")

    def test_modem_stop_prototype_and_impl(self):
        contains(self, self.modem_h, "esp_err_t modem_stop(modem_handle_t *me);", "modem.h")
        body = function_body(self.modem_c, "esp_err_t modem_stop(modem_handle_t *me)")
        contains(self, body, "me->ops->stop", "modem_stop")

    def test_check_ready_allows_off(self):
        body = function_body(self.modem_c, "static esp_err_t check_ready(modem_handle_t *me, bool allow_created)")
        contains(self, body, "MODEM_STATE_OFF", "check_ready")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`
Expected: 4 个测试 FAIL（`MODEM_STATE_OFF`/`modem_stop`/`modem_no_arg_fn stop;` 尚不存在）。

- [ ] **Step 3: modem.h 加 OFF 态**

把（`modem.h:61-62`）：
```c
    MODEM_STATE_ERROR,              /**< 错误； Error */
    MODEM_STATE_DESTROYING,         /**< 销毁中； Destroying */
```
改为：
```c
    MODEM_STATE_ERROR,              /**< 错误； Error */
    MODEM_STATE_OFF,                /**< 已断电、可重启； Powered off, restartable */
    MODEM_STATE_DESTROYING,         /**< 销毁中； Destroying */
```

- [ ] **Step 4: modem.h 加 modem_stop 原型**

在 `modem_start` 原型（`modem.h:302`）之后插入：
```c
/**
 * @brief 停止并对模块断电
 * @details Stop modem and power it off (drive EN low and hold)
 * @note 从任意非 DESTROYING 状态可调用；en_pin==GPIO_NUM_NC 时降级为逻辑停机。
 * @param[in] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 正在销毁
 *         - ESP_ERR_NOT_SUPPORTED: 子类未实现 stop
 *         - 其他 esp_err_t: 下层断电错误
 */
esp_err_t modem_stop(modem_handle_t *me);
```

- [ ] **Step 5: modem_priv.h 的 ops 加 stop**

把（`modem_priv.h:147-148`）：
```c
    modem_no_arg_fn start;                           /**< 启动模块； Start modem */
    modem_no_arg_fn reset;                           /**< 复位模块； Reset modem */
```
改为：
```c
    modem_no_arg_fn start;                           /**< 启动模块； Start modem */
    modem_no_arg_fn stop;                            /**< 停止并断电； Stop and power off */
    modem_no_arg_fn reset;                           /**< 复位模块； Reset modem */
```

- [ ] **Step 6: modem.c 实现 modem_stop**

在 `modem_start`（结束于 `modem.c:346`）之后、`modem_reset` 之前插入：
```c
esp_err_t modem_stop(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying || me->state == MODEM_STATE_DESTROYING;
    xSemaphoreGive(me->lock);
    ESP_RETURN_ON_FALSE(!destroying, ESP_ERR_INVALID_STATE, TAG, "modem destroying");

    ESP_RETURN_ON_FALSE(me->ops && me->ops->stop,
                        ESP_ERR_NOT_SUPPORTED, TAG, "stop not supported");

    return call_no_arg(me, me->ops->stop);
}
```

- [ ] **Step 7: modem.c 的 check_ready 放行 OFF**

把（`modem.c:735-737`）：
```c
    if (state == MODEM_STATE_CREATED && allow_created) {
        return ESP_OK;
    }
```
改为：
```c
    if ((state == MODEM_STATE_CREATED || state == MODEM_STATE_OFF) && allow_created) {
        return ESP_OK;
    }
```

- [ ] **Step 8: modem.c 的 modem_destroy 允许从 OFF 销毁**

把（`modem.c:292-293`）：
```c
    bool allowed = (state == MODEM_STATE_CREATED ||
                    state == MODEM_STATE_READY ||
```
改为：
```c
    bool allowed = (state == MODEM_STATE_CREATED ||
                    state == MODEM_STATE_OFF ||
                    state == MODEM_STATE_READY ||
```

> 说明：`MODEM_STATE_OFF` 插在 `ERROR` 与 `DESTROYING` 之间（值 7，`DESTROYING` 变 8）。`modem_set_state` 的范围判断 `<= MODEM_STATE_DESTROYING` 仍覆盖 OFF，无需改。`modem_destroy:318` 的错误恢复行 `state <= MODEM_STATE_ERROR` 会把 OFF 兜底为 ERROR——仅在 destroy 失败回滚时触发，可接受。

- [ ] **Step 9: 运行测试确认通过**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`
Expected: 4 个测试全 PASS。

- [ ] **Step 10: Checkpoint（NO commit）**

构建确认编译通过（MCP `build_project` 或 `idf.py build`）。**不要提交。** 可 `git status` 查看改动。

### Task 2: Air780EP 子类——hardware_power_off + air780ep_stop

**Files:**
- Modify: `src/modem/modem_air780ep.c`（前置声明 `:135` 附近；ops 表 `:1041-1045`；新函数置于 `hardware_reset` `:2253` 与 `air780ep_start` `:2426` 之间）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

- [ ] **Step 1: 加测试方法（先红）**

在 `LwlteStartStopContractTest` 类内追加：
```python
    # ---- Task 2: air780ep ----
    def test_air780ep_power_off_helper(self):
        body = function_body(self.air780ep_c, "static esp_err_t hardware_power_off(modem_air780ep_t *self)")
        contains(self, body, "gpio_set_level(self->config.en_pin, 0)", "air780ep hardware_power_off")
        absent(self, body, "gpio_set_level(self->config.en_pin, 1)", "air780ep hardware_power_off")

    def test_air780ep_stop_impl(self):
        body = function_body(self.air780ep_c, "static esp_err_t air780ep_stop(modem_handle_t *me)")
        for needle in ["hardware_power_off(self)", "unregister_urcs(self)", "MODEM_STATE_OFF"]:
            contains(self, body, needle, "air780ep_stop")

    def test_air780ep_ops_wires_stop(self):
        contains(self, self.air780ep_c, ".stop = air780ep_stop,", "modem_air780ep.c ops")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_air780ep_stop_impl -v`
Expected: FAIL（函数尚不存在）。

- [ ] **Step 3: 加前置声明**

在 `static esp_err_t air780ep_reset(modem_handle_t *me);`（`:146` 附近）下方加：
```c
static esp_err_t air780ep_stop(modem_handle_t *me);
static esp_err_t hardware_power_off(modem_air780ep_t *self);
```

- [ ] **Step 4: 实现 hardware_power_off（专用静态函数，对齐 hardware_reset）**

在 `hardware_reset` 函数（结束于 `:2304`）之后插入：
```c
/**
 * @brief 硬件关机
 * @details 拉低 EN 引脚并保持，使模块断电
 * @note 持 AT 命令路径独占；en_pin==GPIO_NUM_NC 时降级为无操作返回 ESP_OK。
 * @param[in] self Air780EP 实例
 * @return ESP_OK 成功，其它为 GPIO/AT 错误
 */
static esp_err_t hardware_power_off(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    if (self->config.en_pin == GPIO_NUM_NC) {
        at_engine_end_exclusive(self->base.at);
        ESP_LOGW(TAG, "no EN pin; modem stays powered (logical stop only)");
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << (uint32_t)self->config.en_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure EN GPIO failed");

    ret = gpio_set_level(self->config.en_pin, 0);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set EN GPIO low failed");

    (void)at_engine_flush_rx_exclusive(self->base.at);

err:
    at_engine_end_exclusive(self->base.at);
    return ret;
}
```

- [ ] **Step 5: 实现 air780ep_stop（对称版，逆 air780ep_start 入口）**

在 `air780ep_reset`（结束于 `:2566` 附近）之后插入：
```c
static esp_err_t air780ep_stop(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);

    /* 1. 复位内部运行状态（逆 start 步 1/8）*/
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    set_initialized(self, false);

    /* 2. 注销 URC（逆 start 步 7）*/
    if (self->urc_registered) {
        esp_err_t urc_ret = unregister_urcs(self);
        if (urc_ret != ESP_OK) {
            ESP_LOGW(TAG, "unregister URCs during stop failed: %s", esp_err_to_name(urc_ret));
        }
    }

    /* 3. 硬件断电（逆 start 步 4 的上电）*/
    esp_err_t ret = hardware_power_off(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hardware power off failed: %s", esp_err_to_name(ret));
    }

    /* 4. 即使断电失败也落 OFF（断电幂等，下次 start 的 hardware_reset 会再拉低→拉高）*/
    (void)modem_set_state(me, MODEM_STATE_OFF);

    return ret;
}
```

- [ ] **Step 6: ops 表接线**

把（`:1041-1045`）：
```c
static const modem_ops_t s_air780ep_ops = {
    .destroy = air780ep_destroy,
    .start = air780ep_start,
    .reset = air780ep_reset,
```
改为：
```c
static const modem_ops_t s_air780ep_ops = {
    .destroy = air780ep_destroy,
    .start = air780ep_start,
    .stop = air780ep_stop,
    .reset = air780ep_reset,
```

- [ ] **Step 7: 运行测试确认通过**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`
Expected: Task 1 + Task 2 方法全 PASS。

- [ ] **Step 8: Checkpoint（NO commit）**

构建确认编译通过。**不要提交。**

---

### Task 3: ML307R 子类——hardware_power_off + ml307r_stop

**Files:**
- Modify: `src/modem/modem_ml307r.c`（前置声明 `:123-134` 附近；ops 表 `:1131-1135`；新函数置于 `hardware_reset` `:2207` 与 `ml307r_start` `:2392` 之间）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

> ML307R 与 Air780EP 结构同构（`to_ml307r`/`set_initialized`/`hardware_reset`/`unregister_urcs`/`s_ml307r_ops`）。注意子类标志差异：ML307R 入口复位的是 `self->pdp[0].active`、`self->pdp[0].ip_addr[0]`、`self->mqtt_data_enabled`、`self->mqtt_session_connected`（见 `ml307r_start` `:2404-2407`），**没有** `mqtt_tcp_connected`。

- [ ] **Step 1: 加测试方法（先红）**

在类内追加：
```python
    # ---- Task 3: ml307r ----
    def test_ml307r_power_off_helper(self):
        body = function_body(self.ml307r_c, "static esp_err_t hardware_power_off(modem_ml307r_t *self)")
        contains(self, body, "gpio_set_level(self->config.en_pin, 0)", "ml307r hardware_power_off")
        absent(self, body, "gpio_set_level(self->config.en_pin, 1)", "ml307r hardware_power_off")

    def test_ml307r_stop_impl(self):
        body = function_body(self.ml307r_c, "static esp_err_t ml307r_stop(modem_handle_t *me)")
        for needle in ["hardware_power_off(self)", "unregister_urcs(self)", "MODEM_STATE_OFF"]:
            contains(self, body, needle, "ml307r_stop")

    def test_ml307r_ops_wires_stop(self):
        contains(self, self.ml307r_c, ".stop = ml307r_stop,", "modem_ml307r.c ops")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_ml307r_stop_impl -v`
Expected: FAIL。

- [ ] **Step 3: 加前置声明**

在 `static esp_err_t ml307r_reset(modem_handle_t *me);`（`:134` 附近）下方加：
```c
static esp_err_t ml307r_stop(modem_handle_t *me);
static esp_err_t hardware_power_off(modem_ml307r_t *self);
```

- [ ] **Step 4: 实现 hardware_power_off**

在 `hardware_reset`（结束于 `:2260` 附近）之后插入：
```c
/**
 * @brief 硬件关机
 * @details 拉低 EN 引脚并保持，使模块断电
 * @param[in] self ML307R 实例
 * @return ESP_OK 成功，其它为 GPIO/AT 错误
 */
static esp_err_t hardware_power_off(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");

    esp_err_t ret = at_engine_begin_exclusive(self->base.at);
    ESP_RETURN_ON_ERROR(ret, TAG, "begin AT exclusive failed");

    if (self->config.en_pin == GPIO_NUM_NC) {
        at_engine_end_exclusive(self->base.at);
        ESP_LOGW(TAG, "no EN pin; modem stays powered (logical stop only)");
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << (uint32_t)self->config.en_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure EN GPIO failed");

    ret = gpio_set_level(self->config.en_pin, 0);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set EN GPIO low failed");

    (void)at_engine_flush_rx_exclusive(self->base.at);

err:
    at_engine_end_exclusive(self->base.at);
    return ret;
}
```

- [ ] **Step 5: 实现 ml307r_stop**

在 `ml307r_reset`（结束于 `:2505` 附近）之后插入：
```c
static esp_err_t ml307r_stop(modem_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_ml307r_t *self = to_ml307r(me);

    /* 1. 复位内部运行状态（逆 start 入口）*/
    if (self->base.lock) {
        xSemaphoreTake(self->base.lock, portMAX_DELAY);
    }
    self->pdp[0].active = false;
    self->pdp[0].ip_addr[0] = '\0';
    self->mqtt_data_enabled = false;
    self->mqtt_session_connected = false;
    if (self->base.lock) {
        xSemaphoreGive(self->base.lock);
    }
    set_initialized(self, false);

    /* 2. 注销 URC */
    if (self->urc_registered) {
        esp_err_t urc_ret = unregister_urcs(self);
        if (urc_ret != ESP_OK) {
            ESP_LOGW(TAG, "unregister URCs during stop failed: %s", esp_err_to_name(urc_ret));
        }
    }

    /* 3. 硬件断电 */
    esp_err_t ret = hardware_power_off(self);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hardware power off failed: %s", esp_err_to_name(ret));
    }

    /* 4. 落 OFF */
    (void)modem_set_state(me, MODEM_STATE_OFF);

    return ret;
}
```

- [ ] **Step 6: ops 表接线**

把（`:1131-1135`）：
```c
static const modem_ops_t s_ml307r_ops = {
    .destroy = ml307r_destroy,
    .start = ml307r_start,
    .reset = ml307r_reset,
```
改为：
```c
static const modem_ops_t s_ml307r_ops = {
    .destroy = ml307r_destroy,
    .start = ml307r_start,
    .stop = ml307r_stop,
    .reset = ml307r_reset,
```
> 注：`.destroy` 行实际文本以文件为准（`ml307r_destroy` 名称见 `:1131-1135`），仅在 `.start` 之后插入 `.stop` 行。

- [ ] **Step 7: 运行测试 + Checkpoint（NO commit）**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`（Task 1–3 全 PASS）。构建确认编译通过。**不要提交。**

### Task 4: Core——stop_pending 协作式取消

**Files:**
- Modify: `src/core/core_priv.h`（`struct core_handle` `:103-117`；原型区）
- Modify: `src/core/core.c`（`core_stop` `:260-270`；新增 `core_stop_pending`）
- Modify: `src/core/net_mgr.c`（`check_activation_continue` `:566-577`）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

- [ ] **Step 1: 加测试方法（先红）**

在类内追加：
```python
    # ---- Task 4: core stop_pending ----
    def test_core_handle_has_stop_pending(self):
        contains(self, self.core_priv_h, "stop_pending", "core_priv.h")

    def test_core_stop_sets_pending(self):
        body = function_body(self.core_c, "esp_err_t core_stop(core_handle_t *me)")
        contains(self, body, "stop_pending = true", "core_stop")

    def test_net_mgr_cooperative_cancel(self):
        body = function_body(self.net_mgr_c, "static esp_err_t check_activation_continue(core_handle_t *me,")
        contains(self, body, "core_stop_pending(me)", "check_activation_continue")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_core_stop_sets_pending -v`
Expected: FAIL。

- [ ] **Step 3: core_priv.h 加字段 + 原型**

把（`core_priv.h:110-111`）：
```c
    bool destroying;
    bool destroy_in_progress;
```
改为：
```c
    bool destroying;
    bool destroy_in_progress;
    bool stop_pending;
```

在原型区（如 `bool core_is_destroying(core_handle_t *me);` `:156` 附近）加：
```c
bool core_stop_pending(core_handle_t *me);
```

- [ ] **Step 4: core.c 实现 core_stop_pending + core_stop 置位**

在 `core_is_destroying`（`:406-417`）之后加：
```c
bool core_stop_pending(core_handle_t *me)
{
    if (!me || !me->lock) {
        return false;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool pending = me->stop_pending;
    xSemaphoreGive(me->lock);

    return pending;
}
```

把 `core_stop`（`:260-270`）改为：
```c
esp_err_t core_stop(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_STOP),
                        ESP_ERR_INVALID_STATE, TAG, "stop not allowed");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->stop_pending = true;
    xSemaphoreGive(me->lock);

    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_STOP), TAG,
                        "send stop signal failed");

    return ESP_OK;
}
```

- [ ] **Step 5: net_mgr.c 联网循环响应 stop_pending**

把 `check_activation_continue`（`net_mgr.c:566-577`）改为：
```c
static esp_err_t check_activation_continue(core_handle_t *me,
                                            uint32_t activation_start_ms)
{
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (core_stop_pending(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (activation_timed_out(me, activation_start_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
```

并把 `net_mgr_start_activation` 尾部（`net_mgr.c:405-413`）的：
```c
    if (ret == ESP_ERR_INVALID_STATE && core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }

    return fail_activation(me, ret);
```
改为：
```c
    if (ret == ESP_ERR_INVALID_STATE &&
        (core_is_destroying(me) || core_stop_pending(me))) {
        return ESP_ERR_INVALID_STATE;
    }

    return fail_activation(me, ret);
```
> 这样 stop 期间被取消的激活流程安静退出，不会误报 `CORE_NET_STATE_ERROR`/`NET_ERROR`，FSM 任务随即处理排队的 `CORE_SIG_STOP`。

- [ ] **Step 6: 运行测试通过 + Checkpoint（NO commit）**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`（Task 1–4 PASS）。构建确认编译通过。**不要提交。**

---

### Task 5: Core FSM——handle_stop 断电 + service_cmd 门控

**Files:**
- Modify: `src/core/core_fsm.c`（`handle_start` `:384`；`handle_stop` `:427-441`；`handle_service_cmd` `:544`）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

- [ ] **Step 1: 加测试方法（先红）**

在类内追加：
```python
    # ---- Task 5: handle_stop power-off + service_cmd guard ----
    def test_handle_stop_powers_off_modem(self):
        body = function_body(self.core_fsm_c, "static void handle_stop(core_handle_t *me)")
        contains(self, body, "modem_stop(me->modem)", "handle_stop")
        contains(self, body, "stop_pending = false", "handle_stop")

    def test_handle_start_clears_stop_pending(self):
        body = function_body(self.core_fsm_c, "static void handle_start(core_handle_t *me)")
        contains(self, body, "stop_pending = false", "handle_start")

    def test_service_cmd_guarded_when_stopped(self):
        body = function_body(self.core_fsm_c, "static void handle_service_cmd(core_handle_t *me, core_cmd_t *cmd)")
        contains(self, body, "CORE_STATE_STOPPED", "handle_service_cmd")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_handle_stop_powers_off_modem -v`
Expected: FAIL。

- [ ] **Step 3: handle_start 入口清 stop_pending**

把 `handle_start`（`:384-394`）开头的状态检查块：
```c
    /* 只接受从 STOPPED 状态启动；STARTING / READY / ONLINE 等状态直接忽略 */
    core_state_t state = core_get_state_value(me);

    if (state != CORE_STATE_STOPPED) {
        return;
    }
```
改为：
```c
    /* 只接受从 STOPPED 状态启动；STARTING / READY / ONLINE 等状态直接忽略 */
    core_state_t state = core_get_state_value(me);

    if (state != CORE_STATE_STOPPED) {
        return;
    }

    /* 新一轮启动，清除上一轮可能残留的 stop_pending */
    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->stop_pending = false;
    xSemaphoreGive(me->lock);
```

- [ ] **Step 4: handle_stop 断电 + 清 pending**

当前函数（`:427-441`）为：
```c
static void handle_stop(core_handle_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state == CORE_STATE_STOPPED ||
        state == CORE_STATE_DESTROYING) {
        return;
    }

    net_mgr_set_reconnect_enabled(me, false);
    net_mgr_cancel_reconnect(me);
    net_mgr_deactivate(me);
    core_set_state(me, CORE_STATE_STOPPED);
    post_event_checked(me, LWLTE_EVENT_STOPPED, NULL);
}
```
整体替换为：
```c
static void handle_stop(core_handle_t *me)
{
    core_state_t state = core_get_state_value(me);

    if (state == CORE_STATE_STOPPED ||
        state == CORE_STATE_DESTROYING) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->stop_pending = false;
        xSemaphoreGive(me->lock);
        return;
    }

    net_mgr_set_reconnect_enabled(me, false);
    net_mgr_cancel_reconnect(me);
    net_mgr_deactivate(me);

    esp_err_t off_ret = modem_stop(me->modem);
    if (off_ret != ESP_OK) {
        ESP_LOGW(TAG, "power off modem during stop failed: %s",
                 esp_err_to_name(off_ret));
    }

    core_set_state(me, CORE_STATE_STOPPED);
    post_event_checked(me, LWLTE_EVENT_STOPPED, NULL);

    xSemaphoreTake(me->lock, portMAX_DELAY);
    me->stop_pending = false;
    xSemaphoreGive(me->lock);
}
```
> `net_mgr_deactivate` 在模块仍上电时下发 PDP 去激活 AT；随后 `modem_stop` 断电。顺序不可颠倒。

- [ ] **Step 5: handle_service_cmd 停机门控**

把 `handle_service_cmd`（`:544-550`）开头：
```c
static void handle_service_cmd(core_handle_t *me, core_cmd_t *cmd)
{
    if (!me || !cmd) {
        core_free_cmd(cmd);
        return;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
```
改为：
```c
static void handle_service_cmd(core_handle_t *me, core_cmd_t *cmd)
{
    if (!me || !cmd) {
        core_free_cmd(cmd);
        return;
    }

    core_state_t cmd_state = core_get_state_value(me);
    if (cmd_state == CORE_STATE_STOPPED ||
        cmd_state == CORE_STATE_ERROR ||
        cmd_state == CORE_STATE_DESTROYING) {
        finish_service_cmd(me, cmd, CORE_CMD_RESULT_ERROR, NULL);
        return;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
```
> 停机/错误/销毁态下不把 AT 打到已断电模块；命令以 `CORE_CMD_RESULT_ERROR` 经 `done_cb` 回告调用方。`Ping` 另有 online 校验（`:625`），双重保险。

- [ ] **Step 6: 运行测试通过 + Checkpoint（NO commit）**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`（Task 1–5 PASS）。构建确认编译通过。**不要提交。**

### Task 6: 移除 disconnect 死代码（core 层）

**Files:**
- Modify: `src/core/core.h`（删 `core_disconnect` 原型 `:346-357`，**注意不要误删上方 `core_connect` 块 `:333-344`**）
- Modify: `src/core/core.c`（删 `core_disconnect` `:335-345`；`api_state_allows` `:542-545`；`send_simple_signal` `:506-510`）
- Modify: `src/core/core_priv.h`（删枚举 `CORE_SIG_NET_DEACTIVATE` `:50`）
- Modify: `src/core/core_fsm.c`（删 `CORE_SIG_NET_DEACTIVATE` 分支 `:349-358`）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

> 保留 `net_mgr_deactivate`（`handle_stop` 仍用）。`core_connect`/`CORE_SIG_NET_ACTIVATE` 本次不动（spec 标可选）。

- [ ] **Step 1: 加测试方法（先红）**

在类内追加：
```python
    # ---- Task 6: disconnect removed ----
    def test_core_disconnect_removed(self):
        absent(self, self.core_h, "core_disconnect", "core.h")
        absent(self, self.core_c, "esp_err_t core_disconnect", "core.c")
        absent(self, self.core_priv_h, "CORE_SIG_NET_DEACTIVATE", "core_priv.h")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_core_disconnect_removed -v`
Expected: FAIL。

- [ ] **Step 3: 删 core.h 原型**

删除 `core.h` 中 `core_disconnect` 的文档注释 + 原型（`:346-357`，即 `@brief 断开 LTE 网络` 注释块到 `esp_err_t core_disconnect(core_handle_t *me);`）。**保留上方 `core_connect` 块（`:333-344`，`@brief 连接 LTE 网络`）不动**——它本次不删（spec 标为可选清理）。

- [ ] **Step 4: 删 core.c 实现**

删除 `core.c` 的：
```c
esp_err_t core_disconnect(core_handle_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_NET_DEACTIVATE),
                        ESP_ERR_INVALID_STATE, TAG, "disconnect not allowed");
    ESP_RETURN_ON_ERROR(send_simple_signal(me, CORE_SIG_NET_DEACTIVATE), TAG,
                        "send disconnect signal failed");

    return ESP_OK;
}
```

`send_simple_signal`（`:506-510`）的合法信号集合去掉 DEACTIVATE：把
```c
    ESP_RETURN_ON_FALSE(sig_type == CORE_SIG_START ||
                        sig_type == CORE_SIG_STOP ||
                        sig_type == CORE_SIG_NET_ACTIVATE ||
                        sig_type == CORE_SIG_NET_DEACTIVATE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid simple signal");
```
改为：
```c
    ESP_RETURN_ON_FALSE(sig_type == CORE_SIG_START ||
                        sig_type == CORE_SIG_STOP ||
                        sig_type == CORE_SIG_NET_ACTIVATE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid simple signal");
```

`api_state_allows`（`:542-545`）删除 DEACTIVATE 分支：
```c
    case CORE_SIG_NET_DEACTIVATE:
        return state == CORE_STATE_NET_ACTIVATING ||
               state == CORE_STATE_ONLINE ||
               state == CORE_STATE_ERROR;
```

- [ ] **Step 5: 删 core_priv.h 枚举值**

把（`core_priv.h:47-50`）：
```c
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
    CORE_SIG_NET_DEACTIVATE,
```
改为：
```c
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
```

- [ ] **Step 6: 删 core_fsm.c 分支**

删除 `handle_signal` 中整个 `case CORE_SIG_NET_DEACTIVATE:` 块（`:349-358`）。

- [ ] **Step 7: 运行测试通过 + Checkpoint（NO commit）**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py -v`（Task 1–6 PASS）。构建确认编译通过（确认无对 `core_disconnect`/`CORE_SIG_NET_DEACTIVATE` 的悬挂引用）。**不要提交。**

---

### Task 7: Facade——移除 lwlte_disconnect + 新增 lwlte_stop

**Files:**
- Modify: `src/include/lwlte.h`（删 `lwlte_disconnect` `:363-375`；加 `lwlte_stop`）
- Modify: `src/lwlte/lwlte.c`（替换 `lwlte_disconnect` `:271-281` 为 `lwlte_stop`）
- Modify: `tests/host/test_mqtt_end_to_end_contract.py`（切片锚点 `:864`）
- Test: `tests/host/test_lwlte_start_stop_lifecycle.py`

- [ ] **Step 1: 加测试方法（先红）**

在类内追加：
```python
    # ---- Task 7: facade stop ----
    def test_facade_has_stop_not_disconnect(self):
        contains(self, self.lwlte_h, "esp_err_t lwlte_stop(lwlte_handle_t *me);", "lwlte.h")
        absent(self, self.lwlte_h, "lwlte_disconnect", "lwlte.h")
        absent(self, self.lwlte_c, "lwlte_disconnect", "lwlte.c")

    def test_lwlte_stop_impl(self):
        body = function_body(self.lwlte_c, "esp_err_t lwlte_stop(lwlte_handle_t *me)")
        contains(self, body, "core_stop(core)", "lwlte_stop")
        contains(self, body, "mqtt_client_stop", "lwlte_stop")
```

- [ ] **Step 2: 运行确认失败**

Run: `python3 tests/host/test_lwlte_start_stop_lifecycle.py LwlteStartStopContractTest.test_lwlte_stop_impl -v`
Expected: FAIL。

- [ ] **Step 3: lwlte.h 替换原型**

删除 `lwlte_disconnect` 注释块 + 原型（`:363-375`），替换为：
```c
/**
 * @brief 停止 LTE 并对模块断电（硬件关机）
 * @details Stop LTE and power off the module
 * @note 该函数异步提交停机请求：去激活网络、停止 MQTT、对模块 EN 断电，Core 回到 STOPPED。
 * @note ESP_OK 仅表示请求已提交；完成通过 LWLTE_EVENT_STOPPED 上报，或用 lwlte_get_state() 查询。
 * @note 停机后可再次 lwlte_start() 重新上电联网；重启前应等待状态变为 LWLTE_STATE_STOPPED。
 * @note en_pin 为 GPIO_NUM_NC 时无法物理断电，降级为逻辑停机（模块仍上电）。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许停止或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层停止错误
 */
esp_err_t lwlte_stop(lwlte_handle_t *me);
```

- [ ] **Step 4: lwlte.c 替换实现**

把 `lwlte_disconnect`（`lwlte.c:271-281`）整段替换为：
```c
esp_err_t lwlte_stop(lwlte_handle_t *me)
{
    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    mqtt_client_handle_t *mqtt = me->mqtt;
    xSemaphoreGive(me->lock);
    if (mqtt) {
        esp_err_t mqtt_ret = mqtt_client_stop(mqtt);
        if (mqtt_ret != ESP_OK) {
            ESP_LOGW(TAG, "stop MQTT during lwlte_stop failed: %s",
                     esp_err_to_name(mqtt_ret));
        }
    }

    ret = core_stop(core);
    end_api_call(me);

    return ret;
}
```

- [ ] **Step 5: 更新既有 mqtt 契约测试切片锚点**

`tests/host/test_mqtt_end_to_end_contract.py` 用 `lwlte_disconnect` 作为 `lwlte_start` body 的切片终点（`:862-865`）。把：
```python
        start_body = self.lwlte_c[
            self.lwlte_c.index("esp_err_t lwlte_start"):
            self.lwlte_c.index("esp_err_t lwlte_disconnect")
        ]
```
改为：
```python
        start_body = self.lwlte_c[
            self.lwlte_c.index("esp_err_t lwlte_start"):
            self.lwlte_c.index("esp_err_t lwlte_stop")
        ]
```

- [ ] **Step 6: 运行测试通过 + Checkpoint（NO commit）**

Run:
```bash
python3 tests/host/test_lwlte_start_stop_lifecycle.py -v
python3 tests/host/test_mqtt_end_to_end_contract.py -v
```
Expected: 两者全 PASS。构建确认编译通过。**不要提交。**

### Task 8: 文档同步

**Files:**
- Modify: `docs/agents/err.md`（API 列表 `:12-13`）
- Modify: `docs/agents/classes.md`（生命周期章节，`lwlte_disconnect`/`core_disconnect` 处）
- Modify: `docs/agents/architecture.md`（用户操作 API 列表）
- Modify: `docs/agents/oop-design.md`（如含 connect/disconnect 叙述）

> 这些是叙述性文档，无 host 断言强制；但 `test_lwlte_start_lifecycle.py::test_docs_describe_new_lifecycle` 要求 docs 仍含 `lwlte_start`/`modem_start`/`PDP`/`LWLTE_EVENT_NET_ONLINE`——本任务不得移除这些 token。

- [ ] **Step 1: err.md**

把（`docs/agents/err.md:12-13`）：
```c
esp_err_t lwlte_start(lwlte_handle_t *me);
esp_err_t lwlte_disconnect(lwlte_handle_t *me);
```
改为：
```c
esp_err_t lwlte_start(lwlte_handle_t *me);
esp_err_t lwlte_stop(lwlte_handle_t *me);
```

- [ ] **Step 2: classes.md**

本文件含**两类**需改之处，逐一处理：

1. **代码块里的死符号（必删，否则文档留下已删符号）**：
   - `:972` 附近的 `esp_err_t core_disconnect(core_handle_t *me);` 原型行——删除。
   - `:1187` 附近的 `CORE_SIG_NET_DEACTIVATE,` 枚举行——删除。
   （这两处是 Task 6 删源符号后文档仅剩的残留，全仓 grep `core_disconnect`/`CORE_SIG_NET_DEACTIVATE` 在 classes.md 命中即此两处。）
2. **生命周期叙述**：把 `lwlte_disconnect`/`core_disconnect` 的描述替换为 `lwlte_stop`/`core_stop` 对（`init↔destroy`、`start↔stop`），补一句 `lwlte_stop` 会对 modem EN 断电、modem 进入 `MODEM_STATE_OFF`、可再 `lwlte_start` 重新上电。

> 校验：改完后 `rg "core_disconnect|CORE_SIG_NET_DEACTIVATE|lwlte_disconnect" docs/agents/classes.md` 应无命中（`core_connect`/`CORE_SIG_NET_ACTIVATE` 保留）。

- [ ] **Step 3: architecture.md**

用户操作 API 列表（`:76` 等处）若列了 disconnect 则换成 `lwlte_stop`；如仅以 `lwlte_start()` 为例则补一句 `lwlte_stop()` 为对称停机入口。

- [ ] **Step 4: oop-design.md**

搜索 `disconnect`；如有 `lwlte_disconnect`/`core_disconnect` 叙述则同步为 stop。`core_connect` 内部 helper 叙述保留。

- [ ] **Step 5: Checkpoint（NO commit）**

Run:
```bash
python3 tests/host/test_lwlte_start_lifecycle.py -v   # docs token 测试仍 PASS
rg "lwlte_disconnect|core_disconnect|CORE_SIG_NET_DEACTIVATE" docs/agents   # 应无命中
```
Expected: 测试全绿；grep 无命中（`docs/superpowers/` 下的历史 specs/plans 是不可变存档，不算）。**不要提交。**

---

### Task 9: 整体验证 + 实机往复测试 + 交接（NO commit）

**Files:** 无（仅验证）

- [ ] **Step 1: 全量 host 契约测试**

Run: `python3 -m pytest tests/host -q`
Expected: 全绿（含新 `test_lwlte_start_stop_lifecycle.py` 与既有套件）。

- [ ] **Step 2: 干净构建**

Run: `source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build`（或 MCP `build_project`）。
Expected: 编译链接通过，无对已删符号（`lwlte_disconnect`/`core_disconnect`/`CORE_SIG_NET_DEACTIVATE`）的引用。

- [ ] **Step 3: 实机往复验证（HW，需用户在场/授权烧录）**

按 `docs/agents/build-and-debug.md` 烧录后用 `python3 docs/agents/serial_monitor.py` 观察。验证矩阵（对应 spec §8）：
1. `init → start → ONLINE → stop → start → ONLINE`，重复 ≥ 20 次稳定联网。
2. 示波器/逻辑分析仪量 EN：`stop` 后 EN 维持低、模块断电；`start` 后 EN 低→高、模块重启。
3. `STARTING`/`NET_ACTIVATING` 期间 `stop`：迅速落 `STOPPED`，日志无误报 `NET_ERROR`。
4. start 失败 → `ERROR` → `stop` → `start` 恢复。
5. `stop` 时 MQTT `CONNECTED`：MQTT 落 `STOPPED`，网络 `OFFLINE`，modem 断电。
6. `en_pin == NC` 降级：逻辑停机成立。

> 按 build-and-debug.md「明确区分验证层级」：static / 编译 / 烧录 / 串口 / 实机功能，逐项标注完成度。

- [ ] **Step 4: 交接（NO commit）**

汇总改动清单与验证结果交给用户。**全程不提交**；由用户 review 后自行 `git add`/`git commit`。

---

## Self-Review（计划自查）

**1. Spec 覆盖核对：**
- 移除 `lwlte_disconnect` → Task 7；`core_disconnect`/`CORE_SIG_NET_DEACTIVATE` → Task 6。✅
- 新增 `lwlte_stop`（先 mqtt stop 再 core stop）→ Task 7。✅
- `modem_stop` 对称版 + `hardware_power_off` 专用静态函数（air780ep/ml307r）→ Task 2/3。✅
- `MODEM_STATE_OFF` + `check_ready`/`modem_destroy`/`modem_set_state` 状态判定 → Task 1。✅
- `stop_pending` 协作式取消 → Task 4。✅
- `handle_stop` 断电 + 清 pending、`handle_service_cmd` 门控 → Task 5。✅
- EN=NC 降级 → Task 2/3（`hardware_power_off` 内）。✅
- 往复可靠性验证点 → Task 9 矩阵。✅
- 文档更新清单 → Task 8。✅
- host 契约测试更新 → 各 Task 的新方法 + Task 7 改 mqtt 锚点。✅

**2. 占位符扫描：** 无 TBD/TODO；每个代码步骤给了完整代码。✅

**3. 类型/命名一致性：** `modem_stop`/`ops->stop`/`hardware_power_off`/`MODEM_STATE_OFF`/`core_stop_pending`/`lwlte_stop`/`stop_pending` 全计划统一。✅

**注意（执行期保持警觉，非阻断）：**
- `MODEM_STATE_OFF` 插入后 `DESTROYING` 数值 +1；确认无任何按字面数值比较的代码（现状均用枚举名，安全）。
- Task 6 删符号后务必全量构建，排除悬挂引用。
- 行号为当前快照锚点，编辑时以**就近文本匹配**为准（前序任务可能已使行号漂移）。

---

## Post-Review Adjustments（实际执行中追加）

最终整体验证 review 发现并已修正三点：

- `core_start()` 不再只调用 `send_simple_signal()`；它在同一把 Core lock 下成功 `xQueueSend(CORE_SIG_START)` 后同步标记 `CORE_STATE_STARTING` 并清除 stale `stop_pending`，避免 `lwlte_start()` 返回后立即 `lwlte_stop()` 被误判为仍处于 `STOPPED`。
- `handle_start()` 在 `modem_start()` 前检查 `core_stop_pending(me)`：如果 STOP 已排队则跳过开机，交给 queued STOP 处理；`modem_start()` 返回后再次检查 pending，若 STOP 已排队则立即 `handle_stop()`，不进入网络激活。阻塞式 `modem_start()` 本身仍不是跨线程可中止操作。
- `lwlte_stop()` 仍会 best-effort 提交 MQTT stop 并记录失败，但公共返回值以 `core_stop()` 提交结果为准；一旦 Core stop 已提交，最终停机结果通过 `LWLTE_EVENT_STOPPED`/状态查询观察。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-15-lwlte-start-stop-lifecycle.md`. 两种执行方式：

**1. Subagent-Driven（推荐）** — 每个 Task 派发独立 subagent，任务间我来 review，快速迭代。

**2. Inline Execution** — 本会话内按 executing-plans 批量执行 + 检查点。

> 无论哪种：**全程不 `git commit`**（用户最终统一 review 后自行提交）；C 代码实现遵循 `docs/agents/coding-style.md` 与 `oop-design.md`，每个 Task 走「测试先行 → 实现 → 构建/测试 Checkpoint」。

**你选哪种？**
