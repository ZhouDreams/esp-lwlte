# MQTT Client Example Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `examples/mqtt_client` — a ThingsBoard MQTT client example over LTE using esp-lwlte public APIs.

**Architecture:** Follows `examples/basic_connect` structure exactly. A single `main.c` drives LTE init → MQTT connect → subscribe RPC/attribute topics → periodic telemetry publish → RPC response handling. MQTT parameters come from `Kconfig.projbuild`.

**Tech Stack:** ESP-IDF (C), esp-lwlte component (`lwlte.h` + `lwlte_air780ep.h`), Air780EP LTE module.

---

### Task 1: Create directory structure and CMake files

**Files:**
- Create: `examples/mqtt_client/CMakeLists.txt`
- Create: `examples/mqtt_client/main/CMakeLists.txt`

- [ ] **Step 1: Create example root CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LISTDIR}/../../src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mqtt_client)
```

- [ ] **Step 2: Create main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
```

- [ ] **Step 3: Verify directory structure**

Run: `ls -R examples/mqtt_client/`
Expected: `CMakeLists.txt` and `main/CMakeLists.txt` present.

---

### Task 2: Create Kconfig.projbuild

**Files:**
- Create: `examples/mqtt_client/Kconfig.projbuild`

- [ ] **Step 1: Write Kconfig.projbuild**

```kconfig
menu "Example MQTT Settings"

config EXAMPLE_MQTT_HOST
    string "MQTT broker host"
    default "iot.jovisdreams.site"

config EXAMPLE_MQTT_PORT
    int "MQTT broker port"
    range 1 65535
    default 1883

config EXAMPLE_MQTT_CLIENT_ID
    string "MQTT client ID"
    default "esp-lwlte-mqtt-example"

config EXAMPLE_MQTT_TOKEN
    string "ThingsBoard device access token (used as MQTT username)"
    default ""

config EXAMPLE_MQTT_KEEPALIVE_S
    int "MQTT keepalive seconds"
    range 10 1200
    default 120

endmenu
```

- [ ] **Step 2: Commit**

```bash
git add examples/mqtt_client/CMakeLists.txt examples/mqtt_client/main/CMakeLists.txt examples/mqtt_client/Kconfig.projbuild
git commit -m "feat(examples): add mqtt_client skeleton with Kconfig"
```

---

### Task 3: Create sdkconfig.defaults

**Files:**
- Create: `examples/mqtt_client/sdkconfig.defaults`

- [ ] **Step 1: Write sdkconfig.defaults**

```
CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

Note: Stack is 6144 (vs basic_connect's 4096) to accommodate MQTT event handling and snprintf for telemetry JSON.

---

### Task 4: Create main.c

**Files:**
- Create: `examples/mqtt_client/main/main.c`
- Reference: `examples/basic_connect/main/main.c` (pattern to follow)
- Reference: `src/include/lwlte.h` (public API)
- Reference: `src/include/lwlte_air780ep.h` (Air780EP config)

- [ ] **Step 1: Write main.c with full implementation**

The file structure follows the coding-style template exactly. Key sections:

**INCLUDES:** `lwlte.h`, `lwlte_air780ep.h`, `esp_log.h`, `freertos/FreeRTOS.h`, `freertos/task.h`, `driver/gpio.h`, `driver/uart.h`, `stdio.h`, `string.h`, `stdlib.h`

**DEFINES:**
- `TAG "appmain"`
- UART/GPIO/EN/APN/CID defines identical to basic_connect
- `EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS 10000`
- `EXAMPLE_NET_ONLINE_TIMEOUT_MS 120000`
- `EXAMPLE_MQTT_CONNECT_TIMEOUT_MS 30000`
- `EXAMPLE_TELEMETRY_INTERVAL_MS 5000`
- `EXAMPLE_POLL_INTERVAL_MS 100`
- ThingsBoard topic strings: `TB_TOPIC_TELEMETRY`, `TB_TOPIC_RPC_REQUEST_PREFIX`, `TB_TOPIC_RPC_RESPONSE_PREFIX`, `TB_TOPIC_ATTRIBUTES`
- `EXAMPLE_TELEMETRY_BUF_LEN 128`
- `EXAMPLE_RPC_TOPIC_BUF_LEN 96`

**STATIC PROTOTYPES:**
- `lte_event_cb()` — handles all LTE/MQTT events
- `log_lte_status()` — same as basic_connect
- `cleanup_lte()` — same as basic_connect
- `idle_forever()` — same as basic_connect
- `lte_state_name()` — same as basic_connect
- `net_state_name()` — same as basic_connect
- `lte_event_name()` — extended with MQTT event names
- `mqtt_state_name()` — MQTT state to string
- `handle_rpc_request()` — extract request_id from topic, publish response
- `publish_telemetry()` — format and publish telemetry JSON

**STATIC VARIABLES:**
- `s_net_online` (volatile bool)
- `s_net_error` (volatile bool)
- `s_net_error_code` (volatile int)
- `s_mqtt_connected` (volatile bool)
- `s_rpc_subscribed` (volatile bool)
- `s_attr_subscribed` (volatile bool)
- `s_counter` (volatile uint32_t)

**GLOBAL FUNCTIONS:**
- `app_main()`:
  1. Build `lwlte_air780ep_config_t` with UART/GPIO settings and `mqtt_client` fields from Kconfig:
     ```c
     .mqtt_client = {
         .enabled = true,
         .host = CONFIG_EXAMPLE_MQTT_HOST,
         .port = CONFIG_EXAMPLE_MQTT_PORT,
         .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
         .username = CONFIG_EXAMPLE_MQTT_TOKEN[0] ? CONFIG_EXAMPLE_MQTT_TOKEN : NULL,
         .password = NULL,
         .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
         .clean_session = true,
     },
     ```
  2. `lwlte_air780ep_init()`
  3. `lwlte_register_event_callback()`
  4. `lwlte_connect()`
  5. Poll-wait for `s_net_online` (timeout `EXAMPLE_NET_ONLINE_TIMEOUT_MS`)
  6. `lwlte_mqtt_start()`
  7. Poll-wait for `s_mqtt_connected` (timeout `EXAMPLE_MQTT_CONNECT_TIMEOUT_MS`)
  8. `lwlte_mqtt_subscribe(lte, "v1/devices/me/rpc/request/+", 0)`
  9. `lwlte_mqtt_subscribe(lte, "v1/devices/me/attributes", 0)`
  10. Poll-wait for `s_rpc_subscribed && s_attr_subscribed` (timeout `EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS`)
  11. Loop: every `EXAMPLE_TELEMETRY_INTERVAL_MS`, call `publish_telemetry(lte)`, check `s_mqtt_connected`

**STATIC FUNCTIONS:**

`lte_event_cb()`:
```c
switch (event_id) {
case LWLTE_EVENT_NET_CONNECTING:
    s_net_error_code = 0; s_net_error = false; s_net_online = false; break;
case LWLTE_EVENT_NET_ONLINE:
    s_net_error_code = 0; s_net_error = false; s_net_online = true; break;
case LWLTE_EVENT_NET_OFFLINE:
    s_net_online = false; s_mqtt_connected = false; break;
case LWLTE_EVENT_NET_ERROR:
case LWLTE_EVENT_ERROR:
    s_net_error_code = data ? data->error_code : ESP_FAIL;
    s_net_online = false; s_net_error = true; s_mqtt_connected = false; break;
case LWLTE_EVENT_MQTT_CONNECTED:
    s_mqtt_connected = true; break;
case LWLTE_EVENT_MQTT_DISCONNECTED:
    s_mqtt_connected = false; break;
case LWLTE_EVENT_MQTT_SUBSCRIBED:
    // Increment a simple subscription counter or check data for topic match
    // Use a static int s_subscribe_count++ approach
    break;
case LWLTE_EVENT_MQTT_DATA:
    if (data) handle_rpc_request(lte, &data->data.mqtt_msg);
    break;
default: break;
}
```

`handle_rpc_request()`:
```c
static void handle_rpc_request(lwlte_t *lte, const lwlte_mqtt_msg_t *msg)
{
    if (!msg || !msg->topic || msg->topic_len == 0) return;

    // Check if topic starts with TB_TOPIC_RPC_REQUEST_PREFIX
    const char *prefix = "v1/devices/me/rpc/request/";
    size_t prefix_len = strlen(prefix);
    if (msg->topic_len <= prefix_len || strncmp(msg->topic, prefix, prefix_len) != 0) return;

    // Extract request_id from topic suffix
    const char *id_start = msg->topic + prefix_len;
    size_t id_len = msg->topic_len - prefix_len;
    char id_buf[16] = {0};
    if (id_len == 0 || id_len >= sizeof(id_buf)) return;
    memcpy(id_buf, id_start, id_len);

    // Build response topic
    char resp_topic[96] = {0};
    int written = snprintf(resp_topic, sizeof(resp_topic), "v1/devices/me/rpc/response/%s", id_buf);
    if (written <= 0 || (size_t)written >= sizeof(resp_topic)) return;

    // Publish response
    const char *resp_payload = "{\"status\":\"ok\"}";
    esp_err_t ret = lwlte_mqtt_publish(lte, resp_topic,
        (const uint8_t *)resp_payload, strlen(resp_payload), 0, false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RPC response sent to %s", resp_topic);
    } else {
        ESP_LOGW(TAG, "RPC response failed: %s", esp_err_to_name(ret));
    }
}
```

`publish_telemetry()`:
```c
static void publish_telemetry(lwlte_t *lte)
{
    char buf[EXAMPLE_TELEMETRY_BUF_LEN] = {0};
    int written = snprintf(buf, sizeof(buf), "{\"temperature\":25.5,\"counter\":%lu}",
                           (unsigned long)s_counter);
    if (written <= 0 || (size_t)written >= sizeof(buf)) return;

    esp_err_t ret = lwlte_mqtt_publish(lte, "v1/devices/me/telemetry",
        (const uint8_t *)buf, (size_t)written, 0, false);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "telemetry published: %s", buf);
        s_counter++;
    } else {
        ESP_LOGW(TAG, "telemetry publish failed: %s", esp_err_to_name(ret));
    }
}
```

`lte_event_name()` — extended from basic_connect with MQTT events:
```c
case LWLTE_EVENT_MQTT_STARTED: return "MQTT_STARTED";
case LWLTE_EVENT_MQTT_STOPPED: return "MQTT_STOPPED";
case LWLTE_EVENT_MQTT_CONNECTING: return "MQTT_CONNECTING";
case LWLTE_EVENT_MQTT_CONNECTED: return "MQTT_CONNECTED";
case LWLTE_EVENT_MQTT_DISCONNECTED: return "MQTT_DISCONNECTED";
case LWLTE_EVENT_MQTT_SUBSCRIBED: return "MQTT_SUBSCRIBED";
case LWLTE_EVENT_MQTT_UNSUBSCRIBED: return "MQTT_UNSUBSCRIBED";
case LWLTE_EVENT_MQTT_PUBLISHED: return "MQTT_PUBLISHED";
case LWLTE_EVENT_MQTT_DATA: return "MQTT_DATA";
case LWLTE_EVENT_MQTT_ERROR: return "MQTT_ERROR";
```

`mqtt_state_name()`:
```c
case LWLTE_MQTT_STATE_STOPPED: return "STOPPED";
case LWLTE_MQTT_STATE_WAITING_NET: return "WAITING_NET";
case LWLTE_MQTT_STATE_CONNECTING: return "CONNECTING";
case LWLTE_MQTT_STATE_CONNECTED: return "CONNECTED";
case LWLTE_MQTT_STATE_DISCONNECTING: return "DISCONNECTING";
case LWLTE_MQTT_STATE_ERROR: return "ERROR";
```

Helper functions `log_lte_status()`, `cleanup_lte()`, `idle_forever()`, `lte_state_name()`, `net_state_name()` — identical to basic_connect.

- [ ] **Step 2: Commit**

```bash
git add examples/mqtt_client/sdkconfig.defaults examples/mqtt_client/main/main.c
git commit -m "feat(examples): add mqtt_client example with ThingsBoard interaction"
```

---

### Task 5: Build verification

- [ ] **Step 1: Set target and build**

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py -C examples/mqtt_client set-target esp32c3
idf.py -C examples/mqtt_client build
```

Expected: Build succeeds with no errors.

- [ ] **Step 2: Fix any build issues if needed**

If build fails, fix the reported errors and rebuild. Common issues:
- Missing includes
- Wrong type for `s_counter` format specifier
- Config macro names mismatch

---

### Task 6: Create README.md

**Files:**
- Create: `examples/mqtt_client/README.md`

- [ ] **Step 1: Write README.md following basic_connect README pattern**

Content should cover:
- What the example does (LTE + MQTT + ThingsBoard telemetry + RPC)
- Hardware wiring (same as basic_connect)
- Kconfig parameters (broker host, port, token, etc.)
- Build/flash commands
- Expected logs for successful run
- Troubleshooting (MQTT-specific: wrong token, broker unreachable, TLS vs plain)

- [ ] **Step 2: Final commit**

```bash
git add examples/mqtt_client/README.md
git commit -m "docs(examples): add mqtt_client README"
```
