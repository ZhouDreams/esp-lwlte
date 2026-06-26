# MQTT TLS and SSL Certificate Provisioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add reusable SSL certificate provisioning/status APIs and enable MQTT TLS for Air780EP and ML307R.

**Architecture:** Public Facade APIs submit synchronous Core service commands. Core owns command cloning/freeing and dispatches SSL work to Modem wrappers. Modem adapters implement module-specific certificate writes, certificate status queries, and MQTT TLS binding while MQTT service only carries transport/context configuration.

**Tech Stack:** C, ESP-IDF, FreeRTOS, ESP Event, AT command modem adapters, Python host contract tests, ESP-IDF build/flash tools.

---

## Source Map

- `src/include/lwlte.h`: add public SSL enums/structs/APIs and MQTT transport fields.
- `src/lwlte/lwlte.c`: implement `lwlte_ssl_provision()` and `lwlte_ssl_get_context_status()` as synchronous Facade APIs.
- `src/core/core.h`: add SSL command value objects, command enum entries, and MQTT transport/context fields.
- `src/core/core.c`: clone/free/validate new SSL commands and extended MQTT configure command.
- `src/core/core_fsm.c`: dispatch SSL commands and pass MQTT transport/context to Modem.
- `src/modem/modem.h`: add modem SSL value objects, MQTT transport fields, and SSL wrapper prototypes.
- `src/modem/modem_priv.h`: add SSL ops function pointer types and ops entries.
- `src/modem/modem.c`: implement SSL wrappers and argument validation.
- `src/mqtt_client/mqtt_client.h`: add service transport/context config fields.
- `src/mqtt_client/mqtt_client.c`: stop rejecting TLS, clone/store TLS config, pass it to Core.
- `src/modem/modem_air780ep.c`: implement Air780EP SSL provisioning/status and MQTT TLS binding through `SSLCFG` and `SSLMIPSTART`.
- `src/modem/modem_ml307r.c`: implement ML307R SSL provisioning/status and MQTT TLS binding through `MSSLCFG`, `MSSLCERTWR`, `MSSLKEYWR`, and `MQTTCFG="ssl"`.
- `example/Kconfig.projbuild`: add TLS example switches and CA PEM config path/content option.
- `example/air780ep_mqtt_client.c`: optionally provision SSL and run MQTT over TLS on port `8883`.
- `example/ml307r_mqtt_client.c`: optionally provision SSL and run MQTT over TLS on port `8883`.
- `tests/host/test_mqtt_tls_ssl_contract.py`: new static contract tests for API shape, layering, command mapping, and examples.
- `tests/host/test_mqtt_end_to_end_contract.py`: adjust existing MQTT Facade boundary checks so they remain scoped to MQTT wrappers while allowing the new SSL Facade APIs to submit Core commands directly.
- `docs/agents/feature-roadmap.md`: mark selected SSL provisioning design.

## Execution Notes

- Do not modify unrelated user edits in `docs/agents/at_cmd_air780ep.md` or `docs/agents/at_cmd_ml307r.md`.
- Do not create commits unless the user explicitly authorizes a commit. Commit steps below are approval checkpoints, not permission to run `git commit`.
- Prefer small patches. For large source files, edit one responsibility at a time.
- Run host contract tests before implementation to verify they fail for the intended missing behavior.
- Use the ESP-IDF MCP build tool for build verification.
- Hardware validation is required before claiming implementation complete.

### Task 1: Add Failing Host Contract Tests

**Files:**
- Create: `tests/host/test_mqtt_tls_ssl_contract.py`

- [ ] **Step 1: Create the failing contract test file**

Add `tests/host/test_mqtt_tls_ssl_contract.py` with these checks:

```python
#!/usr/bin/env python3
"""Static contract checks for MQTT TLS and reusable SSL provisioning."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class MqttTlsSslContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.modem_priv_h = read("src/modem/modem_priv.h")
        cls.modem_c = read("src/modem/modem.c")
        cls.mqtt_h = read("src/mqtt_client/mqtt_client.h")
        cls.mqtt_c = read("src/mqtt_client/mqtt_client.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")
        cls.example_kconfig = read("example/Kconfig.projbuild")
        cls.air_example = read("example/air780ep_mqtt_client.c")
        cls.ml_example = read("example/ml307r_mqtt_client.c")
        cls.roadmap = read("docs/agents/feature-roadmap.md")

    def assert_has_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_public_ssl_api_and_mqtt_transport_exist(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_SSL_AUTH_NONE",
            "LWLTE_SSL_AUTH_SERVER",
            "LWLTE_SSL_AUTH_MUTUAL",
            "lwlte_ssl_context_config_t",
            "lwlte_ssl_credentials_t",
            "lwlte_ssl_context_status_t",
            "bool provisioned;",
            "bool ca_cert_present;",
            "bool client_cert_present;",
            "bool client_key_present;",
            "bool check_valid;",
            "esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,",
            "esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,",
            "LWLTE_MQTT_TRANSPORT_PLAIN_TCP",
            "LWLTE_MQTT_TRANSPORT_TLS",
            "lwlte_mqtt_transport_t transport;",
            "uint8_t ssl_context_id;",
        ], "lwlte.h")

    def test_core_and_modem_have_ssl_command_path(self):
        self.assert_has_all(self.core_h + self.core_c + self.core_fsm_c, [
            "CORE_CMD_SSL_PROVISION",
            "CORE_CMD_SSL_GET_CONTEXT_STATUS",
            "return type >= CORE_CMD_SSL_PROVISION",
            "core_ssl_context_config_t",
            "core_ssl_credentials_t",
            "core_ssl_context_status_t",
            "modem_ssl_provision",
            "modem_ssl_get_context_status",
            "ca_cert_pem",
            "client_key_pem",
            "ssl_cmd && ret != ESP_OK",
        ], "core SSL path")
        self.assert_has_all(self.modem_h + self.modem_priv_h + self.modem_c, [
            "modem_ssl_context_config_t",
            "modem_ssl_credentials_t",
            "modem_ssl_context_status_t",
            "modem_ssl_provision",
            "modem_ssl_get_context_status",
            "ssl_provision",
            "ssl_get_context_status",
        ], "modem SSL path")

    def test_mqtt_service_carries_tls_without_crossing_boundaries(self):
        self.assert_has_all(self.mqtt_h + self.mqtt_c, [
            "MQTT_CLIENT_TRANSPORT_TLS",
            "ssl_context_id",
            "cmd.data.mqtt_config.transport",
            "cmd.data.mqtt_config.ssl_context_id",
        ], "mqtt TLS path")
        self.assertNotIn("rejects TLS", self.mqtt_c)
        self.assertNotIn("config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS) {\n        return NULL;", self.mqtt_c)
        for forbidden in ['#include "modem.h"', '#include "at_engine.h"', '#include "core_priv.h"']:
            self.assertNotIn(forbidden, self.mqtt_c + self.mqtt_h)

    def test_air780ep_ssl_mapping_exists(self):
        self.assert_has_all(self.air_c, [
            "AIR780EP_SSL_MQTT_CONTEXT_ID",
            "AIR780EP_SSL_MAX_PEM_LEN",
            "air780ep_ssl_provision",
            "air780ep_ssl_get_context_status",
            "air780ep_query_ssl_auth_mode",
            "air780ep_clear_ssl_state",
            "AT+FSCREATE=",
            "AT+FSWRITE=",
            "AT+FSFLSIZE=",
            "AT+SSLCFG=\"cacert\",88",
            "AT+SSLCFG=\"seclevel\",88",
            "AT+SSLMIPSTART=",
            ".ssl_provision = air780ep_ssl_provision",
            ".ssl_get_context_status = air780ep_ssl_get_context_status",
        ], "Air780EP SSL mapping")

    def test_ml307r_ssl_mapping_exists(self):
        self.assert_has_all(self.ml_c, [
            "ML307R_SSL_MAX_PEM_LEN",
            "ml307r_ssl_provision",
            "ml307r_ssl_get_context_status",
            "ml307r_query_ssl_auth_mode",
            "ml307r_clear_ssl_state",
            "AT+MSSLCERTWR=",
            "AT+MSSLKEYWR=",
            "AT+MSSLCFG=\"auth\"",
            "AT+MSSLCFG=\"cert\"",
            "AT+MSSLCFG=\"ignoreverify\"",
            "AT+MSSLLIST=",
            "AT+MSSLCHECK=",
            "AT+MQTTCFG=\"ssl\",0,1",
            "AT+MQTTCFG=\"ssl\",0,0",
            ".ssl_provision = ml307r_ssl_provision",
            ".ssl_get_context_status = ml307r_ssl_get_context_status",
        ], "ML307R SSL mapping")

    def test_examples_support_tls_8883_validation_path(self):
        self.assert_has_all(self.example_kconfig, [
            "EXAMPLE_MQTT_TLS_ENABLE",
            "EXAMPLE_MQTT_TLS_CA_CERT_PEM",
            "default 8883 if EXAMPLE_MQTT_TLS_ENABLE",
        ], "example Kconfig")
        for label, text in [("Air780EP example", self.air_example), ("ML307R example", self.ml_example)]:
            self.assert_has_all(text, [
                "CONFIG_EXAMPLE_MQTT_TLS_ENABLE",
                "lwlte_ssl_provision",
                "lwlte_ssl_get_context_status",
                "LWLTE_MQTT_TRANSPORT_TLS",
                "8883",
                "v1/devices/me/telemetry",
                "v1/devices/me/attributes",
            ], label)

    def test_roadmap_mentions_selected_ssl_design(self):
        self.assertIn("CORE_CMD_SSL_PROVISION", self.roadmap)
        self.assertIn("CORE_CMD_SSL_GET_CONTEXT_STATUS", self.roadmap)
        self.assertIn("lwlte_ssl_get_context_status", self.roadmap)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the new contract test and verify failure**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract -v`

Expected: FAIL because SSL public API, Core commands, Modem ops, module mappings, and example TLS config do not exist yet.

- [ ] **Step 3: Approval checkpoint instead of commit**

Inspect: `git diff -- tests/host/test_mqtt_tls_ssl_contract.py`

Expected: diff contains only the new failing test file.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 2: Add Public API Types and MQTT Transport Fields

**Files:**
- Modify: `src/include/lwlte.h`

- [ ] **Step 1: Run the focused contract test before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_public_ssl_api_and_mqtt_transport_exist -v`

Expected: FAIL because `LWLTE_SSL_AUTH_NONE` is missing.

- [ ] **Step 2: Add public SSL and MQTT transport declarations**

In `src/include/lwlte.h`, add these typedefs before `lwlte_mqtt_config_t`:

```c
/**
 * @brief LTE SSL 认证模式
 * @details LTE SSL authentication mode
 */
typedef enum {
    LWLTE_SSL_AUTH_NONE = 0,        /**< 不认证； No authentication */
    LWLTE_SSL_AUTH_SERVER,          /**< 服务器认证； Server authentication */
    LWLTE_SSL_AUTH_MUTUAL,          /**< 双向认证； Mutual authentication */
} lwlte_ssl_auth_mode_t;

/**
 * @brief LTE MQTT 传输类型
 * @details LTE MQTT transport type
 */
typedef enum {
    LWLTE_MQTT_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    LWLTE_MQTT_TRANSPORT_TLS,            /**< TLS； TLS */
} lwlte_mqtt_transport_t;

/**
 * @brief LTE SSL context 配置
 * @details LTE SSL context configuration
 */
typedef struct {
    uint8_t context_id;                  /**< SSL context ID； SSL context ID */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
    uint8_t tls_version;                 /**< TLS 版本，0 使用模块默认； TLS version, 0 uses module default */
    uint32_t negotiate_timeout_s;        /**< 协商超时秒数，0 使用模块默认； Negotiation timeout seconds, 0 uses module default */
    bool ignore_cert_time;               /**< 是否忽略证书时间； Whether to ignore certificate time */
    const char *hostname;                /**< 可选主机名/SNI； Optional hostname/SNI */
} lwlte_ssl_context_config_t;

/**
 * @brief LTE SSL 证书材料
 * @details LTE SSL credential material
 */
typedef struct {
    const uint8_t *ca_cert_pem;          /**< CA 证书 PEM； CA certificate PEM */
    size_t ca_cert_len;                  /**< CA 证书长度； CA certificate length */
    const uint8_t *client_cert_pem;      /**< 客户端证书 PEM； Client certificate PEM */
    size_t client_cert_len;              /**< 客户端证书长度； Client certificate length */
    const uint8_t *client_key_pem;       /**< 客户端私钥 PEM； Client private key PEM */
    size_t client_key_len;               /**< 客户端私钥长度； Client private key length */
} lwlte_ssl_credentials_t;

/**
 * @brief LTE SSL context 状态
 * @details LTE SSL context status
 */
typedef struct {
    bool provisioned;                    /**< 必需对象是否已存在； Whether required objects exist */
    bool ca_cert_present;                /**< CA 证书是否存在； Whether CA certificate exists */
    bool client_cert_present;            /**< 客户端证书是否存在； Whether client certificate exists */
    bool client_key_present;             /**< 客户端私钥是否存在； Whether client key exists */
    bool check_valid;                    /**< 模块校验是否通过； Whether module check passed */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 查询到的认证模式； Queried authentication mode */
} lwlte_ssl_context_status_t;
```

Extend `lwlte_mqtt_config_t` with these fields after `port`:

```c
    lwlte_mqtt_transport_t transport;     /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;               /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
```

Add public function prototypes near other Facade APIs:

```c
esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials);
esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status);
```

- [ ] **Step 3: Run the focused contract test**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_public_ssl_api_and_mqtt_transport_exist -v`

Expected: PASS.

- [ ] **Step 4: Approval checkpoint instead of commit**

Inspect: `git diff -- src/include/lwlte.h tests/host/test_mqtt_tls_ssl_contract.py`

Expected: public API changes and the contract test only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 3: Add Modem SSL Value Objects, Ops, and Wrappers

**Files:**
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_priv.h`
- Modify: `src/modem/modem.c`

- [ ] **Step 1: Run the focused contract test before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_core_and_modem_have_ssl_command_path -v`

Expected: FAIL because the shared SSL command path does not exist yet.

- [ ] **Step 2: Add modem SSL/MQTT enums, structs, and MQTT fields**

In `src/modem/modem.h`, add modem-owned enums and SSL structs. Do not make `modem.h` depend on Facade public `lwlte_*` types.

```c
typedef enum {
    MODEM_SSL_AUTH_NONE = 0,             /**< 不认证； No authentication */
    MODEM_SSL_AUTH_SERVER,               /**< 服务器认证； Server authentication */
    MODEM_SSL_AUTH_MUTUAL,               /**< 双向认证； Mutual authentication */
} modem_ssl_auth_mode_t;

typedef enum {
    MODEM_MQTT_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    MODEM_MQTT_TRANSPORT_TLS,            /**< TLS； TLS */
} modem_mqtt_transport_t;

typedef struct {
    uint8_t context_id;                  /**< SSL context ID； SSL context ID */
    modem_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
    uint8_t tls_version;                 /**< TLS 版本； TLS version */
    uint32_t negotiate_timeout_s;        /**< 协商超时秒数； Negotiation timeout seconds */
    bool ignore_cert_time;               /**< 是否忽略证书时间； Whether to ignore certificate time */
    const char *hostname;                /**< 主机名/SNI； Hostname/SNI */
} modem_ssl_context_config_t;

typedef struct {
    const uint8_t *ca_cert_pem;          /**< CA 证书 PEM； CA certificate PEM */
    size_t ca_cert_len;                  /**< CA 证书长度； CA certificate length */
    const uint8_t *client_cert_pem;      /**< 客户端证书 PEM； Client certificate PEM */
    size_t client_cert_len;              /**< 客户端证书长度； Client certificate length */
    const uint8_t *client_key_pem;       /**< 客户端私钥 PEM； Client private key PEM */
    size_t client_key_len;               /**< 客户端私钥长度； Client private key length */
} modem_ssl_credentials_t;

typedef struct {
    bool provisioned;                    /**< 必需对象是否已存在； Whether required objects exist */
    bool ca_cert_present;                /**< CA 证书是否存在； Whether CA certificate exists */
    bool client_cert_present;            /**< 客户端证书是否存在； Whether client certificate exists */
    bool client_key_present;             /**< 客户端私钥是否存在； Whether client key exists */
    bool check_valid;                    /**< 模块校验是否通过； Whether module check passed */
    modem_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
} modem_ssl_context_status_t;
```

Extend `modem_mqtt_config_t`:

```c
    modem_mqtt_transport_t transport; /**< MQTT 传输； MQTT transport */
    uint8_t ssl_context_id;      /**< SSL context ID； SSL context ID */
```

Add wrapper prototypes:

```c
esp_err_t modem_ssl_provision(modem_handle_t *me,
                              const modem_ssl_context_config_t *config,
                              const modem_ssl_credentials_t *credentials);
esp_err_t modem_ssl_get_context_status(modem_handle_t *me,
                                       uint8_t context_id,
                                       modem_ssl_context_status_t *status);
```

- [ ] **Step 3: Add modem ops entries**

In `src/modem/modem_priv.h`, add function pointer typedefs:

```c
typedef esp_err_t (*modem_ssl_provision_fn)(modem_handle_t *me,
                                            const modem_ssl_context_config_t *config,
                                            const modem_ssl_credentials_t *credentials);
typedef esp_err_t (*modem_ssl_get_context_status_fn)(modem_handle_t *me,
                                                     uint8_t context_id,
                                                     modem_ssl_context_status_t *status);
```

Add to `modem_ops_t` after PDP or before MQTT:

```c
    /* ── SSL/TLS context； SSL/TLS context ─────────────────── */
    modem_ssl_provision_fn ssl_provision;              /**< 写入并配置 SSL context； Provision SSL context */
    modem_ssl_get_context_status_fn ssl_get_context_status; /**< 查询 SSL context 状态； Query SSL context status */
```

- [ ] **Step 4: Implement wrappers and validation**

In `src/modem/modem.c`, add helpers near MQTT wrappers:

```c
static bool ssl_credentials_valid(modem_ssl_auth_mode_t auth,
                                  const modem_ssl_credentials_t *credentials)
{
    if (!credentials || auth > MODEM_SSL_AUTH_MUTUAL) {
        return false;
    }
    bool ca_pair = (!credentials->ca_cert_pem && credentials->ca_cert_len == 0) ||
                   (credentials->ca_cert_pem && credentials->ca_cert_len > 0);
    bool cert_pair = (!credentials->client_cert_pem && credentials->client_cert_len == 0) ||
                     (credentials->client_cert_pem && credentials->client_cert_len > 0);
    bool key_pair = (!credentials->client_key_pem && credentials->client_key_len == 0) ||
                    (credentials->client_key_pem && credentials->client_key_len > 0);
    if (!ca_pair || !cert_pair || !key_pair) {
        return false;
    }
    if (auth == MODEM_SSL_AUTH_SERVER) {
        return credentials->ca_cert_pem && credentials->ca_cert_len > 0;
    }
    if (auth == MODEM_SSL_AUTH_MUTUAL) {
        return credentials->ca_cert_pem && credentials->ca_cert_len > 0 &&
               credentials->client_cert_pem && credentials->client_cert_len > 0 &&
               credentials->client_key_pem && credentials->client_key_len > 0;
    }
    return true;
}
```

Add wrappers:

```c
esp_err_t modem_ssl_provision(modem_handle_t *me,
                              const modem_ssl_context_config_t *config,
                              const modem_ssl_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(me && config && ssl_credentials_valid(config->auth_mode, credentials),
                        ESP_ERR_INVALID_ARG, TAG, "invalid SSL provision args");
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ssl_provision,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ssl_provision not supported");
    return me->ops->ssl_provision(me, config, credentials);
}

esp_err_t modem_ssl_get_context_status(modem_handle_t *me,
                                       uint8_t context_id,
                                       modem_ssl_context_status_t *status)
{
    ESP_RETURN_ON_FALSE(me && status, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    memset(status, 0, sizeof(*status));
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->ssl_get_context_status,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ssl_get_context_status not supported");
    return me->ops->ssl_get_context_status(me, context_id, status);
}
```

Update `modem_mqtt_configure()` validation to allow only `MODEM_MQTT_TRANSPORT_PLAIN_TCP` or `MODEM_MQTT_TRANSPORT_TLS`.

- [ ] **Step 5: Run focused Core/Modem contract test**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_core_and_modem_have_ssl_command_path -v`

Expected: still FAIL until Core SSL commands are added in Task 4, but Modem-specific missing tokens are gone.

- [ ] **Step 6: Approval checkpoint instead of commit**

Inspect: `git diff -- src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c`

Expected: shared SSL API path only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 4: Add Core SSL Commands and Command Lifetime Handling

**Files:**
- Modify: `src/core/core.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`

- [ ] **Step 1: Run the focused contract test before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_core_and_modem_have_ssl_command_path -v`

Expected: FAIL because `CORE_CMD_SSL_PROVISION` is missing.

- [ ] **Step 2: Add Core SSL value objects and command data**

In `src/core/core.h`, add Core SSL structs near existing Core command value objects:

```c
typedef struct {
    uint8_t context_id;                  /**< SSL context ID； SSL context ID */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
    uint8_t tls_version;                 /**< TLS 版本； TLS version */
    uint32_t negotiate_timeout_s;        /**< 协商超时秒数； Negotiation timeout seconds */
    bool ignore_cert_time;               /**< 是否忽略证书时间； Whether to ignore certificate time */
    const char *hostname;                /**< 主机名/SNI； Hostname/SNI */
} core_ssl_context_config_t;

typedef struct {
    const uint8_t *ca_cert_pem;          /**< CA 证书 PEM； CA certificate PEM */
    size_t ca_cert_len;                  /**< CA 证书长度； CA certificate length */
    const uint8_t *client_cert_pem;      /**< 客户端证书 PEM； Client certificate PEM */
    size_t client_cert_len;              /**< 客户端证书长度； Client certificate length */
    const uint8_t *client_key_pem;       /**< 客户端私钥 PEM； Client private key PEM */
    size_t client_key_len;               /**< 客户端私钥长度； Client private key length */
} core_ssl_credentials_t;

typedef struct {
    bool provisioned;                    /**< 必需对象是否已存在； Whether required objects exist */
    bool ca_cert_present;                /**< CA 证书是否存在； Whether CA certificate exists */
    bool client_cert_present;            /**< 客户端证书是否存在； Whether client certificate exists */
    bool client_key_present;             /**< 客户端私钥是否存在； Whether client key exists */
    bool check_valid;                    /**< 模块校验是否通过； Whether module check passed */
    lwlte_ssl_auth_mode_t auth_mode;     /**< 认证模式； Authentication mode */
} core_ssl_context_status_t;
```

Add command enum values before MQTT commands so SSL commands are part of service command range:

```c
    CORE_CMD_SSL_PROVISION = 0,          /**< 写入并配置 SSL context； Provision SSL context */
    CORE_CMD_SSL_GET_CONTEXT_STATUS,     /**< 查询 SSL context 状态； Query SSL context status */
    CORE_CMD_MQTT_CONFIGURE,             /**< 配置 MQTT； Configure MQTT */
```

Extend `data.mqtt_config`:

```c
            lwlte_mqtt_transport_t transport; /**< MQTT 传输； MQTT transport */
            uint8_t ssl_context_id;      /**< SSL context ID； SSL context ID */
```

Add command union members:

```c
        struct {
            core_ssl_context_config_t config;
            core_ssl_credentials_t credentials;
        } ssl_provision;                 /**< SSL provision 参数； SSL provision args */
        struct {
            uint8_t context_id;          /**< SSL context ID； SSL context ID */
            core_ssl_context_status_t *status; /**< 状态输出； Status output */
        } ssl_get_context_status;        /**< SSL status 参数； SSL status args */
```

- [ ] **Step 3: Clone/free/validate SSL commands**

In `src/core/core.c`, update `clone_core_cmd()`:

```c
    case CORE_CMD_SSL_PROVISION:
        clone->data.ssl_provision.config.hostname =
            clone_optional_string(cmd->data.ssl_provision.config.hostname);
        clone->data.ssl_provision.credentials.ca_cert_pem =
            clone_payload(cmd->data.ssl_provision.credentials.ca_cert_pem,
                          cmd->data.ssl_provision.credentials.ca_cert_len);
        clone->data.ssl_provision.credentials.client_cert_pem =
            clone_payload(cmd->data.ssl_provision.credentials.client_cert_pem,
                          cmd->data.ssl_provision.credentials.client_cert_len);
        clone->data.ssl_provision.credentials.client_key_pem =
            clone_payload(cmd->data.ssl_provision.credentials.client_key_pem,
                          cmd->data.ssl_provision.credentials.client_key_len);
        if ((cmd->data.ssl_provision.config.hostname &&
             !clone->data.ssl_provision.config.hostname) ||
            (cmd->data.ssl_provision.credentials.ca_cert_pem &&
             !clone->data.ssl_provision.credentials.ca_cert_pem) ||
            (cmd->data.ssl_provision.credentials.client_cert_pem &&
             !clone->data.ssl_provision.credentials.client_cert_pem) ||
            (cmd->data.ssl_provision.credentials.client_key_pem &&
             !clone->data.ssl_provision.credentials.client_key_pem)) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_SSL_GET_CONTEXT_STATUS:
        break;
```

Update MQTT clone to copy the new fields:

```c
        clone->data.mqtt_config.transport = cmd->data.mqtt_config.transport;
        clone->data.mqtt_config.ssl_context_id = cmd->data.mqtt_config.ssl_context_id;
```

Update `free_core_cmd()`:

```c
    case CORE_CMD_SSL_PROVISION:
        free((void *)cmd->data.ssl_provision.config.hostname);
        free((void *)cmd->data.ssl_provision.credentials.ca_cert_pem);
        free((void *)cmd->data.ssl_provision.credentials.client_cert_pem);
        free((void *)cmd->data.ssl_provision.credentials.client_key_pem);
        break;
```

Update `core_cmd_valid()`:

```c
    case CORE_CMD_SSL_PROVISION: {
        const core_ssl_credentials_t *creds = &cmd->data.ssl_provision.credentials;
        lwlte_ssl_auth_mode_t auth = cmd->data.ssl_provision.config.auth_mode;
        bool ca_pair = (!creds->ca_cert_pem && creds->ca_cert_len == 0) ||
                       (creds->ca_cert_pem && creds->ca_cert_len > 0);
        bool cert_pair = (!creds->client_cert_pem && creds->client_cert_len == 0) ||
                         (creds->client_cert_pem && creds->client_cert_len > 0);
        bool key_pair = (!creds->client_key_pem && creds->client_key_len == 0) ||
                        (creds->client_key_pem && creds->client_key_len > 0);
        if (auth > LWLTE_SSL_AUTH_MUTUAL || !ca_pair || !cert_pair || !key_pair) {
            return false;
        }
        if (auth == LWLTE_SSL_AUTH_SERVER) {
            return creds->ca_cert_pem && creds->ca_cert_len > 0;
        }
        if (auth == LWLTE_SSL_AUTH_MUTUAL) {
            return creds->ca_cert_pem && creds->ca_cert_len > 0 &&
                   creds->client_cert_pem && creds->client_cert_len > 0 &&
                   creds->client_key_pem && creds->client_key_len > 0;
        }
        return true;
    }
    case CORE_CMD_SSL_GET_CONTEXT_STATUS:
        return cmd->data.ssl_get_context_status.status != NULL;
```

Update MQTT validation:

```c
               (cmd->data.mqtt_config.transport == LWLTE_MQTT_TRANSPORT_PLAIN_TCP ||
                cmd->data.mqtt_config.transport == LWLTE_MQTT_TRANSPORT_TLS);
```

Update `core_cmd_type_valid()` so the newly inserted SSL commands are accepted:

```c
static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_SSL_PROVISION && type <= CORE_CMD_SOCKET_CLOSE;
}
```

- [ ] **Step 4: Dispatch SSL commands in Core FSM**

In `src/core/core_fsm.c`, update invalid state handling so SSL commands are allowed in `CORE_STATE_READY`, `CORE_STATE_NET_ACTIVATING`, and `CORE_STATE_ONLINE`, but still rejected in stopped/error/destroying.

Add these switch cases before MQTT cases:

```c
    case CORE_CMD_SSL_PROVISION: {
        modem_ssl_auth_mode_t auth_mode = MODEM_SSL_AUTH_NONE;
        if (cmd->data.ssl_provision.config.auth_mode == LWLTE_SSL_AUTH_SERVER) {
            auth_mode = MODEM_SSL_AUTH_SERVER;
        } else if (cmd->data.ssl_provision.config.auth_mode == LWLTE_SSL_AUTH_MUTUAL) {
            auth_mode = MODEM_SSL_AUTH_MUTUAL;
        }
        modem_ssl_context_config_t config = {
            .context_id = cmd->data.ssl_provision.config.context_id,
            .auth_mode = auth_mode,
            .tls_version = cmd->data.ssl_provision.config.tls_version,
            .negotiate_timeout_s = cmd->data.ssl_provision.config.negotiate_timeout_s,
            .ignore_cert_time = cmd->data.ssl_provision.config.ignore_cert_time,
            .hostname = cmd->data.ssl_provision.config.hostname,
        };
        modem_ssl_credentials_t credentials = {
            .ca_cert_pem = cmd->data.ssl_provision.credentials.ca_cert_pem,
            .ca_cert_len = cmd->data.ssl_provision.credentials.ca_cert_len,
            .client_cert_pem = cmd->data.ssl_provision.credentials.client_cert_pem,
            .client_cert_len = cmd->data.ssl_provision.credentials.client_cert_len,
            .client_key_pem = cmd->data.ssl_provision.credentials.client_key_pem,
            .client_key_len = cmd->data.ssl_provision.credentials.client_key_len,
        };
        ret = modem_ssl_provision(me->modem, &config, &credentials);
        break;
    }
    case CORE_CMD_SSL_GET_CONTEXT_STATUS: {
        modem_ssl_context_status_t modem_status = {0};
        ret = modem_ssl_get_context_status(me->modem,
                                           cmd->data.ssl_get_context_status.context_id,
                                           &modem_status);
        if (ret == ESP_OK && cmd->data.ssl_get_context_status.status) {
            cmd->data.ssl_get_context_status.status->provisioned = modem_status.provisioned;
            cmd->data.ssl_get_context_status.status->ca_cert_present = modem_status.ca_cert_present;
            cmd->data.ssl_get_context_status.status->client_cert_present = modem_status.client_cert_present;
            cmd->data.ssl_get_context_status.status->client_key_present = modem_status.client_key_present;
            cmd->data.ssl_get_context_status.status->check_valid = modem_status.check_valid;
            cmd->data.ssl_get_context_status.status->auth_mode = LWLTE_SSL_AUTH_NONE;
            if (modem_status.auth_mode == MODEM_SSL_AUTH_SERVER) {
                cmd->data.ssl_get_context_status.status->auth_mode = LWLTE_SSL_AUTH_SERVER;
            } else if (modem_status.auth_mode == MODEM_SSL_AUTH_MUTUAL) {
                cmd->data.ssl_get_context_status.status->auth_mode = LWLTE_SSL_AUTH_MUTUAL;
            }
        }
        break;
    }
```

Update MQTT config dispatch to include:

```c
            .transport = cmd->data.mqtt_config.transport == LWLTE_MQTT_TRANSPORT_TLS ?
                         MODEM_MQTT_TRANSPORT_TLS : MODEM_MQTT_TRANSPORT_PLAIN_TCP,
            .ssl_context_id = cmd->data.mqtt_config.ssl_context_id,
```

Update the final `finish_service_cmd()` result data handling so SSL command errors preserve the specific `esp_err_t` for the synchronous Facade APIs:

```c
    bool socket_cmd = cmd->type >= CORE_CMD_SOCKET_OPEN &&
                      cmd->type <= CORE_CMD_SOCKET_CLOSE;
    bool ssl_cmd = cmd->type == CORE_CMD_SSL_PROVISION ||
                   cmd->type == CORE_CMD_SSL_GET_CONTEXT_STATUS;
    const core_socket_result_t result = {
        .error_code = ret,
        .modem_error_code = modem_error_code,
    };
    finish_service_cmd(me, cmd, result_from_esp_err(ret),
                       socket_cmd && ret != ESP_OK ? (const void *)&result :
                       ssl_cmd && ret != ESP_OK ? (const void *)&ret : NULL);
```

- [ ] **Step 5: Run focused Core test**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_core_and_modem_have_ssl_command_path -v`

Expected: PASS.

- [ ] **Step 6: Approval checkpoint instead of commit**

Inspect: `git diff -- src/core/core.h src/core/core.c src/core/core_fsm.c`

Expected: only SSL command additions and MQTT transport/context propagation.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 5: Implement Facade SSL APIs

**Files:**
- Modify: `src/lwlte/lwlte.c`

- [ ] **Step 1: Run public API compile contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_public_ssl_api_and_mqtt_transport_exist -v`

Expected: PASS after Task 2.

- [ ] **Step 2: Add synchronous Core command helper types**

In `src/lwlte/lwlte.c`, add a small private context type near static typedefs:

```c
typedef struct {
    SemaphoreHandle_t done;
    core_cmd_result_t result;
    esp_err_t error_code;
} lwlte_sync_cmd_ctx_t;
```

Add a static callback prototype:

```c
static void facade_core_cmd_done_cb(core_handle_t *core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx);
```

Implement callback near static functions:

```c
static void facade_core_cmd_done_cb(core_handle_t *core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx)
{
    (void)core;
    (void)type;
    lwlte_sync_cmd_ctx_t *ctx = (lwlte_sync_cmd_ctx_t *)user_ctx;
    if (!ctx) {
        return;
    }
    ctx->result = result;
    if (result == CORE_CMD_RESULT_OK) {
        ctx->error_code = ESP_OK;
    } else if (result_data) {
        ctx->error_code = *(const esp_err_t *)result_data;
    } else {
        ctx->error_code = ESP_FAIL;
    }
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}
```

- [ ] **Step 3: Add SSL Facade APIs**

In `src/lwlte/lwlte.c`, add these public functions after `lwlte_ping()` or before `lwlte_mqtt_init()`:

```c
esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials)
{
    ESP_RETURN_ON_FALSE(config && credentials, ESP_ERR_INVALID_ARG, TAG,
                        "invalid SSL provision args");

    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_sync_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_cmd_t cmd = {
        .type = CORE_CMD_SSL_PROVISION,
        .done_cb = facade_core_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = 60000,
    };
    cmd.data.ssl_provision.config.context_id = config->context_id;
    cmd.data.ssl_provision.config.auth_mode = config->auth_mode;
    cmd.data.ssl_provision.config.tls_version = config->tls_version;
    cmd.data.ssl_provision.config.negotiate_timeout_s = config->negotiate_timeout_s;
    cmd.data.ssl_provision.config.ignore_cert_time = config->ignore_cert_time;
    cmd.data.ssl_provision.config.hostname = config->hostname;
    cmd.data.ssl_provision.credentials.ca_cert_pem = credentials->ca_cert_pem;
    cmd.data.ssl_provision.credentials.ca_cert_len = credentials->ca_cert_len;
    cmd.data.ssl_provision.credentials.client_cert_pem = credentials->client_cert_pem;
    cmd.data.ssl_provision.credentials.client_cert_len = credentials->client_cert_len;
    cmd.data.ssl_provision.credentials.client_key_pem = credentials->client_key_pem;
    cmd.data.ssl_provision.credentials.client_key_len = credentials->client_key_len;

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
    }
    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    memset(status, 0, sizeof(*status));

    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_sync_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    core_ssl_context_status_t core_status = {0};
    core_cmd_t cmd = {
        .type = CORE_CMD_SSL_GET_CONTEXT_STATUS,
        .done_cb = facade_core_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = 30000,
    };
    cmd.data.ssl_get_context_status.context_id = context_id;
    cmd.data.ssl_get_context_status.status = &core_status;

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
    }
    if (ret == ESP_OK) {
        status->provisioned = core_status.provisioned;
        status->ca_cert_present = core_status.ca_cert_present;
        status->client_cert_present = core_status.client_cert_present;
        status->client_key_present = core_status.client_key_present;
        status->check_valid = core_status.check_valid;
        status->auth_mode = core_status.auth_mode;
    }
    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}
```

- [ ] **Step 4: Run Facade-focused host checks**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_public_ssl_api_and_mqtt_transport_exist -v`

Expected: PASS.

- [ ] **Step 5: Update existing MQTT Facade host contract scope**

In `tests/host/test_mqtt_end_to_end_contract.py`, update `test_facade_mqtt_wrappers_use_mqtt_client_layer_only()` so its forbidden-token scan applies only to the `lwlte_mqtt_init()` through `lwlte_mqtt_destroy()` slice stored in `api_body`, not the whole `lwlte.c` file. Keep the same forbidden MQTT tokens:

```python
        for forbidden in [
            "core_submit_cmd",
            "CORE_CMD_MQTT_CONFIGURE",
            "CORE_CMD_MQTT_TCP_CONNECT",
            "CORE_CMD_MQTT_CONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "CORE_CMD_MQTT_DISCONNECT",
        ]:
            self.assertNotIn(forbidden, api_body)
```

This preserves the MQTT service boundary while allowing `lwlte_ssl_provision()` and `lwlte_ssl_get_context_status()` to submit their own synchronous SSL Core commands.

- [ ] **Step 6: Run existing MQTT Facade host contract**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract.MqttEndToEndContractTest.test_facade_mqtt_wrappers_use_mqtt_client_layer_only -v`

Expected: PASS.

- [ ] **Step 7: Approval checkpoint instead of commit**

Inspect: `git diff -- src/lwlte/lwlte.c src/include/lwlte.h tests/host/test_mqtt_end_to_end_contract.py`

Expected: SSL Facade APIs, helper callback, and the narrowed MQTT Facade contract only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 6: Propagate MQTT TLS Through Facade and MQTT Service

**Files:**
- Modify: `src/lwlte/lwlte.c`
- Modify: `src/mqtt_client/mqtt_client.h`
- Modify: `src/mqtt_client/mqtt_client.c`

- [ ] **Step 1: Run MQTT TLS service contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_mqtt_service_carries_tls_without_crossing_boundaries -v`

Expected: FAIL because the service rejects TLS and does not pass `ssl_context_id` to Core.

- [ ] **Step 2: Extend MQTT service config**

In `src/mqtt_client/mqtt_client.h`, add to `mqtt_client_endpoint_config_t`:

```c
    uint8_t ssl_context_id;
```

- [ ] **Step 3: Stop rejecting TLS and clone the context**

In `src/mqtt_client/mqtt_client.c`:

Remove the TLS rejection block from `mqtt_client_create()`:

```c
    if (config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS) {
        return NULL;
    }
```

Keep `config_valid()` accepting both transports. `normalize_config()` already copies scalar fields through `*normalized = *config`, so `ssl_context_id` is preserved.

Do not add PEM fields or certificate write logic to MQTT service. MQTT connect and reconnect never rewrite certificates; they only submit MQTT configure/connect commands with `transport` and `ssl_context_id`.

In `submit_core_cmd()`, add MQTT configure fields:

```c
        cmd.data.mqtt_config.transport =
            me->config.endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS ?
            LWLTE_MQTT_TRANSPORT_TLS : LWLTE_MQTT_TRANSPORT_PLAIN_TCP;
        cmd.data.mqtt_config.ssl_context_id = me->config.endpoint.ssl_context_id;
```

- [ ] **Step 4: Map Facade MQTT config to service config**

In `src/lwlte/lwlte.c`, update MQTT init validation:

```c
    ESP_RETURN_ON_FALSE(config->transport == LWLTE_MQTT_TRANSPORT_PLAIN_TCP ||
                        config->transport == LWLTE_MQTT_TRANSPORT_TLS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid MQTT transport");
```

Update the service config construction:

```c
            .transport = config->transport == LWLTE_MQTT_TRANSPORT_TLS ?
                         MQTT_CLIENT_TRANSPORT_TLS :
                         MQTT_CLIENT_TRANSPORT_PLAIN_TCP,
            .ssl_context_id = config->ssl_context_id,
```

- [ ] **Step 5: Run MQTT TLS service contract**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_mqtt_service_carries_tls_without_crossing_boundaries -v`

Expected: PASS.

- [ ] **Step 6: Approval checkpoint instead of commit**

Inspect: `git diff -- src/lwlte/lwlte.c src/mqtt_client/mqtt_client.h src/mqtt_client/mqtt_client.c`

Expected: only MQTT transport/context propagation and TLS rejection removal.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 7: Implement Air780EP SSL Provisioning, Status Query, and MQTT TLS Binding

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Run Air780EP contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_air780ep_ssl_mapping_exists -v`

Expected: FAIL because `AIR780EP_SSL_MQTT_CONTEXT_ID` and SSL ops are missing.

- [ ] **Step 2: Add Air780EP constants and state fields**

Add defines near MQTT/TCP defines:

```c
#define AIR780EP_SSL_MQTT_CONTEXT_ID       88
#define AIR780EP_SSL_MAX_PEM_LEN           10240U
#define AIR780EP_SSL_CMD_TIMEOUT_MS        9000
#define AIR780EP_SSL_WRITE_TIMEOUT_S       30U
#define AIR780EP_SSL_CONTEXT_BITMAP_BITS   256U
```

Add fields to `modem_air780ep_t`:

```c
    uint32_t ssl_provisioned_bitmap[(AIR780EP_SSL_CONTEXT_BITMAP_BITS + 31U) / 32U];
    modem_ssl_auth_mode_t ssl_auth_modes[AIR780EP_SSL_CONTEXT_BITMAP_BITS];
    modem_mqtt_transport_t mqtt_transport;
    uint8_t mqtt_ssl_context_id;
```

- [ ] **Step 3: Add static prototypes**

Add prototypes in the static prototype area:

```c
static esp_err_t air780ep_ssl_provision(modem_handle_t *me,
                                        const modem_ssl_context_config_t *config,
                                        const modem_ssl_credentials_t *credentials);
static esp_err_t air780ep_ssl_get_context_status(modem_handle_t *me,
                                                 uint8_t context_id,
                                                 modem_ssl_context_status_t *status);
static void ssl_mark_context(modem_air780ep_t *self, uint8_t context_id,
                             modem_ssl_auth_mode_t auth_mode, bool provisioned);
static bool ssl_context_marked(modem_air780ep_t *self, uint8_t context_id);
static esp_err_t air780ep_ssl_object_names(uint8_t context_id,
                                           char *ca_name, size_t ca_name_size,
                                           char *client_cert_name, size_t client_cert_name_size,
                                           char *client_key_name, size_t client_key_name_size);
static esp_err_t air780ep_delete_file_ignore_missing(modem_air780ep_t *self,
                                                     const char *name);
static esp_err_t air780ep_write_file(modem_air780ep_t *self, const char *name,
                                     const uint8_t *data, size_t len);
static esp_err_t air780ep_bind_ssl_file(modem_air780ep_t *self,
                                        const char *tag, uint8_t context_id,
                                        const char *name);
static esp_err_t air780ep_file_exists(modem_air780ep_t *self,
                                      const char *name, bool *exists);
static esp_err_t air780ep_query_ssl_auth_mode(modem_air780ep_t *self,
                                              uint8_t context_id,
                                              modem_ssl_auth_mode_t *auth_mode);
static void air780ep_clear_ssl_state(modem_air780ep_t *self);
```

Add ops entries:

```c
    .ssl_provision = air780ep_ssl_provision,
    .ssl_get_context_status = air780ep_ssl_get_context_status,
```

- [ ] **Step 4: Implement Air780EP SSL helper functions**

Implement helpers near other static helpers:

```c
static void ssl_mark_context(modem_air780ep_t *self, uint8_t context_id,
                             modem_ssl_auth_mode_t auth_mode, bool provisioned)
{
    if (!self) {
        return;
    }
    uint32_t mask = 1UL << (context_id % 32U);
    size_t index = context_id / 32U;
    if (provisioned) {
        self->ssl_provisioned_bitmap[index] |= mask;
        self->ssl_auth_modes[context_id] = auth_mode;
    } else {
        self->ssl_provisioned_bitmap[index] &= ~mask;
        self->ssl_auth_modes[context_id] = MODEM_SSL_AUTH_NONE;
    }
}

static bool ssl_context_marked(modem_air780ep_t *self, uint8_t context_id)
{
    if (!self) {
        return false;
    }
    uint32_t mask = 1UL << (context_id % 32U);
    return (self->ssl_provisioned_bitmap[context_id / 32U] & mask) != 0;
}

static esp_err_t air780ep_ssl_object_names(uint8_t context_id,
                                           char *ca_name, size_t ca_name_size,
                                           char *client_cert_name, size_t client_cert_name_size,
                                           char *client_key_name, size_t client_key_name_size)
{
    int written = snprintf(ca_name, ca_name_size, "lwlte_ca_%u.crt",
                           (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < ca_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "CA object name truncated");
    written = snprintf(client_cert_name, client_cert_name_size,
                       "lwlte_client_%u.crt", (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < client_cert_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "client cert object name truncated");
    written = snprintf(client_key_name, client_key_name_size,
                       "lwlte_client_%u.key", (unsigned int)context_id);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < client_key_name_size,
                        ESP_ERR_INVALID_ARG, TAG, "client key object name truncated");
    return ESP_OK;
}
```

The helper must generate these exact names:

```c
char ca_name[32];          /* lwlte_ca_<ctx>.crt */
char client_cert_name[32]; /* lwlte_client_<ctx>.crt */
char client_key_name[32];  /* lwlte_client_<ctx>.key */
snprintf(ca_name, sizeof(ca_name), "lwlte_ca_%u.crt", (unsigned int)context_id);
snprintf(client_cert_name, sizeof(client_cert_name), "lwlte_client_%u.crt", (unsigned int)context_id);
snprintf(client_key_name, sizeof(client_key_name), "lwlte_client_%u.key", (unsigned int)context_id);
```

Implement file delete/write/status using existing `send_cmd()`, `ensure_at_ok()`, and `at_engine_send_cmd_with_payload()`:

```c
static esp_err_t air780ep_delete_file_ignore_missing(modem_air780ep_t *self,
                                                     const char *name)
{
    char cmd[96];
    int written = snprintf(cmd, sizeof(cmd), "AT+FSDEL=\"%s\"", name);
    ESP_RETURN_ON_FALSE(written >= 0 && (size_t)written < sizeof(cmd),
                        ESP_ERR_INVALID_ARG, TAG, "AT+FSDEL truncated");
    air780ep_cmd_ctx_t ctx;
    esp_err_t ret = send_cmd(self, cmd, &ctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ctx.response.status == AT_RESP_OK ||
        (ctx.response.status == AT_RESP_CME_ERROR && ctx.response.error_code == 62)) {
        return ESP_OK;
    }
    return ensure_at_ok(&ctx.response, cmd);
}
```

For `air780ep_write_file()`, reject `len > AIR780EP_SSL_MAX_PEM_LEN` with `ESP_ERR_INVALID_SIZE`, run `FSDEL`, `FSCREATE`, then `FSWRITE` with payload prompt `">"` and `AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` unset.

- [ ] **Step 5: Implement `air780ep_ssl_provision()`**

Validation and sequence:

```c
ESP_RETURN_ON_FALSE(config->context_id == AIR780EP_SSL_MQTT_CONTEXT_ID,
                    ESP_ERR_INVALID_ARG, TAG, "Air780EP MQTT SSL context must be 88");
```

Write only required objects by auth mode:

- `MODEM_SSL_AUTH_NONE`: no file writes.
- `MODEM_SSL_AUTH_SERVER`: write CA only and bind `cacert`.
- `MODEM_SSL_AUTH_MUTUAL`: write CA, client cert, and client key; bind all three tags.

Always set seclevel:

```c
AT+SSLCFG="seclevel",88,0
AT+SSLCFG="seclevel",88,1
AT+SSLCFG="seclevel",88,2
```

Apply optional fields as follows. Send `ignorelocaltime` every provisioning run because both `true` and `false` are meaningful modem state; send the other optional fields only when non-zero/non-NULL:

```c
AT+SSLCFG="sslversion",88,<tls_version>
AT+SSLCFG="hostname",88,"<escaped_hostname>"
AT+SSLCFG="ignorelocaltime",88,<ignore_cert_time ? 1 : 0>
AT+SSLCFG="negotiatetimeout",88,<negotiate_timeout_s>
```

On success, mark context provisioned under `self->base.lock`.

Call `air780ep_clear_ssl_state(self)` when reset/power-off paths clear initialization state, including `air780ep_start()` before hardware reset, `air780ep_reset()`, and `air780ep_stop()`/power-off handling. The helper clears `ssl_provisioned_bitmap`, resets `ssl_auth_modes[]` to `MODEM_SSL_AUTH_NONE`, and resets MQTT transport/context to plain TCP/0. This matches the rule that module reset or power-off clears in-memory SSL state.

- [ ] **Step 6: Implement `air780ep_ssl_get_context_status()`**

For context `88`, query module-side auth mode first, then query generated certificate file names with `AT+FSFLSIZE="<name>"`.

Implement `air780ep_query_ssl_auth_mode()` with `AT+SSLCFG="seclevel",88` and parse `+SSLCFG: "seclevel",88,<0|1|2>` into `MODEM_SSL_AUTH_NONE`, `MODEM_SSL_AUTH_SERVER`, or `MODEM_SSL_AUTH_MUTUAL`. If the query fails, return the lower-layer error instead of falling back to `ssl_auth_modes[]`.

Set status:

```c
status->auth_mode = queried_auth_mode;
status->ca_cert_present = ca_exists;
status->client_cert_present = cert_exists;
status->client_key_present = key_exists;
status->check_valid = false;
status->provisioned = status->auth_mode == MODEM_SSL_AUTH_NONE ||
                      (status->auth_mode == MODEM_SSL_AUTH_SERVER && ca_exists) ||
                      (status->auth_mode == MODEM_SSL_AUTH_MUTUAL && ca_exists && cert_exists && key_exists);
```

Synchronize bitmap with `ssl_mark_context(self, context_id, status->auth_mode, status->provisioned)`. The public status result must come from module queries, not the preexisting bitmap.

- [ ] **Step 7: Bind MQTT TLS in Air780EP MQTT configure/connect**

In `air780ep_mqtt_configure()`:

- Copy `transport` and `ssl_context_id` into `self->mqtt_transport` and `self->mqtt_ssl_context_id` on success.
- If TLS, require context `88` and `ssl_context_marked(self, 88)`.
- Do not send `AT+SSLCFG` from `air780ep_mqtt_configure()`. SSL config is owned by `air780ep_ssl_provision()` only; MQTT configure validates the cached context state and stores transport/context.

In `air780ep_mqtt_tcp_connect()`:

- Use `AT+SSLMIPSTART="<host>",<port>` when `self->mqtt_transport == MODEM_MQTT_TRANSPORT_TLS`.
- Keep existing `AT+MIPSTART` for plain TCP.
- Do not call `FSCREATE`, `FSWRITE`, or `SSLCFG` from MQTT connect/reconnect paths.

- [ ] **Step 8: Run Air780EP contract**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_air780ep_ssl_mapping_exists -v`

Expected: PASS.

- [ ] **Step 9: Approval checkpoint instead of commit**

Inspect: `git diff -- src/modem/modem_air780ep.c`

Expected: Air780EP SSL helpers, ops entries, status query, and MQTT TLS command selection only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 8: Implement ML307R SSL Provisioning, Status Query, and MQTT TLS Binding

**Files:**
- Modify: `src/modem/modem_ml307r.c`

- [ ] **Step 1: Run ML307R contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_ml307r_ssl_mapping_exists -v`

Expected: FAIL because `ML307R_SSL_MAX_PEM_LEN` and SSL ops are missing.

- [ ] **Step 2: Add ML307R constants and state fields**

Add defines near MQTT defines:

```c
#define ML307R_SSL_MAX_CONTEXTS          6U
#define ML307R_SSL_MAX_PEM_LEN           8192U
#define ML307R_SSL_CMD_TIMEOUT_MS        9000
#define ML307R_SSL_PAYLOAD_PROMPT        ">"
```

Add fields to `modem_ml307r_t`:

```c
    uint8_t ssl_provisioned_bitmap;
    modem_ssl_auth_mode_t ssl_auth_modes[ML307R_SSL_MAX_CONTEXTS];
    modem_mqtt_transport_t mqtt_transport;
    uint8_t mqtt_ssl_context_id;
```

- [ ] **Step 3: Add prototypes and ops entries**

Add prototypes:

```c
static esp_err_t ml307r_ssl_provision(modem_handle_t *me,
                                      const modem_ssl_context_config_t *config,
                                      const modem_ssl_credentials_t *credentials);
static esp_err_t ml307r_ssl_get_context_status(modem_handle_t *me,
                                               uint8_t context_id,
                                               modem_ssl_context_status_t *status);
static esp_err_t ml307r_write_cert_object(modem_ml307r_t *self, const char *name,
                                          const uint8_t *data, size_t len,
                                          bool private_key);
static esp_err_t ml307r_remove_ssl_object_ignore_missing(modem_ml307r_t *self,
                                                        const char *name);
static esp_err_t ml307r_ssl_object_present(modem_ml307r_t *self,
                                           const char *name, int type,
                                           bool *present);
static esp_err_t ml307r_query_ssl_auth_mode(modem_ml307r_t *self,
                                            uint8_t context_id,
                                            modem_ssl_auth_mode_t *auth_mode);
static bool ml307r_ssl_context_marked(modem_ml307r_t *self, uint8_t context_id);
static void ml307r_ssl_mark_context(modem_ml307r_t *self, uint8_t context_id,
                                    modem_ssl_auth_mode_t auth_mode, bool provisioned);
static void ml307r_clear_ssl_state(modem_ml307r_t *self);
```

Add ops entries:

```c
    .ssl_provision = ml307r_ssl_provision,
    .ssl_get_context_status = ml307r_ssl_get_context_status,
```

- [ ] **Step 4: Implement ML307R write/remove/status helpers**

Use generated names:

```c
snprintf(ca_name, sizeof(ca_name), "lwlte_ca_%u.crt", (unsigned int)context_id);
snprintf(client_cert_name, sizeof(client_cert_name), "lwlte_client_%u.crt", (unsigned int)context_id);
snprintf(client_key_name, sizeof(client_key_name), "lwlte_client_%u.key", (unsigned int)context_id);
```

Write helper behavior:

- Reject `len > ML307R_SSL_MAX_PEM_LEN` with `ESP_ERR_INVALID_SIZE`.
- Run `AT+MSSLRM=<name>` and ignore CME errors for missing objects.
- Use `AT+MSSLCERTWR=<name>,0,<len>` for CA/client cert.
- Use `AT+MSSLKEYWR=<name>,0,<len>` for client key.
- Write payload with `at_engine_send_cmd_with_payload()` and prompt `ML307R_SSL_PAYLOAD_PROMPT`.

Status helper behavior:

- Query `AT+MSSLLIST=1` for public certs and `AT+MSSLLIST=2` for private keys.
- Treat a line containing the exact object name as present.
- For present required objects, run `AT+MSSLCHECK=<name>`; set `check_valid=true` only when all required checks return OK.

- [ ] **Step 5: Implement `ml307r_ssl_provision()`**

Validation:

```c
ESP_RETURN_ON_FALSE(config->context_id < ML307R_SSL_MAX_CONTEXTS,
                    ESP_ERR_INVALID_ARG, TAG, "invalid ML307R ssl_id");
```

Sequence:

- Write required objects by auth mode.
- Send `AT+MSSLCFG="auth",<ssl_id>,<0|1|2>`.
- For `MODEM_SSL_AUTH_SERVER`, send `AT+MSSLCFG="cert",<ssl_id>,<ca_name>,"",""` so the server CA binding is deterministic while client credential slots are empty.
- For `MODEM_SSL_AUTH_MUTUAL`, send `AT+MSSLCFG="cert",<ssl_id>,<ca_name>,<client_cert_name>,<client_key_name>`.
- For `MODEM_SSL_AUTH_NONE`, do not send `AT+MSSLCFG="cert"`.
- Apply optional/defaulted settings: send `AT+MSSLCFG="ignorestamp",<ssl_id>,<ignore_cert_time ? 1 : 0>` every provisioning run because both `true` and `false` are meaningful modem state; send `AT+MSSLCFG="ignoreverify",<ssl_id>,0` every provisioning run to force the safe default because `lwlte_ssl_context_config_t` does not expose a public `ignoreverify` knob; send `version` and `negotime` only when non-zero.
- Mark context provisioned on success.

Call `ml307r_clear_ssl_state(self)` when reset/power-off paths clear initialization state, including `ml307r_start()` before hardware reset, `ml307r_reset()`, and `ml307r_stop()`/power-off handling. The helper clears `ssl_provisioned_bitmap`, resets `ssl_auth_modes[]` to `MODEM_SSL_AUTH_NONE`, and resets MQTT transport/context to plain TCP/0. This matches the rule that module reset or power-off clears in-memory SSL state.

- [ ] **Step 6: Implement `ml307r_ssl_get_context_status()`**

Validation: `context_id < ML307R_SSL_MAX_CONTEXTS`.

Query module-side auth mode first, then query required object presence.

Implement `ml307r_query_ssl_auth_mode()` with `AT+MSSLCFG="auth",<ssl_id>` and parse `+MSSLCFG: "auth",<ssl_id>,<0|1|2>` into `MODEM_SSL_AUTH_NONE`, `MODEM_SSL_AUTH_SERVER`, or `MODEM_SSL_AUTH_MUTUAL`. If the query fails, return the lower-layer error instead of falling back to `ssl_auth_modes[]`.

Set `status->auth_mode` from the query result. Set `status->provisioned` from module object presence for that auth mode only:

```c
status->provisioned = status->auth_mode == MODEM_SSL_AUTH_NONE ||
                      (status->auth_mode == MODEM_SSL_AUTH_SERVER && ca_exists) ||
                      (status->auth_mode == MODEM_SSL_AUTH_MUTUAL && ca_exists && cert_exists && key_exists);
```

Synchronize bitmap with `ml307r_ssl_mark_context(self, context_id, status->auth_mode, status->provisioned)`. The public status result must come from module queries, not the preexisting bitmap.

- [ ] **Step 7: Bind MQTT TLS in ML307R MQTT configure**

In `ml307r_mqtt_configure()`:

- If `config->transport == MODEM_MQTT_TRANSPORT_TLS`, require `config->ssl_context_id < ML307R_SSL_MAX_CONTEXTS` and `ml307r_ssl_context_marked(self, config->ssl_context_id)`.
- Send `AT+MQTTCFG="ssl",0,1,<ssl_id>` before `cached` config or before saving config.
- If plain TCP, send `AT+MQTTCFG="ssl",0,0` to clear stale TLS binding.
- Save `self->mqtt_transport` and `self->mqtt_ssl_context_id` on success.
- Do not call `MSSLCERTWR`, `MSSLKEYWR`, or `MSSLCFG="cert"` from MQTT connect/reconnect paths.

- [ ] **Step 8: Run ML307R contract**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_ml307r_ssl_mapping_exists -v`

Expected: PASS.

- [ ] **Step 9: Approval checkpoint instead of commit**

Inspect: `git diff -- src/modem/modem_ml307r.c`

Expected: ML307R SSL helpers, ops entries, status query, and MQTT TLS config only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 9: Add TLS Example Configuration and Provisioning Flow

**Files:**
- Modify: `example/Kconfig.projbuild`
- Modify: `example/air780ep_mqtt_client.c`
- Modify: `example/ml307r_mqtt_client.c`

- [ ] **Step 1: Run example TLS contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_examples_support_tls_8883_validation_path -v`

Expected: FAIL because `EXAMPLE_MQTT_TLS_ENABLE` is missing.

- [ ] **Step 2: Add Kconfig TLS options**

In `example/Kconfig.projbuild`, update MQTT port default and add TLS config:

```kconfig
config EXAMPLE_MQTT_TLS_ENABLE
    bool "Enable MQTT TLS"
    default n

config EXAMPLE_MQTT_PORT
    int "MQTT broker port"
    range 1 65535
    default 8883 if EXAMPLE_MQTT_TLS_ENABLE
    default 1883

config EXAMPLE_MQTT_TLS_CA_CERT_PEM
    string "MQTT TLS CA certificate PEM"
    default ""
    depends on EXAMPLE_MQTT_TLS_ENABLE
    help
        Paste the server CA PEM used by the MQTT TLS broker. The first TLS example path uses server authentication.
```

Keep the existing host, client ID, token, and keepalive options.

- [ ] **Step 3: Update Air780EP MQTT example**

In `example/air780ep_mqtt_client.c`, keep `lwlte_mqtt_init()` in its existing location. Add TLS provisioning after the network online wait succeeds and before `lwlte_mqtt_start()`, because SSL provisioning sends AT commands and requires the modem to be started and ready:

```c
    if (CONFIG_EXAMPLE_MQTT_TLS_ENABLE) {
        const char *ca_pem = CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM;
        const lwlte_ssl_context_config_t ssl_config = {
            .context_id = 88,
            .auth_mode = LWLTE_SSL_AUTH_SERVER,
            .ignore_cert_time = true,
            .hostname = CONFIG_EXAMPLE_MQTT_HOST,
        };
        const lwlte_ssl_credentials_t credentials = {
            .ca_cert_pem = (const uint8_t *)ca_pem,
            .ca_cert_len = ca_pem[0] ? strlen(ca_pem) : 0,
        };
        ret = lwlte_ssl_provision(lte, &ssl_config, &credentials);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SSL provision failed: %s", esp_err_to_name(ret));
            idle_forever();
        }
        lwlte_ssl_context_status_t status = {0};
        ret = lwlte_ssl_get_context_status(lte, 88, &status);
        if (ret != ESP_OK || !status.provisioned) {
            ESP_LOGE(TAG, "SSL status invalid: ret=%s provisioned=%d",
                     esp_err_to_name(ret), (int)status.provisioned);
            idle_forever();
        }
    }
```

Add `#include <string.h>`.

Set MQTT config fields:

```c
        .transport = CONFIG_EXAMPLE_MQTT_TLS_ENABLE ?
                     LWLTE_MQTT_TRANSPORT_TLS : LWLTE_MQTT_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = 88,
```

Add log line containing `8883`:

```c
    ESP_LOGI(TAG, "MQTT TLS %s; TLS validation uses port 8883",
             CONFIG_EXAMPLE_MQTT_TLS_ENABLE ? "enabled" : "disabled");
```

- [ ] **Step 4: Update ML307R MQTT example**

Apply the same post-network-online, pre-`lwlte_mqtt_start()` flow in `example/ml307r_mqtt_client.c`, but use `.context_id = 0` and `.ssl_context_id = 0`.

Add `#include <string.h>` and the same `8883` log line.

- [ ] **Step 5: Run example TLS contract**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_examples_support_tls_8883_validation_path -v`

Expected: PASS.

- [ ] **Step 6: Approval checkpoint instead of commit**

Inspect: `git diff -- example/Kconfig.projbuild example/air780ep_mqtt_client.c example/ml307r_mqtt_client.c`

Expected: TLS Kconfig and optional example provisioning path only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 10: Update Roadmap Documentation

**Files:**
- Modify: `docs/agents/feature-roadmap.md`

- [ ] **Step 1: Run roadmap contract before editing**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_roadmap_mentions_selected_ssl_design -v`

Expected: FAIL if roadmap does not mention status query command and API.

- [ ] **Step 2: Update roadmap SSL section**

In `docs/agents/feature-roadmap.md`, update the TLS/SSL roadmap section to include:

```md
| 新增命令 | `CORE_CMD_SSL_PROVISION`、`CORE_CMD_SSL_GET_CONTEXT_STATUS` |
| 新增公开 API | `lwlte_ssl_provision()`、`lwlte_ssl_get_context_status()` |
| 查询语义 | 查询模块端证书/密钥对象是否存在；Air780EP 用文件系统查询，ML307R 用 `MSSLLIST`/`MSSLCHECK`。 |
| MQTT TLS 验证 | 使用当前 MQTT example server，端口切换为 `8883`，完成实机连接、订阅和 telemetry 发布。 |
```

Do not edit `docs/agents/at_cmd_air780ep.md` or `docs/agents/at_cmd_ml307r.md` unless a later implementation step proves the existing lines are wrong.

- [ ] **Step 3: Run roadmap contract**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract.MqttTlsSslContractTest.test_roadmap_mentions_selected_ssl_design -v`

Expected: PASS.

- [ ] **Step 4: Approval checkpoint instead of commit**

Inspect: `git diff -- docs/agents/feature-roadmap.md`

Expected: TLS roadmap text only.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 11: Run Host Contract Suite and ESP-IDF Build

**Files:**
- No source edits expected unless verification reveals a defect.

- [ ] **Step 1: Run the new TLS contract suite**

Run: `python3 -m unittest tests.host.test_mqtt_tls_ssl_contract -v`

Expected: PASS.

- [ ] **Step 2: Run existing related host contracts**

Run: `python3 -m unittest tests.host.test_mqtt_end_to_end_contract tests.host.test_ml307r_contract tests.host.test_tcp_client_end_to_end_contract -v`

Expected: PASS.

- [ ] **Step 3: Build with ESP-IDF MCP**

Run the ESP-IDF build tool for the current project.

Expected: build succeeds without C compile errors.

- [ ] **Step 4: If build fails, fix minimally and rerun**

Use the compiler error file/line as the source of truth. Fix only the failing issue. Rerun Step 3.

- [ ] **Step 5: Approval checkpoint instead of commit**

Inspect: `git status --short` and `git diff --stat`.

Expected: only intended source, test, example, roadmap, spec, and plan files are modified/added.

Do not commit unless the user explicitly authorizes committing this checkpoint.
### Task 12: Real Hardware MQTT TLS Validation

**Files:**
- No source edits expected unless hardware validation reveals a defect.

- [ ] **Step 1: Configure the MQTT TLS example**

Set the example to the connected modem target in `example/main.c` by selecting one of:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_MQTT_CLIENT
```

or:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_MQTT_CLIENT
```

Set menuconfig values:

- `CONFIG_EXAMPLE_MQTT_TLS_ENABLE=y`
- `CONFIG_EXAMPLE_MQTT_HOST=admin.jovisdreams.site` unless the current example config was intentionally changed.
- `CONFIG_EXAMPLE_MQTT_PORT=8883`
- `CONFIG_EXAMPLE_MQTT_TOKEN` stays the current device token.
- `CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM` contains the broker CA PEM.

- [ ] **Step 2: Build and flash**

Use ESP-IDF MCP build and flash tools where available. If flash requires a port, detect the current serial port first with the documented serial-device command.

Expected: flash succeeds.

- [ ] **Step 3: Capture serial logs**

Run the project serial monitor script:

```bash
python3 docs/agents/serial_monitor.py --timeout 60
```

Expected logs include:

- Network reaches online.
- SSL provision succeeds.
- SSL status reports `provisioned=1` or equivalent log.
- MQTT connects to port `8883`.
- Subscribe to `v1/devices/me/attributes` succeeds.
- Telemetry publish to `v1/devices/me/telemetry` succeeds.

- [ ] **Step 4: Fix hardware defects minimally**

If validation fails, classify the failure stage:

- SSL provision command failure.
- SSL status query failure.
- MQTT TLS bind/config failure.
- TCP/TLS connect failure.
- MQTT connect/auth failure.
- Subscribe/publish failure.

Fix only the failing stage, then rerun Steps 1-3.

- [ ] **Step 5: Record validation result in final response**

Record which modem was tested, the MQTT host/port, and the observed connection/subscription/publish result. If only one of Air780EP or ML307R is physically tested, state the untested module explicitly.

## Plan Self-Review

- Spec coverage: Tasks 2, 5, and 6 cover public API and MQTT TLS config. Task 3 defines the Modem shared API before Task 4 wires Core command dispatch to it. Tasks 7 and 8 cover Air780EP and ML307R AT mappings. Task 9 covers examples and port `8883`. Task 11 covers build/static verification. Task 12 covers real hardware validation.
- Placeholder scan: The plan intentionally avoids implementation placeholders. Each implementation task names concrete files, functions, command strings, validation commands, and expected outcomes.
- Type consistency: Public types use `lwlte_*`, Core types use `core_*`, Modem types use `modem_*`, MQTT service types use `mqtt_client_*`. Command names match the approved spec: `CORE_CMD_SSL_PROVISION` and `CORE_CMD_SSL_GET_CONTEXT_STATUS`.
- Commit policy: All commit steps are replaced with approval checkpoints because repository instructions prohibit commits without explicit user authorization.
