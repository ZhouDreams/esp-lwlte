# net_mgr Activation Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace premature `NET_ERROR` during normal LTE registration with a staged activation flow that waits for SIM, registration, packet attach, PDP activation, and a valid IP before publishing `NET_ONLINE`.

**Architecture:** Keep `net_mgr` as the owner of Core network activation state. Add one focused modem-layer operation for packet-domain attach status so Core can query `CGATT` semantically, then refactor `net_mgr_start_activation()` from rapid whole-flow retries into a timeout-bounded staged loop with polling. URCs remain useful for modem cache updates, but correctness comes from active queries and the total activation timeout.

**Tech Stack:** ESP-IDF v6.0, FreeRTOS tasks/ticks, C, Air780EP AT commands, existing Python 3 host static regression checks.

---

## File Structure

- Create: `tests/host/test_net_mgr_activation_flow.py`
  - Static regression checks for the new activation contract because this repository currently has no host unit-test harness for Core/Modem objects.
  - Checks that packet attach is exposed through the modem abstraction, `net_mgr` uses staged polling instead of rapid full retries, registration searching maps to waiting, and PDP URCs do not publish `NET_ONLINE` without IP.
- Modify: `src/modem/modem.h`
  - Add internal layer API `modem_get_packet_attach_status()` after `modem_get_registration()`.
- Modify: `src/modem/modem_priv.h`
  - Add `get_packet_attach_status` to `modem_ops_t`.
- Modify: `src/modem/modem.c`
  - Add the wrapper that validates readiness and dispatches to the ops method.
- Modify: `src/modem/modem_air780ep.c`
  - Add the Air780EP ops method that delegates to existing private `query_cgatt()`.
- Modify: `src/core/core_priv.h`
  - Extend `net_mgr_step_t` with explicit `WAIT_REGISTRATION`, `WAIT_PACKET_ATTACH`, and `QUERY_IP` stages.
- Modify: `src/core/net_mgr.c`
  - Replace three rapid activation attempts with a staged activation loop.
  - Post `NET_CONNECTING` once per activation request.
  - Treat SIM busy, registration searching, and `CGATT=0` as waiting states.
  - Require valid PDP/IP before `NET_ONLINE`.
  - Prevent `MODEM_EVENT_PDP_ACTIVATED` without IP from publishing `NET_ONLINE`.
- Modify: `docs/agents/classes.md`
  - Document the new `get_packet_attach_status` modem operation and Air780EP `AT+CGATT?` mapping.

Current workspace note: `docs/superpowers/specs/2026-05-27-net-mgr-activation-flow-design.md` is an untracked design document from the approved design phase. Preserve it.

---

### Task 1: Add Static Regression Coverage

**Files:**
- Create: `tests/host/test_net_mgr_activation_flow.py`

- [ ] **Step 1: Write the failing host regression test**

Create `tests/host/test_net_mgr_activation_flow.py` with this exact content:

```python
#!/usr/bin/env python3
"""Static regression checks for the Core net_mgr activation flow."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
CORE_PRIV = ROOT / "src/core/core_priv.h"
NET_MGR = ROOT / "src/core/net_mgr.c"
MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP = ROOT / "src/modem/modem_air780ep.c"
CLASSES_DOC = ROOT / "docs/agents/classes.md"


class NetMgrActivationFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core_priv = CORE_PRIV.read_text(encoding="utf-8")
        cls.net_mgr = NET_MGR.read_text(encoding="utf-8")
        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv = MODEM_PRIV.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep = AIR780EP.read_text(encoding="utf-8")
        cls.classes_doc = CLASSES_DOC.read_text(encoding="utf-8")

    def test_packet_attach_is_core_visible_modem_operation(self):
        self.assertIn(
            "esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached);",
            self.modem_h,
        )
        self.assertIn("get_packet_attach_status", self.modem_priv)
        self.assertIn("esp_err_t modem_get_packet_attach_status", self.modem_c)
        self.assertIn("air780ep_get_packet_attach_status", self.air780ep)
        self.assertIn("query_cgatt(self, attached)", self.air780ep)
        self.assertIn("get_packet_attach_status", self.classes_doc)

    def test_net_mgr_has_explicit_wait_stages(self):
        for step_name in [
            "NET_STEP_WAIT_REGISTRATION",
            "NET_STEP_WAIT_PACKET_ATTACH",
            "NET_STEP_QUERY_IP",
        ]:
            self.assertIn(step_name, self.core_priv)
            self.assertIn(step_name, self.net_mgr)

    def test_net_mgr_uses_staged_polling_not_rapid_full_retries(self):
        self.assertIn("NET_MGR_WAIT_POLL_INTERVAL_MS", self.net_mgr)
        self.assertIn("run_activation_loop", self.net_mgr)
        self.assertIn("run_activation_step", self.net_mgr)
        self.assertIn("ESP_ERR_NOT_FINISHED", self.net_mgr)
        self.assertIn("modem_get_packet_attach_status", self.net_mgr)
        self.assertNotIn("while (me->net_mgr.retry_count < me->net_mgr.max_retry)", self.net_mgr)
        self.assertNotIn("activation attempt %d failed", self.net_mgr)

    def test_registration_searching_is_waiting_not_failure(self):
        self.assertIn("registration_denied", self.net_mgr)
        self.assertIn("registration_ready(reg_status)", self.net_mgr)
        self.assertIn("return ESP_ERR_NOT_FINISHED", self.net_mgr)
        self.assertIn("MODEM_REG_DENIED", self.net_mgr)

    def test_online_requires_valid_ip_and_pdp_urc_does_not_bypass_ip(self):
        self.assertIn("pdp.ip_addr[0] == '\\0'", self.net_mgr)
        self.assertIn("pdp->ip_addr[0] == '\\0'", self.net_mgr)
        self.assertIn("old_state != CORE_NET_STATE_ACTIVATING", self.net_mgr)
        self.assertIn("complete_activation", self.net_mgr)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test and verify it fails before implementation**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: FAIL. The first failure should mention missing `modem_get_packet_attach_status` or missing `NET_STEP_WAIT_REGISTRATION` because production code has not been changed yet.

- [ ] **Step 3: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add tests/host/test_net_mgr_activation_flow.py
git commit -m "test(core): cover staged net activation flow"
```

Expected: one commit containing only `tests/host/test_net_mgr_activation_flow.py`.

---

### Task 2: Expose Packet Attach Status Through Modem API

**Files:**
- Modify: `src/modem/modem.h:267-292`
- Modify: `src/modem/modem_priv.h:40-45`
- Modify: `src/modem/modem.c:415-427`

- [ ] **Step 1: Add the public layer prototype to `src/modem/modem.h`**

Immediately after the existing `modem_get_registration()` declaration, insert:

```c
/**
 * @brief 获取分组域附着状态
 * @details Get packet domain attach status
 * @param[in] me 调制解调器句柄
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 查询失败
 */
esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached);
```

- [ ] **Step 2: Add the ops slot to `src/modem/modem_priv.h`**

In `modem_ops_t`, immediately after `get_registration`, add:

```c
    esp_err_t (*get_packet_attach_status)(modem_t *me, bool *attached);
```

The resulting ops section must contain this order:

```c
    esp_err_t (*get_sim_status)(modem_t *me, modem_sim_status_t *status);
    esp_err_t (*get_signal)(modem_t *me, modem_signal_t *signal);
    esp_err_t (*get_registration)(modem_t *me, modem_reg_status_t *status);
    esp_err_t (*get_packet_attach_status)(modem_t *me, bool *attached);
    esp_err_t (*set_apn)(modem_t *me, uint8_t cid, const char *apn);
```

- [ ] **Step 3: Add the wrapper to `src/modem/modem.c`**

Immediately after `modem_get_registration()`, add:

```c
esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_packet_attach_status,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "get_packet_attach_status not supported");

    return me->ops->get_packet_attach_status(me, attached);
}
```

- [ ] **Step 4: Run the host regression test and verify the expected remaining failures**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: FAIL. The modem API assertions for `modem.h`, `modem_priv.h`, and `modem.c` should pass; Air780EP and `net_mgr` assertions should still fail.

- [ ] **Step 5: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c tests/host/test_net_mgr_activation_flow.py
git commit -m "feat(modem): expose packet attach status"
```

Expected: one commit containing the test plus modem abstraction changes, unless Task 1 was already committed.

---

### Task 3: Implement Air780EP Packet Attach Operation

**Files:**
- Modify: `src/modem/modem_air780ep.c:173-197`
- Modify: `src/modem/modem_air780ep.c:733-745`
- Modify: `src/modem/modem_air780ep.c:2329-2330`

- [ ] **Step 1: Add the Air780EP method prototype**

Immediately after the existing `air780ep_get_registration()` prototype and before `air780ep_set_apn()`, insert:

```c
/**
 * @brief 获取 Air780EP 分组域附着状态
 * @details Get Air780EP packet domain attach status
 * @param[in] me 调制解调器句柄
 * @param[out] attached 是否已附着
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他: AT 命令错误
 */
static esp_err_t air780ep_get_packet_attach_status(modem_t *me, bool *attached);
```

- [ ] **Step 2: Wire the method into `s_air780ep_ops`**

In `s_air780ep_ops`, add the new field immediately after `.get_registration`:

```c
    .get_registration = air780ep_get_registration,
    .get_packet_attach_status = air780ep_get_packet_attach_status,
    .set_apn = air780ep_set_apn,
```

- [ ] **Step 3: Add the Air780EP method implementation**

Immediately after `air780ep_get_registration()` and before `air780ep_set_apn()`, add:

```c
static esp_err_t air780ep_get_packet_attach_status(modem_t *me, bool *attached)
{
    ESP_RETURN_ON_FALSE(me && attached, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    modem_air780ep_t *self = to_air780ep(me);
    return query_cgatt(self, attached);
}
```

- [ ] **Step 4: Run the host regression test and verify the expected remaining failures**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: FAIL. Packet attach modem and Air780EP assertions should pass; `net_mgr` staged-flow assertions should still fail.

- [ ] **Step 5: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add src/modem/modem_air780ep.c src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c tests/host/test_net_mgr_activation_flow.py
git commit -m "feat(air780ep): implement packet attach status query"
```

Expected: one commit containing the Air780EP packet attach method and any uncommitted modem API changes.

---

### Task 4: Add Explicit net_mgr Activation Stages

**Files:**
- Modify: `src/core/core_priv.h:73-82`
- Modify: `src/core/net_mgr.c:19-139`

- [ ] **Step 1: Extend `net_mgr_step_t` in `src/core/core_priv.h`**

Replace the existing `net_mgr_step_t` enum with:

```c
typedef enum {
    NET_STEP_IDLE = 0,
    NET_STEP_CHECK_SIM,
    NET_STEP_CHECK_SIGNAL,
    NET_STEP_WAIT_REGISTRATION,
    NET_STEP_WAIT_PACKET_ATTACH,
    NET_STEP_SET_APN,
    NET_STEP_ACTIVATE_PDP,
    NET_STEP_QUERY_IP,
    NET_STEP_DONE,
    NET_STEP_ERROR,
} net_mgr_step_t;
```

- [ ] **Step 2: Add the polling interval define to `src/core/net_mgr.c`**

In the define section immediately after `#define TAG "net_mgr"`, add:

```c
#define NET_MGR_WAIT_POLL_INTERVAL_MS 1000
```

- [ ] **Step 3: Replace obsolete activation prototypes**

Delete the current `run_activation_once()` prototype block. In its place, add these prototypes after `now_ms()`:

```c
/**
 * @brief 进入网络激活流程
 * @details Enter network activation flow
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - other: 状态设置或事件发布失败
 */
static esp_err_t enter_activation(core_t *me);

/**
 * @brief 执行网络激活阶段循环
 * @details Run network activation stage loop
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_TIMEOUT: 激活超时
 *         - other: 激活失败
 */
static esp_err_t run_activation_loop(core_t *me,
                                     uint32_t activation_start_ms);

/**
 * @brief 执行当前网络激活阶段
 * @details Run current network activation stage
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 当前阶段完成
 *         - ESP_ERR_NOT_FINISHED: 当前阶段仍需等待
 *         - other: 当前阶段失败
 */
static esp_err_t run_activation_step(core_t *me,
                                     uint32_t activation_start_ms);

/**
 * @brief 设置网络激活阶段
 * @details Set network activation stage
 * @param[in] me LTE 核心服务句柄
 * @param[in] step 网络激活阶段
 */
static void set_activation_step(core_t *me, net_mgr_step_t step);

/**
 * @brief 等待下一次网络激活轮询
 * @details Wait for next network activation poll
 * @param[in] me LTE 核心服务句柄
 * @param[in] activation_start_ms 激活开始时间
 * @return
 *         - ESP_OK: 可以继续
 *         - ESP_ERR_INVALID_STATE: Core 正在销毁
 *         - ESP_ERR_TIMEOUT: 网络激活已超时
 */
static esp_err_t wait_next_poll(core_t *me, uint32_t activation_start_ms);

/**
 * @brief 完成网络激活
 * @details Complete network activation
 * @param[in] me LTE 核心服务句柄
 * @param[in] pdp PDP 上下文
 * @return
 *         - ESP_OK: 成功
 *         - other: 状态设置或事件发布失败
 */
static esp_err_t complete_activation(core_t *me,
                                     const modem_pdp_context_t *pdp);
```

- [ ] **Step 4: Add fatal/waiting classification prototypes**

Immediately after the `registration_ready()` prototype block, add:

```c
/**
 * @brief 判断 SIM 状态是否为终止错误
 * @details Check whether SIM status is terminal failure
 * @param[in] status SIM 状态
 * @return
 *         - true: 终止错误
 *         - false: 可等待或已就绪
 */
static bool sim_status_fatal(modem_sim_status_t status);

/**
 * @brief 判断网络注册状态是否被拒绝
 * @details Check whether network registration is denied
 * @param[in] status 网络注册状态
 * @return
 *         - true: 注册被拒绝
 *         - false: 未被拒绝
 */
static bool registration_denied(modem_reg_status_t status);
```

- [ ] **Step 5: Run the host regression test and verify the expected remaining failures**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: FAIL. The explicit stage-name assertions may pass; staged loop implementation assertions should still fail.

---

### Task 5: Replace Rapid Activation Retries With Staged Loop

**Files:**
- Modify: `src/core/net_mgr.c:301-345`
- Modify: `src/core/net_mgr.c:554-665`
- Modify: `src/core/net_mgr.c:687-691`

- [ ] **Step 1: Replace `net_mgr_start_activation()`**

Replace the entire existing `net_mgr_start_activation()` function with:

```c
esp_err_t net_mgr_start_activation(core_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    net_mgr_cancel_reconnect(me);
    me->net_mgr.reconnect_enabled = true;
    me->net_mgr.retry_count = 0;

    const uint32_t activation_start_ms = now_ms();
    esp_err_t ret = enter_activation(me);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = run_activation_loop(me, activation_start_ms);
    if (ret == ESP_OK) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_STATE && core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }

    return fail_activation(me, ret);
}
```

- [ ] **Step 2: Replace `run_activation_once()` with staged helpers**

Delete the entire existing `run_activation_once()` function. Add this code at the same location in the static functions section:

```c
static esp_err_t enter_activation(core_t *me)
{
    esp_err_t ret = core_set_state(me, CORE_STATE_NET_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = net_mgr_set_state(me, CORE_NET_STATE_ACTIVATING);
    if (ret != ESP_OK) {
        return ret;
    }

    set_activation_step(me, NET_STEP_CHECK_SIM);
    post_net_state(me, CORE_EVENT_NET_CONNECTING,
                   CORE_NET_STATE_ACTIVATING, 0);

    return ESP_OK;
}

static esp_err_t run_activation_loop(core_t *me,
                                     uint32_t activation_start_ms)
{
    while (true) {
        esp_err_t ret = check_activation_continue(me, activation_start_ms);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = run_activation_step(me, activation_start_ms);
        if (ret == ESP_OK) {
            if (me->net_mgr.current_step == NET_STEP_DONE) {
                return ESP_OK;
            }
            continue;
        }
        if (ret == ESP_ERR_NOT_FINISHED) {
            ret = wait_next_poll(me, activation_start_ms);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }

        return ret;
    }
}

static esp_err_t run_activation_step(core_t *me,
                                     uint32_t activation_start_ms)
{
    (void)activation_start_ms;

    esp_err_t ret = ESP_OK;
    switch (me->net_mgr.current_step) {
    case NET_STEP_CHECK_SIM: {
        modem_sim_status_t sim_status = MODEM_SIM_UNKNOWN;
        ret = modem_get_sim_status(me->modem, &sim_status);
        if (ret == ESP_ERR_TIMEOUT) {
            return ESP_ERR_NOT_FINISHED;
        }
        if (ret != ESP_OK) {
            return ret;
        }
        if (sim_status == MODEM_SIM_READY) {
            set_activation_step(me, NET_STEP_CHECK_SIGNAL);
            return ESP_OK;
        }
        if (sim_status_fatal(sim_status)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_CHECK_SIGNAL: {
        modem_signal_t signal = {0};
        ret = modem_get_signal(me->modem, &signal);
        if (ret != ESP_OK) {
            return ret;
        }
        set_activation_step(me, NET_STEP_WAIT_REGISTRATION);
        return ESP_OK;
    }

    case NET_STEP_WAIT_REGISTRATION: {
        modem_reg_status_t reg_status = MODEM_REG_UNKNOWN;
        ret = modem_get_registration(me->modem, &reg_status);
        if (ret != ESP_OK) {
            return ret;
        }
        if (registration_ready(reg_status)) {
            set_activation_step(me, NET_STEP_WAIT_PACKET_ATTACH);
            return ESP_OK;
        }
        if (registration_denied(reg_status)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_WAIT_PACKET_ATTACH: {
        bool attached = false;
        ret = modem_get_packet_attach_status(me->modem, &attached);
        if (ret != ESP_OK) {
            return ret;
        }
        if (attached) {
            set_activation_step(me, NET_STEP_SET_APN);
            return ESP_OK;
        }
        return ESP_ERR_NOT_FINISHED;
    }

    case NET_STEP_SET_APN:
        if (me->config.apn[0] != '\0') {
            ret = modem_set_apn(me->modem, me->config.primary_cid,
                                me->config.apn);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        set_activation_step(me, NET_STEP_ACTIVATE_PDP);
        return ESP_OK;

    case NET_STEP_ACTIVATE_PDP:
        ret = modem_activate_pdp(me->modem, me->config.primary_cid);
        if (ret == ESP_ERR_INVALID_STATE) {
            set_activation_step(me, NET_STEP_WAIT_REGISTRATION);
            return ESP_ERR_NOT_FINISHED;
        }
        if (ret != ESP_OK) {
            esp_err_t cleanup_ret = modem_deactivate_pdp(me->modem,
                                                         me->config.primary_cid);
            if (cleanup_ret != ESP_OK) {
                ESP_LOGW(TAG, "cleanup after PDP activation failed: %s",
                         esp_err_to_name(cleanup_ret));
            }
            return ret;
        }
        set_activation_step(me, NET_STEP_QUERY_IP);
        return ESP_OK;

    case NET_STEP_QUERY_IP: {
        modem_pdp_context_t pdp = {0};
        ret = modem_get_pdp_context(me->modem, me->config.primary_cid, &pdp);
        if (ret != ESP_OK) {
            return ret;
        }
        if (!pdp.active) {
            set_activation_step(me, NET_STEP_WAIT_PACKET_ATTACH);
            return ESP_ERR_NOT_FINISHED;
        }
        if (pdp.ip_addr[0] == '\0') {
            return ESP_ERR_NOT_FINISHED;
        }
        return complete_activation(me, &pdp);
    }

    case NET_STEP_DONE:
        return ESP_OK;

    case NET_STEP_IDLE:
    case NET_STEP_ERROR:
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

static void set_activation_step(core_t *me, net_mgr_step_t step)
{
    me->net_mgr.current_step = step;
    me->net_mgr.step_start_time_ms = now_ms();
}

static esp_err_t wait_next_poll(core_t *me, uint32_t activation_start_ms)
{
    esp_err_t ret = check_activation_continue(me, activation_start_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(NET_MGR_WAIT_POLL_INTERVAL_MS));
    return check_activation_continue(me, activation_start_ms);
}

static esp_err_t complete_activation(core_t *me,
                                     const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(pdp && pdp->active && pdp->ip_addr[0] != '\0',
                        ESP_ERR_INVALID_STATE, TAG, "PDP is not ready");

    esp_err_t ret = net_mgr_set_state(me, CORE_NET_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = core_set_state(me, CORE_STATE_ONLINE);
    if (ret != ESP_OK) {
        return ret;
    }

    pdp_mgr_update(&me->pdp_mgr, pdp);
    set_activation_step(me, NET_STEP_DONE);
    post_net_state(me, CORE_EVENT_NET_ONLINE, CORE_NET_STATE_ONLINE, 0);

    return ESP_OK;
}
```

- [ ] **Step 3: Add SIM and registration classifiers**

Immediately after `registration_ready()`, add:

```c
static bool sim_status_fatal(modem_sim_status_t status)
{
    return status == MODEM_SIM_PIN_REQUIRED ||
           status == MODEM_SIM_PUK_REQUIRED ||
           status == MODEM_SIM_NOT_INSERTED ||
           status == MODEM_SIM_ERROR;
}

static bool registration_denied(modem_reg_status_t status)
{
    return status == MODEM_REG_DENIED;
}
```

- [ ] **Step 4: Run the host regression test and verify the expected remaining failure**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: FAIL. Staged loop and registration waiting assertions should pass; the PDP URC/IP guard assertion may still fail until Task 6.

- [ ] **Step 5: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add src/core/core_priv.h src/core/net_mgr.c tests/host/test_net_mgr_activation_flow.py
git commit -m "feat(core): use staged net activation polling"
```

Expected: one commit containing the staged activation loop and any uncommitted test changes.

---

### Task 6: Prevent PDP URC From Bypassing IP Requirement

**Files:**
- Modify: `src/core/net_mgr.c:378-410`

- [ ] **Step 1: Replace `net_mgr_handle_pdp_activated()`**

Replace the entire existing `net_mgr_handle_pdp_activated()` function with:

```c
esp_err_t net_mgr_handle_pdp_activated(core_t *me,
                                       const modem_pdp_context_t *pdp)
{
    ESP_RETURN_ON_FALSE(me && pdp, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    if (core_is_destroying(me)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_primary_pdp(me, pdp)) {
        return ESP_OK;
    }

    core_net_state_t old_state = CORE_NET_STATE_OFFLINE;
    esp_err_t ret = net_mgr_get_state(me, &old_state);
    ESP_RETURN_ON_ERROR(ret, TAG, "get net state failed");

    pdp_mgr_update(&me->pdp_mgr, pdp);
    if (old_state == CORE_NET_STATE_ONLINE) {
        return ESP_OK;
    }
    if (old_state != CORE_NET_STATE_ACTIVATING) {
        return ESP_OK;
    }
    if (!pdp->active || pdp->ip_addr[0] == '\0') {
        return ESP_OK;
    }

    return complete_activation(me, pdp);
}
```

- [ ] **Step 2: Run the host regression test and verify it passes**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: PASS. Output should end with `OK`.

- [ ] **Step 3: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add src/core/net_mgr.c tests/host/test_net_mgr_activation_flow.py
git commit -m "fix(core): require IP before PDP URC marks online"
```

Expected: one commit containing the PDP URC guard and any uncommitted test changes.

---

### Task 7: Update Internal Class Documentation

**Files:**
- Modify: `docs/agents/classes.md:449-460`

- [ ] **Step 1: Update the modem ops mapping table**

In `docs/agents/classes.md`, update the modem ops table so the rows around registration, packet attach, APN, and PDP activation read:

```markdown
| `get_registration` | 查询蜂窝网络注册状态 | Air780EP 优先 `AT+CEREG?`，必要时用 `AT+CGREG?` 补充分组域状态，用 `AT+CREG?` 作为通用注册状态兜底 |
| `get_packet_attach_status` | 查询分组域附着状态 | `AT+CGATT?`，要求 `+CGATT: 1` 后才能进入 PDP/TCPIP 激活阶段 |
| `set_apn` | 配置 PDP context 的 APN | `AT+CGDCONT=<cid>,"IP","<apn>"`；APN 用户名/密码后续再通过单独能力扩展，不塞进当前 API |
| `activate_pdp` | 注册和附着已就绪后激活数据面并获得 IP | Air780EP TCPIP 路径使用 `AT+CSTT`、`AT+CIICR`、`AT+CIFSR`；Core 先通过 `get_sim_status`、`get_registration`、`get_packet_attach_status` 等阶段确认前置条件 |
```

- [ ] **Step 2: Run the host regression test and verify it still passes**

Run:

```bash
python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: PASS. Output should end with `OK`.

- [ ] **Step 3: Commit checkpoint if commits are explicitly allowed**

Run only when the execution session has explicit permission to commit:

```bash
git add docs/agents/classes.md tests/host/test_net_mgr_activation_flow.py
git commit -m "docs(core): document packet attach activation stage"
```

Expected: one commit containing the documentation update and any uncommitted test changes.

---

### Task 8: Build And Hardware Verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Run the full host static regression set**

Run:

```bash
python3 tests/host/test_air780ep_cpin_policy.py && python3 tests/host/test_net_mgr_activation_flow.py
```

Expected: PASS. Both scripts should end with `OK`.

- [ ] **Step 2: Build the example**

Run from the repository root:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py -C examples/basic_connect build
```

Expected: build exits with code 0 and reports the `basic_connect` binary size.

- [ ] **Step 3: Flash only if explicit hardware permission is present**

Run only if the user explicitly approves flashing in the execution session:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py -C examples/basic_connect -p /dev/cu.usbserial-120 flash
```

Expected: flash exits with code 0. If `/dev/cu.usbserial-120` is not present, first list ports with `zsh -c 'setopt null_glob; ls /dev/tty.usb* /dev/cu.usb*'` and use the detected `/dev/cu.*` port.

- [ ] **Step 4: Capture serial logs after flashing or on the existing image**

Run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 60 --port /dev/cu.usbserial-120
```

Expected if the new image is running:

```text
LTE event: NET_CONNECTING net=ACTIVATING err=0
LTE event: NET_ONLINE net=ONLINE err=0
periodic: lte=ONLINE net=ONLINE
```

The log must not contain this sequence during normal registration searching:

```text
NET_ERROR
NET_ONLINE
```

- [ ] **Step 5: Final status check**

Run:

```bash
git status --short
```

Expected: only intended files are modified or untracked. Do not revert unrelated user changes. Do not commit unless the user explicitly requested commits.
