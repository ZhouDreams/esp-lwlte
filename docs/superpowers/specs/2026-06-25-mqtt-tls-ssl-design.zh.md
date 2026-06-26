# MQTT TLS 与 SSL 证书 Provision 设计

## 目标

为 MQTT client 增加 TLS 支持，并为 ESP-IDF modem 模块引入可复用的 SSL 证书 provision 能力。首版实现目标为 Air780EP 与 ML307R。

## 范围

- 新增 SSL context provision 与证书存在性查询的公开 API。
- 允许 MQTT client 使用明文 TCP 或 TLS。
- 支持 TLS 认证模式：不认证、服务器认证、双向认证。
- 通过公开 API 接收 PEM 格式的证书与密钥内容。
- 为后续 HTTPS 与 TCP-TLS 工作复用 SSL 能力。
- 在真实连接硬件上验证该功能。

首版不包含：

- MQTT connect 或 reconnect 时自动写入证书。
- 对超过模块单次写入包大小的 PEM 文件做分片上传。
- 未显式查询时跨模块掉电周期持久发现证书。
- 基于 PSK 的 TLS。

## 选定方案

使用通用 SSL context 模型，并显式 provision 证书。MQTT TLS 通过 ID 引用一个已 provision 的 context。

这将证书生命周期管理放在 MQTT service 外部，并避免重复写入 modem flash。该设计遵循现有项目分层：Facade 调用 Core，Core 调用 Modem，Modem 适配模块专属 AT 命令，AT Engine 执行命令与 payload I/O。

被拒绝的替代方案：

- 将 PEM 内容直接存入 `lwlte_mqtt_config_t` 并在 connect 时写入证书。该方案对调用者更简单，但会把 MQTT 与证书生命周期耦合，并可能重复写模块 flash。
- 只暴露模块证书文件名或原始 `ssl_id` 处理。该方案会把 Air780EP 与 ML307R 的差异泄漏到应用代码，也不满足 PEM 输入要求。

## 公开 API

在 `src/include/lwlte.h` 中新增公开 SSL 类型。

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

新增公开函数：

```c
esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,
                              const lwlte_ssl_context_config_t *config,
                              const lwlte_ssl_credentials_t *credentials);

esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,
                                       uint8_t context_id,
                                       lwlte_ssl_context_status_t *status);
```

扩展 `lwlte_mqtt_config_t`：

```c
lwlte_mqtt_transport_t transport;
uint8_t ssl_context_id;
```

零初始化的 MQTT config 仍然是明文 TCP，因为 `LWLTE_MQTT_TRANSPORT_PLAIN_TCP` 为 `0`。

## 公开使用流程

明文 MQTT 保持现有流程不变。

MQTTS 在 MQTT 初始化前或 MQTT start 前显式 provision：

```c
lwlte_ssl_provision(lte, &ssl_cfg, &creds);
lwlte_ssl_get_context_status(lte, ssl_cfg.context_id, &status);
lwlte_mqtt_init(lte, &mqtt_cfg_with_tls_context);
lwlte_mqtt_start(lte);
```

`lwlte_mqtt_start()` 与 reconnect 路径不写证书。如果 TLS context 未 provision 或不可用，modem MQTT configure 步骤失败，MQTT client 进入 `ERROR`，并发送 `LWLTE_MQTT_EVENT_ERROR`。

## 证书查询语义

`lwlte_ssl_get_context_status()` 查询模块端状态，而不是只检查内存 flag。

首版语义：

- `provisioned` 表示模块报告该 context 与 auth mode 所需对象存在。
- `ca_cert_present`、`client_cert_present`、`client_key_present` 报告对象存在性。
- `check_valid` 只有在模块专属 check 命令对每个必需对象都成功时才为 true。若模块没有等价 check 命令，或 check 命令失败，则为 false。
- Air780EP 通过 `AT+FSFLSIZE` 检查生成的证书文件名来报告文件存在性。
- ML307R 通过 `AT+MSSLLIST=<type>` 报告证书/密钥存在性。它会对已存在的必需对象执行 `AT+MSSLCHECK=<name>`，只有全部必需检查成功才设置 `check_valid=true`。

状态查询可以更新 modem 内存中的 provisioned bitmap，但公开结果来自模块查询。

## 认证校验

`lwlte_ssl_provision()` 按 auth mode 校验必需 PEM 输入：

- `LWLTE_SSL_AUTH_NONE`：不需要证书材料。
- `LWLTE_SSL_AUTH_SERVER`：需要 CA PEM。
- `LWLTE_SSL_AUTH_MUTUAL`：需要 CA PEM、客户端证书 PEM、客户端私钥 PEM。

当某项材料是必需项时，PEM 指针与长度必须同时有效。可选材料若指针为 NULL 或长度为 0，则视为不存在。

## Context 规则

公开 API 使用 `context_id`。

- Air780EP MQTT TLS 使用 SSL context `88`。Air780EP 上 MQTT TLS 使用其他 context 时，在 public 参数校验或 modem MQTT configure 阶段返回 `ESP_ERR_INVALID_ARG`。
- ML307R 使用 SSL ID `0..5`。
- 后续 HTTPS 与 TCP-TLS 支持可复用相同公开 SSL 模型，并做协议专属 context 映射。

modem 保存最小状态：

- 已 provision 的 SSL context bitmap。
- 每个已 provision context 的 auth mode。
- 当前 MQTT transport。
- 当前 MQTT SSL context ID。

Provision 成功会设置 bitmap。模块 reset 或掉电会清除内存 bitmap。后续 context status query 可从模块端证书对象重新同步 bitmap。

## 内部数据流

Provisioning flow：

```text
App
  -> lwlte_ssl_provision()
  -> Core: CORE_CMD_SSL_PROVISION
  -> Modem: modem_ssl_provision()
  -> Air780EP / ML307R AT commands
  -> AT Engine payload write
```

Status query flow：

```text
App
  -> lwlte_ssl_get_context_status()
  -> Core: CORE_CMD_SSL_GET_CONTEXT_STATUS
  -> Modem: modem_ssl_get_context_status()
  -> Air780EP / ML307R AT queries
```

MQTT TLS connection flow：

```text
App
  -> lwlte_mqtt_init(transport=TLS, ssl_context_id=...)
  -> mqtt_client_start()
  -> Core: CORE_CMD_MQTT_CONFIGURE
  -> Modem: mqtt_configure()
  -> Modem binds module-specific MQTT TLS settings
  -> Core: CORE_CMD_MQTT_TCP_CONNECT / CORE_CMD_MQTT_CONNECT
```

## 分层职责

Facade：

- 暴露 SSL provision 与 status query API。
- 校验公开参数。
- 提交 Core commands，并为同步公开 API 等待完成。
- 将公开 MQTT transport 字段映射到 MQTT service config。

MQTT Client service：

- 保存 `transport` 与 `ssl_context_id`。
- 将它们包含在 MQTT configure commands 中。
- 永不处理 PEM 内容，也不直接调用 Modem。

Core：

- 新增 `CORE_CMD_SSL_PROVISION` 与 `CORE_CMD_SSL_GET_CONTEXT_STATUS`。
- 安全 clone 与 free PEM buffers 和 strings。
- 将 SSL commands 分发到 Modem wrappers。
- 扩展 MQTT configure command data，加入 transport 与 SSL context。

Modem common layer：

- 新增通用 SSL value objects 与 wrappers。
- 在 `modem_ops_t` 中新增 SSL ops。
- dispatch 前校验通用 modem 参数。

Module adapters：

- 实现 Air780EP 与 ML307R provisioning、status query 与 MQTT TLS binding。
- 将全部 AT 命令语法保留在模块专属文件中。

AT Engine：

- 复用现有 command 与 payload write 支持。
- 不需要新增证书专属 AT Engine API。

## 证书对象命名

公开 API 不暴露模块证书对象名。

内部生成名称是确定性的：

```text
lwlte_ca_<ctx>.crt
lwlte_client_<ctx>.crt
lwlte_client_<ctx>.key
```

Provision 与 status query 命令使用相同名称。

## Provisioning 策略

`lwlte_ssl_provision()` 是唯一写入证书的 API。

- 对同一 context 再次调用 provision 是显式证书更新。
- 写入替换对象前先删除旧的生成证书对象。
- 因文件或证书对象不存在导致的删除失败会被忽略。
- 写入、绑定或 SSL 配置失败会返回错误。
- MQTT connect 与 reconnect 永不重写证书。

首版大小限制：

- Air780EP `AT+FSWRITE` 支持单个 payload 最大 10240 bytes。更大的单个 PEM 对象返回 `ESP_ERR_INVALID_SIZE`。
- ML307R certificate/key write 支持单个 payload 最大 8192 bytes。更大的单个 PEM 对象返回 `ESP_ERR_INVALID_SIZE`。
- 本范围不实现分片上传。

## Air780EP 映射

Air780EP 将证书材料写入模块文件系统，然后将文件绑定到 MQTT TLS 使用的 SSL context `88`。

Provision sequence：

```text
AT+FSDEL="<name>"               // 忽略文件不存在
AT+FSCREATE="<name>"
AT+FSWRITE="<name>",0,<len>,<timeout>
<PEM payload>
AT+SSLCFG="cacert",88,"<ca_name>"
AT+SSLCFG="clientcert",88,"<client_cert_name>"
AT+SSLCFG="clientkey",88,"<client_key_name>"
AT+SSLCFG="seclevel",88,<0|1|2>
```

Optional settings：

```text
AT+SSLCFG="sslversion",88,<version>
AT+SSLCFG="hostname",88,"<hostname>"
AT+SSLCFG="ignorelocaltime",88,<0|1>
AT+SSLCFG="negotiatetimeout",88,<seconds>
```

MQTT TLS connect 使用 Air780EP MQTT SSL context `88`，以及已在 Air780EP 文档中记录的模块专属 MQTT SSL TCP 命令路径。

Status query 通过 `AT+FSFLSIZE="<name>"` 检查生成的文件名。

## ML307R 映射

ML307R 通过 SSL 专属命令写入证书和密钥对象，然后将它们绑定到 SSL ID。

Provision sequence：

```text
AT+MSSLRM=<name>                         // 忽略对象不存在
AT+MSSLCERTWR=<ca_name>,0,<len>
<CA PEM payload>
AT+MSSLCERTWR=<client_cert_name>,0,<len>
<client cert PEM payload>
AT+MSSLKEYWR=<client_key_name>,0,<len>
<client key PEM payload>
AT+MSSLCFG="auth",<ssl_id>,<0|1|2>
AT+MSSLCFG="cert",<ssl_id>,<ca_name>,<client_cert_name>,<client_key_name>
```

Optional settings：

```text
AT+MSSLCFG="version",<ssl_id>,<version>
AT+MSSLCFG="negotime",<ssl_id>,<seconds>
AT+MSSLCFG="ignorestamp",<ssl_id>,<0|1>
AT+MSSLCFG="ignoreverify",<ssl_id>,<0|1>
```

MQTT TLS binding 在 MQTT configure 期间执行：

```text
AT+MQTTCFG="ssl",0,1,<ssl_id>
```

明文 MQTT configure 会禁用 SSL binding，以避免之前配置留下 stale TLS 状态。

Status query 使用 `AT+MSSLLIST=<type>` 检查对象存在性。可用时，可以使用 `AT+MSSLCHECK=<name>` 设置 `check_valid`。

## 错误处理

- 无效公开参数返回 `ESP_ERR_INVALID_ARG`。
- 无效 context range 返回 `ESP_ERR_INVALID_ARG`。
- 过大的 PEM material 返回 `ESP_ERR_INVALID_SIZE`。
- 不支持的 modem SSL operation 返回 `ESP_ERR_NOT_SUPPORTED`。
- MQTT TLS configure 时缺少已 provision TLS context 返回 `ESP_ERR_INVALID_STATE`。
- AT 命令失败返回 `ESP_FAIL`，除非下层有更具体错误。
- 清理阶段缺少旧证书对象会被忽略。
- MQTT TLS configure 或 connect 失败会将 MQTT state 转为 `ERROR` 并发出 `LWLTE_MQTT_EVENT_ERROR`。

## 验证

Build 与静态验证：

- 新增 SSL 与 MQTT transport 类型后 public headers 可编译。
- 现有明文 MQTT examples 继续构建，并默认使用明文 TCP。
- Core command clone/free 路径处理 PEM buffers 时无泄漏或悬空指针。
- 无效 auth mode、缺失必需 PEM、过大 PEM、无效 context、缺失已 provision context 返回预期错误。

Module command verification：

- Air780EP 命令顺序覆盖 `FSDEL`、`FSCREATE`、`FSWRITE` payload 与 `SSLCFG` binding。
- ML307R 命令顺序覆盖 `MSSLCERTWR`、`MSSLKEYWR`、`MSSLCFG` 与 `MQTTCFG="ssl"`。
- Status query 在每个模块上确认生成的证书对象存在。

Real hardware verification：

- 假定硬件已连接，声明实现完成前必须使用硬件验证。
- 使用当前 MQTT example server 配置：`CONFIG_EXAMPLE_MQTT_HOST`、`CONFIG_EXAMPLE_MQTT_TOKEN`、当前 ThingsBoard topics，以及现有 publish/subscribe flow。
- TLS MQTT 验证时，将 MQTT port 改为 `8883`，并将 MQTT transport 配置为 TLS。
- 至少验证一次成功连接、订阅 `v1/devices/me/attributes`，并向 `v1/devices/me/telemetry` 发布 telemetry。
- 若 Air780EP 与 ML307R 都可用，优先验证两者；否则记录实际物理测试的模块。

## 文档更新

- 更新 `src/include/lwlte.h` 中的公开 API 注释。
- 更新 examples，展示 TLS provisioning 与 TLS 验证路径的 MQTT port `8883`。
- 更新 `docs/agents/feature-roadmap.md`，反映选定的 SSL provisioning 设计。
- 避免覆盖 AT command reference docs 中无关的用户改动。只有当实现需要定向修正时，才更新 `docs/agents/at_cmd_air780ep.md` 或 `docs/agents/at_cmd_ml307r.md`。
