# MQTT Config Cache and TCP Disconnect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Do not commit unless the user explicitly asks.

**Goal:** Merge MQTT modem connection options into one cached config and add explicit MQTT TCP disconnect support.

**Architecture:** `modem_mqtt_configure()` becomes the single configuration entry point and deep-copies all MQTT client, broker, and session fields into the Air780EP modem instance. `modem_mqtt_tcp_connect()`, `modem_mqtt_connect()`, `modem_mqtt_disconnect()`, and the new `modem_mqtt_tcp_disconnect()` operate on modem-owned state, with Core and MQTT client stop flow sequencing `MDISCONNECT` before `MIPCLOSE`.

**Tech Stack:** C, ESP-IDF, FreeRTOS mutexes, ESP error codes, Python static host tests.

---

## File Map

- Modify `tests/host/test_mqtt_end_to_end_contract.py`: add static contract checks for unified config, removed split config types, no-arg connect wrappers, `MIPCLOSE`, Core command sequencing, and MQTT stop flow.
- Modify `src/modem/modem.h`: expand `modem_mqtt_config_t`, remove `modem_mqtt_tcp_config_t` and `modem_mqtt_connect_config_t`, update MQTT API prototypes and comments.
- Modify `src/modem/modem_priv.h`: update `modem_ops_t` signatures and add `mqtt_tcp_disconnect` op.
- Modify `src/modem/modem.c`: update public wrappers, validation, and new TCP disconnect wrapper.
- Modify `src/modem/modem_air780ep.c`: add cached MQTT config ownership, lifecycle flags, state validation, `AT+MIPCLOSE`, and cleanup.
- Modify `src/core/core.h`: add `CORE_CMD_MQTT_TCP_DISCONNECT`, merge MQTT config command payload, remove per-step connect payload structs.
- Modify `src/core/core.c`: update command validation and cloning/freeing for the merged MQTT config and the new no-payload commands.
- Modify `src/core/core_fsm.c`: dispatch no-arg `modem_mqtt_tcp_connect()`, no-arg `modem_mqtt_connect()`, and new `modem_mqtt_tcp_disconnect()`.
- Modify `src/mqtt_client/mqtt_client_priv.h`: track stop close phase using the existing pending-command model.
- Modify `src/mqtt_client/mqtt_client.c`: submit complete config once, use no-payload connect commands, and stop with `DISCONNECT` then `TCP_DISCONNECT`.
- Modify `docs/agents/classes.md`, `docs/agents/at_cmd_air780ep.md`, and related references if stale tokens remain.

## Task 1: Add Failing Static Contract Tests

**Files:**
- Modify: `tests/host/test_mqtt_end_to_end_contract.py`

- [ ] **Step 1: Add tests for the new Modem MQTT API contract**

Append these assertions to the existing MQTT API test area, or add a focused method near other Modem/Core MQTT command tests:

```python
    def test_modem_mqtt_config_is_unified_and_cached(self):
        self.assertIn("typedef struct {", self.modem_h)
        self.assertIn("const char *client_id;", self.modem_h)
        self.assertIn("const char *username;", self.modem_h)
        self.assertIn("const char *password;", self.modem_h)
        self.assertIn("const char *host;", self.modem_h)
        self.assertIn("uint16_t port;", self.modem_h)
        self.assertIn("bool clean_session;", self.modem_h)
        self.assertIn("uint16_t keepalive_s;", self.modem_h)
        self.assertIn("} modem_mqtt_config_t;", self.modem_h)

        self.assertNotIn("modem_mqtt_tcp_config_t", self.modem_h + self.modem_priv + self.modem_c + self.air780ep_c)
        self.assertNotIn("modem_mqtt_connect_config_t", self.modem_h + self.modem_priv + self.modem_c + self.air780ep_c)

        self.assertIn("esp_err_t modem_mqtt_tcp_connect(modem_t *me);", self.modem_h)
        self.assertIn("esp_err_t modem_mqtt_connect(modem_t *me);", self.modem_h)
        self.assertIn("esp_err_t modem_mqtt_tcp_disconnect(modem_t *me);", self.modem_h)
        self.assertIn("mqtt_tcp_disconnect", self.modem_priv)

        self.assertIn("mqtt_configured", self.air780ep_c)
        self.assertIn("mqtt_tcp_connected", self.air780ep_c)
        self.assertIn("mqtt_session_connected", self.air780ep_c)
        self.assertIn("free_mqtt_config", self.air780ep_c)
```

- [ ] **Step 2: Add tests for Core commands and Air780EP AT commands**

Add this method to the same test class:

```python
    def test_core_dispatches_cached_mqtt_commands_and_tcp_disconnect(self):
        self.assertIn("CORE_CMD_MQTT_TCP_DISCONNECT", self.core_h)
        self.assertIn("modem_mqtt_tcp_connect(me->modem);", self.core_fsm_c)
        self.assertIn("modem_mqtt_connect(me->modem);", self.core_fsm_c)
        self.assertIn("modem_mqtt_tcp_disconnect(me->modem);", self.core_fsm_c)
        self.assertIn("AT+MIPCLOSE", self.air780ep_c)

        self.assertIn("cmd.data.mqtt_config.host", self.mqtt_c + self.core_c + self.core_h)
        self.assertIn("cmd.data.mqtt_config.port", self.mqtt_c + self.core_c + self.core_h)
        self.assertIn("cmd.data.mqtt_config.clean_session", self.mqtt_c + self.core_c + self.core_h)
        self.assertIn("cmd.data.mqtt_config.keepalive_s", self.mqtt_c + self.core_c + self.core_h)
        self.assertNotIn("mqtt_tcp_connect.host", self.core_h + self.core_c + self.mqtt_c)
        self.assertNotIn("mqtt_connect.clean_session", self.core_h + self.core_c + self.mqtt_c)
```

- [ ] **Step 3: Add tests for MQTT stop ordering**

Extend the stop-flow tests with these checks:

```python
        self.assertIn("CORE_CMD_MQTT_TCP_DISCONNECT", self.mqtt_c)
        self.assertIn("submit_core_cmd(me, CORE_CMD_MQTT_TCP_DISCONNECT", self.mqtt_c)

        stop_done_start = self.mqtt_c.rindex("static void handle_core_cmd_done")
        stop_done_body = self.mqtt_c[
            stop_done_start:
            self.mqtt_c.index("static void handle_runtime_operation", stop_done_start)
        ]
        self.assertLess(
            stop_done_body.index("CORE_CMD_MQTT_DISCONNECT"),
            stop_done_body.index("CORE_CMD_MQTT_TCP_DISCONNECT")
        )
```

- [ ] **Step 4: Run tests and confirm failure**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL. The failure should mention missing `modem_mqtt_tcp_disconnect`, stale split config types, missing `CORE_CMD_MQTT_TCP_DISCONNECT`, or missing `AT+MIPCLOSE`.

## Task 2: Update Public Modem API and Ops Signatures

**Files:**
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_priv.h`
- Modify: `src/modem/modem.c`

- [ ] **Step 1: Update `modem_mqtt_config_t` and prototypes in `modem.h`**

Replace the three MQTT config typedefs with:

```c
typedef struct {
    const char *client_id;        /**< 客户端 ID； Client ID */
    const char *username;         /**< 用户名，可为 NULL； Username, can be NULL */
    const char *password;         /**< 密码，可为 NULL； Password, can be NULL */
    const char *host;             /**< Broker 主机名或 IP； Broker host name or IP */
    uint16_t port;                /**< Broker 端口号； Broker port */
    bool clean_session;           /**< 是否使用 clean session； Whether to use clean session */
    uint16_t keepalive_s;         /**< 保活时间（秒）； Keepalive in seconds */
} modem_mqtt_config_t;
```

Update prototypes to:

```c
esp_err_t modem_mqtt_tcp_connect(modem_t *me);
esp_err_t modem_mqtt_connect(modem_t *me);
esp_err_t modem_mqtt_disconnect(modem_t *me);
esp_err_t modem_mqtt_tcp_disconnect(modem_t *me);
```

- [ ] **Step 2: Update `modem_ops_t` in `modem_priv.h`**

Replace the MQTT ops section with:

```c
    esp_err_t (*mqtt_configure)(modem_t *me,
                                const modem_mqtt_config_t *config);   /**< 配置 MQTT； Configure MQTT */
    esp_err_t (*mqtt_tcp_connect)(modem_t *me); /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    esp_err_t (*mqtt_connect)(modem_t *me);     /**< 连接 MQTT； Connect MQTT */
    esp_err_t (*mqtt_disconnect)(modem_t *me);  /**< 断开 MQTT； Disconnect MQTT */
    esp_err_t (*mqtt_tcp_disconnect)(modem_t *me); /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
```

- [ ] **Step 3: Update wrappers in `modem.c`**

Use these wrapper bodies:

```c
esp_err_t modem_mqtt_configure(modem_t *me,
                               const modem_mqtt_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config && config->client_id && config->host && config->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_configure,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_configure not supported");
    return me->ops->mqtt_configure(me, config);
}

esp_err_t modem_mqtt_tcp_connect(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_tcp_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_tcp_connect not supported");
    return me->ops->mqtt_tcp_connect(me);
}

esp_err_t modem_mqtt_connect(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_connect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_connect not supported");
    return me->ops->mqtt_connect(me);
}

esp_err_t modem_mqtt_tcp_disconnect(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->mqtt_tcp_disconnect,
                        ESP_ERR_NOT_SUPPORTED, TAG, "mqtt_tcp_disconnect not supported");
    return me->ops->mqtt_tcp_disconnect(me);
}
```

- [ ] **Step 4: Run focused static test and confirm remaining failures move to implementation files**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL remains because Air780EP/Core/MQTT client are not updated yet, but failures about public prototypes should be resolved.

## Task 3: Implement Air780EP MQTT Config Cache and TCP Disconnect

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add fields to `modem_air780ep_t`**

Extend the struct near `mqtt_data_enabled`:

```c
    modem_mqtt_config_t mqtt_config;
    bool mqtt_configured;
    bool mqtt_tcp_connected;
    bool mqtt_session_connected;
    bool mqtt_data_enabled;
```

- [ ] **Step 2: Add helper prototypes**

Add prototypes near existing MQTT helpers:

```c
static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src);
static char *clone_mqtt_string(const char *value);
static void free_mqtt_config(modem_mqtt_config_t *config);
static void clear_mqtt_state(modem_air780ep_t *self);
```

Change MQTT function prototypes to:

```c
static esp_err_t air780ep_mqtt_tcp_connect(modem_t *me);
static esp_err_t air780ep_mqtt_connect(modem_t *me);
static esp_err_t air780ep_mqtt_tcp_disconnect(modem_t *me);
```

- [ ] **Step 3: Register new ops**

In `air780ep_ops`, set:

```c
    .mqtt_configure = air780ep_mqtt_configure,
    .mqtt_tcp_connect = air780ep_mqtt_tcp_connect,
    .mqtt_connect = air780ep_mqtt_connect,
    .mqtt_disconnect = air780ep_mqtt_disconnect,
    .mqtt_tcp_disconnect = air780ep_mqtt_tcp_disconnect,
```

- [ ] **Step 4: Implement config ownership helpers**

Add helper implementations:

```c
static char *clone_mqtt_string(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t len = strlen(value) + 1U;
    char *copy = malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len);
    return copy;
}

static esp_err_t copy_mqtt_config(modem_mqtt_config_t *dst,
                                  const modem_mqtt_config_t *src)
{
    ESP_RETURN_ON_FALSE(dst && src && src->client_id && src->host && src->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT config");

    modem_mqtt_config_t copy = {
        .client_id = clone_mqtt_string(src->client_id),
        .username = src->username ? clone_mqtt_string(src->username) : NULL,
        .password = src->password ? clone_mqtt_string(src->password) : NULL,
        .host = clone_mqtt_string(src->host),
        .port = src->port,
        .clean_session = src->clean_session,
        .keepalive_s = src->keepalive_s,
    };
    if (!copy.client_id || !copy.host ||
        (src->username && !copy.username) ||
        (src->password && !copy.password)) {
        free_mqtt_config(&copy);
        return ESP_ERR_NO_MEM;
    }

    free_mqtt_config(dst);
    *dst = copy;
    return ESP_OK;
}

static void free_mqtt_config(modem_mqtt_config_t *config)
{
    if (!config) {
        return;
    }
    free((void *)config->client_id);
    free((void *)config->username);
    free((void *)config->password);
    free((void *)config->host);
    memset(config, 0, sizeof(*config));
}

static void clear_mqtt_state(modem_air780ep_t *self)
{
    if (!self) {
        return;
    }
    self->mqtt_configured = false;
    self->mqtt_tcp_connected = false;
    self->mqtt_session_connected = false;
    set_mqtt_data_enabled(self, false);
    free_mqtt_config(&self->mqtt_config);
}
```

- [ ] **Step 5: Update destroy/deinit cleanup**

In Air780EP destroy path, before freeing the object-specific resources, call:

```c
    clear_mqtt_state(self);
```

If the destroy function already clears event handlers and semaphores, keep that logic unchanged.

- [ ] **Step 6: Update `air780ep_mqtt_configure()`**

At the start, reject reconfigure while connected:

```c
    modem_air780ep_t *self = to_air780ep(me);
    ESP_RETURN_ON_FALSE(!self->mqtt_tcp_connected && !self->mqtt_session_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT is connected");
```

After `AT+MCONFIG` succeeds, store config:

```c
    if (ret == ESP_OK) {
        ret = copy_mqtt_config(&self->mqtt_config, config);
    }
    if (ret == ESP_OK) {
        self->mqtt_configured = true;
    }
```

- [ ] **Step 7: Update `air780ep_mqtt_tcp_connect()` to use cached config**

Change signature to no config argument and use:

```c
    modem_air780ep_t *self = to_air780ep(me);
    ESP_RETURN_ON_FALSE(self->mqtt_configured, ESP_ERR_INVALID_STATE, TAG, "MQTT not configured");
    ESP_RETURN_ON_FALSE(!self->mqtt_tcp_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT TCP already connected");

    const modem_mqtt_config_t *config = &self->mqtt_config;
```

Leave the existing `AT+MIPSTART` command construction intact, using `config->host` and `config->port`. On success set:

```c
    if (ret == ESP_OK) {
        self->mqtt_tcp_connected = true;
    }
```

- [ ] **Step 8: Update `air780ep_mqtt_connect()` to use cached config**

Change signature to no config argument and use:

```c
    modem_air780ep_t *self = to_air780ep(me);
    ESP_RETURN_ON_FALSE(self->mqtt_configured, ESP_ERR_INVALID_STATE, TAG, "MQTT not configured");
    ESP_RETURN_ON_FALSE(self->mqtt_tcp_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT TCP not connected");
    ESP_RETURN_ON_FALSE(!self->mqtt_session_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT already connected");

    const modem_mqtt_config_t *config = &self->mqtt_config;
```

Leave the `AT+MCONNECT` formatting intact, using `config->clean_session` and `config->keepalive_s`. On success set:

```c
    if (ret == ESP_OK) {
        self->mqtt_session_connected = true;
        set_mqtt_data_enabled(self, true);
    }
```

- [ ] **Step 9: Update `air780ep_mqtt_disconnect()` state handling**

Require a session before `MDISCONNECT`:

```c
    ESP_RETURN_ON_FALSE(self->mqtt_session_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT session not connected");
```

After successful `AT+MDISCONNECT`, clear session state:

```c
    if (ret == ESP_OK) {
        self->mqtt_session_connected = false;
    }
```

Keep `set_mqtt_data_enabled(self, false);` before sending the command.

- [ ] **Step 10: Add `air780ep_mqtt_tcp_disconnect()`**

Implement:

```c
static esp_err_t air780ep_mqtt_tcp_disconnect(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");

    modem_air780ep_t *self = to_air780ep(me);
    ESP_RETURN_ON_FALSE(self->mqtt_tcp_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT TCP not connected");
    ESP_RETURN_ON_FALSE(!self->mqtt_session_connected,
                        ESP_ERR_INVALID_STATE, TAG, "MQTT session still connected");

    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, "AT+MIPCLOSE", &ctx,
                             AIR780EP_MQTT_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MIPCLOSE");
    }
    if (ret == ESP_OK) {
        self->mqtt_tcp_connected = false;
    }
    return ret;
}
```

- [ ] **Step 11: Clear MQTT state on modem/network closures**

Where the current implementation calls `set_mqtt_data_enabled(self, false);` for broader modem reset, PDP deactivate, protocol closed, or destroy events, also clear session/TCP flags as appropriate:

```c
    self->mqtt_session_connected = false;
    self->mqtt_tcp_connected = false;
```

Do not free the cached config for transient network closure; only free it on destroy or successful reconfigure.

- [ ] **Step 12: Run tests and confirm remaining failures are Core/MQTT client only**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL remains until Core and MQTT client are updated.

## Task 4: Update Core Command Payloads and Dispatch

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`

- [ ] **Step 1: Update command enum and union in `core.h`**

Insert the new command after `CORE_CMD_MQTT_DISCONNECT`:

```c
    CORE_CMD_MQTT_TCP_DISCONNECT,        /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
```

Replace the MQTT config/connect union entries with:

```c
        struct {
            const char *client_id;       /**< 客户端 ID； Client ID */
            const char *username;        /**< 用户名； Username */
            const char *password;        /**< 密码； Password */
            const char *host;            /**< 主机； Host */
            uint16_t port;               /**< 端口； Port */
            bool clean_session;          /**< 清理会话； Clean session */
            uint16_t keepalive_s;        /**< 保活秒数； Keepalive seconds */
        } mqtt_config;                   /**< MQTT 配置； MQTT config */
```

Remove `mqtt_tcp_connect` and `mqtt_connect` payload structs.

- [ ] **Step 2: Update command type validation in `core.c`**

Ensure `core_cmd_type_valid()` accepts `CORE_CMD_MQTT_TCP_DISCONNECT`. The valid range should include the new enum before `CORE_CMD_PING`.

- [ ] **Step 3: Update command validation in `core.c`**

For `CORE_CMD_MQTT_CONFIGURE`, require complete config:

```c
    case CORE_CMD_MQTT_CONFIGURE:
        return cmd->data.mqtt_config.client_id &&
               cmd->data.mqtt_config.host &&
               cmd->data.mqtt_config.port > 0;
```

For no-payload commands, accept:

```c
    case CORE_CMD_MQTT_TCP_CONNECT:
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        return true;
```

- [ ] **Step 4: Update command cloning/freeing in `core.c`**

When cloning `CORE_CMD_MQTT_CONFIGURE`, clone all strings:

```c
        clone->data.mqtt_config.client_id = clone_optional_string(cmd->data.mqtt_config.client_id);
        clone->data.mqtt_config.username = clone_optional_string(cmd->data.mqtt_config.username);
        clone->data.mqtt_config.password = clone_optional_string(cmd->data.mqtt_config.password);
        clone->data.mqtt_config.host = clone_optional_string(cmd->data.mqtt_config.host);
        clone->data.mqtt_config.port = cmd->data.mqtt_config.port;
        clone->data.mqtt_config.clean_session = cmd->data.mqtt_config.clean_session;
        clone->data.mqtt_config.keepalive_s = cmd->data.mqtt_config.keepalive_s;
```

Free the four cloned strings in `free_core_cmd()`:

```c
        free((void *)cmd->data.mqtt_config.client_id);
        free((void *)cmd->data.mqtt_config.username);
        free((void *)cmd->data.mqtt_config.password);
        free((void *)cmd->data.mqtt_config.host);
```

- [ ] **Step 5: Update service command dispatch in `core_fsm.c`**

Use:

```c
    case CORE_CMD_MQTT_CONFIGURE: {
        modem_mqtt_config_t config = {
            .client_id = cmd->data.mqtt_config.client_id,
            .username = cmd->data.mqtt_config.username,
            .password = cmd->data.mqtt_config.password,
            .host = cmd->data.mqtt_config.host,
            .port = cmd->data.mqtt_config.port,
            .clean_session = cmd->data.mqtt_config.clean_session,
            .keepalive_s = cmd->data.mqtt_config.keepalive_s,
        };
        ret = modem_mqtt_configure(me->modem, &config);
        break;
    }
    case CORE_CMD_MQTT_TCP_CONNECT:
        ret = modem_mqtt_tcp_connect(me->modem);
        break;
    case CORE_CMD_MQTT_CONNECT:
        ret = modem_mqtt_connect(me->modem);
        break;
    case CORE_CMD_MQTT_DISCONNECT:
        ret = modem_mqtt_disconnect(me->modem);
        break;
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        ret = modem_mqtt_tcp_disconnect(me->modem);
        break;
```

- [ ] **Step 6: Run tests**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: FAIL remains until MQTT client stop flow is updated.

## Task 5: Update MQTT Client Command Submission and Stop Flow

**Files:**
- Modify: `src/mqtt_client/mqtt_client_priv.h`
- Modify: `src/mqtt_client/mqtt_client.c`

- [ ] **Step 1: Add stop phase enum to `mqtt_client_priv.h`**

Add after `mqtt_connect_step_t`:

```c
typedef enum {
    MQTT_STOP_STEP_IDLE = 0,
    MQTT_STOP_STEP_DISCONNECT,
    MQTT_STOP_STEP_TCP_DISCONNECT,
} mqtt_stop_step_t;
```

Add to `struct mqtt_client`:

```c
    mqtt_stop_step_t stop_step;
```

- [ ] **Step 2: Update `submit_core_cmd()` payload setup**

For `CORE_CMD_MQTT_CONFIGURE`, fill the complete config:

```c
    case CORE_CMD_MQTT_CONFIGURE:
        cmd.data.mqtt_config.client_id = me->config.client_id;
        cmd.data.mqtt_config.username = me->config.username;
        cmd.data.mqtt_config.password = me->config.password;
        cmd.data.mqtt_config.host = me->config.host;
        cmd.data.mqtt_config.port = me->config.port;
        cmd.data.mqtt_config.clean_session = me->config.clean_session;
        cmd.data.mqtt_config.keepalive_s = me->config.keepalive_s;
        break;
    case CORE_CMD_MQTT_TCP_CONNECT:
    case CORE_CMD_MQTT_CONNECT:
    case CORE_CMD_MQTT_DISCONNECT:
    case CORE_CMD_MQTT_TCP_DISCONNECT:
        break;
```

Remove old `mqtt_tcp_connect` and `mqtt_connect` payload assignment blocks.

- [ ] **Step 3: Track TCP open as soon as TCP connect succeeds**

Keep existing logic:

```c
    if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_CONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->transport_open = true;
    }
```

When `CORE_CMD_MQTT_TCP_DISCONNECT` succeeds, clear:

```c
    if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_DISCONNECT &&
        sig->core_result == CORE_CMD_RESULT_OK) {
        me->transport_open = false;
    }
```

- [ ] **Step 4: Split stop requests into session and TCP close phases**

Replace `request_stop_disconnect()` with phase-aware logic:

```c
static void request_stop_disconnect(mqtt_client_t *me)
{
    mqtt_client_state_t state = MQTT_CLIENT_STATE_STOPPED;
    (void)mqtt_client_get_state(me, &state);

    if (state == MQTT_CLIENT_STATE_CONNECTED && me->stop_step == MQTT_STOP_STEP_IDLE) {
        me->stop_step = MQTT_STOP_STEP_DISCONNECT;
        set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
        esp_err_t ret = submit_core_cmd(me, CORE_CMD_MQTT_DISCONNECT,
                                        MQTT_CLIENT_OPERATION_DISCONNECT, NULL);
        if (ret != ESP_OK) {
            complete_stop(me);
        }
        return;
    }

    if (me->transport_open) {
        me->stop_step = MQTT_STOP_STEP_TCP_DISCONNECT;
        set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
        esp_err_t ret = submit_core_cmd(me, CORE_CMD_MQTT_TCP_DISCONNECT,
                                        MQTT_CLIENT_OPERATION_DISCONNECT, NULL);
        if (ret != ESP_OK) {
            complete_stop(me);
        }
        return;
    }

    complete_stop(me);
}
```

- [ ] **Step 5: Continue stop sequence after each command completes**

In the `if (me->stop_requested)` branch in `handle_core_cmd_done()`, use:

```c
    if (me->stop_requested) {
        if (sig->core_cmd_type == CORE_CMD_MQTT_DISCONNECT &&
            sig->core_result == CORE_CMD_RESULT_OK) {
            set_state(me, MQTT_CLIENT_STATE_DISCONNECTING);
            request_stop_disconnect(me);
        } else if (sig->core_cmd_type == CORE_CMD_MQTT_TCP_DISCONNECT) {
            complete_stop(me);
        } else {
            request_stop_disconnect(me);
        }
        return;
    }
```

This intentionally completes stop even if TCP disconnect reports an error, matching the existing stop-cleanup behavior.

- [ ] **Step 6: Reset stop state in `complete_stop()`**

Add:

```c
    me->stop_step = MQTT_STOP_STEP_IDLE;
```

Keep existing resets for `pending_cmd`, `stop_requested`, `transport_open`, `net_online`, and `connect_step`.

- [ ] **Step 7: Run tests**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: PASS for static tests, or failures only for docs/stale-token checks added later.

## Task 6: Update Documentation and Stale References

**Files:**
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/at_cmd_air780ep.md`
- Modify: any matching files found by stale-token search under `docs/agents` and `docs/interview-preparation`

- [ ] **Step 1: Update class/API documentation**

Replace MQTT lifecycle references with:

```text
modem_mqtt_configure()      -> AT+MCONFIG, stores full MQTT config
modem_mqtt_tcp_connect()    -> AT+MIPSTART, uses cached host/port
modem_mqtt_connect()        -> AT+MCONNECT, uses cached clean_session/keepalive_s
modem_mqtt_disconnect()     -> AT+MDISCONNECT
modem_mqtt_tcp_disconnect() -> AT+MIPCLOSE
```

- [ ] **Step 2: Update Air780EP AT command notes**

Ensure `docs/agents/at_cmd_air780ep.md` says the disconnect order is:

```text
AT+MDISCONNECT
AT+MIPCLOSE
```

and maps `AT+MIPCLOSE` to `modem_mqtt_tcp_disconnect()`.

- [ ] **Step 3: Search for stale split types and old payload names**

Run:

`rg "modem_mqtt_tcp_config_t|modem_mqtt_connect_config_t|mqtt_tcp_connect\.host|mqtt_connect\.clean_session|CORE_CMD_MQTT_OPEN|CORE_CMD_MQTT_LOGIN|modem_mqtt_open|modem_mqtt_login" src tests docs/agents docs/interview-preparation`

Expected: no output.

## Task 7: Final Verification

**Files:**
- No code edits unless verification finds an issue.

- [ ] **Step 1: Run host tests**

Run: `PYTHONDONTWRITEBYTECODE=1 python -m pytest tests/host/test_mqtt_end_to_end_contract.py -q`

Expected: `28 passed` or updated count with all tests passing.

- [ ] **Step 2: Run whitespace check**

Run: `git diff --check`

Expected: no output.

- [ ] **Step 3: Run stale-token search**

Run: `rg "modem_mqtt_tcp_config_t|modem_mqtt_connect_config_t|mqtt_tcp_connect\.host|mqtt_connect\.clean_session|CORE_CMD_MQTT_OPEN|CORE_CMD_MQTT_LOGIN|modem_mqtt_open|modem_mqtt_login" src tests docs/agents docs/interview-preparation`

Expected: no output.

- [ ] **Step 4: Try ESP-IDF build if available**

Run: `idf.py build`

Expected: build passes. If the shell reports `idf.py: command not found`, record that ESP-IDF build could not be run in this environment.

- [ ] **Step 5: Inspect final diff**

Run: `git diff --stat`

Expected: changes limited to MQTT Modem/Core/client/tests/docs and this plan.

Run: `git diff`

Expected: diff implements the approved spec without unrelated edits.

Do not commit unless the user explicitly asks.
