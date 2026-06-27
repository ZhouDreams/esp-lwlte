# TCP over TLS（SSL socket）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为现有 TCP client 增加可选 TLS，使 `lwlte_ssl_provision()` 配出的 SSL context 能被 socket/TCP 路径复用（Air780EP 用 `CIPSSL=1`+`CIPSTART` ctx 0；ML307R 用 `MIPCFG="ssl"`+`MIPOPEN`）。

**Architecture:** 与 MQTT TLS 完全镜像：公开 `lwlte_tcp_open_config_t` 增加 `transport`+`ssl_context_id`，逐层透传到 `modem_socket_open`；TLS 协商由模块在 open 命令内完成，tcp_client FSM 不变；`lwlte_ssl_provision()` 不改（已协议无关）。明文路径显式关闭模块 SSL 绑定避免残留。

**Tech Stack:** C, ESP-IDF, FreeRTOS, ESP Event, AT command modem adapters, Python host contract tests, ESP-IDF build/flash tools.

对应设计：`docs/superpowers/specs/2026-06-27-tcp-tls-ssl-socket-design.md`。

---

## Source Map

- `src/include/lwlte.h`：增加 `lwlte_tcp_transport_t` 枚举，`lwlte_tcp_open_config_t` 增加 `transport`+`ssl_context_id`。
- `src/lwlte/lwlte.c`：`lwlte_tcp_open()` 把新字段映射进 tcp_client 打开配置。
- `src/core/core.h`：增加 `core_socket_transport_t`，`core_socket_open_t` 增加字段。
- `src/core/core.c`：socket_open 命令 clone 复制新字段；`core_cmd_validate` 增加 transport 合法性。
- `src/core/core_fsm.c`：`CORE_CMD_SOCKET_OPEN` 派发时把 core 字段映射进 `modem_socket_open_t`。
- `src/modem/modem.h`：增加 `modem_socket_transport_t`，`modem_socket_open_t` 增加字段。
- `src/tcp_client/tcp_client.h`：`tcp_client_open_config_t` 增加字段。
- `src/tcp_client/tcp_client_priv.h`：打开参数存储结构增加字段。
- `src/tcp_client/tcp_client.c`：`tcp_client_open()` 复制新字段；`submit_socket_open()` 写入 `CORE_CMD_SOCKET_OPEN`。
- `src/modem/modem_air780ep.c`：`air780ep_socket_open()` 增加 TLS 分支（`SSLCFG hostname,0` + `CIPSSL=1`/`CIPSSL=0` + ctx 0 校验）。
- `src/modem/modem_ml307r.c`：`ml307r_socket_open()` 增加 TLS 分支（`MIPCFG="ssl"` + ctx<6 校验）。
- `example/Kconfig.projbuild`：新增 `EXAMPLE_TCP_TLS_ENABLE`、`EXAMPLE_TCP_TLS_CA_CERT_PEM`、TLS 时端口默认 443。
- `example/air780ep_tcp_client.c`、`example/ml307r_tcp_client.c`：TLS 时先 provision SSL context 再用 transport=TLS 打开。
- `tests/host/test_tcp_tls_ssl_socket_contract.py`：新增静态契约测试。
- `docs/agents/feature-roadmap.md`、`docs/agents/classes.md`、`example/README.md`：定点文档更新。

## Execution Notes

- 未经用户明确授权不要 `git commit`；每个 Task 末尾的 commit 说明是审批检查点，不是执行许可。
- 优先用 ESP-IDF MCP build 工具做编译验证；串口用 `docs/agents/serial_monitor.py`。
- 对大源文件一次只改一个职责，保持小补丁。
- 先跑 host 契约测试确认对新行为失败，再实现使其通过。
- 硬件验证前不得声明完成。
- 不修改 `docs/agents/at_cmd_*.md` 无关内容。

### Task 1: 增加失败的 host 契约测试

**Files:**
- Create: `tests/host/test_tcp_tls_ssl_socket_contract.py`

- [ ] **Step 1: 创建失败契约测试**

```python
#!/usr/bin/env python3
"""Static contract checks for TCP over TLS (SSL socket) reuse of SSL context."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class TcpTlsSslSocketContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.tcp_h = read("src/tcp_client/tcp_client.h")
        cls.tcp_c = read("src/tcp_client/tcp_client.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")
        cls.kconfig = read("example/Kconfig.projbuild")
        cls.air_example = read("example/air780ep_tcp_client.c")
        cls.ml_example = read("example/ml307r_tcp_client.c")

    def assert_has_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_public_tcp_transport_api_exists(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_TCP_TRANSPORT_PLAIN_TCP",
            "LWLTE_TCP_TRANSPORT_TLS",
            "lwlte_tcp_transport_t transport;",
            "uint8_t ssl_context_id;",
        ], "lwlte.h TCP transport")

    def test_core_and_tcp_thread_transport(self):
        self.assert_has_all(self.core_h, [
            "CORE_SOCKET_TRANSPORT_PLAIN_TCP",
            "CORE_SOCKET_TRANSPORT_TLS",
            "core_socket_transport_t transport;",
        ], "core.h socket transport")
        self.assert_has_all(self.core_c, [
            "clone->data.socket_open.transport",
            "clone->data.socket_open.ssl_context_id",
        ], "core.c socket clone")
        self.assert_has_all(self.core_fsm_c, [
            ".transport =",
            ".ssl_context_id =",
        ], "core_fsm.c socket mapping")
        self.assert_has_all(self.tcp_h + self.tcp_c, [
            "tcp_client_open_config_t",
            ".transport =",
            ".ssl_context_id =",
        ], "tcp_client transport threading")

    def test_modem_socket_transport_and_module_tls_mappings(self):
        self.assert_has_all(self.modem_h, [
            "MODEM_SOCKET_TRANSPORT_PLAIN_TCP",
            "MODEM_SOCKET_TRANSPORT_TLS",
            "modem_socket_transport_t transport;",
        ], "modem.h socket transport")
        self.assert_has_all(self.air_c, [
            "AIR780EP_SSL_TCP_CONTEXT_ID",
            "AT+CIPSSL=1",
            "AT+CIPSSL=0",
            'AT+SSLCFG="hostname",0,',
        ], "air780ep socket TLS")
        self.assert_has_all(self.ml_c, [
            'AT+MIPCFG="ssl",',
            "ml307r_ssl_context_marked",
        ], "ml307r socket TLS")

    def test_example_tls_config(self):
        self.assert_has_all(self.kconfig, [
            "EXAMPLE_TCP_TLS_ENABLE",
            "EXAMPLE_TCP_TLS_CA_CERT_PEM",
        ], "example Kconfig TCP TLS")
        self.assert_has_all(self.air_example + self.ml_example, [
            "LWLTE_TCP_TRANSPORT_TLS",
            "lwlte_ssl_provision",
        ], "tcp examples TLS path")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: 跑测试确认失败**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract -v`
Expected: FAIL（公开 API、core/tcp 穿线、modem 映射、example TLS 均不存在）。

- [ ] **Step 3: 检查改动范围**

Inspect: `git status --short` 应只多 `tests/host/test_tcp_tls_ssl_socket_contract.py`。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 2: 公开 API 类型

**Files:**
- Modify: `src/include/lwlte.h`（`lwlte_tcp_open_config_t` 附近，约 251-259 行）

- [ ] **Step 1: 增加 TCP 传输枚举**

在 `lwlte.h` 中 `lwlte_tcp_open_config_t` 定义之前加入：

```c
/**
 * @brief TCP 传输类型
 * @details TCP transport type
 */
typedef enum {
    LWLTE_TCP_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    LWLTE_TCP_TRANSPORT_TLS,            /**< TLS； TLS */
} lwlte_tcp_transport_t;
```

- [ ] **Step 2: 扩展 `lwlte_tcp_open_config_t`**

把现有：

```c
typedef struct {
    const char *host;                     /**< 目标主机或 IP； Target host or IP */
    uint16_t port;                        /**< 目标端口； Target port */
    void *user_ctx;                       /**< 用户上下文，事件中原样返回； User context returned in events */
} lwlte_tcp_open_config_t;
```

改为：

```c
typedef struct {
    const char           *host;           /**< 目标主机或 IP； Target host or IP */
    uint16_t              port;           /**< 目标端口； Target port */
    lwlte_tcp_transport_t transport;      /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t               ssl_context_id; /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
    void                 *user_ctx;       /**< 用户上下文，事件中原样返回； User context returned in events */
} lwlte_tcp_open_config_t;
```

- [ ] **Step 3: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_public_tcp_transport_api_exists -v`
Expected: PASS（其余用例仍 FAIL）。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 3: Modem socket 值对象

**Files:**
- Modify: `src/modem/modem.h`（`modem_socket_proto_t` 与 `modem_socket_open_t`，约 343-358 行）

- [ ] **Step 1: 增加 modem socket 传输枚举**

在 `modem_socket_proto_t` 之后加入：

```c
/**
 * @brief Socket 传输类型
 * @details Socket transport type
 */
typedef enum {
    MODEM_SOCKET_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    MODEM_SOCKET_TRANSPORT_TLS,            /**< TLS； TLS */
} modem_socket_transport_t;
```

- [ ] **Step 2: 扩展 `modem_socket_open_t`**

把现有：

```c
typedef struct {
    modem_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
    int *modem_error_code;
} modem_socket_open_t;
```

改为（新增 transport + ssl_context_id，保持字段顺序向后兼容）：

```c
typedef struct {
    modem_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
    int *modem_error_code;
    modem_socket_transport_t transport;  /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;              /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
} modem_socket_open_t;
```

- [ ] **Step 3: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_modem_socket_transport_and_module_tls_mappings -v`
Expected: 仍 FAIL（modem.h 的枚举通过，但模块映射 token 还没加）。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 4: Core socket 值对象、clone、派发映射、校验

**Files:**
- Modify: `src/core/core.h`（`core_socket_proto_t` 与 `core_socket_open_t`，约 190-200 行）
- Modify: `src/core/core.c`（socket_open clone 约 1011-1013、校验约 1197-1201）
- Modify: `src/core/core_fsm.c`（`CORE_CMD_SOCKET_OPEN` 派发约 736-747）

- [ ] **Step 1: 增加 core socket 传输枚举并扩展 `core_socket_open_t`**

`core.h` 在 `core_socket_proto_t` 之后加入：

```c
/**
 * @brief Socket 传输类型
 * @details Socket transport type
 */
typedef enum {
    CORE_SOCKET_TRANSPORT_PLAIN_TCP = 0,  /**< 明文 TCP； Plain TCP */
    CORE_SOCKET_TRANSPORT_TLS,            /**< TLS； TLS */
} core_socket_transport_t;
```

把 `core_socket_open_t` 改为：

```c
typedef struct {
    core_socket_proto_t proto;            /**< Socket 协议； Socket protocol */
    uint8_t conn_id;                      /**< 连接 ID； Connection ID */
    const char *host;                     /**< 主机； Host */
    uint16_t port;                        /**< 端口； Port */
    uint32_t timeout_ms;                  /**< 打开超时； Open timeout */
    core_socket_transport_t transport;    /**< 传输类型，0 为明文 TCP； Transport, 0 is plain TCP */
    uint8_t ssl_context_id;               /**< TLS 使用的 SSL context ID； SSL context ID for TLS */
} core_socket_open_t;
```

- [ ] **Step 2: core.c clone 复制新字段**

在 `core.c` 的 `case CORE_CMD_SOCKET_OPEN:` clone 分支（现有 `clone->data.socket_open.host = clone_optional_string(...)` 之后）追加：

```c
        clone->data.socket_open.transport = cmd->data.socket_open.transport;
        clone->data.socket_open.ssl_context_id = cmd->data.socket_open.ssl_context_id;
```

（free 分支只需释放 `host`；transport/ssl_context_id 是值类型，无需释放。）

- [ ] **Step 3: core.c 校验 transport 合法性**

在 `core_cmd_validate` 的 `case CORE_CMD_SOCKET_OPEN:`（现有 proto/host/port 校验）中追加 transport 范围校验：

```c
        return cmd->data.socket_open.proto == CORE_SOCKET_PROTO_TCP &&
               (cmd->data.socket_open.transport == CORE_SOCKET_TRANSPORT_PLAIN_TCP ||
                cmd->data.socket_open.transport == CORE_SOCKET_TRANSPORT_TLS) &&
               cmd->data.socket_open.host &&
               cmd->data.socket_open.host[0] != '\0' &&
               cmd->data.socket_open.port > 0;
```

- [ ] **Step 4: core_fsm.c 派发时映射到 modem**

在 `core_fsm.c` 的 `case CORE_CMD_SOCKET_OPEN:`（现有 `modem_socket_open_t request = {...}`）中追加两行，并做 core→modem 枚举转换：

```c
            modem_socket_open_t request = {
                .proto = MODEM_SOCKET_PROTO_TCP,
                .conn_id = cmd->data.socket_open.conn_id,
                .host = cmd->data.socket_open.host,
                .port = cmd->data.socket_open.port,
                .timeout_ms = cmd->data.socket_open.timeout_ms,
                .transport = (cmd->data.socket_open.transport == CORE_SOCKET_TRANSPORT_TLS)
                                 ? MODEM_SOCKET_TRANSPORT_TLS
                                 : MODEM_SOCKET_TRANSPORT_PLAIN_TCP,
                .ssl_context_id = cmd->data.socket_open.ssl_context_id,
            };
            ret = modem_socket_open(me->modem, &request);
```

- [ ] **Step 5: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_core_and_tcp_thread_transport -v`
Expected: core 相关断言 PASS（tcp_client 断言仍 FAIL）。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 5: tcp_client 与 Facade 穿线

**Files:**
- Modify: `src/tcp_client/tcp_client.h`（`tcp_client_open_config_t` 约 57-61 行）
- Modify: `src/tcp_client/tcp_client_priv.h`（打开参数存储结构，约 55-56 行 `host`/`port`）
- Modify: `src/tcp_client/tcp_client.c`（`tcp_client_open()` 约 256、`submit_socket_open()` 约 87/1443 及其调用处约 1148、`lwlte_tcp_open` 调用映射）
- Modify: `src/lwlte/lwlte.c`（`lwlte_tcp_open()` 把公开字段映射进 tcp_client 配置）

- [ ] **Step 1: 扩展 `tcp_client_open_config_t`**

`tcp_client.h` 已 include `core.h`（未 include `lwlte.h`），因此直接复用 core 的枚举，避免新增枚举与反向依赖。在现有 `tcp_client_open_config_t`（`host`/`port`/`user_ctx`）中增加：

```c
    core_socket_transport_t transport;    /**< 传输类型； Transport */
    uint8_t ssl_context_id;               /**< TLS SSL context ID； SSL context ID for TLS */
```

- [ ] **Step 2: 扩展打开参数存储结构**

`tcp_client_priv.h` 中存储打开参数的结构 `tcp_open_owned_t`（含 `char *host; uint16_t port;`）增加：

```c
    core_socket_transport_t transport;
    uint8_t ssl_context_id;
```

并在其 free 路径无需改动（值类型）。

- [ ] **Step 3: `tcp_client_open()` 复制新字段**

在 `tcp_client.c` 的 `tcp_client_open()` 中，克隆 `host`（现有 `open->host = clone_string(config->host)`）之后追加：

```c
    open->transport = config->transport;
    open->ssl_context_id = config->ssl_context_id;
```

- [ ] **Step 4: `submit_socket_open()` 写入命令并改签名**

把 `submit_socket_open(tcp_client_conn_t *conn, const char *host, uint16_t port)` 改为增加两个参数（直接用 core 枚举，tcp_client 不做转换）：

```c
static esp_err_t submit_socket_open(tcp_client_conn_t *conn, const char *host,
                                    uint16_t port,
                                    core_socket_transport_t transport,
                                    uint8_t ssl_context_id)
```

在其构造的 `core_cmd_t cmd` 的 `.data.socket_open = {...}` 中追加：

```c
            .transport = transport,
            .ssl_context_id = ssl_context_id,
```

同步更新前置声明（约 87 行）签名。

- [ ] **Step 5: 更新 `submit_socket_open` 调用处**

`tcp_client.c` 约 1148 行调用处改为：

```c
    if (submit_socket_open(conn, open->host, open->port,
                           open->transport, open->ssl_context_id) != ESP_OK) {
```

- [ ] **Step 6: Facade `lwlte_tcp_open()` 映射公开字段**

`src/lwlte/lwlte.c` 的 `lwlte_tcp_open()` 中，构造 `tcp_client_open_config_t` 处，把公开枚举映射成 core 枚举并透传 context id：

```c
        .transport = (config->transport == LWLTE_TCP_TRANSPORT_TLS)
                         ? CORE_SOCKET_TRANSPORT_TLS
                         : CORE_SOCKET_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = config->ssl_context_id,
```

并在该函数的公开参数校验中增加：`transport` 为 `LWLTE_TCP_TRANSPORT_PLAIN_TCP` 或 `LWLTE_TCP_TRANSPORT_TLS`。

- [ ] **Step 7: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_core_and_tcp_thread_transport -v`
Expected: PASS。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 6: Air780EP socket TLS

**Files:**
- Modify: `src/modem/modem_air780ep.c`（常量区约 63 行、`air780ep_socket_open()` 约 4268-4329）

- [ ] **Step 1: 增加 TCP SSL context 常量**

在 `AIR780EP_SSL_MQTT_CONTEXT_ID` 定义附近加入：

```c
#define AIR780EP_SSL_TCP_CONTEXT_ID       0   /**< TCP SSL socket 固定 context 0； TCP SSL socket fixed ctx 0 */
```

- [ ] **Step 2: 在 `air780ep_socket_open()` 增加 TLS/明文分支**

在现有 `char *host = escape_at_string(open->host);` 成功之后、构造 `AT+CIPSTART` 之前，插入 SSL 通道配置。完整新函数开头到 CIPSTART 构造前：

```c
    modem_air780ep_t *self = to_air780ep(me);

    /* SSL 通道开关：TLS 写 ctx0 hostname + CIPSSL=1；明文显式 CIPSSL=0 清残留。 */
    if (open->transport == MODEM_SOCKET_TRANSPORT_TLS) {
        ESP_RETURN_ON_FALSE(open->ssl_context_id == AIR780EP_SSL_TCP_CONTEXT_ID,
                            ESP_ERR_INVALID_ARG, TAG,
                            "Air780EP TCP TLS requires ssl_context_id 0");
        ESP_RETURN_ON_FALSE(ssl_context_marked(self, AIR780EP_SSL_TCP_CONTEXT_ID),
                            ESP_ERR_INVALID_STATE, TAG,
                            "Air780EP TCP TLS context 0 not provisioned");

        char *host = escape_at_string(open->host);
        ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape socket host failed");

        char hostname_cmd[160];
        int hn = snprintf(hostname_cmd, sizeof(hostname_cmd),
                          "AT+SSLCFG=\"hostname\",0,\"%s\"", host);
        free(host);
        ESP_RETURN_ON_FALSE(hn > 0 && (size_t)hn < sizeof(hostname_cmd),
                            ESP_ERR_INVALID_ARG, TAG, "SSLCFG hostname truncated");

        air780ep_cmd_ctx_t hctx;
        esp_err_t ret = send_cmd(self, hostname_cmd, &hctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set SSL hostname failed");
        ret = ensure_at_ok(&hctx.response, "AT+SSLCFG hostname");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+SSLCFG hostname not OK");

        air780ep_cmd_ctx_t sctx;
        ret = send_cmd(self, "AT+CIPSSL=1", &sctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set CIPSSL=1 failed");
        ret = ensure_at_ok(&sctx.response, "AT+CIPSSL=1");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPSSL=1 not OK");
    } else {
        air780ep_cmd_ctx_t sctx;
        esp_err_t ret = send_cmd(self, "AT+CIPSSL=0", &sctx, AIR780EP_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set CIPSSL=0 failed");
        ret = ensure_at_ok(&sctx.response, "AT+CIPSSL=0");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+CIPSSL=0 not OK");
    }

    char *host = escape_at_string(open->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape socket host failed");
```

> 后续 CIPSTART 构造、`send_cmd_with_options`、`CONNECT OK`/`ALREADY CONNECT` 判定全部保留原样。TLS 分支内的 `host` 在 `if` 块作用域内、在外层 `host` 之前释放，二者作用域不重叠，无重定义冲突。

- [ ] **Step 3: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_modem_socket_transport_and_module_tls_mappings -v`
Expected: Air780EP 相关断言 PASS（ML307R 断言仍 FAIL）。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 7: ML307R socket TLS

**Files:**
- Modify: `src/modem/modem_ml307r.c`（`ml307r_socket_open()` 约 4250-4331）

- [ ] **Step 1: 在 `ml307r_socket_open()` 增加 TLS/明文分支**

在现有 `esp_err_t ret = ml307r_socket_prepare(self);` 成功之后、`escape_at_string(open->host)` 之前，插入 `MIPCFG="ssl"` 配置：

```c
    modem_ml307r_t *self = to_ml307r(me);
    esp_err_t ret = ml307r_socket_prepare(self);
    ESP_RETURN_ON_ERROR(ret, TAG, "prepare ML307R TCP socket failed");

    /* SSL 绑定：TLS 绑 ssl_id；明文显式解绑清残留。 */
    {
        char ssl_cfg_cmd[48];
        int sn;
        if (open->transport == MODEM_SOCKET_TRANSPORT_TLS) {
            ESP_RETURN_ON_FALSE(open->ssl_context_id < ML307R_SSL_MAX_CONTEXTS,
                                ESP_ERR_INVALID_ARG, TAG,
                                "ML307R TCP TLS ssl_context_id out of range");
            ESP_RETURN_ON_FALSE(ml307r_ssl_context_marked(self, open->ssl_context_id),
                                ESP_ERR_INVALID_STATE, TAG,
                                "ML307R TCP TLS context not provisioned");
            sn = snprintf(ssl_cfg_cmd, sizeof(ssl_cfg_cmd),
                          "AT+MIPCFG=\"ssl\",%u,1,%u",
                          (unsigned int)open->conn_id,
                          (unsigned int)open->ssl_context_id);
        } else {
            sn = snprintf(ssl_cfg_cmd, sizeof(ssl_cfg_cmd),
                          "AT+MIPCFG=\"ssl\",%u,0,0",
                          (unsigned int)open->conn_id);
        }
        ESP_RETURN_ON_FALSE(sn > 0 && (size_t)sn < sizeof(ssl_cfg_cmd),
                            ESP_ERR_INVALID_ARG, TAG, "AT+MIPCFG ssl truncated");

        ml307r_cmd_ctx_t sctx;
        ret = send_cmd(self, ssl_cfg_cmd, &sctx, ML307R_SSL_CMD_TIMEOUT_MS);
        ESP_RETURN_ON_ERROR(ret, TAG, "set MIPCFG ssl failed");
        ret = ensure_at_ok(&sctx.response, "AT+MIPCFG ssl");
        ESP_RETURN_ON_ERROR(ret, TAG, "AT+MIPCFG ssl not OK");
    }

    char *host = escape_at_string(open->host);
    ESP_RETURN_ON_FALSE(host, ESP_ERR_NO_MEM, TAG, "escape socket host failed");
```

> 后续 `AT+MIPOPEN` 构造、`+MIPOPEN:` 解析、`parse_mipopen_response`、`ml307r_map_mipopen_result` 全部保留原样。TLS 握手失败（如 753）会经 `open_result` 写入 `open->modem_error_code` 并由 `ml307r_map_mipopen_result` 映射。

- [ ] **Step 2: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract -v`
Expected: PASS（全部用例）。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 8: Example TLS 配置与流程

**Files:**
- Modify: `example/Kconfig.projbuild`（TCP settings 菜单约 40-80 行）
- Modify: `example/air780ep_tcp_client.c`
- Modify: `example/ml307r_tcp_client.c`

- [ ] **Step 1: Kconfig 增加 TCP TLS 开关与 CA**

在 `Example TCP Settings` 菜单内（`config EXAMPLE_TCP_PORT` 之后）加入：

```kconfig
config EXAMPLE_TCP_TLS_ENABLE
    bool "Enable TCP TLS (SSL socket)"
    default n

config EXAMPLE_TCP_TLS_CA_CERT_PEM
    string "TCP TLS CA certificate PEM"
    default ""
    depends on EXAMPLE_TCP_TLS_ENABLE
    help
        Paste the server CA PEM used by the TCP TLS endpoint. Required when
        EXAMPLE_TCP_TLS_ENABLE=y and auth is server authentication.
```

并把 `config EXAMPLE_TCP_PORT` 的默认值改为受 TLS 开关影响：

```kconfig
config EXAMPLE_TCP_PORT
    int "TCP server port"
    range 1 65535
    default 443 if EXAMPLE_TCP_TLS_ENABLE
    default 4242
```

- [ ] **Step 2: Air780EP TCP example TLS 流程**

在 `example/air780ep_tcp_client.c` 中，打开 socket 之前、网络 online 之后，按 `CONFIG_EXAMPLE_TCP_TLS_ENABLE` 进行 SSL provision（镜像 `air780ep_mqtt_client.c` 的 provision 段：`auth_mode = LWLTE_SSL_AUTH_SERVER`、`ignore_cert_time=true`、`hostname=CONFIG_EXAMPLE_TCP_HOST`、CA 取自 `CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM`，`context_id=0`），并在打开时设置：

```c
    const lwlte_tcp_open_config_t open_cfg = {
        .host = CONFIG_EXAMPLE_TCP_HOST,
        .port = CONFIG_EXAMPLE_TCP_PORT,
        .transport = CONFIG_EXAMPLE_TCP_TLS_ENABLE
                        ? LWLTE_TCP_TRANSPORT_TLS
                        : LWLTE_TCP_TRANSPORT_PLAIN_TCP,
        .ssl_context_id = 0,
        .user_ctx = NULL,
    };
```

TLS 开启但 CA 为空时打印错误并 idle（与 MQTT example 一致）。Plain 路径保持现状。

- [ ] **Step 3: ML307R TCP example TLS 流程**

在 `example/ml307r_tcp_client.c` 做同样处理：TLS 时先 provision（`context_id=0`，ML307R ssl_id 0..5，这里用 0），再用上面的 `open_cfg`（`transport=TLS`、`ssl_context_id=0`）打开。两块板子对外的 `lwlte_tcp_open_config_t` 用法完全一致（模块差异在 modem 层吸收）。

- [ ] **Step 4: 跑契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract.TcpTlsSslSocketContractTest.test_example_tls_config -v`
Expected: PASS。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 9: 文档更新

**Files:**
- Modify: `docs/agents/feature-roadmap.md`
- Modify: `docs/agents/classes.md`（`modem_socket_open_t` 约 514-521、socket ops 表约 640）
- Modify: `example/README.md`（TCP example 段）

- [ ] **Step 1: roadmap** 把 TCP-TLS 从"后续/预留"标记为已实现（参照 MQTT-TLS 那一行的写法），注明复用 `lwlte_ssl_provision()`、Air780EP 用 ctx 0（`CIPSSL`）、ML307R 用 `MIPCFG="ssl"`。

- [ ] **Step 2: classes.md** 在 `modem_socket_open_t` 结构体补充 `transport`/`ssl_context_id` 字段说明；在 socket ops 表 `socket_open` 行的"Air780EP `AT+CIPSTART`；ML307R `AT+MIPOPEN`"后补“TLS 时分别前置 `AT+CIPSSL=1`（ctx 0）/ `AT+MIPCFG="ssl",<conn>,1,<ssl_id>`”。

- [ ] **Step 3: example/README.md** 在 TCP Client Examples 段补一句：`EXAMPLE_TCP_TLS_ENABLE=y` + `EXAMPLE_TCP_TLS_CA_CERT_PEM` 启用 SSL socket（端口默认 443），TLS 前需先 `lwlte_ssl_provision()`。

- [ ] **Step 4: 跑全量契约测试**

Run: `python3 -m unittest tests.host.test_tcp_tls_ssl_socket_contract -v`
Expected: PASS。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 10: 全量 host 契约套件 + ESP-IDF 编译

**Files:** 无新改动（验证 Task）

- [ ] **Step 1: 跑全量 host 契约套件（含既有用例，确认无回归）**

Run: `python3 -m unittest discover -s tests/host -v`
Expected: 全部 PASS（既有 `test_tcp_client_end_to_end_contract`、`test_mqtt_*` 等不得回归）。

- [ ] **Step 2: ESP-IDF 编译（plain 默认配置）**

确保 `sdkconfig` 中 `EXAMPLE_TCP_TLS_ENABLE` 未设/为 n，`EXAMPLE_SELECTED` 任选一个 TCP example。
Run（MCP）: build project。
Expected: 编译成功，plain TCP 行为不变。

- [ ] **Step 3: ESP-IDF 编译（TLS 配置）**

`sdkconfig` 设 `CONFIG_EXAMPLE_TCP_TLS_ENABLE=y`、`CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM` 填一段测试 CA、`EXAMPLE_TCP_PORT=443`、`EXAMPLE_TCP_HOST` 设为国内端点（见 Task 11）。
Run（MCP）: build project。
Expected: 编译成功。
Do not commit unless the user explicitly authorizes committing this checkpoint.

### Task 11: 真机 TCP-TLS 验证（国内端点）

**Files:** 无（硬件验证 Task）

- [ ] **Step 1: 准备国内 TLS 端点**

目标端点 `www.baidu.com:443`（国内、ECDSA/RSA 证书、不要求客户端证书）。准备其 CA PEM（可用 `openssl s_client -showcerts -connect www.baidu.com:443 </dev/null` 取链路根 CA）填入 `CONFIG_EXAMPLE_TCP_TLS_CA_CERT_PEM`，`CONFIG_EXAMPLE_TCP_HOST=www.baidu.com`、`CONFIG_EXAMPLE_TCP_PORT=443`、`CONFIG_EXAMPLE_TCP_TLS_ENABLE=y`。

- [ ] **Step 2: Air780EP 真机验证**

`EXAMPLE_SELECTED=EXAMPLE_AIR780EP_TCP_CLIENT`，build、flash `/dev/cu.usbserial-1130`，monitor。
Run: `python3 docs/agents/serial_monitor.py --timeout 60 --port /dev/cu.usbserial-1130`
Expected: 网络 online → SSL provision 成功（`SSLCFG`/`CIPSSL=1`）→ `CIPSTART` 后 `CONNECT OK` → 一次收发 → 正常关闭；无 `CONNECT FAIL`。

- [ ] **Step 3: ML307R 真机验证**

`EXAMPLE_SELECTED=EXAMPLE_ML307R_TCP_CLIENT`，build、flash、monitor。
Run: `python3 docs/agents/serial_monitor.py --timeout 75 --port /dev/cu.usbserial-1130`
Expected: 网络 online → SSL provision 成功（`MSSLCFG`/`MSSLCERTWR`）→ `MIPCFG="ssl",0,1,0` → `MIPOPEN` 返回 `+MIPOPEN: 0,0` → 一次收发 → 正常关闭；无 `+MIPOPEN: 0,753` 之类握手错误。

- [ ] **Step 4: 记录结果**

在交付说明里记录：两块板子分别是否完成 TLS 建链+收发+关闭；若只有一块可用，记录实际验证的模块与端点；附关键串口片段。

> 若 `www.baidu.com` 端点在验证中不可用，可换其它国内 TLS 端点并在记录里注明。MQTT-TLS 仍指向海外 broker，属已知网络限制，不作为本扩展回归判据。
