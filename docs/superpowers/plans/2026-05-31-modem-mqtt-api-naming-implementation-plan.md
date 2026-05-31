# Modem MQTT API Naming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the Modem/Core internal MQTT connection APIs so the names distinguish MQTT parameter configuration, MQTT TCP channel connection, and MQTT protocol connection.

**Architecture:** Keep the existing layered flow and three-step MQTT FSM intact. Rename symbols across Modem Adapter wrappers, Core command queue, MQTT service command submission, Air780EP implementation hooks, tests, and docs without changing AT command behavior.

**Tech Stack:** C99, ESP-IDF, FreeRTOS, Python `unittest`/`pytest`, static host contract tests.

---

## Commit Policy

Do not run `git commit` during execution unless the user explicitly requests commits. Each task ends with a diff checkpoint instead of a commit because the current instruction set requires explicit commit approval.

## File Structure

- Modify `tests/host/test_mqtt_end_to_end_contract.py`: host-side static contract checks for the new API names and removed old names.
- Modify `src/modem/modem.h`: public Modem Adapter MQTT value types and wrapper declarations.
- Modify `src/modem/modem_priv.h`: internal `modem_ops_t` MQTT virtual function names and parameter types.
- Modify `src/modem/modem.c`: generic wrapper implementations and ops dispatch.
- Modify `src/modem/modem_air780ep.c`: Air780EP static function names, signatures, ops table fields, and parameter variable names.
- Modify `src/core/core.h`: Core MQTT command enum names and command payload union field names.
- Modify `src/core/core.c`: command clone/free/validation logic for the renamed Core commands.
- Modify `src/core/core_fsm.c`: service command dispatch from Core to Modem wrappers.
- Modify `src/mqtt_client/mqtt_client_priv.h`: MQTT connect-step enum names.
- Modify `src/mqtt_client/mqtt_client.c`: command submission and connect-step sequencing.
- Modify `docs/agents/classes.md`: class/API design documentation.
- Modify `docs/agents/at_cmd_air780ep.md`: Air780EP AT command mapping recommendations.
- Modify `docs/interview-preparation/modem-module-analysis.md`: interview/reference analysis naming table.

## Naming Map

| Current | New |
|---------|-----|
| `modem_mqtt_config_t` | unchanged |
| `modem_mqtt_config()` | `modem_mqtt_configure()` |
| `modem_mqtt_open_t` | `modem_mqtt_tcp_config_t` |
| `modem_mqtt_open()` | `modem_mqtt_tcp_connect()` |
| `modem_mqtt_login_t` | `modem_mqtt_connect_config_t` |
| `modem_mqtt_login()` | `modem_mqtt_connect()` |
| `modem_mqtt_disconnect()` | unchanged |
| `CORE_CMD_MQTT_CONFIG` | `CORE_CMD_MQTT_CONFIGURE` |
| `CORE_CMD_MQTT_OPEN` | `CORE_CMD_MQTT_TCP_CONNECT` |
| `CORE_CMD_MQTT_LOGIN` | `CORE_CMD_MQTT_CONNECT` |
| `core_cmd_t.data.mqtt_open` | `core_cmd_t.data.mqtt_tcp_connect` |
| `core_cmd_t.data.mqtt_login` | `core_cmd_t.data.mqtt_connect` |

---

### Task 1: Update Static Contract Tests First

**Files:**
- Modify: `tests/host/test_mqtt_end_to_end_contract.py:269-295`
- Modify: `tests/host/test_mqtt_end_to_end_contract.py:426-472`

- [ ] **Step 1: Replace Core command queue name assertions**

Replace `test_core_command_queue_contract_exists()` with this method:

```python
    def test_core_command_queue_contract_exists(self):
        for token in [
            "CORE_EVENT_PROTOCOL_DATA",
            "CORE_EVENT_PROTOCOL_CLOSED",
            "CORE_PROTOCOL_MQTT",
            "core_protocol_data_t",
            "CORE_CMD_MQTT_CONFIGURE",
            "CORE_CMD_MQTT_TCP_CONNECT",
            "CORE_CMD_MQTT_CONNECT",
            "CORE_CMD_MQTT_DISCONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "core_cmd_t",
            "core_submit_cmd(core_t *me, const core_cmd_t *cmd);",
        ]:
            self.assertIn(token, self.core_h)

        old_tokens = [
            "CORE_CMD_MQTT_" + "CONFIG,",
            "CORE_CMD_MQTT_" + "CONFIG =",
            "CORE_CMD_MQTT_" + "OPEN",
            "CORE_CMD_MQTT_" + "LOGIN",
            "mqtt_" + "open",
            "mqtt_" + "login",
        ]
        for token in old_tokens:
            self.assertNotIn(token, self.core_h)

        self.assertIn("CORE_SIG_SERVICE_CMD", self.core_priv)
        self.assertIn("core_cmd_t *service_cmd;", self.core_priv)
        self.assertIn("static core_cmd_t *clone_core_cmd", self.core_c)
        self.assertIn("static void free_core_cmd", self.core_c)
        self.assertIn("esp_err_t core_submit_cmd", self.core_c)
        self.assertIn("handle_service_cmd", self.core_fsm_c)
        self.assertIn("modem_mqtt_configure", self.core_fsm_c)
        self.assertIn("modem_mqtt_publish", self.core_fsm_c)
```

- [ ] **Step 2: Replace Modem MQTT ops name assertions**

Replace `test_modem_mqtt_ops_and_air780ep_commands_exist()` with this method:

```python
    def test_modem_mqtt_ops_and_air780ep_commands_exist(self):
        for token in [
            "modem_mqtt_config_t",
            "modem_mqtt_tcp_config_t",
            "modem_mqtt_connect_config_t",
            "modem_mqtt_topic_t",
            "modem_mqtt_publish_t",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "MODEM_PROTOCOL_MQTT",
            "modem_mqtt_configure(modem_t *me",
            "modem_mqtt_tcp_connect(modem_t *me",
            "modem_mqtt_connect(modem_t *me",
            "modem_mqtt_publish(modem_t *me",
        ]:
            self.assertIn(token, self.modem_h)

        old_modem_h_tokens = [
            "modem_mqtt_" + "open_t",
            "modem_mqtt_" + "login_t",
            "modem_mqtt_" + "config(modem_t *me",
            "modem_mqtt_" + "open(modem_t *me",
            "modem_mqtt_" + "login(modem_t *me",
        ]
        for token in old_modem_h_tokens:
            self.assertNotIn(token, self.modem_h)

        for token in [
            "mqtt_configure",
            "mqtt_tcp_connect",
            "mqtt_connect",
            "mqtt_disconnect",
            "mqtt_subscribe",
            "mqtt_unsubscribe",
            "mqtt_publish",
        ]:
            self.assertIn(token, self.modem_priv)

        old_modem_priv_tokens = [
            "mqtt_" + "config)(",
            "mqtt_" + "open",
            "mqtt_" + "login",
        ]
        for token in old_modem_priv_tokens:
            self.assertNotIn(token, self.modem_priv)

        for token in [
            "esp_err_t modem_mqtt_configure",
            "esp_err_t modem_mqtt_tcp_connect",
            "esp_err_t modem_mqtt_connect",
            "esp_err_t modem_mqtt_publish",
            "release_event_payload",
        ]:
            self.assertIn(token, self.modem_c)

        old_modem_c_tokens = [
            "esp_err_t modem_mqtt_" + "config(",
            "esp_err_t modem_mqtt_" + "open",
            "esp_err_t modem_mqtt_" + "login",
        ]
        for token in old_modem_c_tokens:
            self.assertNotIn(token, self.modem_c)

        for token in [
            "AIR780EP_URC_MSUB",
            "AT+MCONFIG",
            "AT+MIPSTART",
            "AT+MCONNECT",
            "AT+MDISCONNECT",
            "AT+MSUB",
            "AT+MUNSUB",
            "AT+MPUBEX",
            "air780ep_mqtt_configure",
            "air780ep_mqtt_tcp_connect",
            "air780ep_mqtt_connect",
            "air780ep_mqtt_publish",
            "handle_msub_urc",
        ]:
            self.assertIn(token, self.air780ep_c)

        old_air780ep_tokens = [
            "air780ep_mqtt_" + "config(",
            "air780ep_mqtt_" + "open",
            "air780ep_mqtt_" + "login",
        ]
        for token in old_air780ep_tokens:
            self.assertNotIn(token, self.air780ep_c)
```

- [ ] **Step 3: Run the host contract test and verify it fails for the expected reason**

Run: `python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL. The failure should mention at least one missing new token such as `CORE_CMD_MQTT_CONFIGURE` or `modem_mqtt_configure(modem_t *me`.

- [ ] **Step 4: Check the diff**

Run: `git diff -- tests/host/test_mqtt_end_to_end_contract.py`

Expected: the diff only changes the MQTT naming assertions in the two methods above.

---

### Task 2: Rename Modem Adapter MQTT API

**Files:**
- Modify: `src/modem/modem.h:126-152`
- Modify: `src/modem/modem.h:465-510`
- Modify: `src/modem/modem_priv.h:60-66`
- Modify: `src/modem/modem.c:503-540`
- Modify: `src/modem/modem_air780ep.c:264-270`
- Modify: `src/modem/modem_air780ep.c:814-817`
- Modify: `src/modem/modem_air780ep.c:2746-2873`

- [ ] **Step 1: Update Modem public MQTT value types**

In `src/modem/modem.h`, keep `modem_mqtt_config_t` unchanged and replace the open/login type definitions with:

```c
/**
 * @brief MQTT TCP 连接参数
 * @details MQTT TCP connection parameters
 */
typedef struct {
    const char *host;             /**< Broker 主机名或 IP； Broker host name or IP */
    uint16_t port;                /**< Broker 端口号； Broker port */
} modem_mqtt_tcp_config_t;

/**
 * @brief MQTT 连接参数
 * @details MQTT connection parameters
 */
typedef struct {
    bool clean_session;           /**< 是否使用 clean session； Whether to use clean session */
    uint16_t keepalive_s;         /**< 保活时间（秒）； Keepalive in seconds */
} modem_mqtt_connect_config_t;
```

- [ ] **Step 2: Update Modem public wrapper declarations**

In `src/modem/modem.h`, replace the three MQTT declarations and comments with:

```c
/**
 * @brief 配置 MQTT 参数
 * @details Configure MQTT parameters
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT 配置参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 配置失败
 */
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config);

/**
 * @brief 建立 MQTT TCP 通道
 * @details Connect MQTT TCP channel
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT TCP 连接参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_FAIL: 连接失败
 */
esp_err_t modem_mqtt_tcp_connect(modem_t *me,
                                 const modem_mqtt_tcp_config_t *config);

/**
 * @brief 连接 MQTT Broker
 * @details Connect to MQTT broker
 * @param[in] me 调制解调器句柄
 * @param[in] config MQTT 连接参数
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_FAIL: 连接失败
 */
esp_err_t modem_mqtt_connect(modem_t *me,
                             const modem_mqtt_connect_config_t *config);
```

- [ ] **Step 3: Update `modem_ops_t` MQTT function pointers**

In `src/modem/modem_priv.h`, replace the MQTT config/open/login ops entries with:

```c
    esp_err_t (*mqtt_configure)(modem_t *me,
                                const modem_mqtt_config_t *config);   /**< 配置 MQTT； Configure MQTT */
    esp_err_t (*mqtt_tcp_connect)(modem_t *me,
                                  const modem_mqtt_tcp_config_t *config); /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    esp_err_t (*mqtt_connect)(modem_t *me,
                              const modem_mqtt_connect_config_t *config);  /**< 连接 MQTT； Connect MQTT */
    esp_err_t (*mqtt_disconnect)(modem_t *me);      /**< 断开 MQTT； Disconnect MQTT */
```

- [ ] **Step 4: Update Modem wrapper implementations**

In `src/modem/modem.c`, replace the three wrapper functions with:

```c
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_configure,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_configure not supported");
    return me->ops->mqtt_configure(me, config);
}

esp_err_t modem_mqtt_tcp_connect(modem_t *me,
                                 const modem_mqtt_tcp_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->host && config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_tcp_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_tcp_connect not supported");

    return me->ops->mqtt_tcp_connect(me, config);
}

esp_err_t modem_mqtt_connect(modem_t *me,
                             const modem_mqtt_connect_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_connect not supported");

    return me->ops->mqtt_connect(me, config);
}
```

- [ ] **Step 5: Update Air780EP static declarations and ops table**

In `src/modem/modem_air780ep.c`, replace the static declarations with:

```c
static esp_err_t air780ep_mqtt_configure(modem_t *me,
                                         const modem_mqtt_config_t *config);
static esp_err_t air780ep_mqtt_tcp_connect(modem_t *me,
                                           const modem_mqtt_tcp_config_t *config);
static esp_err_t air780ep_mqtt_connect(modem_t *me,
                                       const modem_mqtt_connect_config_t *config);
static esp_err_t air780ep_mqtt_disconnect(modem_t *me);
```

In `s_air780ep_ops`, replace the MQTT config/open/login field assignments with:

```c
    .mqtt_configure = air780ep_mqtt_configure,
    .mqtt_tcp_connect = air780ep_mqtt_tcp_connect,
    .mqtt_connect = air780ep_mqtt_connect,
    .mqtt_disconnect = air780ep_mqtt_disconnect,
```

- [ ] **Step 6: Update Air780EP MQTT implementation names and parameter names**

In `src/modem/modem_air780ep.c`, rename the three static function definitions and use `config` as the parameter name in the TCP and MQTT connect functions:

```c
static esp_err_t air780ep_mqtt_configure(modem_t *me,
                                         const modem_mqtt_config_t *config)
```

```c
static esp_err_t air780ep_mqtt_tcp_connect(modem_t *me,
                                           const modem_mqtt_tcp_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->host && config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char *host = escape_at_string(config->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape host failed");

    int needed = snprintf(NULL, 0, "AT+MIPSTART=\"%s\",%u",
                          host, (unsigned int)config->port);
    if (needed < 0) {
        free(host);
        return ESP_ERR_INVALID_ARG;
    }
    char *cmd = malloc((size_t)needed + 1U);
    if (!cmd) {
        free(host);
        return ESP_ERR_NO_MEM;
    }
    snprintf(cmd, (size_t)needed + 1U, "AT+MIPSTART=\"%s\",%u",
             host, (unsigned int)config->port);

    const at_cmd_success_match_t matches[] = {
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "CONNECT OK" },
        { .type = AT_CMD_SUCCESS_MATCH_EXACT, .value = "ALREADY CONNECT" },
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CONNECT_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = matches,
        .success_match_count = sizeof(matches) / sizeof(matches[0]),
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MIPSTART");
    }

    free(cmd);
    free(host);
    return ret;
}
```

```c
static esp_err_t air780ep_mqtt_connect(modem_t *me,
                                       const modem_mqtt_connect_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    char cmd[48];
    int written = snprintf(cmd, sizeof(cmd), "AT+MCONNECT=%u,%u",
                           config->clean_session ? 1U : 0U,
                           (unsigned int)config->keepalive_s);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+MCONNECT command truncated");

    const at_cmd_success_match_t match = {
        .type = AT_CMD_SUCCESS_MATCH_EXACT,
        .value = "CONNACK OK",
    };
    const at_cmd_options_t options = {
        .timeout_ms = AIR780EP_MQTT_CONNECT_TIMEOUT_MS,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &match,
        .success_match_count = 1,
    };

    modem_air780ep_t *self = to_air780ep(me);
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd_with_options(self, cmd, &ctx, &options);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MCONNECT");
    }
    if (ret == ESP_OK) {
        set_mqtt_data_enabled(self, true);
    }
    return ret;
}
```

- [ ] **Step 7: Run the host contract test and verify only Core/MQTT names remain failing**

Run: `python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL. The failure should now be about Core command names or MQTT service names, not about missing `modem_mqtt_configure`, `modem_mqtt_tcp_connect`, or `modem_mqtt_connect` in Modem files.

- [ ] **Step 8: Check the Modem diff**

Run: `git diff -- src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c src/modem/modem_air780ep.c`

Expected: the diff is limited to MQTT config/open/login naming, parameter names, and comments. Existing unrelated comment changes in these files must remain intact.

---

### Task 3: Rename Core Command Boundary and MQTT FSM Steps

**Files:**
- Modify: `src/core/core.h:133-204`
- Modify: `src/core/core.c:921-976`
- Modify: `src/core/core.c:987-995`
- Modify: `src/core/core.c:1046-1075`
- Modify: `src/core/core_fsm.c:535-558`
- Modify: `src/mqtt_client/mqtt_client_priv.h:51-57`
- Modify: `src/mqtt_client/mqtt_client.c:566-579`
- Modify: `src/mqtt_client/mqtt_client.c:616-628`
- Modify: `src/mqtt_client/mqtt_client.c:793-823`

- [ ] **Step 1: Update Core command enum and union fields**

In `src/core/core.h`, replace the MQTT command enum entries with:

```c
typedef enum {
    CORE_CMD_MQTT_CONFIGURE = 0,          /**< 配置 MQTT； Configure MQTT */
    CORE_CMD_MQTT_TCP_CONNECT,           /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    CORE_CMD_MQTT_CONNECT,               /**< 连接 MQTT； Connect MQTT */
    CORE_CMD_MQTT_DISCONNECT,            /**< 断开 MQTT； Disconnect MQTT */
    CORE_CMD_MQTT_SUBSCRIBE,             /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    CORE_CMD_MQTT_UNSUBSCRIBE,           /**< 退订 MQTT 主题； Unsubscribe MQTT topic */
    CORE_CMD_MQTT_PUBLISH,               /**< 发布 MQTT 消息； Publish MQTT message */
    CORE_CMD_PING,                       /**< 执行 Ping 诊断； Perform Ping diagnostic */
} core_cmd_type_t;
```

In the `core_cmd_t` union, keep `mqtt_config` and replace `mqtt_open`/`mqtt_login` with:

```c
        struct {
            const char *host;            /**< 主机； Host */
            uint16_t port;               /**< 端口； Port */
        } mqtt_tcp_connect;              /**< MQTT TCP 连接参数； MQTT TCP connect args */
        struct {
            bool clean_session;          /**< 清理会话； Clean session */
            uint16_t keepalive_s;        /**< 保活秒数； Keepalive seconds */
        } mqtt_connect;                  /**< MQTT 连接参数； MQTT connect args */
```

- [ ] **Step 2: Update Core command clone/free/validation logic**

In `src/core/core.c`, update `clone_core_cmd()` cases to use the new command and union names:

```c
    switch (cmd->type) {
    case CORE_CMD_MQTT_CONFIGURE:
        clone->data.mqtt_config.client_id = clone_optional_string(cmd->data.mqtt_config.client_id);
        clone->data.mqtt_config.username = clone_optional_string(cmd->data.mqtt_config.username);
        clone->data.mqtt_config.password = clone_optional_string(cmd->data.mqtt_config.password);
        if (!clone->data.mqtt_config.client_id ||
            (cmd->data.mqtt_config.username && !clone->data.mqtt_config.username) ||
            (cmd->data.mqtt_config.password && !clone->data.mqtt_config.password)) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_MQTT_TCP_CONNECT:
        clone->data.mqtt_tcp_connect.host = clone_optional_string(cmd->data.mqtt_tcp_connect.host);
        if (!clone->data.mqtt_tcp_connect.host) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
```

Also replace the empty login case with:

```c
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
        break;
```

In `free_core_cmd()`, replace the affected cases with:

```c
    case CORE_CMD_MQTT_CONFIGURE:
        free((void *)cmd->data.mqtt_config.client_id);
        free((void *)cmd->data.mqtt_config.username);
        free((void *)cmd->data.mqtt_config.password);
        break;
    case CORE_CMD_MQTT_TCP_CONNECT:
        free((void *)cmd->data.mqtt_tcp_connect.host);
        break;
```

In `core_cmd_type_valid()` and `core_cmd_valid()`, use:

```c
static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_MQTT_CONFIGURE && type <= CORE_CMD_PING;
}
```

```c
    case CORE_CMD_MQTT_CONFIGURE:
        return cmd->data.mqtt_config.client_id != NULL;
    case CORE_CMD_MQTT_TCP_CONNECT:
        return cmd->data.mqtt_tcp_connect.host != NULL &&
               cmd->data.mqtt_tcp_connect.port > 0;
    case CORE_CMD_MQTT_CONNECT:
        return true;
```

- [ ] **Step 3: Update Core FSM service command dispatch**

In `src/core/core_fsm.c`, replace the first three MQTT command cases in `handle_service_cmd()` with:

```c
    case CORE_CMD_MQTT_CONFIGURE: {
        modem_mqtt_config_t config = {
            .client_id = cmd->data.mqtt_config.client_id,
            .username = cmd->data.mqtt_config.username,
            .password = cmd->data.mqtt_config.password,
        };
        ret = modem_mqtt_configure(me->modem, &config);
        break;
    }
    case CORE_CMD_MQTT_TCP_CONNECT: {
        modem_mqtt_tcp_config_t config = {
            .host = cmd->data.mqtt_tcp_connect.host,
            .port = cmd->data.mqtt_tcp_connect.port,
        };
        ret = modem_mqtt_tcp_connect(me->modem, &config);
        break;
    }
    case CORE_CMD_MQTT_CONNECT: {
        modem_mqtt_connect_config_t config = {
            .clean_session = cmd->data.mqtt_connect.clean_session,
            .keepalive_s = cmd->data.mqtt_connect.keepalive_s,
        };
        ret = modem_mqtt_connect(me->modem, &config);
        break;
    }
```

- [ ] **Step 4: Update MQTT connect-step enum names**

In `src/mqtt_client/mqtt_client_priv.h`, replace `mqtt_connect_step_t` with:

```c
typedef enum {
    MQTT_CONNECT_STEP_IDLE = 0,
    MQTT_CONNECT_STEP_CONFIGURE,
    MQTT_CONNECT_STEP_TCP_CONNECT,
    MQTT_CONNECT_STEP_CONNECT,
    MQTT_CONNECT_STEP_DONE,
    MQTT_CONNECT_STEP_ERROR,
} mqtt_connect_step_t;
```

- [ ] **Step 5: Update MQTT service command submission**

In `src/mqtt_client/mqtt_client.c`, replace the affected `submit_core_cmd()` switch cases with:

```c
    switch (type) {
    case CORE_CMD_MQTT_CONFIGURE:
        cmd.data.mqtt_config.client_id = me->config.client_id;
        cmd.data.mqtt_config.username = me->config.username;
        cmd.data.mqtt_config.password = me->config.password;
        break;
    case CORE_CMD_MQTT_TCP_CONNECT:
        cmd.data.mqtt_tcp_connect.host = me->config.host;
        cmd.data.mqtt_tcp_connect.port = me->config.port;
        break;
    case CORE_CMD_MQTT_CONNECT:
        cmd.data.mqtt_connect.clean_session = me->config.clean_session;
        cmd.data.mqtt_connect.keepalive_s = me->config.keepalive_s;
        break;
```

In `begin_connect()`, replace the initial connect step and command with:

```c
    set_state(me, MQTT_CLIENT_STATE_CONNECTING);
    me->connect_step = MQTT_CONNECT_STEP_CONFIGURE;
```

```c
    return submit_core_cmd(me, CORE_CMD_MQTT_CONFIGURE,
                           MQTT_CLIENT_OPERATION_CONNECT, NULL);
```

In `handle_core_cmd_done()`, replace the open/connect sequencing with:

```c
    if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_CONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->transport_open = true;
    }
```

```c
    if (operation == MQTT_CLIENT_OPERATION_CONNECT) {
        if (me->connect_step == MQTT_CONNECT_STEP_CONFIGURE) {
            me->connect_step = MQTT_CONNECT_STEP_TCP_CONNECT;
            (void)submit_core_cmd(me, CORE_CMD_MQTT_TCP_CONNECT,
                                  MQTT_CLIENT_OPERATION_CONNECT, NULL);
        } else if (me->connect_step == MQTT_CONNECT_STEP_TCP_CONNECT) {
            me->connect_step = MQTT_CONNECT_STEP_CONNECT;
            (void)submit_core_cmd(me, CORE_CMD_MQTT_CONNECT,
                                  MQTT_CLIENT_OPERATION_CONNECT, NULL);
        } else if (me->connect_step == MQTT_CONNECT_STEP_CONNECT) {
            me->connect_step = MQTT_CONNECT_STEP_DONE;
            me->transport_open = true;
            set_state(me, MQTT_CLIENT_STATE_CONNECTED);
            (void)post_mqtt_event(me, MQTT_CLIENT_EVENT_CONNECTED, NULL);
        }
        return;
    }
```

- [ ] **Step 6: Run the host contract test and verify it passes**

Run: `python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: PASS with output ending in `passed`.

- [ ] **Step 7: Check source diff**

Run: `git diff -- src/core/core.h src/core/core.c src/core/core_fsm.c src/mqtt_client/mqtt_client_priv.h src/mqtt_client/mqtt_client.c`

Expected: the diff only renames MQTT config/open/login command symbols and connect-step names. It must not alter MQTT event IDs, publish/subscribe behavior, queue ownership, or payload lifetime code.

---

### Task 4: Update Documentation and Run Full Verification

**Files:**
- Modify: `docs/agents/classes.md:344-346`
- Modify: `docs/agents/classes.md:399-404`
- Modify: `docs/agents/classes.md:449-457`
- Modify: `docs/agents/classes.md:517-521`
- Modify: `docs/agents/classes.md:572-575`
- Modify: `docs/agents/classes.md:1672-1675`
- Modify: `docs/agents/at_cmd_air780ep.md:221-223`
- Modify: `docs/agents/at_cmd_air780ep.md:351-353`
- Modify: `docs/interview-preparation/modem-module-analysis.md:51-54`

- [ ] **Step 1: Update `docs/agents/classes.md` type/API references**

Use these names in the class overview and API sections:

```markdown
| `modem_mqtt_config_t` | 层间 API | Core + Modem 层 | 值对象 | MQTT 配置命令参数，Core 执行 `CORE_CMD_MQTT_CONFIGURE` 时使用 |
| `modem_mqtt_tcp_config_t` | 层间 API | Core + Modem 层 | 值对象 | MQTT TCP 通道连接参数 |
| `modem_mqtt_connect_config_t` | 层间 API | Core + Modem 层 | 值对象 | MQTT 协议连接参数 |
```

```c
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config);
esp_err_t modem_mqtt_tcp_connect(modem_t *me,
                                 const modem_mqtt_tcp_config_t *config);
esp_err_t modem_mqtt_connect(modem_t *me,
                             const modem_mqtt_connect_config_t *config);
esp_err_t modem_mqtt_disconnect(modem_t *me);
```

```c
typedef struct {
    const char *host;
    uint16_t port;
} modem_mqtt_tcp_config_t;

typedef struct {
    bool clean_session;
    uint16_t keepalive_s;
} modem_mqtt_connect_config_t;
```

- [ ] **Step 2: Update `docs/agents/classes.md` Modem ops mapping table**

Replace the MQTT rows with:

```markdown
| `mqtt_configure` | 配置模块内置 MQTT client 参数，不建立网络连接 | `AT+MCONFIG` |
| `mqtt_tcp_connect` | 建立模块 MQTT TCP 通道 | `AT+MIPSTART`，成功接受 `CONNECT OK` / `ALREADY CONNECT`，失败识别 `CONNECT FAIL` |
| `mqtt_connect` | 执行 MQTT CONNECT 建立 broker 会话 | `AT+MCONNECT`，成功 `CONNACK OK` |
| `mqtt_disconnect` | 断开 MQTT broker 会话 | `AT+MDISCONNECT` |
```

- [ ] **Step 3: Update `docs/agents/classes.md` Core command table**

Replace the first four MQTT command rows with:

```markdown
| 配置 MQTT 参数 | `CORE_CMD_MQTT_CONFIGURE` | `AT+MCONFIG` |
| 建立 MQTT TCP 通道 | `CORE_CMD_MQTT_TCP_CONNECT` | `AT+MIPSTART`，成功接受 `CONNECT OK` / `ALREADY CONNECT` |
| MQTT 协议连接 | `CORE_CMD_MQTT_CONNECT` | `AT+MCONNECT`，成功接受 `CONNACK OK` |
| 断开 MQTT | `CORE_CMD_MQTT_DISCONNECT` | `AT+MDISCONNECT` |
```

- [ ] **Step 4: Update Air780EP AT command reference mapping recommendations**

In `docs/agents/at_cmd_air780ep.md`, update mapping suggestions for the three commands to:

```markdown
| MQTT 参数配置 | `AT+MCONFIG=<clientid>[,<username>,<password>[,<will_qos>,<will_retain>,<will_topic>,<will_message>]]` | `OK` 或 `ERROR` | `clientid`、`username`、`password` 最长 256 字节；`will_qos=0..2`；`will_retain=0/1`；`will_message` 最长 1360 字节 | 9s | `modem_mqtt_configure()` | 客户端 ID 不能与服务器上其他连接重复；遗嘱主题和消息需要加引号 |
| 建立 MQTT TCP 连接 | 普通：`AT+MIPSTART=<svraddr>,<port>`；SSL：`AT+SSLMIPSTART=<svraddr>,<port>` | 立即 `OK`，后续 `CONNECT OK`、`ALREADY CONNECT`、`CONNECT FAIL`；多连接可能上报 `7,CONNECT OK` | `svraddr` 为 IP 或域名；`port=1..65535` | 命令 9s；连接结果按业务预算等待 | `modem_mqtt_tcp_connect()` | 使用 SSL 时先配置 `SSLCFG` context `88`；等待 `CONNECT OK` 后立即发送 `MCONNECT`，否则可能被服务器踢掉 |
| MQTT 协议连接 | `AT+MCONNECT=<clean_session>,<keepalive>[,<mode>]` | 立即 `OK`；成功 URC `CONNACK OK`；失败 `ERROR` | `clean_session=0/1`；`keepalive=1..65535s`；`mode=1` 启用大于 300s 长心跳支持 | 命令 9s；CONNACK 按业务预算等待 | `modem_mqtt_connect()` | 收到 `CONNACK OK` 后才能 publish/subscribe；建议 keepalive 取 300s 以上 |
```

In the command sequence section, use:

```markdown
3. `modem_mqtt_configure()` 发送 `AT+MCONFIG=<clientid>,<username>,<password>`，用户名密码为空时使用 `"",""`。
4. `modem_mqtt_tcp_connect()` 发送普通连接 `AT+MIPSTART="<host>",<port>`；TLS 后续可映射 `AT+SSLMIPSTART="<host>",<port>`。
5. 等待 `CONNECT OK` 后立即通过 `modem_mqtt_connect()` 发送 `AT+MCONNECT=1,<keepalive>[,<mode>]`。
```

- [ ] **Step 5: Update interview/reference modem analysis table**

In `docs/interview-preparation/modem-module-analysis.md`, replace the MQTT rows with:

```markdown
| `modem_mqtt_configure(me, config)` | send/config | 发送 `AT+MCONFIG` 设置 MQTT client id/user/password |
| `modem_mqtt_tcp_connect(me, config)` | start/enable | 发送 `AT+MIPSTART` 建立 MQTT TCP 通道，接受 `CONNECT OK` 或 `ALREADY CONNECT` |
| `modem_mqtt_connect(me, config)` | start/enable | 发送 `AT+MCONNECT`，成功 `CONNACK OK` 后打开 `mqtt_data_enabled` |
| `modem_mqtt_disconnect(me)` | stop/disable | 关闭 `mqtt_data_enabled` 并发送 `AT+MDISCONNECT` |
```

- [ ] **Step 6: Verify old source/test/doc symbols are gone from maintained paths**

Run: `rg "modem_mqtt_config\\(|modem_mqtt_open|modem_mqtt_login|CORE_CMD_MQTT_CONFIG([[:space:]]|,|=)|CORE_CMD_MQTT_OPEN|CORE_CMD_MQTT_LOGIN|modem_mqtt_open_t|modem_mqtt_login_t|mqtt_config\\)\\(|mqtt_open|mqtt_login|air780ep_mqtt_config\\(|air780ep_mqtt_open|air780ep_mqtt_login" src tests docs/agents docs/interview-preparation`

Expected: no output.

- [ ] **Step 7: Verify `modem_mqtt_config_t` remains present**

Run: `rg "modem_mqtt_config_t" src tests docs/agents docs/interview-preparation`

Expected: output includes `src/modem/modem.h`, `src/modem/modem.c`, `src/modem/modem_air780ep.c`, and `tests/host/test_mqtt_end_to_end_contract.py`.

- [ ] **Step 8: Run host tests**

Run: `python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: PASS with output ending in `passed`.

- [ ] **Step 9: Run formatting whitespace check**

Run: `git diff --check`

Expected: no output and exit code 0.

- [ ] **Step 10: Run ESP-IDF build if the shell has ESP-IDF loaded**

Run: `idf.py build`

Expected when ESP-IDF is available: build completes successfully. If the command fails with `idf.py: command not found`, record that build verification is blocked by the shell environment and do not treat it as a code failure.

- [ ] **Step 11: Final diff review**

Run: `git diff -- src tests docs/agents docs/interview-preparation docs/superpowers/specs docs/superpowers/plans`

Expected: diff shows only the approved MQTT naming design/spec, implementation plan, symbol renames, tests, and related docs. It must not include unrelated edits to `.gitignore`, examples, or other user-modified files.
