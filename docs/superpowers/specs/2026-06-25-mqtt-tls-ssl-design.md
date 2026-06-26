# MQTT TLS and SSL Certificate Provisioning Design

## Goal

Add TLS support for the MQTT client and introduce a reusable SSL certificate provisioning capability for ESP-IDF modem modules. The first implementation targets Air780EP and ML307R.

## Scope

- Add public APIs for SSL context provisioning and certificate presence queries.
- Allow MQTT clients to use plain TCP or TLS.
- Support TLS authentication modes: no authentication, server authentication, and mutual authentication.
- Accept certificate and key material as PEM content through the public API.
- Reuse the SSL capability for future HTTPS and TCP-TLS work.
- Validate the feature on real connected hardware.

Out of scope for the first version:

- Automatic certificate writing during MQTT connect or reconnect.
- Chunked certificate upload for PEM files larger than one module write packet.
- Persistent certificate discovery across module power cycles without an explicit query.
- PSK-based TLS.

## Selected Approach

Use a common SSL context model with explicit certificate provisioning. MQTT TLS references a provisioned context by ID.

This keeps certificate lifecycle management outside the MQTT service and avoids repeated writes to modem flash. The design follows the existing project layering: Facade calls Core, Core calls Modem, Modem adapts module-specific AT commands, and AT Engine performs command and payload I/O.

Rejected alternatives:

- Store PEM content directly in `lwlte_mqtt_config_t` and write certificates during connect. This is easier for callers but couples MQTT to certificate lifecycle and can repeatedly write module flash.
- Expose module certificate file names or raw `ssl_id` handling only. This leaks Air780EP and ML307R differences to application code and does not satisfy the PEM-input requirement.

## Public API

Add public SSL types in `src/include/lwlte.h`.

```c
typedef enum {
    LWLTE_SSL_AUTH_NONE = 0,
    LWLTE_SSL_AUTH_SERVER,
    LWLTE_SSL_AUTH_MUTUAL,
} lwlte_ssl_auth_mode_t;

typedef enum {
    LWLTE_MQTT_TRANSPORT_PLAIN_TCP = 0,
    LWLTE_MQTT_TRANSPORT_TLS,
} lwlte_mqtt_transport_t;

typedef struct {
    uint8_t context_id;
    lwlte_ssl_auth_mode_t auth_mode;
    uint8_t tls_version;
    uint32_t negotiate_timeout_s;
    bool ignore_cert_time;
    const char *hostname;
} lwlte_ssl_context_config_t;

typedef struct {
    const uint8_t *ca_cert_pem;
    size_t ca_cert_len;
    const uint8_t *client_cert_pem;
    size_t client_cert_len;
    const uint8_t *client_key_pem;
    size_t client_key_len;
} lwlte_ssl_credentials_t;

typedef struct {
    bool provisioned;
    bool ca_cert_present;
    bool client_cert_present;
    bool client_key_present;
    bool check_valid;
    lwlte_ssl_auth_mode_t auth_mode;
} lwlte_ssl_context_status_t;
```

Add public functions:

```c
esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials);

esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status);
```

Extend `lwlte_mqtt_config_t`:

```c
lwlte_mqtt_transport_t transport;
uint8_t ssl_context_id;
```

Zero-initialized MQTT config remains plain TCP because `LWLTE_MQTT_TRANSPORT_PLAIN_TCP` is `0`.

## Public Usage Flow

Plain MQTT keeps the existing flow.

MQTTS uses explicit provisioning before MQTT initialization or before MQTT start:

```c
lwlte_ssl_provision(lte, &ssl_cfg, &creds);
lwlte_ssl_get_context_status(lte, ssl_cfg.context_id, &status);
lwlte_mqtt_init(lte, &mqtt_cfg_with_tls_context);
lwlte_mqtt_start(lte);
```

`lwlte_mqtt_start()` and reconnect paths do not write certificates. If a TLS context was not provisioned or is not usable, the modem MQTT configure step fails, the MQTT client enters `ERROR`, and `LWLTE_MQTT_EVENT_ERROR` is posted.

## Certificate Query Semantics

`lwlte_ssl_get_context_status()` queries module-side state rather than only checking in-memory flags.

First-version semantics:

- `provisioned` means the module reports the required objects for the context and auth mode.
- `ca_cert_present`, `client_cert_present`, and `client_key_present` report object presence.
- `check_valid` is true only when a module-specific check command succeeds for every required object. It is false when the module has no equivalent check command or the check command fails.
- Air780EP reports file presence by checking generated certificate file names with `AT+FSFLSIZE`.
- ML307R reports certificate/key presence with `AT+MSSLLIST=<type>`. It runs `AT+MSSLCHECK=<name>` for present required objects and sets `check_valid=true` only if all required checks succeed.

The status query may update the modem in-memory provisioned bitmap, but the public result comes from the module query.

## Authentication Validation

`lwlte_ssl_provision()` validates required PEM inputs by auth mode:

- `LWLTE_SSL_AUTH_NONE`: no certificate material required.
- `LWLTE_SSL_AUTH_SERVER`: CA PEM is required.
- `LWLTE_SSL_AUTH_MUTUAL`: CA PEM, client certificate PEM, and client private key PEM are required.

PEM pointer and length must both be valid when a material is required. Optional material with a NULL pointer or zero length is treated as absent.

## Context Rules

The public API uses `context_id`.

- Air780EP MQTT TLS uses SSL context `88`. MQTT TLS with any other Air780EP context returns `ESP_ERR_INVALID_ARG` during public argument validation or modem MQTT configuration.
- ML307R uses SSL IDs `0..5`.
- Later HTTPS and TCP-TLS support can reuse the same public SSL model with protocol-specific context mapping.

The modem stores minimal state:

- Provisioned SSL context bitmap.
- Auth mode per provisioned context.
- Current MQTT transport.
- Current MQTT SSL context ID.

Provision success sets the bitmap. Module reset or power-off clears the in-memory bitmap. A later context status query can resynchronize the bitmap from module-side certificate objects.

## Internal Data Flow

Provisioning flow:

```text
App
  -> lwlte_ssl_provision()
  -> Core: CORE_CMD_SSL_PROVISION
  -> Modem: modem_ssl_provision()
  -> Air780EP / ML307R AT commands
  -> AT Engine payload write
```

Status query flow:

```text
App
  -> lwlte_ssl_get_context_status()
  -> Core: CORE_CMD_SSL_GET_CONTEXT_STATUS
  -> Modem: modem_ssl_get_context_status()
  -> Air780EP / ML307R AT queries
```

MQTT TLS connection flow:

```text
App
  -> lwlte_mqtt_init(transport=TLS, ssl_context_id=...)
  -> mqtt_client_start()
  -> Core: CORE_CMD_MQTT_CONFIGURE
  -> Modem: mqtt_configure()
  -> Modem binds module-specific MQTT TLS settings
  -> Core: CORE_CMD_MQTT_TCP_CONNECT / CORE_CMD_MQTT_CONNECT
```

## Layer Responsibilities

Facade:

- Expose SSL provision and status query APIs.
- Validate public arguments.
- Submit Core commands and wait for completion for synchronous public APIs.
- Map public MQTT transport fields into MQTT service config.

MQTT Client service:

- Store `transport` and `ssl_context_id`.
- Include them in MQTT configure commands.
- Never handle PEM content or call Modem directly.

Core:

- Add `CORE_CMD_SSL_PROVISION` and `CORE_CMD_SSL_GET_CONTEXT_STATUS`.
- Clone and free PEM buffers and strings safely.
- Dispatch SSL commands to Modem wrappers.
- Extend MQTT configure command data with transport and SSL context.

Modem common layer:

- Add common SSL value objects and wrappers.
- Add SSL ops to `modem_ops_t`.
- Validate generic modem arguments before dispatch.

Module adapters:

- Implement Air780EP and ML307R provisioning, status query, and MQTT TLS binding.
- Keep all AT command syntax in module-specific files.

AT Engine:

- Reuse existing command and payload write support.
- No certificate-specific AT Engine API is needed.

## Certificate Object Naming

The public API does not expose module certificate object names.

Internal generated names are deterministic:

```text
lwlte_ca_<ctx>.crt
lwlte_client_<ctx>.crt
lwlte_client_<ctx>.key
```

The same names are used by provision and status query commands.

## Provisioning Strategy

`lwlte_ssl_provision()` is the only API that writes certificates.

- Calling provision again for the same context is an explicit certificate update.
- Old generated certificate objects are deleted before writing replacements.
- Delete failures caused by missing files or missing certificate objects are ignored.
- Write, bind, or SSL configuration failures return an error.
- MQTT connect and reconnect never rewrite certificates.

Size limits for first version:

- Air780EP `AT+FSWRITE` supports one payload up to 10240 bytes. Larger single PEM objects return `ESP_ERR_INVALID_SIZE`.
- ML307R certificate/key write supports one payload up to 8192 bytes. Larger single PEM objects return `ESP_ERR_INVALID_SIZE`.
- Chunked upload is not implemented in this scope.

## Air780EP Mapping

Air780EP writes certificate material into the module file system, then binds files to SSL context `88` for MQTT TLS.

Provision sequence:

```text
AT+FSDEL="<name>"               // ignore missing file
AT+FSCREATE="<name>"
AT+FSWRITE="<name>",0,<len>,<timeout>
<PEM payload>
AT+SSLCFG="cacert",88,"<ca_name>"
AT+SSLCFG="clientcert",88,"<client_cert_name>"
AT+SSLCFG="clientkey",88,"<client_key_name>"
AT+SSLCFG="seclevel",88,<0|1|2>
```

Optional settings:

```text
AT+SSLCFG="sslversion",88,<version>
AT+SSLCFG="hostname",88,"<hostname>"
AT+SSLCFG="ignorelocaltime",88,<0|1>
AT+SSLCFG="negotiatetimeout",88,<seconds>
```

MQTT TLS connect uses Air780EP MQTT SSL context `88` and the module-specific MQTT SSL TCP command path already documented for Air780EP.

Status query checks generated file names with `AT+FSFLSIZE="<name>"`.

## ML307R Mapping

ML307R writes certificate and key objects through SSL-specific commands, then binds them to an SSL ID.

Provision sequence:

```text
AT+MSSLRM=<name>                         // ignore missing object
AT+MSSLCERTWR=<ca_name>,0,<len>
<CA PEM payload>
AT+MSSLCERTWR=<client_cert_name>,0,<len>
<client cert PEM payload>
AT+MSSLKEYWR=<client_key_name>,0,<len>
<client key PEM payload>
AT+MSSLCFG="auth",<ssl_id>,<0|1|2>
AT+MSSLCFG="cert",<ssl_id>,<ca_name>,<client_cert_name>,<client_key_name>
```

Optional settings:

```text
AT+MSSLCFG="version",<ssl_id>,<version>
AT+MSSLCFG="negotime",<ssl_id>,<seconds>
AT+MSSLCFG="ignorestamp",<ssl_id>,<0|1>
AT+MSSLCFG="ignoreverify",<ssl_id>,<0|1>
```

MQTT TLS binding occurs during MQTT configure:

```text
AT+MQTTCFG="ssl",0,1,<ssl_id>
```

Plain MQTT configure disables SSL binding to avoid stale TLS state from earlier configurations.

Status query uses `AT+MSSLLIST=<type>` to check object presence. `AT+MSSLCHECK=<name>` may be used when available to set `check_valid`.

## Error Handling

- Invalid public arguments return `ESP_ERR_INVALID_ARG`.
- Invalid context range returns `ESP_ERR_INVALID_ARG`.
- Oversized PEM material returns `ESP_ERR_INVALID_SIZE`.
- Unsupported modem SSL operations return `ESP_ERR_NOT_SUPPORTED`.
- Missing provisioned TLS context during MQTT TLS configure returns `ESP_ERR_INVALID_STATE`.
- AT command failures return `ESP_FAIL` unless a more specific lower-layer error is available.
- Missing old certificate objects during cleanup are ignored.
- MQTT TLS configure or connect failure moves MQTT state to `ERROR` and emits `LWLTE_MQTT_EVENT_ERROR`.

## Verification

Build and static verification:

- Public headers compile after adding SSL and MQTT transport types.
- Existing plain MQTT examples continue to build and default to plain TCP.
- Core command clone/free paths handle PEM buffers without leaks or dangling pointers.
- Invalid auth mode, missing required PEM, oversized PEM, invalid context, and missing provisioned context return expected errors.

Module command verification:

- Air780EP command order covers `FSDEL`, `FSCREATE`, `FSWRITE` payload, and `SSLCFG` binding.
- ML307R command order covers `MSSLCERTWR`, `MSSLKEYWR`, `MSSLCFG`, and `MQTTCFG="ssl"`.
- Status query confirms generated certificate object presence on each module.

Real hardware verification:

- Hardware is assumed connected and must be used before declaring implementation complete.
- Use the current MQTT example server configuration: `CONFIG_EXAMPLE_MQTT_HOST`, `CONFIG_EXAMPLE_MQTT_TOKEN`, current ThingsBoard topics, and existing publish/subscribe flow.
- For TLS MQTT validation, change the MQTT port to `8883` and configure MQTT transport as TLS.
- Verify at least one successful connection, subscription to `v1/devices/me/attributes`, and telemetry publish to `v1/devices/me/telemetry`.
- Prefer validating both Air780EP and ML307R if both are available; otherwise record which module was physically tested.

## Documentation Updates

- Update public API comments in `src/include/lwlte.h`.
- Update examples to show TLS provisioning and MQTT port `8883` for the TLS validation path.
- Update `docs/agents/feature-roadmap.md` to reflect the selected SSL provisioning design.
- Avoid overwriting unrelated user edits in AT command reference docs. Only update `docs/agents/at_cmd_air780ep.md` or `docs/agents/at_cmd_ml307r.md` if implementation needs a targeted correction.
