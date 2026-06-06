# Air780EP Example Slimming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the existing basic connect and MQTT examples as Air780EP examples, then slim them to the smallest useful flows with Chinese comments.

**Architecture:** Keep the unified `example/main.c` selector and replace the two long Air780EP example implementations with focused files. The basic example demonstrates Air780EP init, async LTE start, online wait, and one ping; the MQTT example demonstrates Air780EP init, async LTE start, MQTT start, one downlink subscription, telemetry publishing, and received-data logging.

**Tech Stack:** ESP-IDF, C, FreeRTOS, esp-lwlte public facade API, Air780EP modem facade, ThingsBoard MQTT topics.

**Repository Rule:** Do not create commits unless the user explicitly asks. End tasks with verification and `git status --short` instead of committing.

---

## File Structure

- Create `example/air780ep_basic_connect.c`: slim Air780EP basic connection example with one ping.
- Create `example/air780ep_mqtt_client.c`: slim Air780EP MQTT example with publish and subscribe receive logging.
- Delete `example/basic_connect.c`: old generic-name Air780EP basic example.
- Delete `example/mqtt_client.c`: old generic-name Air780EP MQTT example.
- Modify `example/example.h`: replace generic selection macros and run declarations with Air780EP-specific names.
- Modify `example/main.c`: select `EXAMPLE_AIR780EP_BASIC_CONNECT` by default and dispatch new run functions.
- Modify `example/CMakeLists.txt`: build the renamed source files.
- Modify `example/README.md`: document the new Air780EP-specific example names and simplified flows.
- Modify `docs/agents/directory-structure.md`: keep the active repository guide aligned with the renamed example files.

## Task 1: Replace Basic Connect With Slim Air780EP Example

**Files:**
- Create: `example/air780ep_basic_connect.c`
- Delete later in Task 3: `example/basic_connect.c`

- [ ] **Step 1: Run the red static check for current generic naming**

Run:

```bash
rg "example_basic_connect_run|EXAMPLE_BASIC_CONNECT|basic_connect.c" example
```

Expected: matches in `example/example.h`, `example/main.c`, `example/CMakeLists.txt`, `example/README.md`, and `example/basic_connect.c`. This confirms the old generic basic-connect naming still exists before the change.

- [ ] **Step 2: Create the slim Air780EP basic example**

Use `apply_patch` to add `example/air780ep_basic_connect.c` with this complete content:

```c
/**
 * @file air780ep_basic_connect.c
 * @brief Air780EP LTE 基础连接示例
 * @details Air780EP LTE basic connection example
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                  "air780ep_basic"

#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1

#define EXAMPLE_MODEM_RESET_PULSE_MS         500
#define EXAMPLE_INIT_READY_TIMEOUT_MS        30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS        120000
#define EXAMPLE_POLL_INTERVAL_MS             100
#define EXAMPLE_IDLE_DELAY_MS                1000

#define EXAMPLE_PING_HOST                    "8.8.8.8"
#define EXAMPLE_PING_COUNT                   4

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief LTE 事件回调
 * @details LTE event callback
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据
 * @param[in] user_ctx 用户上下文
 */
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx);

/**
 * @brief 执行一次 Ping 测试
 * @details Run one Ping test
 * @param[in] lte LTE 用户门面句柄
 */
static void do_ping(lwlte_t *lte);

/**
 * @brief 进入常驻等待
 * @details Enter forever delay loop
 */
static void idle_forever(void);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool s_net_online;
static volatile bool s_net_error;
static volatile int s_last_error;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void example_air780ep_basic_connect_run(void)
{
    s_net_online = false;
    s_net_error = false;
    s_last_error = 0;

    lwlte_t *lte = NULL;
    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
    };

    ESP_LOGI(TAG, "Air780EP basic connect example");
    ESP_LOGI(TAG, "UART%d TX=%d RX=%d baud=%d EN=%d APN='%s'",
             EXAMPLE_LTE_UART_NUM, EXAMPLE_LTE_UART_TX_PIN,
             EXAMPLE_LTE_UART_RX_PIN, EXAMPLE_LTE_UART_BAUD_RATE,
             EXAMPLE_LTE_EN_PIN, EXAMPLE_LTE_APN);

    /* 创建 Air780EP 门面：这里只填写必填字段和启动相关超时。 */
    esp_err_t ret = lwlte_air780ep_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init Air780EP failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    /* 注册事件回调：联网结果会异步从回调里返回。 */
    ret = lwlte_register_event_callback(lte, lte_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register callback failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 启动异步联网；ESP_OK 只表示请求已经提交。 */
    ret = lwlte_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 简单示例用轮询等待事件回调设置 online 标志。 */
    uint32_t elapsed_ms = 0;
    while (!s_net_online && !s_net_error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }

    if (!s_net_online) {
        ESP_LOGW(TAG, "network wait ended: error=%d code=%d",
                 (int)s_net_error, s_last_error);
        idle_forever();
    }

    ESP_LOGI(TAG, "Air780EP network is online");
    do_ping(lte);
    idle_forever();
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx)
{
    (void)lte;
    (void)user_ctx;

    ESP_LOGI(TAG, "LTE event=%d net=%d err=%d", (int)event_id,
             data ? (int)data->net_state : -1,
             data ? data->error_code : 0);

    switch (event_id) {
    case LWLTE_EVENT_NET_CONNECTING:
        s_net_online = false;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_ONLINE:
        s_net_online = true;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        s_net_online = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_online = false;
        s_net_error = true;
        s_last_error = data ? data->error_code : ESP_FAIL;
        break;
    default:
        break;
    }
}

static void do_ping(lwlte_t *lte)
{
    const lwlte_ping_request_t req = {
        .host = EXAMPLE_PING_HOST,
        .count = EXAMPLE_PING_COUNT,
        .data_len = 64,
        .timeout_100ms = 10,
        .ttl = 64,
        .total_timeout_ms = 0,
    };
    lwlte_ping_reply_t replies[EXAMPLE_PING_COUNT] = {0};
    lwlte_ping_summary_t summary = {0};

    ESP_LOGI(TAG, "ping %s count=%u", req.host, (unsigned int)req.count);
    esp_err_t ret = lwlte_ping(lte, &req, replies, req.count, &summary);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ping failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "ping summary: sent=%u recv=%u lost=%u min=%lums max=%lums avg=%lums",
             (unsigned int)summary.sent, (unsigned int)summary.received,
             (unsigned int)summary.lost, (unsigned long)summary.min_time_ms,
             (unsigned long)summary.max_time_ms,
             (unsigned long)summary.avg_time_ms);
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}
```

- [ ] **Step 3: Verify the new basic example contains the intended core flow**

Run:

```bash
rg "example_air780ep_basic_connect_run|lwlte_air780ep_init|lwlte_start|lwlte_ping" example/air780ep_basic_connect.c
```

Expected: all four names are present.

Run:

```bash
rg "lte_state_name|net_state_name|lte_event_name|log_lte_status|cleanup_lte" example/air780ep_basic_connect.c
```

Expected: no output. These were the redundant helpers removed from the simple example.

## Task 2: Replace MQTT Client With Slim Air780EP Publish/Subscribe Example

**Files:**
- Create: `example/air780ep_mqtt_client.c`
- Delete later in Task 3: `example/mqtt_client.c`

- [ ] **Step 1: Run the red static check for current MQTT redundancy**

Run:

```bash
rg "handle_rpc_request|TB_TOPIC_RPC|mqtt_state_name|example_mqtt_client_run" example/mqtt_client.c
```

Expected: matches in the old `example/mqtt_client.c`. This confirms the old RPC-heavy MQTT example is still present before replacement.

- [ ] **Step 2: Create the slim Air780EP MQTT example**

Use `apply_patch` to add `example/air780ep_mqtt_client.c` with this complete content:

```c
/**
 * @file air780ep_mqtt_client.c
 * @brief Air780EP LTE MQTT 客户端示例
 * @details Air780EP LTE MQTT client example
 * @author JovisDreams
 * @date 2026-06-06
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                                      "air780ep_mqtt"

#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1

#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_INIT_READY_TIMEOUT_MS            30000
#define EXAMPLE_NET_ONLINE_TIMEOUT_MS            120000
#define EXAMPLE_MQTT_CONNECT_TIMEOUT_MS          30000
#define EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS        10000
#define EXAMPLE_POLL_INTERVAL_MS                 100
#define EXAMPLE_IDLE_DELAY_MS                    1000
#define EXAMPLE_TELEMETRY_INTERVAL_MS            5000
#define EXAMPLE_TELEMETRY_BUF_LEN                96

#define TB_TOPIC_TELEMETRY                       "v1/devices/me/telemetry"
#define TB_TOPIC_ATTRIBUTES                      "v1/devices/me/attributes"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief LTE 与 MQTT 事件回调
 * @details LTE and MQTT event callback
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据
 * @param[in] user_ctx 用户上下文
 */
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx);

/**
 * @brief 发布一条 telemetry 数据
 * @details Publish one telemetry message
 * @param[in] lte LTE 用户门面句柄
 */
static void publish_telemetry(lwlte_t *lte);

/**
 * @brief 进入常驻等待
 * @details Enter forever delay loop
 */
static void idle_forever(void);

/**********************
 *  STATIC VARIABLES
 **********************/
static volatile bool s_net_online;
static volatile bool s_net_error;
static volatile bool s_mqtt_connected;
static volatile bool s_mqtt_subscribed;
static volatile int s_last_error;
static volatile uint32_t s_counter;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void example_air780ep_mqtt_client_run(void)
{
    s_net_online = false;
    s_net_error = false;
    s_mqtt_connected = false;
    s_mqtt_subscribed = false;
    s_last_error = 0;
    s_counter = 0;

    lwlte_t *lte = NULL;
    const char *mqtt_username = CONFIG_EXAMPLE_MQTT_TOKEN;
    if (mqtt_username[0] == '\0') {
        mqtt_username = NULL;
    }

    const lwlte_air780ep_config_t config = {
        .uart_num = EXAMPLE_LTE_UART_NUM,
        .uart_tx_pin = EXAMPLE_LTE_UART_TX_PIN,
        .uart_rx_pin = EXAMPLE_LTE_UART_RX_PIN,
        .uart_baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
        .en_pin = EXAMPLE_LTE_EN_PIN,
        .apn = EXAMPLE_LTE_APN,
        .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
        .init_ready_timeout_ms = EXAMPLE_INIT_READY_TIMEOUT_MS,
        .modem_reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
        .mqtt_client = {
            .enabled = true,
            .host = CONFIG_EXAMPLE_MQTT_HOST,
            .port = CONFIG_EXAMPLE_MQTT_PORT,
            .client_id = CONFIG_EXAMPLE_MQTT_CLIENT_ID,
            .username = mqtt_username,
            .password = NULL,
            .keepalive_s = CONFIG_EXAMPLE_MQTT_KEEPALIVE_S,
            .clean_session = true,
        },
    };

    ESP_LOGI(TAG, "Air780EP MQTT client example");
    ESP_LOGI(TAG, "MQTT host=%s port=%d client_id=%s",
             CONFIG_EXAMPLE_MQTT_HOST, CONFIG_EXAMPLE_MQTT_PORT,
             CONFIG_EXAMPLE_MQTT_CLIENT_ID);

    /* 创建启用 MQTT 的 Air780EP 门面。 */
    esp_err_t ret = lwlte_air780ep_init(&config, &lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init Air780EP failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    /* 注册事件回调：网络、MQTT 连接和下行数据都会从这里返回。 */
    ret = lwlte_register_event_callback(lte, lte_event_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register callback failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    /* 先启动 LTE 联网，MQTT 会在网络 online 后再启动。 */
    ret = lwlte_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start LTE failed: %s", esp_err_to_name(ret));
        (void)lwlte_destroy(lte);
        idle_forever();
    }

    uint32_t elapsed_ms = 0;
    while (!s_net_online && !s_net_error &&
           elapsed_ms < EXAMPLE_NET_ONLINE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }
    if (!s_net_online) {
        ESP_LOGW(TAG, "network wait ended: error=%d code=%d",
                 (int)s_net_error, s_last_error);
        idle_forever();
    }

    /* 网络 online 后启动 MQTT 客户端。 */
    ret = lwlte_mqtt_start(lte);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    elapsed_ms = 0;
    while (!s_mqtt_connected && elapsed_ms < EXAMPLE_MQTT_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT did not connect within timeout");
        idle_forever();
    }

    /* 订阅 ThingsBoard attributes topic，用于验证下行接收。 */
    ret = lwlte_mqtt_subscribe(lte, TB_TOPIC_ATTRIBUTES, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "subscribe attributes failed: %s", esp_err_to_name(ret));
        idle_forever();
    }

    elapsed_ms = 0;
    while (!s_mqtt_subscribed && elapsed_ms < EXAMPLE_MQTT_SUBSCRIBE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_INTERVAL_MS));
        elapsed_ms += EXAMPLE_POLL_INTERVAL_MS;
    }
    if (!s_mqtt_subscribed) {
        ESP_LOGW(TAG, "attributes subscribe not confirmed within timeout");
        idle_forever();
    }

    while (1) {
        if (s_mqtt_connected) {
            publish_telemetry(lte);
        } else {
            ESP_LOGW(TAG, "MQTT disconnected, skip telemetry");
        }
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_TELEMETRY_INTERVAL_MS));
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void lte_event_cb(lwlte_t *lte, lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data, void *user_ctx)
{
    (void)lte;
    (void)user_ctx;

    ESP_LOGI(TAG, "LTE event=%d net=%d mqtt=%d err=%d", (int)event_id,
             data ? (int)data->net_state : -1,
             data ? (int)data->mqtt_state : -1,
             data ? data->error_code : 0);

    switch (event_id) {
    case LWLTE_EVENT_NET_CONNECTING:
        s_net_online = false;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_ONLINE:
        s_net_online = true;
        s_net_error = false;
        s_last_error = 0;
        break;
    case LWLTE_EVENT_NET_OFFLINE:
        s_net_online = false;
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    case LWLTE_EVENT_NET_ERROR:
    case LWLTE_EVENT_ERROR:
        s_net_online = false;
        s_net_error = true;
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        s_last_error = data ? data->error_code : ESP_FAIL;
        break;
    case LWLTE_EVENT_MQTT_CONNECTED:
        s_mqtt_connected = true;
        break;
    case LWLTE_EVENT_MQTT_DISCONNECTED:
        s_mqtt_connected = false;
        s_mqtt_subscribed = false;
        break;
    case LWLTE_EVENT_MQTT_SUBSCRIBED:
        s_mqtt_subscribed = true;
        ESP_LOGI(TAG, "MQTT subscribed: %s", TB_TOPIC_ATTRIBUTES);
        break;
    case LWLTE_EVENT_MQTT_DATA:
        if (data) {
            const lwlte_mqtt_msg_t *msg = &data->data.mqtt_msg;
            const char *topic = msg->topic ? msg->topic : "";
            const char *payload = msg->payload ? (const char *)msg->payload : "";
            size_t topic_len = msg->topic ? msg->topic_len : 0;
            size_t payload_len = msg->payload ? msg->payload_len : 0;
            ESP_LOGI(TAG, "MQTT RX topic=%.*s payload=%.*s",
                     (int)topic_len, topic, (int)payload_len, payload);
        }
        break;
    default:
        break;
    }
}

static void publish_telemetry(lwlte_t *lte)
{
    char payload[EXAMPLE_TELEMETRY_BUF_LEN] = {0};
    int len = snprintf(payload, sizeof(payload),
                       "{\"temperature\":25.5,\"counter\":%lu}",
                       (unsigned long)s_counter);
    if (len <= 0 || (size_t)len >= sizeof(payload)) {
        ESP_LOGW(TAG, "telemetry payload too long");
        return;
    }

    esp_err_t ret = lwlte_mqtt_publish(lte, TB_TOPIC_TELEMETRY,
                                       (const uint8_t *)payload,
                                       (size_t)len, 0, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "telemetry publish failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "telemetry published: %s", payload);
    s_counter++;
}

static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}
```

- [ ] **Step 3: Verify the new MQTT example contains publish and subscribe but no RPC response flow**

Run:

```bash
rg "example_air780ep_mqtt_client_run|lwlte_mqtt_start|lwlte_mqtt_subscribe|lwlte_mqtt_publish|LWLTE_EVENT_MQTT_DATA" example/air780ep_mqtt_client.c
```

Expected: all five names are present.

Run:

```bash
rg "handle_rpc_request|TB_TOPIC_RPC|RPC response|mqtt_state_name|lte_state_name|net_state_name" example/air780ep_mqtt_client.c
```

Expected: no output. The MQTT example still subscribes for downlink data but no longer implements RPC replies or large state-name helper switches.

## Task 3: Update Example Selector And Build Wiring

**Files:**
- Modify: `example/example.h`
- Modify: `example/main.c`
- Modify: `example/CMakeLists.txt`
- Delete: `example/basic_connect.c`
- Delete: `example/mqtt_client.c`

- [ ] **Step 1: Replace the example selector header**

Use `apply_patch` to replace `example/example.h` with this complete content:

```c
#ifndef EXAMPLE_H
#define EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_AIR780EP_BASIC_CONNECT  1
#define EXAMPLE_AIR780EP_MQTT_CLIENT    2
#define EXAMPLE_ML307R_PROBE            3

void example_air780ep_basic_connect_run(void);
void example_air780ep_mqtt_client_run(void);
void example_ml307r_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_H */
```

- [ ] **Step 2: Replace the unified example entry**

Use `apply_patch` to replace `example/main.c` with this complete content:

```c
/**
 * @file main.c
 * @brief 统一示例入口
 * @details Unified example entry.
 */

/*********************
 *      INCLUDES
 *********************/
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                 "example"

#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void idle_forever(void);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    switch (EXAMPLE_SELECTED) {
    case EXAMPLE_AIR780EP_BASIC_CONNECT:
        example_air780ep_basic_connect_run();
        break;
    case EXAMPLE_AIR780EP_MQTT_CLIENT:
        example_air780ep_mqtt_client_run();
        break;
    case EXAMPLE_ML307R_PROBE:
        example_ml307r_probe_run();
        break;
    default:
        ESP_LOGE(TAG, "invalid EXAMPLE_SELECTED=%d", EXAMPLE_SELECTED);
        idle_forever();
        break;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 3: Replace the example component source list**

Use `apply_patch` to replace `example/CMakeLists.txt` with this complete content:

```cmake
idf_component_register(
    SRCS "main.c"
         "air780ep_basic_connect.c"
         "air780ep_mqtt_client.c"
         "ml307r_probe.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
```

- [ ] **Step 4: Delete the old generic Air780EP example files**

Use `apply_patch` to delete these files:

```text
example/basic_connect.c
example/mqtt_client.c
```

- [ ] **Step 5: Verify selector and build wiring use the new names**

Run:

```bash
rg "EXAMPLE_AIR780EP_BASIC_CONNECT|EXAMPLE_AIR780EP_MQTT_CLIENT|example_air780ep_basic_connect_run|example_air780ep_mqtt_client_run|air780ep_basic_connect.c|air780ep_mqtt_client.c" example/example.h example/main.c example/CMakeLists.txt
```

Expected: matches for all new names.

Run:

```bash
rg "EXAMPLE_BASIC_CONNECT|EXAMPLE_MQTT_CLIENT|example_basic_connect_run|example_mqtt_client_run|\"basic_connect\.c\"|\"mqtt_client\.c\"" example/example.h example/main.c example/CMakeLists.txt
```

Expected: no output.

## Task 4: Update Example README

**Files:**
- Modify: `example/README.md`

- [ ] **Step 1: Replace README content with Air780EP-specific example names**

Use `apply_patch` to replace `example/README.md` with this complete content:

```markdown
# Unified Examples

All examples build from the repository root. Select the example to run by editing `EXAMPLE_SELECTED` in `example/main.c`:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT
```

Available selections:

| Value | Description |
|-------|-------------|
| `EXAMPLE_AIR780EP_BASIC_CONNECT` | Air780EP LTE basic connect and ping example |
| `EXAMPLE_AIR780EP_MQTT_CLIENT` | Air780EP ThingsBoard MQTT publish/subscribe example |
| `EXAMPLE_ML307R_PROBE` | ML307R raw UART probe example |

## Build

From the repository root:

```bash
idf.py set-target esp32c3
idf.py build
```

## Flash And Monitor

Replace `/dev/cu.usbserial-XXXX` with the board serial port:

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

In a non-interactive agent environment, use the repository helper after flashing:

```bash
python3 docs/agents/serial_monitor.py --timeout 30 --port /dev/cu.usbserial-XXXX
```

## Air780EP Wiring

Default wiring targets the ESP32-C3 Pro DevKit setup used during development.

| ESP32-C3 | Air780EP | Notes |
|----------|----------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN | Controlled by modem adapter |
| GND | GND | Common ground required |

The Air780EP EN pin is level-controlled. High keeps the module running; low powers it down. The modem adapter toggles EN low then high during start/reset, polls `AT` until `OK`, and then sends basic AT initialization commands.

## Air780EP Basic Connect

`EXAMPLE_AIR780EP_BASIC_CONNECT` demonstrates the minimum useful esp-lwlte Air780EP flow:

1. Create an Air780EP LWLTE facade over UART.
2. Register LTE facade events.
3. Start LTE network activation asynchronously.
4. Wait for `LWLTE_EVENT_NET_ONLINE`.
5. Run one ping after the network is online.

Expected logs include:

```text
Air780EP basic connect example
LTE event=2 net=1 err=0
LTE event=3 net=2 err=0
Air780EP network is online
ping summary: sent=4 recv=4 lost=0 min=... max=... avg=...
```

## Air780EP MQTT Client

`EXAMPLE_AIR780EP_MQTT_CLIENT` demonstrates a ThingsBoard MQTT client over Air780EP LTE.

Configure MQTT settings with `idf.py menuconfig` under **Example MQTT Settings**:

| Setting | Default | Description |
|---------|---------|-------------|
| MQTT broker host | `admin.jovisdreams.site` | ThingsBoard server hostname |
| MQTT broker port | `1883` | Plain TCP MQTT port |
| MQTT client ID | `esp-lwlte-mqtt-example` | Device client identifier |
| ThingsBoard device access token | `Air780EP` | Used as MQTT username |
| MQTT keepalive seconds | `120` | Keepalive interval |

Topics used by the example:

| Direction | Topic | Purpose |
|-----------|-------|---------|
| Publish | `v1/devices/me/telemetry` | Periodic telemetry data |
| Subscribe | `v1/devices/me/attributes` | Downlink/shared attribute updates from ThingsBoard |

The MQTT example intentionally keeps downlink handling simple: received MQTT data is printed in the event callback. It does not implement RPC response logic.

## ML307R UART Probe

`EXAMPLE_ML307R_PROBE` probes an ML307R module over UART without using esp-lwlte library initialization.

Default wiring:

| ESP32-C3 | ML307R | Notes |
|----------|--------|-------|
| GPIO0 | RX | ESP32-C3 UART1 TX |
| GPIO1 | TX | ESP32-C3 UART1 RX |
| GPIO2 | EN or power enable | Held high by this example |
| GND | GND | Common ground required |

Probe flow:

1. Configure `UART_NUM_1` at `115200` baud.
2. Drive GPIO2 high.
3. Listen for startup UART data for 3 seconds.
4. Send `ATE0` and print raw response bytes.
5. Send `AT` up to 3 times and print raw response bytes.
6. Stay in an idle loop and keep printing unsolicited UART data.
```

- [ ] **Step 2: Verify README no longer documents generic Air780EP example names**

Run:

```bash
rg "EXAMPLE_BASIC_CONNECT|EXAMPLE_MQTT_CLIENT|Basic Connect\||MQTT Client\|" example/README.md
```

Expected: no output.

Run:

```bash
rg "EXAMPLE_AIR780EP_BASIC_CONNECT|EXAMPLE_AIR780EP_MQTT_CLIENT|Air780EP Basic Connect|Air780EP MQTT Client" example/README.md
```

Expected: matches for the new Air780EP-specific names and headings.

## Task 5: Verify Static Consistency And Build

**Files:**
- Verify: `example/`
- Verify: `docs/superpowers/specs/2026-06-06-air780ep-example-slimming-design.md`

- [ ] **Step 1: Run final old-name static check**

Run:

```bash
rg "\bEXAMPLE_BASIC_CONNECT\b|\bEXAMPLE_MQTT_CLIENT\b|example_basic_connect_run|example_mqtt_client_run|\"basic_connect\.c\"|\"mqtt_client\.c\"" example
```

Expected: no output.

- [ ] **Step 2: Run final new-name static check**

Run:

```bash
rg "EXAMPLE_AIR780EP_BASIC_CONNECT|EXAMPLE_AIR780EP_MQTT_CLIENT|example_air780ep_basic_connect_run|example_air780ep_mqtt_client_run|air780ep_basic_connect.c|air780ep_mqtt_client.c" example
```

Expected: matches in `example/example.h`, `example/main.c`, `example/CMakeLists.txt`, and `example/README.md`.

- [ ] **Step 3: Confirm the slim examples still use the Air780EP facade**

Run:

```bash
rg "lwlte_air780ep_init" example/air780ep_basic_connect.c example/air780ep_mqtt_client.c
```

Expected: one match in each new Air780EP example file.

- [ ] **Step 4: Confirm the MQTT example demonstrates both publish and receive**

Run:

```bash
rg "lwlte_mqtt_publish|lwlte_mqtt_subscribe|LWLTE_EVENT_MQTT_DATA|MQTT RX" example/air780ep_mqtt_client.c
```

Expected: matches for publish, subscribe, MQTT data event handling, and receive logging.

- [ ] **Step 5: Check patch formatting**

Run:

```bash
git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 6: Build with ESP-IDF MCP**

Run the ESP-IDF MCP build tool:

```text
esp-idf-eim_build_project
```

Expected: project builds successfully for the current target. If the target is unset or wrong, run the ESP-IDF MCP target tool first with `esp32c3`, then build again:

```text
esp-idf-eim_set_target target=esp32c3
esp-idf-eim_build_project
```

- [ ] **Step 7: Inspect final working tree status**

Run:

```bash
git status --short
```

Expected: only the planned files are changed:

```text
 M example/CMakeLists.txt
 M example/README.md
 M example/example.h
 M example/main.c
 D example/basic_connect.c
 D example/mqtt_client.c
?? docs/superpowers/plans/2026-06-06-air780ep-example-slimming.md
?? docs/superpowers/specs/2026-06-06-air780ep-example-slimming-design.md
?? example/air780ep_basic_connect.c
?? example/air780ep_mqtt_client.c
```

The exact order may differ. Do not commit unless the user explicitly requests it.

## Task 6: Refresh Active Agent Directory Guide

**Files:**
- Modify: `docs/agents/directory-structure.md`

- [ ] **Step 1: Update example file-name references**

Use `apply_patch` to replace the example implementation sentence with:

```markdown
示例实现按文件拆分，例如 `air780ep_basic_connect.c`、`air780ep_mqtt_client.c`、`ml307r_probe.c`。新增示例时应在 `example/example.h` 中新增选择宏和 run 函数声明，并在 `example/main.c` 的选择逻辑中接入。
```

- [ ] **Step 2: Verify active example docs no longer cite old source names**

Run:

```bash
rg "`basic_connect\.c`|`mqtt_client\.c`" docs/agents example/README.md
```

Expected: no output.

- [ ] **Step 3: Verify final status includes the guide update**

Run:

```bash
git status --short
```

Expected: includes `M docs/agents/directory-structure.md` along with the planned example and superpowers doc changes.

## Self-Review Notes

- Spec coverage: Air780EP-specific naming is covered by Tasks 3, 4, and 6; slim basic flow is covered by Task 1; MQTT publish/subscribe receive is covered by Task 2; Chinese code comments are included in both new example files; build/static verification is covered by Task 5.
- Placeholder scan: no placeholder markers or deferred implementation language is used.
- Type consistency: new macros and functions use `EXAMPLE_AIR780EP_BASIC_CONNECT`, `EXAMPLE_AIR780EP_MQTT_CLIENT`, `example_air780ep_basic_connect_run()`, and `example_air780ep_mqtt_client_run()` consistently across header, main, CMake, source files, and README.
