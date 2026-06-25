# Example Event Name Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update all Air780EP and ML307R examples so event logs include both numeric IDs and symbolic event/state names in `id(NAME)` format.

**Architecture:** Add one example-only event-name helper module and use it from the six example programs. Host static tests lock the helper mappings, build wiring, and log format before C code changes are made. Hardware verification builds, flashes, and monitors each example separately.

**Tech Stack:** C, ESP-IDF, Python `unittest`, ESP-IDF MCP tools, `docs/agents/serial_monitor.py`.

---

## File Structure

- Create `example/example_event_names.h`: public helper prototypes for example code only.
- Create `example/example_event_names.c`: switch-based enum-to-name mappings for public lwLTE event/state enums.
- Modify `example/CMakeLists.txt`: add `example_event_names.c` to example component sources.
- Modify six examples to include `example_event_names.h` and log event/state fields as `id(NAME)`:
  - `example/air780ep_basic_connect.c`
  - `example/air780ep_mqtt_client.c`
  - `example/air780ep_tcp_client.c`
  - `example/ml307r_basic_connect.c`
  - `example/ml307r_mqtt_client.c`
  - `example/ml307r_tcp_client.c`
- Create `tests/host/test_example_event_name_logging_contract.py`: static contract for helper mappings, CMake wiring, and example log usage.
- Temporarily modify `example/main.c` during hardware verification to select each example. Leave it on the last verified example unless the user requests a different final selection.

No commit is included in required tasks. Repository policy requires explicit user authorization before every `git commit`.

### Task 1: Add Failing Static Contract

**Files:**
- Create: `tests/host/test_example_event_name_logging_contract.py`

- [ ] **Step 1: Create the host contract test file**

Create `tests/host/test_example_event_name_logging_contract.py` with this content:

```python
#!/usr/bin/env python3
"""Static checks for human-readable example event logs."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = ROOT / "example"
HELPER_H = EXAMPLE_DIR / "example_event_names.h"
HELPER_C = EXAMPLE_DIR / "example_event_names.c"
CMAKE = EXAMPLE_DIR / "CMakeLists.txt"

EXAMPLE_FILES = [
    EXAMPLE_DIR / "air780ep_basic_connect.c",
    EXAMPLE_DIR / "air780ep_mqtt_client.c",
    EXAMPLE_DIR / "air780ep_tcp_client.c",
    EXAMPLE_DIR / "ml307r_basic_connect.c",
    EXAMPLE_DIR / "ml307r_mqtt_client.c",
    EXAMPLE_DIR / "ml307r_tcp_client.c",
]


def read_optional(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class ExampleEventNameLoggingContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper_h = read_optional(HELPER_H)
        cls.helper_c = read_optional(HELPER_C)
        cls.cmake = read_optional(CMAKE)
        cls.examples = {path.name: read_optional(path) for path in EXAMPLE_FILES}

    def assert_contains_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_helper_header_declares_name_functions(self):
        self.assertTrue(HELPER_H.exists(), "missing example_event_names.h")
        self.assert_contains_all(self.helper_h, [
            "#include \"lwlte.h\"",
            "const char *example_lwlte_event_name(lwlte_event_id_t id);",
            "const char *example_lwlte_net_state_name(lwlte_net_state_t state);",
            "const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id);",
            "const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id);",
            "const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state);",
        ], "example_event_names.h")

    def test_helper_source_maps_public_event_and_state_values(self):
        self.assertTrue(HELPER_C.exists(), "missing example_event_names.c")
        for token in [
            "LWLTE_EVENT_STARTED", "return \"STARTED\";",
            "LWLTE_EVENT_READY", "return \"READY\";",
            "LWLTE_EVENT_NET_CONNECTING", "return \"NET_CONNECTING\";",
            "LWLTE_EVENT_NET_ONLINE", "return \"NET_ONLINE\";",
            "LWLTE_EVENT_NET_OFFLINE", "return \"NET_OFFLINE\";",
            "LWLTE_EVENT_NET_ERROR", "return \"NET_ERROR\";",
            "LWLTE_EVENT_STOPPED", "return \"STOPPED\";",
            "LWLTE_EVENT_ERROR", "return \"ERROR\";",
            "LWLTE_NET_STATE_OFFLINE", "return \"OFFLINE\";",
            "LWLTE_NET_STATE_ACTIVATING", "return \"ACTIVATING\";",
            "LWLTE_NET_STATE_ONLINE", "return \"ONLINE\";",
            "LWLTE_NET_STATE_ERROR", "return \"ERROR\";",
            "LWLTE_MQTT_EVENT_STARTED", "return \"STARTED\";",
            "LWLTE_MQTT_EVENT_STOPPED", "return \"STOPPED\";",
            "LWLTE_MQTT_EVENT_CONNECTING", "return \"CONNECTING\";",
            "LWLTE_MQTT_EVENT_CONNECTED", "return \"CONNECTED\";",
            "LWLTE_MQTT_EVENT_DISCONNECTED", "return \"DISCONNECTED\";",
            "LWLTE_MQTT_EVENT_SUBSCRIBED", "return \"SUBSCRIBED\";",
            "LWLTE_MQTT_EVENT_UNSUBSCRIBED", "return \"UNSUBSCRIBED\";",
            "LWLTE_MQTT_EVENT_PUBLISHED", "return \"PUBLISHED\";",
            "LWLTE_MQTT_EVENT_DATA", "return \"DATA\";",
            "LWLTE_MQTT_EVENT_ERROR", "return \"ERROR\";",
            "LWLTE_TCP_EVENT_STARTED", "return \"STARTED\";",
            "LWLTE_TCP_EVENT_STOPPED", "return \"STOPPED\";",
            "LWLTE_TCP_EVENT_CONNECTED", "return \"CONNECTED\";",
            "LWLTE_TCP_EVENT_DISCONNECTED", "return \"DISCONNECTED\";",
            "LWLTE_TCP_EVENT_SENT", "return \"SENT\";",
            "LWLTE_TCP_EVENT_DATA", "return \"DATA\";",
            "LWLTE_TCP_EVENT_ERROR", "return \"ERROR\";",
            "LWLTE_TCP_CONN_STATE_CREATED", "return \"CREATED\";",
            "LWLTE_TCP_CONN_STATE_CONNECTING", "return \"CONNECTING\";",
            "LWLTE_TCP_CONN_STATE_CONNECTED", "return \"CONNECTED\";",
            "LWLTE_TCP_CONN_STATE_CLOSING", "return \"CLOSING\";",
            "LWLTE_TCP_CONN_STATE_CLOSED", "return \"CLOSED\";",
            "LWLTE_TCP_CONN_STATE_ERROR", "return \"ERROR\";",
            "return \"UNKNOWN\";",
        ]:
            self.assertIn(token, self.helper_c, token)

    def test_cmake_builds_helper_source(self):
        self.assertIn('"example_event_names.c"', self.cmake)

    def test_lte_event_logs_include_event_and_net_state_names(self):
        for name in [
            "air780ep_basic_connect.c",
            "ml307r_basic_connect.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"LTE event=%d(%s) net=%d(%s) err=%d"', source, name)
            self.assertIn("example_lwlte_event_name((lwlte_event_id_t)event_id)", source, name)
            self.assertIn("example_lwlte_net_state_name(data ? data->net_state : (lwlte_net_state_t)-1)", source, name)
            self.assertNotIn('"LTE event=%d net=%d err=%d"', source, name)

    def test_mqtt_examples_include_lte_and_mqtt_event_names(self):
        for name in [
            "air780ep_mqtt_client.c",
            "ml307r_mqtt_client.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"LTE event=%d(%s)"', source, name)
            self.assertIn("example_lwlte_event_name((lwlte_event_id_t)event_id)", source, name)
            self.assertIn('"MQTT event=%d(%s)"', source, name)
            self.assertIn("example_lwlte_mqtt_event_name((lwlte_mqtt_event_id_t)event_id)", source, name)
            self.assertNotIn('"LTE event=%d"', source, name)
            self.assertNotIn('"MQTT event=%d"', source, name)

    def test_tcp_examples_include_tcp_event_and_state_names(self):
        for name in [
            "air780ep_tcp_client.c",
            "ml307r_tcp_client.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"TCP event=%d(%s) state=%d(%s) err=%d modem=%d reason=%d"', source, name)
            self.assertIn("example_lwlte_tcp_event_name((lwlte_tcp_event_id_t)event_id)", source, name)
            self.assertIn("example_lwlte_tcp_conn_state_name(data ? data->conn_state : (lwlte_tcp_conn_state_t)-1)", source, name)
            self.assertNotIn('"TCP event=%d state=%d err=%d modem=%d reason=%d"', source, name)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the contract and verify it fails**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
```

Expected: FAIL because `example/example_event_names.h`, `example/example_event_names.c`, CMake wiring, and named event logs do not exist yet.

### Task 2: Add Shared Example Event Name Helper

**Files:**
- Create: `example/example_event_names.h`
- Create: `example/example_event_names.c`
- Modify: `example/CMakeLists.txt`
- Test: `tests/host/test_example_event_name_logging_contract.py`

- [ ] **Step 1: Create the helper header**

Create `example/example_event_names.h` with this content:

```c
/**
 * @file example_event_names.h
 * @brief 示例事件名称辅助函数
 * @details Example event name helper functions
 * @author JovisDreams
 * @date 2026-06-25
 */
#ifndef EXAMPLE_EVENT_NAMES_H
#define EXAMPLE_EVENT_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lwlte.h"

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 获取 LTE 事件名称
 * @details Get LTE event name
 * @param[in] id LTE 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_event_name(lwlte_event_id_t id);

/**
 * @brief 获取 LTE 网络状态名称
 * @details Get LTE network state name
 * @param[in] state LTE 网络状态
 * @return 字符串常量名称
 */
const char *example_lwlte_net_state_name(lwlte_net_state_t state);

/**
 * @brief 获取 MQTT 事件名称
 * @details Get MQTT event name
 * @param[in] id MQTT 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id);

/**
 * @brief 获取 TCP 事件名称
 * @details Get TCP event name
 * @param[in] id TCP 事件 ID
 * @return 字符串常量名称
 */
const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id);

/**
 * @brief 获取 TCP 连接状态名称
 * @details Get TCP connection state name
 * @param[in] state TCP 连接状态
 * @return 字符串常量名称
 */
const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EXAMPLE_EVENT_NAMES_H */
```

- [ ] **Step 2: Create the helper source**

Create `example/example_event_names.c` with this content:

```c
/**
 * @file example_event_names.c
 * @brief 示例事件名称辅助函数
 * @details Example event name helper functions
 * @author JovisDreams
 * @date 2026-06-25
 */

/*********************
 *      INCLUDES
 *********************/
#include "example_event_names.h"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
const char *example_lwlte_event_name(lwlte_event_id_t id)
{
    switch (id) {
    case LWLTE_EVENT_STARTED:
        return "STARTED";
    case LWLTE_EVENT_READY:
        return "READY";
    case LWLTE_EVENT_NET_CONNECTING:
        return "NET_CONNECTING";
    case LWLTE_EVENT_NET_ONLINE:
        return "NET_ONLINE";
    case LWLTE_EVENT_NET_OFFLINE:
        return "NET_OFFLINE";
    case LWLTE_EVENT_NET_ERROR:
        return "NET_ERROR";
    case LWLTE_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_net_state_name(lwlte_net_state_t state)
{
    switch (state) {
    case LWLTE_NET_STATE_OFFLINE:
        return "OFFLINE";
    case LWLTE_NET_STATE_ACTIVATING:
        return "ACTIVATING";
    case LWLTE_NET_STATE_ONLINE:
        return "ONLINE";
    case LWLTE_NET_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id)
{
    switch (id) {
    case LWLTE_MQTT_EVENT_STARTED:
        return "STARTED";
    case LWLTE_MQTT_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_MQTT_EVENT_CONNECTING:
        return "CONNECTING";
    case LWLTE_MQTT_EVENT_CONNECTED:
        return "CONNECTED";
    case LWLTE_MQTT_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    case LWLTE_MQTT_EVENT_SUBSCRIBED:
        return "SUBSCRIBED";
    case LWLTE_MQTT_EVENT_UNSUBSCRIBED:
        return "UNSUBSCRIBED";
    case LWLTE_MQTT_EVENT_PUBLISHED:
        return "PUBLISHED";
    case LWLTE_MQTT_EVENT_DATA:
        return "DATA";
    case LWLTE_MQTT_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id)
{
    switch (id) {
    case LWLTE_TCP_EVENT_STARTED:
        return "STARTED";
    case LWLTE_TCP_EVENT_STOPPED:
        return "STOPPED";
    case LWLTE_TCP_EVENT_CONNECTED:
        return "CONNECTED";
    case LWLTE_TCP_EVENT_DISCONNECTED:
        return "DISCONNECTED";
    case LWLTE_TCP_EVENT_SENT:
        return "SENT";
    case LWLTE_TCP_EVENT_DATA:
        return "DATA";
    case LWLTE_TCP_EVENT_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state)
{
    switch (state) {
    case LWLTE_TCP_CONN_STATE_CREATED:
        return "CREATED";
    case LWLTE_TCP_CONN_STATE_CONNECTING:
        return "CONNECTING";
    case LWLTE_TCP_CONN_STATE_CONNECTED:
        return "CONNECTED";
    case LWLTE_TCP_CONN_STATE_CLOSING:
        return "CLOSING";
    case LWLTE_TCP_CONN_STATE_CLOSED:
        return "CLOSED";
    case LWLTE_TCP_CONN_STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
```

- [ ] **Step 3: Add helper source to example component**

In `example/CMakeLists.txt`, change the `SRCS` block from:

```cmake
idf_component_register(
    SRCS "main.c"
         "air780ep_basic_connect.c"
```

to:

```cmake
idf_component_register(
    SRCS "main.c"
         "example_event_names.c"
         "air780ep_basic_connect.c"
```

- [ ] **Step 4: Run the contract and verify remaining failures**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
```

Expected: FAIL only for the six examples not yet including `example_event_names.h` and not yet using named event log formats.

### Task 3: Update Basic Connect Event Logs

**Files:**
- Modify: `example/air780ep_basic_connect.c`
- Modify: `example/ml307r_basic_connect.c`
- Test: `tests/host/test_example_event_name_logging_contract.py`

- [ ] **Step 1: Include the helper header in both basic examples**

In `example/air780ep_basic_connect.c` and `example/ml307r_basic_connect.c`, add:

```c
#include "example_event_names.h"
```

immediately before:

```c
#include "lwlte.h"
```

- [ ] **Step 2: Update Air780EP basic LTE event log**

In `example/air780ep_basic_connect.c`, replace:

```c
    ESP_LOGI(TAG, "LTE event=%d net=%d err=%d", (int)event_id,
             data ? (int)data->net_state : -1,
             data ? data->error_code : 0);
```

with:

```c
    ESP_LOGI(TAG, "LTE event=%d(%s) net=%d(%s) err=%d", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id),
             data ? (int)data->net_state : -1,
             example_lwlte_net_state_name(data ? data->net_state : (lwlte_net_state_t)-1),
             data ? data->error_code : 0);
```

- [ ] **Step 3: Update ML307R basic LTE event log**

In `example/ml307r_basic_connect.c`, replace:

```c
    ESP_LOGI(TAG, "LTE event=%d net=%d err=%d", (int)event_id,
             data ? (int)data->net_state : -1,
             data ? data->error_code : 0);
```

with:

```c
    ESP_LOGI(TAG, "LTE event=%d(%s) net=%d(%s) err=%d", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id),
             data ? (int)data->net_state : -1,
             example_lwlte_net_state_name(data ? data->net_state : (lwlte_net_state_t)-1),
             data ? data->error_code : 0);
```

- [ ] **Step 4: Run the contract and verify remaining failures**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
```

Expected: FAIL only for MQTT and TCP examples not yet using named event log formats.

### Task 4: Update MQTT Example Event Logs

**Files:**
- Modify: `example/air780ep_mqtt_client.c`
- Modify: `example/ml307r_mqtt_client.c`
- Test: `tests/host/test_example_event_name_logging_contract.py`

- [ ] **Step 1: Include the helper header in both MQTT examples**

In `example/air780ep_mqtt_client.c` and `example/ml307r_mqtt_client.c`, add:

```c
#include "example_event_names.h"
```

immediately before:

```c
#include "lwlte.h"
```

- [ ] **Step 2: Update Air780EP MQTT LTE event log**

In `example/air780ep_mqtt_client.c`, replace:

```c
    ESP_LOGI(TAG, "LTE event=%d", (int)event_id);
```

with:

```c
    ESP_LOGI(TAG, "LTE event=%d(%s)", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id));
```

- [ ] **Step 3: Update Air780EP MQTT event log**

In `example/air780ep_mqtt_client.c`, replace:

```c
    ESP_LOGI(TAG, "MQTT event=%d", (int)event_id);
```

with:

```c
    ESP_LOGI(TAG, "MQTT event=%d(%s)", (int)event_id,
             example_lwlte_mqtt_event_name((lwlte_mqtt_event_id_t)event_id));
```

- [ ] **Step 4: Update ML307R MQTT LTE event log**

In `example/ml307r_mqtt_client.c`, replace:

```c
    ESP_LOGI(TAG, "LTE event=%d", (int)event_id);
```

with:

```c
    ESP_LOGI(TAG, "LTE event=%d(%s)", (int)event_id,
             example_lwlte_event_name((lwlte_event_id_t)event_id));
```

- [ ] **Step 5: Update ML307R MQTT event log**

In `example/ml307r_mqtt_client.c`, replace:

```c
    ESP_LOGI(TAG, "MQTT event=%d", (int)event_id);
```

with:

```c
    ESP_LOGI(TAG, "MQTT event=%d(%s)", (int)event_id,
             example_lwlte_mqtt_event_name((lwlte_mqtt_event_id_t)event_id));
```

- [ ] **Step 6: Run the contract and verify remaining failures**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
```

Expected: FAIL only for TCP examples not yet using named event/state log formats.

### Task 5: Update TCP Example Event Logs

**Files:**
- Modify: `example/air780ep_tcp_client.c`
- Modify: `example/ml307r_tcp_client.c`
- Test: `tests/host/test_example_event_name_logging_contract.py`

- [ ] **Step 1: Include the helper header in both TCP examples**

In `example/air780ep_tcp_client.c` and `example/ml307r_tcp_client.c`, add:

```c
#include "example_event_names.h"
```

immediately before:

```c
#include "lwlte.h"
```

- [ ] **Step 2: Update Air780EP TCP event log**

In `example/air780ep_tcp_client.c`, replace:

```c
    ESP_LOGI(TAG, "TCP event=%d state=%d err=%d modem=%d reason=%d",
             (int)event_id,
             data ? (int)data->conn_state : -1,
             data ? data->error_code : 0,
             data ? data->modem_error_code : 0,
             data ? data->reason : 0);
```

with:

```c
    ESP_LOGI(TAG, "TCP event=%d(%s) state=%d(%s) err=%d modem=%d reason=%d",
             (int)event_id,
             example_lwlte_tcp_event_name((lwlte_tcp_event_id_t)event_id),
             data ? (int)data->conn_state : -1,
             example_lwlte_tcp_conn_state_name(data ? data->conn_state : (lwlte_tcp_conn_state_t)-1),
             data ? data->error_code : 0,
             data ? data->modem_error_code : 0,
             data ? data->reason : 0);
```

- [ ] **Step 3: Update ML307R TCP event log**

In `example/ml307r_tcp_client.c`, replace:

```c
    ESP_LOGI(TAG, "TCP event=%d state=%d err=%d modem=%d reason=%d",
             (int)event_id,
             data ? (int)data->conn_state : -1,
             data ? data->error_code : 0,
             data ? data->modem_error_code : 0,
             data ? data->reason : 0);
```

with:

```c
    ESP_LOGI(TAG, "TCP event=%d(%s) state=%d(%s) err=%d modem=%d reason=%d",
             (int)event_id,
             example_lwlte_tcp_event_name((lwlte_tcp_event_id_t)event_id),
             data ? (int)data->conn_state : -1,
             example_lwlte_tcp_conn_state_name(data ? data->conn_state : (lwlte_tcp_conn_state_t)-1),
             data ? data->error_code : 0,
             data ? data->modem_error_code : 0,
             data ? data->reason : 0);
```

- [ ] **Step 4: Run the contract and verify it passes**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
```

Expected: OK.

### Task 6: Host Regression And Build Checks

**Files:**
- Verify only: `tests/host/test_example_event_name_logging_contract.py`
- Verify only: `tests/host/test_ml307r_examples_contract.py`
- Verify only: `tests/host/test_tcp_client_end_to_end_contract.py`

- [ ] **Step 1: Run focused host tests**

Run:

```bash
python3 -m unittest tests/host/test_example_event_name_logging_contract.py -v
python3 -m unittest tests/host/test_ml307r_examples_contract.py -v
python3 -m unittest tests/host/test_tcp_client_end_to_end_contract.py -v
```

Expected: all commands end with `OK`.

- [ ] **Step 2: Run whitespace validation**

Run:

```bash
git diff --check
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Set ESP-IDF target and build once**

Use MCP tools:

```text
esp-idf-eim_set_target target=esp32c3
esp-idf-eim_build_project
```

Expected: target selection succeeds and build succeeds.

### Task 7: Hardware Verification For All Six Examples

**Files:**
- Modify during verification: `example/main.c`
- Verify examples:
  - `EXAMPLE_AIR780EP_BASIC_CONNECT`
  - `EXAMPLE_AIR780EP_MQTT_CLIENT`
  - `EXAMPLE_AIR780EP_TCP_CLIENT`
  - `EXAMPLE_ML307R_BASIC_CONNECT`
  - `EXAMPLE_ML307R_MQTT_CLIENT`
  - `EXAMPLE_ML307R_TCP_CLIENT`

- [ ] **Step 1: Verify Air780EP basic connect**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 60 --port /dev/cu.usbserial-1130
```

Expected: log contains `Air780EP basic connect example`, at least one `LTE event=<number>(<name>) net=<number>(<name>) err=`, reaches network online, and prints `ping summary`.

- [ ] **Step 2: Verify Air780EP MQTT client**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_MQTT_CLIENT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 75 --port /dev/cu.usbserial-1130
```

Expected: log contains `Air780EP MQTT client example`, `LTE event=<number>(<name>)`, `MQTT event=<number>(<name>)`, MQTT connected/subscribed, and at least one `telemetry published`.

- [ ] **Step 3: Verify Air780EP TCP client**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_TCP_CLIENT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 75 --port /dev/cu.usbserial-1130
```

Expected: log contains `Air780EP TCP client example`, `TCP event=<number>(<name>) state=<number>(<name>)`, TCP open succeeds, payload is sent, payload is received, and connection closes.

- [ ] **Step 4: Verify ML307R basic connect**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_BASIC_CONNECT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 60 --port /dev/cu.usbserial-1130
```

Expected: log contains `ML307R basic connect example`, at least one `LTE event=<number>(<name>) net=<number>(<name>) err=`, reaches network online, and prints `ping summary`.

- [ ] **Step 5: Verify ML307R MQTT client**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_MQTT_CLIENT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 75 --port /dev/cu.usbserial-1130
```

Expected: log contains `ML307R MQTT client example`, `LTE event=<number>(<name>)`, `MQTT event=<number>(<name>)`, MQTT connected/subscribed, and at least one `telemetry published`.

- [ ] **Step 6: Verify ML307R TCP client**

Set `example/main.c` to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_TCP_CLIENT
```

Use MCP tools:

```text
esp-idf-eim_build_project
esp-idf-eim_flash_project port=/dev/cu.usbserial-1130
```

Capture serial output:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && python3 docs/agents/serial_monitor.py --timeout 75 --port /dev/cu.usbserial-1130
```

Expected: log contains `ML307R TCP client example`, `TCP event=<number>(<name>) state=<number>(<name>)`, TCP open succeeds, payload is sent, payload is received, and connection closes.

- [ ] **Step 7: Record final example selection**

Read `example/main.c` and report the final selected example. If the user did not request another final selection, leave it as the last verified example:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_TCP_CLIENT
```

### Task 8: Final Review

**Files:**
- Review all changed files from this plan.

- [ ] **Step 1: Inspect final diff**

Run:

```bash
git diff -- example/example_event_names.h example/example_event_names.c example/CMakeLists.txt example/air780ep_basic_connect.c example/air780ep_mqtt_client.c example/air780ep_tcp_client.c example/ml307r_basic_connect.c example/ml307r_mqtt_client.c example/ml307r_tcp_client.c tests/host/test_example_event_name_logging_contract.py example/main.c
```

Expected: diff contains only event-name helper, named example logs, the host contract, CMake wiring, and the final `example/main.c` selection.

- [ ] **Step 2: Summarize verification evidence**

Report each example with build, flash, monitor result, and the observed named event log snippets. Do not claim hardware success for any example whose serial log did not show the expected example banner and named event logs.
