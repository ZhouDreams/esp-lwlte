# Ping Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the synchronous user-callable Ping Service described in `docs/agents/classes.md` and `docs/superpowers/specs/2026-05-28-ping-service-design.md`.

**Architecture:** App code calls `lwlte_ping()`, Facade maps public values to `ping_client_ping()`, Ping Service submits `CORE_CMD_PING`, Core FSM serializes execution and calls `modem_ping()`, and Air780EP maps that semantic operation to `AT+CIPPING`. Ping Service remains lightweight: it owns no FSM task, no queue, and no event loop.

**Tech Stack:** ESP-IDF C, FreeRTOS semaphores, existing C OOP handle pattern, Core command queue, Modem ops vtable, Air780EP AT Engine, Python `unittest` static contract tests, ESP-IDF build tooling.

---

## Classes Contract Alignment

This implementation must stay aligned with `docs/agents/classes.md`. If source code needs a narrow contract adjustment, update `classes.md` in the same task and extend the static contract tests so docs and code cannot drift again.

Fixed alignment points:

- `src/ping_client/ping_client.h` is a layer-internal API with `ping_client_` prefix.
- Ping Service may include Core headers only; it must not include `modem.h`, `modem_air780ep.h`, `at_engine.h`, or any `_priv.h` outside its own module.
- Ping Service must not create an FSM task, FSM queue, or esp_event loop.
- `lwlte_ping()` is the only first-version public user API; do not add `lwlte_ping_async()` or `LWLTE_EVENT_PING_DONE`.
- Facade, Ping Service, Core, and Modem each keep their own value types. Copy fields explicitly; do not cast `lwlte_ping_reply_t *` to `core_ping_reply_t *` or `core_ping_reply_t *` to `modem_ping_reply_t *`.
- Core remains the only layer that calls `modem_ping()`.
- Air780EP remains the only layer that builds `AT+CIPPING`.
- `total_timeout_ms` must not make Ping Service return while Core can still write caller-owned output buffers. The safe implementation waits for Core command completion; timeout is represented by Core/Modem completing the command with `ESP_ERR_TIMEOUT`.

---

## File Structure

- Create: `tests/host/test_ping_end_to_end_contract.py` — static end-to-end contract covering public API, Ping Service boundaries, Core command queue, Modem ops, Air780EP parser, CMake registration, and `classes.md` alignment.
- Create: `src/ping_client/ping_client.h` — Ping Service layer-internal API used only by Facade.
- Create: `src/ping_client/ping_client_priv.h` — Ping Service private struct and wait context declarations.
- Create: `src/ping_client/ping_client.c` — lightweight synchronous Ping Service implementation.
- Modify: `src/CMakeLists.txt` — compile `ping_client/ping_client.c` and add `ping_client` to private include dirs.
- Modify: `src/include/lwlte.h` — add public ping value types and `lwlte_ping()`.
- Modify: `src/lwlte/lwlte_priv.h` — include Ping Service and add `ping_client_t *ping` to `lwlte_t`.
- Modify: `src/lwlte/lwlte.c` — implement `lwlte_ping()`, map value types explicitly, and destroy Ping Service before Core.
- Modify: `src/lwlte/lwlte_air780ep.c` — create Ping Service after Core and before optional MQTT/Core start.
- Modify: `src/core/core.h` — add `core_ping_*` types, `CORE_CMD_PING`, and ping command data.
- Modify: `src/core/core.c` — validate/clone/free ping command host and preserve output-buffer pointers.
- Modify: `src/core/core_fsm.c` — handle `CORE_CMD_PING`, check Core network online, call `modem_ping()`, and copy Modem results to Core results.
- Modify: `src/modem/modem.h` — add `modem_ping_*` value types and `modem_ping()` wrapper declaration.
- Modify: `src/modem/modem_priv.h` — add `modem_ops_t.ping`.
- Modify: `src/modem/modem.c` — implement `modem_ping()` wrapper with validation and ops dispatch.
- Modify: `src/modem/modem_air780ep.c` — add `air780ep_ping()`, `AT+CIPPING` command construction, parser, loss detection, and summary calculation.
- Modify if needed: `docs/agents/classes.md` — only for source/doc alignment if implementation requires a narrow clarified timeout field or helper naming.

---

### Task 1: Add Ping End-To-End Static Contract Test

**Files:**
- Create: `tests/host/test_ping_end_to_end_contract.py`
- Read: `docs/agents/classes.md`

- [ ] **Step 1: Write the failing static contract test**

Create `tests/host/test_ping_end_to_end_contract.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static end-to-end contract checks for Ping Service implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


class PingEndToEndContractTest(unittest.TestCase):
    def test_public_facade_api_matches_classes(self):
        lwlte_h = read("src/include/lwlte.h")
        classes_md = read("docs/agents/classes.md")

        for token in [
            "typedef struct {\n    const char *host;",
            "uint8_t count;",
            "uint16_t data_len;",
            "uint16_t timeout_100ms;",
            "uint8_t ttl;",
            "uint32_t total_timeout_ms;",
            "} lwlte_ping_request_t;",
            "} lwlte_ping_reply_t;",
            "} lwlte_ping_summary_t;",
            "esp_err_t lwlte_ping(lwlte_t *me,",
            "const lwlte_ping_request_t *request,",
            "lwlte_ping_reply_t *replies,",
            "size_t max_replies,",
            "lwlte_ping_summary_t *summary);",
        ]:
            self.assertIn(token, lwlte_h)
            self.assertIn(token, classes_md)

        self.assertNotIn("lwlte_ping_async(", lwlte_h)
        self.assertNotIn("LWLTE_EVENT_PING_DONE", lwlte_h)

    def test_ping_client_is_lightweight_and_core_only(self):
        header_path = ROOT / "src/ping_client/ping_client.h"
        priv_path = ROOT / "src/ping_client/ping_client_priv.h"
        source_path = ROOT / "src/ping_client/ping_client.c"
        self.assertTrue(header_path.exists())
        self.assertTrue(priv_path.exists())
        self.assertTrue(source_path.exists())

        header = read("src/ping_client/ping_client.h")
        priv = read("src/ping_client/ping_client_priv.h")
        source = read("src/ping_client/ping_client.c")
        combined = "\n".join([header, priv, source])

        for token in [
            "typedef struct ping_client ping_client_t;",
            "ping_client_t *ping_client_create(core_t *core);",
            "esp_err_t ping_client_destroy(ping_client_t *me);",
            "esp_err_t ping_client_ping(ping_client_t *me,",
            "core_submit_cmd(me->core, &cmd)",
            ".type = CORE_CMD_PING",
            "xSemaphoreCreateBinary()",
            "ping_core_cmd_done_cb",
        ]:
            self.assertIn(token, combined)

        for forbidden in [
            '#include "modem.h"',
            '#include "modem_air780ep.h"',
            '#include "at_engine.h"',
            '#include "core_priv.h"',
            "xTaskCreate(",
            "xQueueCreate(",
            "esp_event_loop_create(",
            "modem_ping(",
            "at_engine_",
            "AT+CIPPING",
        ]:
            self.assertNotIn(forbidden, combined)

    def test_facade_wires_ping_and_maps_types_without_casting(self):
        lwlte_priv_h = read("src/lwlte/lwlte_priv.h")
        lwlte_c = read("src/lwlte/lwlte.c")
        air780ep_factory = read("src/lwlte/lwlte_air780ep.c")

        for token in [
            '#include "ping_client.h"',
            "ping_client_t *ping;",
        ]:
            self.assertIn(token, lwlte_priv_h)

        self.assertIn("me->ping = ping_client_create(me->core);", air780ep_factory)
        self.assertLess(
            air780ep_factory.index("me->core = core_create"),
            air780ep_factory.index("me->ping = ping_client_create"),
        )
        self.assertLess(
            air780ep_factory.index("me->ping = ping_client_create"),
            air780ep_factory.index("core_start(me->core)"),
        )

        for token in [
            "esp_err_t lwlte_ping(lwlte_t *me,",
            "ping_client_request_t ping_request",
            "core_ping_reply_t *core_replies",
            "ping_client_ping(ping, &ping_request, core_replies,",
            "replies[i].seq = core_replies[i].seq;",
            "summary->sent = core_summary.sent;",
            "ping_client_destroy(me->ping)",
        ]:
            self.assertIn(token, lwlte_c)

        self.assertLess(lwlte_c.index("ping_client_destroy(me->ping)"),
                        lwlte_c.index("core_destroy(me->core)"))
        self.assertNotRegex(lwlte_c, r"\(\s*core_ping_reply_t\s*\*\s*\)\s*replies")
        self.assertNotRegex(lwlte_c, r"\(\s*lwlte_ping_reply_t\s*\*\s*\)\s*core_replies")

    def test_core_ping_command_boundary_matches_classes(self):
        core_h = read("src/core/core.h")
        core_c = read("src/core/core.c")
        core_fsm_c = read("src/core/core_fsm.c")

        for token in [
            "CORE_CMD_PING",
            "core_ping_reply_t",
            "core_ping_summary_t",
            "core_ping_reply_t *replies;",
            "size_t max_replies;",
            "core_ping_summary_t *summary;",
        ]:
            self.assertIn(token, core_h)

        for token in [
            "case CORE_CMD_PING:",
            "clone->data.ping.host = clone_optional_string(cmd->data.ping.host);",
            "free((void *)cmd->data.ping.host);",
            "cmd->data.ping.host != NULL",
            "cmd->data.ping.replies != NULL",
        ]:
            self.assertIn(token, core_c)

        for token in [
            "handle_ping_cmd(me, cmd)",
            "core_get_net_state(me, &net_state)",
            "net_state != CORE_NET_STATE_ONLINE",
            "ESP_ERR_INVALID_STATE",
            "modem_ping_request_t request",
            "modem_ping_reply_t *modem_replies",
            "modem_ping(me->modem, &request, modem_replies,",
            "copy_core_ping_replies(cmd->data.ping.replies, modem_replies,",
            "copy_core_ping_summary(cmd->data.ping.summary, &modem_summary)",
            "finish_service_cmd(me, cmd, result_from_esp_err(ret), &ret)",
        ]:
            self.assertIn(token, core_fsm_c)

        self.assertNotRegex(core_fsm_c, r"\(\s*modem_ping_reply_t\s*\*\s*\)\s*cmd->data\.ping\.replies")
        self.assertNotRegex(core_fsm_c, r"\(\s*core_ping_reply_t\s*\*\s*\)\s*modem_replies")

    def test_modem_ping_boundary_matches_classes(self):
        modem_h = read("src/modem/modem.h")
        modem_priv_h = read("src/modem/modem_priv.h")
        modem_c = read("src/modem/modem.c")

        for token in [
            "} modem_ping_request_t;",
            "} modem_ping_reply_t;",
            "} modem_ping_summary_t;",
            "esp_err_t modem_ping(modem_t *me,",
            "const modem_ping_request_t *request,",
            "modem_ping_reply_t *replies,",
            "size_t max_replies,",
            "modem_ping_summary_t *summary);",
        ]:
            self.assertIn(token, modem_h)

        self.assertIn("esp_err_t (*ping)(modem_t *me,", modem_priv_h)
        for token in [
            "esp_err_t modem_ping(modem_t *me,",
            "request->host && request->host[0]",
            "request->count >= 1 && request->count <= 100",
            "request->data_len <= 1024",
            "request->timeout_100ms >= 1 && request->timeout_100ms <= 600",
            "request->ttl >= 1",
            "max_replies >= request->count",
            "ESP_ERR_NOT_SUPPORTED",
            "return me->ops->ping(me, request, replies, max_replies, summary);",
        ]:
            self.assertIn(token, modem_c)

    def test_air780ep_cipping_mapping_and_parser(self):
        air780ep_c = read("src/modem/modem_air780ep.c")

        for token in [
            "#define AIR780EP_CIPPING_PREFIX         \"+CIPPING:\"",
            "#define AIR780EP_CIPPING_MAX_COUNT      100",
            "static esp_err_t air780ep_ping(modem_t *me,",
            ".ping = air780ep_ping,",
            "AT+CIPPING=\"%s\",%u,%u,%u,%u",
            "parse_cipping_line",
            "calculate_ping_summary",
            "reply_time == (uint32_t)request->timeout_100ms * 100U",
            "parsed.ttl == 255",
            "reply->success = !lost",
            "summary->sent = request->count;",
            "summary->lost = summary->sent - summary->received;",
            "ESP_ERR_INVALID_RESPONSE",
        ]:
            self.assertIn(token, air780ep_c)

        self.assertNotIn("AT+CIPPING", read("src/core/core_fsm.c"))
        self.assertNotIn("AT+CIPPING", read("src/ping_client/ping_client.c"))

    def test_cmake_registers_ping_client_source(self):
        cmake = read("src/CMakeLists.txt")
        self.assertIn('"ping_client/ping_client.c"', cmake)
        self.assertRegex(cmake, r"PRIV_INCLUDE_DIRS\s+lwlte core mqtt_client ping_client modem at_engine")

    def test_classes_doc_and_source_keep_ping_boundary_aligned(self):
        classes_md = read("docs/agents/classes.md")
        for rel_path in [
            "src/include/lwlte.h",
            "src/ping_client/ping_client.h",
            "src/core/core.h",
            "src/modem/modem.h",
        ]:
            source = read(rel_path)
            for token in [
                "lwlte_ping_request_t" if rel_path.endswith("lwlte.h") else None,
                "ping_client_request_t" if rel_path.endswith("ping_client.h") else None,
                "CORE_CMD_PING" if rel_path.endswith("core.h") else None,
                "modem_ping_request_t" if rel_path.endswith("modem.h") else None,
            ]:
                if token:
                    self.assertIn(token, source)
                    self.assertIn(token, classes_md)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the new test and verify it fails**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract -v
```

Expected: FAIL. The first failures should mention missing `src/ping_client/ping_client.h`, missing public `lwlte_ping_*` source API, and missing `CORE_CMD_PING` source implementation.

- [ ] **Step 3: Run existing docs contract to ensure baseline still passes**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_classes_doc_contract -v
```

Expected: PASS. If this fails before implementation, stop and fix the docs contract first.

---

### Task 2: Add Public Facade Types And Ping Service Skeleton

**Files:**
- Modify: `src/include/lwlte.h`
- Create: `src/ping_client/ping_client.h`
- Create: `src/ping_client/ping_client_priv.h`
- Create: `src/ping_client/ping_client.c`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add public ping types and function declaration**

In `src/include/lwlte.h`, add the public ping structs after `lwlte_mqtt_msg_t` and before `lwlte_event_id_t`:

```c
/**
 * @brief LTE Ping 请求
 * @details LTE Ping request
 */
typedef struct {
    const char *host;                /**< 目标主机或 IP； Target host or IP */
    uint8_t count;                   /**< 发送次数，1..100； Request count, 1..100 */
    uint16_t data_len;               /**< 数据长度，0..1024； Data length, 0..1024 */
    uint16_t timeout_100ms;          /**< 单包超时，单位 100ms； Per-packet timeout in 100ms */
    uint8_t ttl;                     /**< TTL，1..255； TTL, 1..255 */
    uint32_t total_timeout_ms;       /**< 总等待超时，0 使用派生默认值； Total timeout, 0 derives default */
} lwlte_ping_request_t;

/**
 * @brief LTE Ping 单包响应
 * @details LTE Ping single reply
 */
typedef struct {
    uint8_t seq;                     /**< 响应序号； Reply sequence */
    char ip[48];                     /**< 响应 IP； Reply IP */
    uint32_t time_ms;                /**< 耗时毫秒； Time in milliseconds */
    uint8_t ttl;                     /**< 响应 TTL； Reply TTL */
    bool success;                    /**< 是否成功； Whether successful */
} lwlte_ping_reply_t;

/**
 * @brief LTE Ping 汇总
 * @details LTE Ping summary
 */
typedef struct {
    uint8_t sent;                    /**< 已发送数量； Sent count */
    uint8_t received;                /**< 已收到数量； Received count */
    uint8_t lost;                    /**< 丢包数量； Lost count */
    uint32_t min_time_ms;            /**< 最小耗时； Minimum time */
    uint32_t max_time_ms;            /**< 最大耗时； Maximum time */
    uint32_t avg_time_ms;            /**< 平均耗时； Average time */
} lwlte_ping_summary_t;
```

Add the public function prototype after `lwlte_get_net_state()` and before MQTT public functions:

```c
/**
 * @brief 执行同步 Ping 诊断
 * @details Perform synchronous Ping diagnostic
 * @note replies 由调用方提供，max_replies 必须大于等于 request->count。
 * @note 该函数阻塞直到 Core command 完成；不应在时间敏感回调中调用。
 * @param[in] me LTE 用户门面句柄
 * @param[in] request Ping 请求
 * @param[out] replies 调用方提供的单包响应数组
 * @param[in] max_replies replies 数组容量
 * @param[out] summary 可选汇总结果，可为 NULL
 * @return
 *         - ESP_OK: 命令完成
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 网络未 online 或门面正在销毁
 *         - ESP_ERR_TIMEOUT: Ping 超时
 *         - ESP_ERR_INVALID_RESPONSE: 模块响应格式无效
 *         - other: 下层错误
 */
esp_err_t lwlte_ping(lwlte_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary);
```

- [ ] **Step 2: Create Ping Service public internal header**

Create `src/ping_client/ping_client.h`:

```c
/**
 * @file ping_client.h
 * @brief Ping 诊断服务层间接口
 * @details Ping diagnostic service inter-layer interface
 * @author JovisDreams
 * @date 2026-05-28
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stddef.h>
#include <stdint.h>

#include "core.h"
#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct ping_client ping_client_t;

typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} ping_client_request_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
ping_client_t *ping_client_create(core_t *core);
esp_err_t ping_client_destroy(ping_client_t *me);
esp_err_t ping_client_ping(ping_client_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 3: Create Ping Service private header**

Create `src/ping_client/ping_client_priv.h`:

```c
/**
 * @file ping_client_priv.h
 * @brief Ping 诊断服务内部接口
 * @details Ping diagnostic service internal interface
 * @author JovisDreams
 * @date 2026-05-28
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>

#include "ping_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*********************
 *      DEFINES
 *********************/
#define PING_CLIENT_MAX_COUNT              100U
#define PING_CLIENT_MAX_DATA_LEN           1024U
#define PING_CLIENT_MAX_TIMEOUT_100MS      600U
#define PING_CLIENT_DEFAULT_OVERHEAD_MS    5000U

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    SemaphoreHandle_t done_sema;
    core_cmd_result_t core_result;
    esp_err_t esp_result;
    bool completed;
} ping_wait_ctx_t;

struct ping_client {
    core_t *core;
    SemaphoreHandle_t lock;
    bool destroying;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 4: Create Ping Service implementation**

Create `src/ping_client/ping_client.c` with this implementation pattern. Keep the file free of Modem, Air780EP, AT Engine, task, queue, and event-loop dependencies:

```c
/**
 * @file ping_client.c
 * @brief Ping 诊断服务实现
 * @details Ping diagnostic service implementation
 * @author JovisDreams
 * @date 2026-05-28
 */

/*********************
 *      INCLUDES
 *********************/
#include "ping_client_priv.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "ping_client"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool request_valid(const ping_client_request_t *request,
                          const core_ping_reply_t *replies,
                          size_t max_replies);
static uint32_t effective_timeout_ms(const ping_client_request_t *request);
static esp_err_t map_core_result(core_cmd_result_t result, esp_err_t esp_result);
static esp_err_t map_submit_error(esp_err_t err);
static void ping_core_cmd_done_cb(core_t *core,
                                  core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data,
                                  void *user_ctx);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
ping_client_t *ping_client_create(core_t *core)
{
    if (!core) {
        return NULL;
    }

    ping_client_t *me = calloc(1, sizeof(ping_client_t));
    if (!me) {
        return NULL;
    }

    me->lock = xSemaphoreCreateMutex();
    if (!me->lock) {
        free(me);
        return NULL;
    }

    me->core = core;
    me->destroying = false;
    return me;
}

esp_err_t ping_client_destroy(ping_client_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    me->destroying = true;
    xSemaphoreGive(me->lock);

    vSemaphoreDelete(me->lock);
    me->lock = NULL;
    me->core = NULL;
    free(me);
    return ESP_OK;
}

esp_err_t ping_client_ping(ping_client_t *me,
                           const ping_client_request_t *request,
                           core_ping_reply_t *replies,
                           size_t max_replies,
                           core_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(request_valid(request, replies, max_replies),
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping request");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    core_t *core = me->core;
    bool destroying = me->destroying;
    xSemaphoreGive(me->lock);
    ESP_RETURN_ON_FALSE(core && !destroying, ESP_ERR_INVALID_STATE, TAG,
                        "ping client is destroying");

    ping_wait_ctx_t wait_ctx = {
        .done_sema = xSemaphoreCreateBinary(),
        .core_result = CORE_CMD_RESULT_ERROR,
        .esp_result = ESP_FAIL,
        .completed = false,
    };
    ESP_RETURN_ON_FALSE(wait_ctx.done_sema, ESP_ERR_NO_MEM, TAG,
                        "create done semaphore failed");

    const uint32_t timeout_ms = effective_timeout_ms(request);
    core_cmd_t cmd = {
        .type = CORE_CMD_PING,
        .done_cb = ping_core_cmd_done_cb,
        .user_ctx = &wait_ctx,
        .timeout_ms = timeout_ms,
        .data.ping = {
            .host = request->host,
            .count = request->count,
            .data_len = request->data_len,
            .timeout_100ms = request->timeout_100ms,
            .ttl = request->ttl,
            .replies = replies,
            .max_replies = max_replies,
            .summary = summary,
        },
    };

    esp_err_t ret = core_submit_cmd(me->core, &cmd);
    if (ret != ESP_OK) {
        vSemaphoreDelete(wait_ctx.done_sema);
        return map_submit_error(ret);
    }

    xSemaphoreTake(wait_ctx.done_sema, portMAX_DELAY);
    ret = map_core_result(wait_ctx.core_result, wait_ctx.esp_result);
    vSemaphoreDelete(wait_ctx.done_sema);
    return ret;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static bool request_valid(const ping_client_request_t *request,
                          const core_ping_reply_t *replies,
                          size_t max_replies)
{
    return request && replies && request->host && request->host[0] &&
           request->count >= 1 && request->count <= PING_CLIENT_MAX_COUNT &&
           request->data_len <= PING_CLIENT_MAX_DATA_LEN &&
           request->timeout_100ms >= 1 &&
           request->timeout_100ms <= PING_CLIENT_MAX_TIMEOUT_100MS &&
           request->ttl >= 1 && max_replies >= request->count;
}

static uint32_t effective_timeout_ms(const ping_client_request_t *request)
{
    if (!request) {
        return PING_CLIENT_DEFAULT_OVERHEAD_MS;
    }
    if (request->total_timeout_ms > 0) {
        return request->total_timeout_ms;
    }

    return ((uint32_t)request->count * (uint32_t)request->timeout_100ms * 100U) +
           PING_CLIENT_DEFAULT_OVERHEAD_MS;
}

static esp_err_t map_core_result(core_cmd_result_t result, esp_err_t esp_result)
{
    switch (result) {
    case CORE_CMD_RESULT_OK:
        return ESP_OK;
    case CORE_CMD_RESULT_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case CORE_CMD_RESULT_INVALID_RESPONSE:
        return ESP_ERR_INVALID_RESPONSE;
    case CORE_CMD_RESULT_ERROR:
    default:
        return esp_result == ESP_OK ? ESP_FAIL : esp_result;
    }
}

static esp_err_t map_submit_error(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT) {
        return ESP_FAIL;
    }
    return err;
}

static void ping_core_cmd_done_cb(core_t *core,
                                  core_cmd_type_t type,
                                  core_cmd_result_t result,
                                  const void *result_data,
                                  void *user_ctx)
{
    (void)core;

    ping_wait_ctx_t *wait_ctx = (ping_wait_ctx_t *)user_ctx;
    if (!wait_ctx || !wait_ctx->done_sema || type != CORE_CMD_PING) {
        return;
    }

    wait_ctx->core_result = result;
    wait_ctx->esp_result = result_data ? *(const esp_err_t *)result_data : ESP_FAIL;
    wait_ctx->completed = true;
    xSemaphoreGive(wait_ctx->done_sema);
}
```

- [ ] **Step 5: Register Ping Service in CMake**

Update `src/CMakeLists.txt` so it contains:

```cmake
idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "core/core.c"
         "core/core_fsm.c"
         "core/net_mgr.c"
         "core/pdp_mgr.c"
         "mqtt_client/mqtt_client.c"
         "ping_client/ping_client.c"
         "lwlte/lwlte.c"
         "lwlte/lwlte_air780ep.c"
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS lwlte core mqtt_client ping_client modem at_engine
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
```

- [ ] **Step 6: Run contract test and verify remaining failures moved forward**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract -v
```

Expected: still FAIL, but failures should now be in Facade wiring, Core, Modem, and Air780EP implementation rather than missing Ping Service files.

---

### Task 3: Add Core Ping Command Boundary

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`

- [ ] **Step 1: Add Core ping command types**

In `src/core/core.h`, add `CORE_CMD_PING` after `CORE_CMD_MQTT_PUBLISH`:

```c
typedef enum {
    CORE_CMD_MQTT_CONFIG = 0,            /**< 配置 MQTT； Configure MQTT */
    CORE_CMD_MQTT_OPEN,                  /**< 打开 MQTT 连接； Open MQTT connection */
    CORE_CMD_MQTT_LOGIN,                 /**< 登录 MQTT； Login MQTT */
    CORE_CMD_MQTT_DISCONNECT,            /**< 断开 MQTT； Disconnect MQTT */
    CORE_CMD_MQTT_SUBSCRIBE,             /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    CORE_CMD_MQTT_UNSUBSCRIBE,           /**< 退订 MQTT 主题； Unsubscribe MQTT topic */
    CORE_CMD_MQTT_PUBLISH,               /**< 发布 MQTT 消息； Publish MQTT message */
    CORE_CMD_PING,                       /**< 执行 Ping 诊断； Perform Ping diagnostic */
} core_cmd_type_t;
```

Add Core ping value types before `core_cmd_done_callback_t`:

```c
typedef struct {
    uint8_t seq;                         /**< 响应序号； Reply sequence */
    char ip[48];                         /**< 响应 IP； Reply IP */
    uint32_t time_ms;                    /**< 耗时毫秒； Time in milliseconds */
    uint8_t ttl;                         /**< 响应 TTL； Reply TTL */
    bool success;                        /**< 是否成功； Whether successful */
} core_ping_reply_t;

typedef struct {
    uint8_t sent;                        /**< 已发送数量； Sent count */
    uint8_t received;                    /**< 已收到数量； Received count */
    uint8_t lost;                        /**< 丢包数量； Lost count */
    uint32_t min_time_ms;                /**< 最小耗时； Minimum time */
    uint32_t max_time_ms;                /**< 最大耗时； Maximum time */
    uint32_t avg_time_ms;                /**< 平均耗时； Average time */
} core_ping_summary_t;
```

Add ping fields to `core_cmd_t.data` after `mqtt_publish`:

```c
        struct {
            const char *host;            /**< 主机； Host */
            uint8_t count;               /**< 发送次数； Request count */
            uint16_t data_len;           /**< 数据长度； Data length */
            uint16_t timeout_100ms;      /**< 单包超时，单位 100ms； Per-packet timeout in 100ms */
            uint8_t ttl;                 /**< TTL； TTL */
            core_ping_reply_t *replies;  /**< 响应输出数组； Reply output array */
            size_t max_replies;          /**< 响应数组容量； Reply array capacity */
            core_ping_summary_t *summary; /**< 可选汇总输出； Optional summary output */
        } ping;                          /**< Ping 参数； Ping args */
```

- [ ] **Step 2: Clone, validate, and free Core ping commands**

In `src/core/core.c`, update `clone_core_cmd()` with this case before `CORE_CMD_MQTT_LOGIN`:

```c
    case CORE_CMD_PING:
        clone->data.ping.host = clone_optional_string(cmd->data.ping.host);
        if (!clone->data.ping.host) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
```

Update `free_core_cmd()` with this case:

```c
    case CORE_CMD_PING:
        free((void *)cmd->data.ping.host);
        break;
```

Update `core_cmd_type_valid()`:

```c
static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_MQTT_CONFIG && type <= CORE_CMD_PING;
}
```

Update `core_cmd_valid()` with this case:

```c
    case CORE_CMD_PING:
        return cmd->data.ping.host != NULL &&
               cmd->data.ping.host[0] != '\0' &&
               cmd->data.ping.count >= 1 &&
               cmd->data.ping.count <= 100 &&
               cmd->data.ping.data_len <= 1024 &&
               cmd->data.ping.timeout_100ms >= 1 &&
               cmd->data.ping.timeout_100ms <= 600 &&
               cmd->data.ping.ttl >= 1 &&
               cmd->data.ping.replies != NULL &&
               cmd->data.ping.max_replies >= cmd->data.ping.count;
```

- [ ] **Step 3: Add Core FSM ping helpers and dispatch**

In `src/core/core_fsm.c`, add static prototypes near the service command helpers:

```c
static void handle_ping_cmd(core_t *me, core_cmd_t *cmd);
static void copy_core_ping_replies(core_ping_reply_t *dst,
                                   const modem_ping_reply_t *src,
                                   size_t count);
static void copy_core_ping_summary(core_ping_summary_t *dst,
                                   const modem_ping_summary_t *src);
```

Update `handle_service_cmd()` so the `CORE_CMD_PING` case delegates and returns early:

```c
    case CORE_CMD_PING:
        handle_ping_cmd(me, cmd);
        return;
```

Add these static functions before `result_from_esp_err()`:

```c
static void handle_ping_cmd(core_t *me, core_cmd_t *cmd)
{
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    if (!me || !cmd) {
        finish_service_cmd(me, cmd, CORE_CMD_RESULT_ERROR, &ret);
        return;
    }

    core_net_state_t net_state = CORE_NET_STATE_OFFLINE;
    ret = core_get_net_state(me, &net_state);
    if (ret == ESP_OK && net_state != CORE_NET_STATE_ONLINE) {
        ret = ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK) {
        finish_service_cmd(me, cmd, result_from_esp_err(ret), &ret);
        return;
    }

    modem_ping_reply_t *modem_replies = calloc(cmd->data.ping.count,
                                               sizeof(modem_ping_reply_t));
    if (!modem_replies) {
        ret = ESP_ERR_NO_MEM;
        finish_service_cmd(me, cmd, CORE_CMD_RESULT_ERROR, &ret);
        return;
    }

    modem_ping_summary_t modem_summary = {0};
    modem_ping_request_t request = {
        .host = cmd->data.ping.host,
        .count = cmd->data.ping.count,
        .data_len = cmd->data.ping.data_len,
        .timeout_100ms = cmd->data.ping.timeout_100ms,
        .ttl = cmd->data.ping.ttl,
    };

    ret = modem_ping(me->modem, &request, modem_replies,
                     cmd->data.ping.max_replies,
                     cmd->data.ping.summary ? &modem_summary : NULL);
    if (ret == ESP_OK) {
        copy_core_ping_replies(cmd->data.ping.replies, modem_replies,
                               cmd->data.ping.count);
        copy_core_ping_summary(cmd->data.ping.summary, &modem_summary);
    }

    free(modem_replies);
    finish_service_cmd(me, cmd, result_from_esp_err(ret), &ret);
}

static void copy_core_ping_replies(core_ping_reply_t *dst,
                                   const modem_ping_reply_t *src,
                                   size_t count)
{
    if (!dst || !src) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        dst[i].seq = src[i].seq;
        strlcpy(dst[i].ip, src[i].ip, sizeof(dst[i].ip));
        dst[i].time_ms = src[i].time_ms;
        dst[i].ttl = src[i].ttl;
        dst[i].success = src[i].success;
    }
}

static void copy_core_ping_summary(core_ping_summary_t *dst,
                                   const modem_ping_summary_t *src)
{
    if (!dst || !src) {
        return;
    }

    dst->sent = src->sent;
    dst->received = src->received;
    dst->lost = src->lost;
    dst->min_time_ms = src->min_time_ms;
    dst->max_time_ms = src->max_time_ms;
    dst->avg_time_ms = src->avg_time_ms;
}
```

Add `#include <string.h>` near the top of `src/core/core_fsm.c` because `strlcpy()` is used.

- [ ] **Step 4: Run contract test and inspect Core failures**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract.PingEndToEndContractTest.test_core_ping_command_boundary_matches_classes -v
```

Expected: PASS for Core boundary, or FAIL only because Modem types are not yet added. If it fails for Core tokens, fix Core before continuing.

---

### Task 4: Add Modem Ping Boundary

**Files:**
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_priv.h`
- Modify: `src/modem/modem.c`

- [ ] **Step 1: Add Modem ping value types and wrapper declaration**

In `src/modem/modem.h`, add these types after `modem_mqtt_publish_t`:

```c
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
} modem_ping_request_t;

typedef struct {
    uint8_t seq;
    char ip[48];
    uint32_t time_ms;
    uint8_t ttl;
    bool success;
} modem_ping_reply_t;

typedef struct {
    uint8_t sent;
    uint8_t received;
    uint8_t lost;
    uint32_t min_time_ms;
    uint32_t max_time_ms;
    uint32_t avg_time_ms;
} modem_ping_summary_t;
```

Add this declaration after `modem_mqtt_publish()`:

```c
esp_err_t modem_ping(modem_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary);
```

- [ ] **Step 2: Add ping to modem ops**

In `src/modem/modem_priv.h`, add this function pointer after `mqtt_publish`:

```c
    esp_err_t (*ping)(modem_t *me,
                      const modem_ping_request_t *request,
                      modem_ping_reply_t *replies,
                      size_t max_replies,
                      modem_ping_summary_t *summary);
```

- [ ] **Step 3: Implement the Modem wrapper**

In `src/modem/modem.c`, add this function after `modem_mqtt_publish()`:

```c
esp_err_t modem_ping(modem_t *me,
                     const modem_ping_request_t *request,
                     modem_ping_reply_t *replies,
                     size_t max_replies,
                     modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && request && request->host && request->host[0] &&
                        replies && request->count >= 1 && request->count <= 100 &&
                        request->data_len <= 1024 &&
                        request->timeout_100ms >= 1 &&
                        request->timeout_100ms <= 600 &&
                        request->ttl >= 1 &&
                        max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping request");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ping,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ping not supported");

    return me->ops->ping(me, request, replies, max_replies, summary);
}
```

- [ ] **Step 4: Run the Modem boundary contract**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract.PingEndToEndContractTest.test_modem_ping_boundary_matches_classes -v
```

Expected: PASS.

---

### Task 5: Implement Air780EP AT+CIPPING Mapping

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add Air780EP ping constants and prototypes**

Near existing Air780EP `#define` values, add:

```c
#define AIR780EP_CIPPING_PREFIX         "+CIPPING:"
#define AIR780EP_CIPPING_MAX_COUNT      100
#define AIR780EP_CIPPING_CMD_OVERHEAD_MS 5000U
```

After the MQTT static prototypes, add:

```c
static esp_err_t air780ep_ping(modem_t *me,
                               const modem_ping_request_t *request,
                               modem_ping_reply_t *replies,
                               size_t max_replies,
                               modem_ping_summary_t *summary);
static esp_err_t parse_cipping_line(const char *line,
                                    const modem_ping_request_t *request,
                                    modem_ping_reply_t *reply);
static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary);
static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request);
```

- [ ] **Step 2: Add ping op to Air780EP ops table**

In `s_air780ep_ops`, add:

```c
    .ping = air780ep_ping,
```

- [ ] **Step 3: Implement `air780ep_ping()`**

Add this function near MQTT command implementations, before `post_mqtt_data_event()`:

```c
static esp_err_t air780ep_ping(modem_t *me,
                               const modem_ping_request_t *request,
                               modem_ping_reply_t *replies,
                               size_t max_replies,
                               modem_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(me && request && request->host && request->host[0] &&
                        replies && max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char *host = escape_at_string(request->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape ping host failed");

    int needed = snprintf(NULL, 0, "AT+CIPPING=\"%s\",%u,%u,%u,%u",
                          host, (unsigned int)request->count,
                          (unsigned int)request->data_len,
                          (unsigned int)request->timeout_100ms,
                          (unsigned int)request->ttl);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }

    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+CIPPING=\"%s\",%u,%u,%u,%u",
             host, (unsigned int)request->count,
             (unsigned int)request->data_len,
             (unsigned int)request->timeout_100ms,
             (unsigned int)request->ttl);

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, ping_cmd_timeout_ms(request));
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+CIPPING");
    }
    if (ret != ESP_OK) {
        free(cmd);
        free(host);
        return ret;
    }

    size_t parsed_count = 0;
    int line_count = ctx.response.line_count;
    if (line_count > ctx.response.max_lines) {
        line_count = ctx.response.max_lines;
    }
    for (int i = 0; i < line_count && parsed_count < request->count; i++) {
        const char *line = ctx.response.lines[i];
        if (!line || strncmp(line, AIR780EP_CIPPING_PREFIX,
                            sizeof(AIR780EP_CIPPING_PREFIX) - 1U) != 0) {
            continue;
        }
        ret = parse_cipping_line(line, request, &replies[parsed_count]);
        if (ret != ESP_OK) {
            free(cmd);
            free(host);
            return ret;
        }
        parsed_count++;
    }

    if (parsed_count != request->count) {
        free(cmd);
        free(host);
        return ESP_ERR_INVALID_RESPONSE;
    }

    calculate_ping_summary(request, replies, parsed_count, summary);
    free(cmd);
    free(host);
    return ESP_OK;
}
```

- [ ] **Step 4: Implement parser, summary, and timeout helpers**

Add these helpers after `air780ep_ping()`:

```c
static esp_err_t parse_cipping_line(const char *line,
                                    const modem_ping_request_t *request,
                                    modem_ping_reply_t *reply)
{
    ESP_RETURN_ON_FALSE(line && request && reply, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    const char *value = skip_prefix_value(line, AIR780EP_CIPPING_PREFIX);
    ESP_RETURN_ON_FALSE(value, ESP_ERR_INVALID_RESPONSE, TAG,
                        "missing CIPPING prefix");

    errno = 0;
    char *end = NULL;
    long seq = strtol(value, &end, 10);
    ESP_RETURN_ON_FALSE(end != value && errno != ERANGE && seq >= 0 && seq <= UINT8_MAX,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid CIPPING seq");

    const char *cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE, TAG,
                        "missing CIPPING IP separator");
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }

    const char *ip_start = cursor;
    while (*cursor && *cursor != ',') {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE, TAG,
                        "missing CIPPING time separator");
    const char *ip_end = cursor;
    while (ip_end > ip_start && isspace((unsigned char)*(ip_end - 1))) {
        ip_end--;
    }
    if (ip_end > ip_start + 1 && *ip_start == '"' && *(ip_end - 1) == '"') {
        ip_start++;
        ip_end--;
    }
    size_t ip_len = (size_t)(ip_end - ip_start);
    ESP_RETURN_ON_FALSE(ip_len > 0 && ip_len < sizeof(reply->ip),
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid CIPPING IP");
    memcpy(reply->ip, ip_start, ip_len);
    reply->ip[ip_len] = '\0';

    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    errno = 0;
    long reply_time = strtol(cursor, &end, 10);
    ESP_RETURN_ON_FALSE(end != cursor && errno != ERANGE && reply_time >= 0,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid CIPPING time");
    cursor = end;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    ESP_RETURN_ON_FALSE(*cursor == ',', ESP_ERR_INVALID_RESPONSE, TAG,
                        "missing CIPPING ttl separator");
    cursor++;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    errno = 0;
    long ttl = strtol(cursor, &end, 10);
    ESP_RETURN_ON_FALSE(end != cursor && errno != ERANGE && ttl >= 0 && ttl <= UINT8_MAX,
                        ESP_ERR_INVALID_RESPONSE, TAG, "invalid CIPPING ttl");
    while (isspace((unsigned char)*end)) {
        end++;
    }
    ESP_RETURN_ON_FALSE(*end == '\0', ESP_ERR_INVALID_RESPONSE, TAG,
                        "trailing CIPPING data");

    const bool lost = reply_time == (uint32_t)request->timeout_100ms * 100U &&
                      ttl == 255;
    reply->seq = (uint8_t)seq;
    reply->time_ms = (uint32_t)reply_time;
    reply->ttl = (uint8_t)ttl;
    reply->success = !lost;
    return ESP_OK;
}

static void calculate_ping_summary(const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t reply_count,
                                   modem_ping_summary_t *summary)
{
    if (!request || !replies || !summary) {
        return;
    }

    summary->sent = request->count;
    summary->received = 0;
    summary->lost = 0;
    summary->min_time_ms = 0;
    summary->max_time_ms = 0;
    summary->avg_time_ms = 0;

    uint32_t total_time_ms = 0;
    for (size_t i = 0; i < reply_count; i++) {
        if (!replies[i].success) {
            continue;
        }
        if (summary->received == 0 || replies[i].time_ms < summary->min_time_ms) {
            summary->min_time_ms = replies[i].time_ms;
        }
        if (summary->received == 0 || replies[i].time_ms > summary->max_time_ms) {
            summary->max_time_ms = replies[i].time_ms;
        }
        total_time_ms += replies[i].time_ms;
        summary->received++;
    }

    summary->lost = summary->sent - summary->received;
    if (summary->received > 0) {
        summary->avg_time_ms = total_time_ms / summary->received;
    }
}

static uint32_t ping_cmd_timeout_ms(const modem_ping_request_t *request)
{
    if (!request) {
        return AIR780EP_DEFAULT_CMD_TIMEOUT_MS;
    }

    return ((uint32_t)request->count * (uint32_t)request->timeout_100ms * 100U) +
           AIR780EP_CIPPING_CMD_OVERHEAD_MS;
}
```

- [ ] **Step 5: Ensure AT response storage can hold max CIPPING replies**

Check `AIR780EP_MAX_RESPONSE_LINES` in `src/modem/modem_air780ep.c`. If it is still `8`, change it to at least `AIR780EP_CIPPING_MAX_COUNT`:

```c
#define AIR780EP_MAX_RESPONSE_LINES      100
```

If default AT Engine response pool still stores only 8 lines, update `AT_ENGINE_DEFAULT_MAX_RESP_LINES` in `src/at_engine/at_engine.c` to `100` and update `docs/agents/classes.md` if it documents the previous default. This is required because `AT+CIPPING` can return one line per ping request.

- [ ] **Step 6: Run Air780EP parser contract**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract.PingEndToEndContractTest.test_air780ep_cipping_mapping_and_parser -v
```

Expected: PASS.

---

### Task 6: Wire Facade And Factory

**Files:**
- Modify: `src/lwlte/lwlte_priv.h`
- Modify: `src/lwlte/lwlte.c`
- Modify: `src/lwlte/lwlte_air780ep.c`

- [ ] **Step 1: Add Ping Service to Facade private state**

In `src/lwlte/lwlte_priv.h`, add the include:

```c
#include "ping_client.h"
```

Add the member after `mqtt_client_t *mqtt;`:

```c
    ping_client_t *ping;
```

- [ ] **Step 2: Create Ping Service in Air780EP factory**

In `src/lwlte/lwlte_air780ep.c`, after `core_register_event_callback()` succeeds and before optional MQTT creation, add:

```c
    me->ping = ping_client_create(me->core);
    if (!me->ping) {
        ESP_LOGE(TAG, "create Ping client failed");
        return cleanup_after_failure(me, ESP_OK);
    }
```

- [ ] **Step 3: Destroy Ping Service before Core**

In `destroy_owned_resources()` in `src/lwlte/lwlte.c`, add this block after MQTT destroy and before Core destroy:

```c
    if (me->ping) {
        ret = ping_client_destroy(me->ping);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy Ping client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->ping = NULL;
    }
```

- [ ] **Step 4: Implement `lwlte_ping()` with explicit mapping**

Add this function after `lwlte_get_net_state()` and before MQTT functions in `src/lwlte/lwlte.c`:

```c
esp_err_t lwlte_ping(lwlte_t *me,
                     const lwlte_ping_request_t *request,
                     lwlte_ping_reply_t *replies,
                     size_t max_replies,
                     lwlte_ping_summary_t *summary)
{
    ESP_RETURN_ON_FALSE(request && replies && request->host && request->host[0] &&
                        request->count >= 1 && request->count <= 100 &&
                        request->data_len <= 1024 &&
                        request->timeout_100ms >= 1 &&
                        request->timeout_100ms <= 600 &&
                        request->ttl >= 1 &&
                        max_replies >= request->count,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ping request");

    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    ping_client_t *ping = me->ping;
    xSemaphoreGive(me->lock);
    if (!ping) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    core_ping_reply_t *core_replies = calloc(request->count,
                                             sizeof(core_ping_reply_t));
    if (!core_replies) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_ping_summary_t core_summary = {0};
    ping_client_request_t ping_request = {
        .host = request->host,
        .count = request->count,
        .data_len = request->data_len,
        .timeout_100ms = request->timeout_100ms,
        .ttl = request->ttl,
        .total_timeout_ms = request->total_timeout_ms,
    };

    ret = ping_client_ping(ping, &ping_request, core_replies, request->count,
                           summary ? &core_summary : NULL);
    if (ret == ESP_OK) {
        for (size_t i = 0; i < request->count; i++) {
            replies[i].seq = core_replies[i].seq;
            strlcpy(replies[i].ip, core_replies[i].ip, sizeof(replies[i].ip));
            replies[i].time_ms = core_replies[i].time_ms;
            replies[i].ttl = core_replies[i].ttl;
            replies[i].success = core_replies[i].success;
        }
        if (summary) {
            summary->sent = core_summary.sent;
            summary->received = core_summary.received;
            summary->lost = core_summary.lost;
            summary->min_time_ms = core_summary.min_time_ms;
            summary->max_time_ms = core_summary.max_time_ms;
            summary->avg_time_ms = core_summary.avg_time_ms;
        }
    }

    free(core_replies);
    end_api_call(me);
    return ret;
}
```

Add `#include <string.h>` near the top of `src/lwlte/lwlte.c` because `strlcpy()` is used.

- [ ] **Step 5: Run Facade wiring contract**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_end_to_end_contract.PingEndToEndContractTest.test_facade_wires_ping_and_maps_types_without_casting -v
```

Expected: PASS.

---

### Task 7: Keep Documentation And Static Contracts Aligned

**Files:**
- Modify if needed: `docs/agents/classes.md`
- Modify if needed: `tests/host/test_ping_classes_doc_contract.py`
- Modify if needed: `tests/host/test_ping_end_to_end_contract.py`

- [ ] **Step 1: Compare source contracts against `classes.md`**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_classes_doc_contract tests.host.test_ping_end_to_end_contract -v
```

Expected: `test_ping_classes_doc_contract` PASS and `test_ping_end_to_end_contract` PASS except for any exact-token mismatch caused by legitimate implementation naming. If a legitimate mismatch exists, update the docs and tests together.

- [ ] **Step 2: Update `classes.md` only for real source/doc drift**

If source code adds a field that is necessary for safety, such as carrying a total timeout through the Modem request boundary, update all relevant `classes.md` sections:

```markdown
typedef struct {
    const char *host;
    uint8_t count;
    uint16_t data_len;
    uint16_t timeout_100ms;
    uint8_t ttl;
    uint32_t total_timeout_ms;
} modem_ping_request_t;
```

Only make this edit if the source actually carries that field. Otherwise leave `classes.md` unchanged.

- [ ] **Step 3: Re-run docs/static tests**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_classes_doc_contract tests.host.test_ping_end_to_end_contract tests.host.test_mqtt_end_to_end_contract tests.host.test_net_mgr_activation_flow -v
```

Expected: PASS for all tests.

---

### Task 8: Build And Final Verification

**Files:**
- All implementation files above

- [ ] **Step 1: Run whitespace check**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 2: Run host static regression suite**

Run:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest tests.host.test_ping_classes_doc_contract tests.host.test_ping_end_to_end_contract tests.host.test_mqtt_end_to_end_contract tests.host.test_net_mgr_activation_flow tests.host.test_air780ep_cpin_policy -v
```

Expected: PASS.

- [ ] **Step 3: Build ESP-IDF project**

Use the ESP-IDF MCP build tool first:

```text
esp-idf-eim_build_project
```

Expected: build succeeds.

If MCP build is unavailable or fails before invoking ESP-IDF, run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: build succeeds. If build fails, fix the compile error before continuing.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
git status --short
git diff -- src/include/lwlte.h src/ping_client/ping_client.h src/ping_client/ping_client_priv.h src/ping_client/ping_client.c src/core/core.h src/core/core.c src/core/core_fsm.c src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c src/modem/modem_air780ep.c src/lwlte/lwlte_priv.h src/lwlte/lwlte.c src/lwlte/lwlte_air780ep.c src/CMakeLists.txt docs/agents/classes.md tests/host/test_ping_end_to_end_contract.py
```

Expected: diff contains only Ping implementation, necessary source/doc alignment, CMake registration, and tests.

- [ ] **Step 5: Commit if explicitly requested for this execution**

If commit approval is present, run:

```bash
git add src/include/lwlte.h src/ping_client/ping_client.h src/ping_client/ping_client_priv.h src/ping_client/ping_client.c src/core/core.h src/core/core.c src/core/core_fsm.c src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c src/modem/modem_air780ep.c src/lwlte/lwlte_priv.h src/lwlte/lwlte.c src/lwlte/lwlte_air780ep.c src/CMakeLists.txt docs/agents/classes.md tests/host/test_ping_end_to_end_contract.py
git commit -m "feat: add synchronous ping service"
```

Expected: commit succeeds and includes only intended Ping implementation files.

---

## Self-Review

- Spec coverage: The plan covers public `lwlte_ping()`, Ping Service, Core `CORE_CMD_PING`, Modem `modem_ping()`, Air780EP `AT+CIPPING`, validation, caller-owned reply buffers, no Ping FSM/task/queue, boundary restrictions, static tests, and build verification.
- Placeholder scan: No incomplete markers, open implementation gaps, or vague edge-case instructions remain. The only conditional step is the explicit docs/source drift rule, scoped to actual implementation alignment.
- Type consistency: Public `lwlte_ping_*`, internal `ping_client_request_t`, Core `core_ping_*`, and Modem `modem_ping_*` names match `docs/agents/classes.md`. The plan explicitly forbids reply pointer casts and requires field-by-field copies.
