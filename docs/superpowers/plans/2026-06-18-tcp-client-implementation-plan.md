# TCP Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the first asynchronous plain TCP client API for Air780EP and ML307R, with binary-safe TX/RX and MQTT coexistence.

**Architecture:** Public code uses `lwlte_tcp_*` APIs and opaque `lwlte_tcp_conn_t *` handles. Facade owns a new internal `tcp_client` service, the service submits generic socket commands to Core, Core serializes operations through Modem, and each Modem subclass maps socket ops to module-specific AT commands. Core protocol callbacks become protocol-indexed so MQTT and TCP can run on the same `lwlte_handle_t`.

**Tech Stack:** ESP-IDF C, FreeRTOS queues/tasks/semaphores, ESP event loop, existing AT Engine command APIs, static Python host contract tests with `pytest`.

---

## Scope Check

This plan implements one feature slice: plain TCP client v1. UDP, TLS, server/listen mode, local port binding, multi-connection support, automatic reconnect, and high-throughput streaming are intentionally outside this implementation.

The plan follows the approved spec at `docs/superpowers/specs/2026-06-18-tcp-client-design.md`.

Repository commit policy: do not run `git commit` unless the user explicitly authorizes commits. Each task includes a guarded commit command for sessions where that authorization has been granted.

## File Structure

Create:

- `tests/host/test_tcp_client_end_to_end_contract.py`: static regression contract for public API, service boundaries, Core routing, Modem socket API, module command mapping, examples, and docs.
- `src/tcp_client/tcp_client.h`: private service API used by Facade only.
- `src/tcp_client/tcp_client_priv.h`: TCP service FSM internals, connection object, send FIFO item, default constants.
- `src/tcp_client/tcp_client.c`: TCP client service implementation, event posting, protocol callbacks, connection lifecycle, send FIFO, receive pump.
- `example/air780ep_tcp_client.c`: Air780EP TCP echo/client example.
- `example/ml307r_tcp_client.c`: ML307R TCP echo/client example.

Modify:

- `src/CMakeLists.txt`: add `tcp_client/tcp_client.c` and `tcp_client` private include directory.
- `src/include/lwlte.h`: add public TCP types, `LWLTE_TCP_EVENT`, config structs, event data, and `lwlte_tcp_*` prototypes.
- `src/lwlte/lwlte_priv.h`: include `tcp_client.h` and add `tcp_client_handle_t *tcp` to `struct lwlte_handle`.
- `src/lwlte/lwlte.c`: define `LWLTE_TCP_EVENT`, map TCP states, add TCP facade wrappers, release TCP event payloads, destroy TCP before MQTT/Ping/Core.
- `src/core/core.h`: add `CORE_PROTOCOL_TCP`, `conn_id/reason/modem_error_code` protocol fields, protocol-indexed callback registration signatures, socket command types and command data.
- `src/core/core_priv.h`: replace single protocol callback slots with arrays indexed by `CORE_PROTOCOL_MAX`.
- `src/core/core.c`: clone/free/validate socket commands, update protocol callback registration implementation.
- `src/core/core_fsm.c`: route protocol data/closed events by protocol, execute `CORE_CMD_SOCKET_*`, return socket receive result ownership to TCP service.
- `src/modem/modem.h`: add `MODEM_PROTOCOL_TCP`, protocol routing fields, socket value objects, socket wrapper prototypes.
- `src/modem/modem_priv.h`: add socket op typedefs and fields to `modem_ops_t`.
- `src/modem/modem.c`: implement socket wrapper validation and free protocol payload routing fields safely.
- `src/modem/modem_air780ep.c`: implement TCP socket setup/open/send/recv/close and Air780EP TCP URCs.
- `src/modem/modem_ml307r.c`: implement ML307R socket setup/open/send/recv/close and ML307R TCP URCs.
- `src/mqtt_client/mqtt_client.c`: update Core protocol callback registration calls to pass `CORE_PROTOCOL_MQTT`.
- `example/CMakeLists.txt`: add TCP examples.
- `example/example.h`: add TCP example IDs and run prototypes.
- `example/main.c`: add switch cases for TCP examples.
- `example/Kconfig.projbuild`: add TCP example host/port/payload/timeouts/config.
- `example/README.md`: document TCP examples and settings.
- `docs/agents/directory-structure.md`: add `src/tcp_client/` and TCP examples.
- `docs/agents/feature-roadmap.md`: mark TCP client v1 status and UDP remaining planned.
- `docs/agents/architecture.md`: add `tcp_client -> Core -> Modem -> AT Engine` boundary.
- `docs/agents/classes.md`: add TCP Client Service and socket command/value-object sections.
- `docs/agents/at_cmd_air780ep.md`: mark chosen Air780EP TCP RX/TX path.
- `docs/agents/at_cmd_ml307r.md`: mark chosen ML307R TCP RX/TX path.

---

### Task 1: TCP Contract Test

**Files:**

- Create: `tests/host/test_tcp_client_end_to_end_contract.py`

- [ ] **Step 1: Write the failing static contract test**

Create `tests/host/test_tcp_client_end_to_end_contract.py` with this complete content:

```python
#!/usr/bin/env python3
"""Static end-to-end contract checks for TCP client v1."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read_optional(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class TcpClientEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional("src/include/lwlte.h")
        cls.lwlte_priv = read_optional("src/lwlte/lwlte_priv.h")
        cls.lwlte_c = read_optional("src/lwlte/lwlte.c")
        cls.core_h = read_optional("src/core/core.h")
        cls.core_priv = read_optional("src/core/core_priv.h")
        cls.core_c = read_optional("src/core/core.c")
        cls.core_fsm_c = read_optional("src/core/core_fsm.c")
        cls.modem_h = read_optional("src/modem/modem.h")
        cls.modem_priv = read_optional("src/modem/modem_priv.h")
        cls.modem_c = read_optional("src/modem/modem.c")
        cls.air780ep_c = read_optional("src/modem/modem_air780ep.c")
        cls.ml307r_c = read_optional("src/modem/modem_ml307r.c")
        cls.mqtt_c = read_optional("src/mqtt_client/mqtt_client.c")
        cls.tcp_h = read_optional("src/tcp_client/tcp_client.h")
        cls.tcp_priv = read_optional("src/tcp_client/tcp_client_priv.h")
        cls.tcp_c = read_optional("src/tcp_client/tcp_client.c")
        cls.src_cmake = read_optional("src/CMakeLists.txt")
        cls.example_h = read_optional("example/example.h")
        cls.example_main = read_optional("example/main.c")
        cls.example_cmake = read_optional("example/CMakeLists.txt")
        cls.example_kconfig = read_optional("example/Kconfig.projbuild")
        cls.air_example = read_optional("example/air780ep_tcp_client.c")
        cls.ml_example = read_optional("example/ml307r_tcp_client.c")
        cls.classes_md = read_optional("docs/agents/classes.md")
        cls.arch_md = read_optional("docs/agents/architecture.md")
        cls.roadmap_md = read_optional("docs/agents/feature-roadmap.md")

    def assert_contains_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def assert_function_body(self, source: str, name: str) -> str:
        match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
        self.assertIsNotNone(match, f"missing function body for {name}")
        start = match.end() - 1
        depth = 0
        for pos in range(start, len(source)):
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:pos + 1]
        self.fail(f"unterminated function body for {name}")

    def test_public_tcp_api_exists(self):
        self.assert_contains_all(self.lwlte_h, [
            "typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;",
            "LWLTE_TCP_CONN_STATE_CREATED",
            "LWLTE_TCP_CONN_STATE_CONNECTED",
            "LWLTE_TCP_EVENT_CONNECTED",
            "LWLTE_TCP_EVENT_DATA",
            "ESP_EVENT_DECLARE_BASE(LWLTE_TCP_EVENT)",
            "lwlte_tcp_config_t",
            "lwlte_tcp_open_config_t",
            "lwlte_tcp_event_data_t",
            "esp_err_t lwlte_tcp_init(lwlte_handle_t *me,",
            "esp_err_t lwlte_tcp_open(lwlte_handle_t *me,",
            "esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn,",
            "esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t *conn);",
            "esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t *conn);",
            "void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data);",
        ], "lwlte.h")
        self.assertIn("ESP_EVENT_DEFINE_BASE(LWLTE_TCP_EVENT)", self.lwlte_c)
        release_body = self.assert_function_body(self.lwlte_c, "lwlte_tcp_event_data_release")
        self.assertIn("data->owns_payload", release_body)
        self.assertIn("free((void *)data->payload)", release_body)

    def test_tcp_service_layer_boundary_and_cmake(self):
        self.assertIn('"tcp_client/tcp_client.c"', self.src_cmake)
        self.assertRegex(self.src_cmake, r"PRIV_INCLUDE_DIRS[\s\S]*\btcp_client\b")
        self.assert_contains_all(self.tcp_h + self.tcp_priv + self.tcp_c, [
            "typedef struct tcp_client_handle tcp_client_handle_t;",
            "typedef struct tcp_client_conn tcp_client_conn_t;",
            "tcp_client_create",
            "tcp_client_open",
            "tcp_client_send",
            "tcp_client_close",
            "tcp_client_conn_destroy",
            "TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN",
            "TCP_SIG_PROTOCOL_DATA",
            "TCP_SIG_PROTOCOL_CLOSED",
            "CORE_CMD_SOCKET_OPEN",
            "CORE_CMD_SOCKET_SEND",
            "CORE_CMD_SOCKET_RECV",
            "CORE_CMD_SOCKET_CLOSE",
            "core_register_protocol_callback(core, CORE_PROTOCOL_TCP",
            "core_register_protocol_closed_callback(core, CORE_PROTOCOL_TCP",
        ], "tcp service")
        for forbidden in [
            '#include "modem.h"',
            '#include "modem_air780ep.h"',
            '#include "modem_ml307r.h"',
            '#include "at_engine.h"',
            '#include "core_priv.h"',
        ]:
            self.assertNotIn(forbidden, self.tcp_h + self.tcp_priv + self.tcp_c)

    def test_facade_owns_tcp_and_destroys_before_core(self):
        self.assertIn('#include "tcp_client.h"', self.lwlte_priv)
        self.assertIn("tcp_client_handle_t *tcp;", self.lwlte_priv)
        self.assert_contains_all(self.lwlte_c, [
            "static lwlte_tcp_conn_state_t map_tcp_conn_state",
            "tcp_client_create(&tcp_config, core)",
            "tcp_client_open(tcp, &open_config",
            "tcp_client_send((tcp_client_conn_t *)conn",
            "tcp_client_close((tcp_client_conn_t *)conn)",
            "tcp_client_conn_destroy((tcp_client_conn_t *)conn)",
        ], "lwlte.c")
        destroy_body = self.lwlte_c[self.lwlte_c.rindex("static esp_err_t destroy_owned_resources"):]
        self.assertIn("tcp_client_destroy(me->tcp)", destroy_body)
        self.assertLess(destroy_body.index("tcp_client_destroy(me->tcp)"), destroy_body.index("core_destroy(me->core)"))

    def test_core_protocol_callbacks_are_protocol_indexed(self):
        self.assert_contains_all(self.core_h + self.core_priv + self.core_c, [
            "CORE_PROTOCOL_MQTT",
            "CORE_PROTOCOL_TCP",
            "CORE_PROTOCOL_MAX",
            "uint8_t conn_id;",
            "int reason;",
            "int modem_error_code;",
            "protocol_callbacks[CORE_PROTOCOL_MAX]",
            "protocol_closed_callbacks[CORE_PROTOCOL_MAX]",
            "core_register_protocol_callback(core_handle_t *me,",
            "core_protocol_t protocol,",
            "core_register_protocol_closed_callback(core_handle_t *me,",
        ], "core protocol routing")
        self.assertIn("core_register_protocol_callback(core, CORE_PROTOCOL_MQTT", self.mqtt_c)
        self.assertIn("core_register_protocol_closed_callback(core, CORE_PROTOCOL_MQTT", self.mqtt_c)
        self.assertNotIn("core_register_protocol_callback(core, mqtt_protocol_data_cb, me)", self.mqtt_c)

    def test_core_socket_commands_exist_and_clone_ownership(self):
        self.assert_contains_all(self.core_h + self.core_c + self.core_fsm_c, [
            "CORE_SOCKET_PROTO_TCP",
            "core_socket_open_t",
            "core_socket_send_t",
            "core_socket_recv_t",
            "core_socket_recv_result_t",
            "CORE_CMD_SOCKET_OPEN",
            "CORE_CMD_SOCKET_SEND",
            "CORE_CMD_SOCKET_RECV",
            "CORE_CMD_SOCKET_CLOSE",
            "modem_socket_open",
            "modem_socket_send",
            "modem_socket_recv",
            "modem_socket_close",
            "clone_payload(cmd->data.socket_send.data",
            "free((void *)cmd->data.socket_open.host)",
            "free((void *)cmd->data.socket_send.data)",
            "CORE_NET_STATE_ONLINE",
            "ESP_ERR_INVALID_STATE",
        ], "core socket commands")

    def test_modem_socket_api_exists(self):
        self.assert_contains_all(self.modem_h + self.modem_priv + self.modem_c, [
            "MODEM_PROTOCOL_TCP",
            "MODEM_SOCKET_PROTO_TCP",
            "modem_socket_open_t",
            "modem_socket_send_t",
            "modem_socket_recv_t",
            "modem_socket_recv_result_t",
            "modem_socket_close_t",
            "esp_err_t modem_socket_open(modem_handle_t *me,",
            "esp_err_t modem_socket_send(modem_handle_t *me,",
            "esp_err_t modem_socket_recv(modem_handle_t *me,",
            "esp_err_t modem_socket_close(modem_handle_t *me,",
            "modem_socket_open_fn socket_open;",
            "modem_socket_send_fn socket_send;",
            "modem_socket_recv_fn socket_recv;",
            "modem_socket_close_fn socket_close;",
        ], "modem socket api")

    def test_air780ep_tcp_mapping_tokens(self):
        self.assert_contains_all(self.air780ep_c, [
            "AT+CIPMUX=0",
            "AT+CIPMODE=0",
            "AT+CIPQSEND=1",
            "AT+CIPRXF=1",
            "AT+CIPRXGET=5",
            "AT+CIPSTART=\"TCP\",\"%s\",%u",
            "AT+CIPSEND=%u",
            "at_engine_send_cmd_with_payload",
            "AT+CIPRXGET=3,%u",
            "AIR780EP_TCP_MAX_HEX_READ_BYTES",
            "CONNECT OK",
            "ALREADY CONNECT",
            "DATAACCEPT",
            "SEND OK",
            "CLOSE OK",
            "MODEM_PROTOCOL_TCP",
            ".socket_open = air780ep_socket_open",
            ".socket_send = air780ep_socket_send",
            ".socket_recv = air780ep_socket_recv",
            ".socket_close = air780ep_socket_close",
        ], "air780ep tcp mapping")

    def test_ml307r_tcp_mapping_tokens(self):
        self.assert_contains_all(self.ml307r_c, [
            "AT+MIPCFG=\"cid\",0,%u",
            "AT+MIPCFG=\"encoding\",0,0,1",
            "AT+MIPCFG=\"autofree\",0,0",
            "AT+MIPOPEN=0,\"TCP\",\"%s\",%u,%u,2",
            "AT+MIPSEND=0,%u",
            "at_engine_send_cmd_with_payload",
            "AT+MIPRD=0,%u",
            "+MIPOPEN:",
            "+MIPSEND:",
            "+MIPURC: \"rtcp\"",
            "+MIPURC: \"disconn\"",
            "MODEM_PROTOCOL_TCP",
            ".socket_open = ml307r_socket_open",
            ".socket_send = ml307r_socket_send",
            ".socket_recv = ml307r_socket_recv",
            ".socket_close = ml307r_socket_close",
        ], "ml307r tcp mapping")

    def test_examples_and_docs_are_wired(self):
        self.assert_contains_all(self.example_h + self.example_main + self.example_cmake, [
            "EXAMPLE_AIR780EP_TCP_CLIENT",
            "EXAMPLE_ML307R_TCP_CLIENT",
            "example_air780ep_tcp_client_run",
            "example_ml307r_tcp_client_run",
            '"air780ep_tcp_client.c"',
            '"ml307r_tcp_client.c"',
        ], "example wiring")
        self.assert_contains_all(self.example_kconfig, [
            "EXAMPLE_TCP_HOST",
            "EXAMPLE_TCP_PORT",
            "EXAMPLE_TCP_PAYLOAD",
            "EXAMPLE_TCP_PAYLOAD_HEX",
            "EXAMPLE_TCP_MAX_RX_EVENT_LEN",
        ], "example kconfig")
        for label, text in [("air example", self.air_example), ("ml example", self.ml_example)]:
            self.assert_contains_all(text, [
                "lwlte_tcp_init",
                "esp_event_handler_register(LWLTE_TCP_EVENT",
                "lwlte_tcp_open",
                "lwlte_tcp_send",
                "lwlte_tcp_event_data_release",
                "base.at_engine.rx_line_buf_size = 2048",
            ], label)
        self.assertIn("TCP Client Service", self.classes_md)
        self.assertIn("tcp_client -> Core -> Modem", self.arch_md)
        self.assertIn("TCP client v1", self.roadmap_md)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the contract test and verify it fails**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py -q
```

Expected: FAIL, with missing TCP API/service tokens such as `lwlte_tcp_config_t`, `tcp_client_create`, or `CORE_CMD_SOCKET_OPEN`.

- [ ] **Step 3: Commit if explicitly authorized**

Only if the user has explicitly authorized commits, run:

```bash
git add tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "test: add tcp client contract"
```

Expected: commit succeeds. If commit authorization is absent, leave the file unstaged or staged according to the current session workflow and continue.

---

### Task 2: Public TCP Facade Types

**Files:**

- Modify: `src/include/lwlte.h`
- Modify: `src/lwlte/lwlte.c`

- [ ] **Step 1: Add public TCP types and event base declaration**

In `src/include/lwlte.h`, add this block after `typedef struct lwlte_handle lwlte_handle_t;` and before the state enums:

```c
/**
 * @brief LTE TCP 连接句柄
 * @details LTE TCP connection handle
 */
typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;
```

Add this block after `lwlte_mqtt_state_t`:

```c
/**
 * @brief LTE TCP 连接状态
 * @details LTE TCP connection state
 */
typedef enum {
    LWLTE_TCP_CONN_STATE_CREATED = 0,     /**< 已创建； Created */
    LWLTE_TCP_CONN_STATE_CONNECTING,      /**< 连接中； Connecting */
    LWLTE_TCP_CONN_STATE_CONNECTED,       /**< 已连接； Connected */
    LWLTE_TCP_CONN_STATE_CLOSING,         /**< 关闭中； Closing */
    LWLTE_TCP_CONN_STATE_CLOSED,          /**< 已关闭； Closed */
    LWLTE_TCP_CONN_STATE_ERROR,           /**< 错误； Error */
} lwlte_tcp_conn_state_t;
```

Add this event base declaration after `ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT);`:

```c
/**
 * @brief LTE TCP 用户事件 base
 * @details LTE TCP user event base (esp_event_base_t string identifier)
 */
ESP_EVENT_DECLARE_BASE(LWLTE_TCP_EVENT);
```

Add this event enum after `lwlte_mqtt_event_id_t`:

```c
/**
 * @brief LTE TCP 用户事件 ID
 * @details LTE TCP user event ID
 * @note 投递到共享事件总线 LWLTE_TCP_EVENT。
 */
typedef enum {
    LWLTE_TCP_EVENT_STARTED = 0,          /**< TCP 服务已启动； TCP service started */
    LWLTE_TCP_EVENT_STOPPED,              /**< TCP 服务已停止； TCP service stopped */
    LWLTE_TCP_EVENT_CONNECTED,            /**< TCP 已连接； TCP connected */
    LWLTE_TCP_EVENT_DISCONNECTED,         /**< TCP 已断开； TCP disconnected */
    LWLTE_TCP_EVENT_SENT,                 /**< TCP 数据已送入模块协议栈； TCP data accepted by module stack */
    LWLTE_TCP_EVENT_DATA,                 /**< TCP 数据； TCP data */
    LWLTE_TCP_EVENT_ERROR,                /**< TCP 错误； TCP error */
} lwlte_tcp_event_id_t;
```

- [ ] **Step 2: Add public TCP config and event data structs**

In `src/include/lwlte.h`, add this block after `lwlte_mqtt_event_data_t`:

```c
/**
 * @brief TCP 客户端配置
 * @details TCP client configuration
 * @note 0 值使用默认值。v1 仅支持 max_conns 为 0 或 1；大于 1 返回 ESP_ERR_NOT_SUPPORTED。
 */
typedef struct {
    uint8_t max_conns;                    /**< 最大连接数，0 使用默认值 1； Maximum connections, 0 uses default 1 */
    int send_queue_size;                  /**< 发送队列长度，0 使用默认值； Send queue size, 0 uses default */
    size_t max_tx_len;                    /**< 单次发送最大长度，0 使用默认值； Maximum TX length, 0 uses default */
    size_t max_rx_event_len;              /**< 单个 DATA 事件最大 RX 长度，0 使用默认值； Maximum RX event length, 0 uses default */
    uint32_t open_timeout_ms;             /**< 打开超时，0 使用默认值； Open timeout, 0 uses default */
    uint32_t send_timeout_ms;             /**< 发送超时，0 使用默认值； Send timeout, 0 uses default */
    uint32_t close_timeout_ms;            /**< 关闭超时，0 使用默认值； Close timeout, 0 uses default */
    int fsm_queue_size;                   /**< TCP FSM 队列长度，0 使用默认值； TCP FSM queue size, 0 uses default */
    int fsm_task_stack;                   /**< TCP FSM 任务栈大小，0 使用默认值； TCP FSM task stack, 0 uses default */
    int fsm_task_priority;                /**< TCP FSM 任务优先级，0 使用默认值； TCP FSM task priority, 0 uses default */
} lwlte_tcp_config_t;

/**
 * @brief TCP 打开连接配置
 * @details TCP open connection configuration
 */
typedef struct {
    const char *host;                     /**< 目标主机或 IP； Target host or IP */
    uint16_t port;                        /**< 目标端口； Target port */
    void *user_ctx;                       /**< 用户上下文，事件中原样返回； User context returned in events */
} lwlte_tcp_open_config_t;

/**
 * @brief LTE TCP 用户事件数据
 * @details LTE TCP user event data
 */
typedef struct {
    lwlte_tcp_conn_t *conn;               /**< TCP 连接句柄； TCP connection handle */
    void *user_ctx;                       /**< 用户上下文； User context */
    lwlte_tcp_conn_state_t conn_state;    /**< 连接状态； Connection state */
    esp_err_t error_code;                 /**< ESP 错误码； ESP error code */
    int modem_error_code;                 /**< 模块原始错误码； Raw modem error code */
    int reason;                           /**< 断开或错误原因； Disconnect or error reason */
    size_t sent_len;                      /**< SENT 事件已接受长度； Accepted length for SENT event */
    const uint8_t *payload;               /**< DATA 事件负载； DATA event payload */
    size_t payload_len;                   /**< DATA 事件负载长度； DATA event payload length */
    bool owns_payload;                    /**< DATA 事件为 true，其余为 false； True for DATA events */
} lwlte_tcp_event_data_t;
```

- [ ] **Step 3: Add public TCP function prototypes**

In `src/include/lwlte.h`, add this block after `lwlte_mqtt_event_data_release()` and before `lwlte_mqtt_init()`:

```c
/**
 * @brief 释放 TCP DATA 事件堆缓冲
 * @details Release heap buffers carried by LWLTE_TCP_EVENT_DATA
 * @param[in] data 事件数据指针，可为 NULL
 */
void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data);

esp_err_t lwlte_tcp_init(lwlte_handle_t *me, const lwlte_tcp_config_t *config);
esp_err_t lwlte_tcp_destroy(lwlte_handle_t *me);
esp_err_t lwlte_tcp_open(lwlte_handle_t *me,
                         const lwlte_tcp_open_config_t *config,
                         lwlte_tcp_conn_t **out_conn);
esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn,
                         const uint8_t *data,
                         size_t len);
esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t *conn);
esp_err_t lwlte_tcp_conn_get_state(lwlte_tcp_conn_t *conn,
                                   lwlte_tcp_conn_state_t *state);
esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t *conn);
```

- [ ] **Step 4: Define the TCP event base and release helper**

In `src/lwlte/lwlte.c`, add this line next to the MQTT event base definition:

```c
ESP_EVENT_DEFINE_BASE(LWLTE_TCP_EVENT);
```

Add this function after `lwlte_mqtt_event_data_release()`:

```c
void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data)
{
    if (!data || !data->owns_payload) {
        return;
    }
    free((void *)data->payload);
    data->payload = NULL;
    data->payload_len = 0;
    data->owns_payload = false;
}
```

- [ ] **Step 5: Run the focused contract test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_public_tcp_api_exists -q
```

Expected: still FAIL because service wrappers are not implemented yet, but errors about missing public type declarations and `LWLTE_TCP_EVENT` should be gone.

- [ ] **Step 6: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/include/lwlte.h src/lwlte/lwlte.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add tcp public api contract"
```

---

### Task 3: Core Protocol Routing And Socket Commands

**Files:**

- Modify: `src/core/core.h`
- Modify: `src/core/core_priv.h`
- Modify: `src/core/core.c`
- Modify: `src/core/core_fsm.c`
- Modify: `src/mqtt_client/mqtt_client.c`

- [ ] **Step 1: Add protocol enum values and callback signatures**

In `src/core/core.h`, replace the existing `core_protocol_t` enum with:

```c
typedef enum {
    CORE_PROTOCOL_MQTT = 0,              /**< MQTT 协议； MQTT protocol */
    CORE_PROTOCOL_TCP,                   /**< TCP 协议； TCP protocol */
    CORE_PROTOCOL_MAX,                   /**< 协议数量； Protocol count */
} core_protocol_t;
```

Extend `core_protocol_data_t` to this exact shape:

```c
typedef struct {
    core_protocol_t protocol;            /**< 协议类型； Protocol type */
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const char *topic;                   /**< 主题，MQTT 使用； Topic, used by MQTT */
    size_t topic_len;                    /**< 主题长度； Topic length */
    const uint8_t *payload;              /**< 负载； Payload */
    size_t payload_len;                  /**< 负载长度； Payload length */
    int reason;                          /**< 事件原因； Event reason */
    int modem_error_code;                /**< 模块原始错误码； Raw modem error code */
} core_protocol_data_t;
```

Replace callback registration prototypes with:

```c
esp_err_t core_register_protocol_callback(core_handle_t *me,
                                           core_protocol_t protocol,
                                           core_protocol_callback_t callback,
                                           void *user_ctx);

esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                  core_protocol_t protocol,
                                                  core_protocol_closed_callback_t callback,
                                                  void *user_ctx);
```

- [ ] **Step 2: Store protocol callbacks per protocol**

In `src/core/core_priv.h`, replace the four single callback fields in `struct core_handle` with:

```c
    core_protocol_callback_t protocol_callbacks[CORE_PROTOCOL_MAX];
    void *protocol_user_ctxs[CORE_PROTOCOL_MAX];
    core_protocol_closed_callback_t protocol_closed_callbacks[CORE_PROTOCOL_MAX];
    void *protocol_closed_user_ctxs[CORE_PROTOCOL_MAX];
```

- [ ] **Step 3: Add socket command value objects**

In `src/core/core.h`, add these types before `core_cmd_type_t`:

```c
typedef enum {
    CORE_SOCKET_PROTO_TCP = 0,           /**< TCP socket； TCP socket */
} core_socket_proto_t;

typedef struct {
    core_socket_proto_t proto;           /**< Socket 协议； Socket protocol */
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const char *host;                    /**< 主机； Host */
    uint16_t port;                       /**< 端口； Port */
    uint32_t timeout_ms;                 /**< 打开超时； Open timeout */
} core_socket_open_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    const uint8_t *data;                 /**< 发送数据； Send data */
    size_t len;                          /**< 发送长度； Send length */
    uint32_t timeout_ms;                 /**< 发送超时； Send timeout */
} core_socket_send_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    size_t max_len;                      /**< 最大读取长度； Maximum read length */
} core_socket_recv_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    uint8_t *payload;                    /**< 堆负载，接收方拥有； Heap payload, receiver owns */
    size_t payload_len;                  /**< 负载长度； Payload length */
    size_t remaining_len;                /**< 模块缓存剩余长度； Remaining cached length */
    int modem_error_code;                /**< 模块错误码； Modem error code */
} core_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;                     /**< 连接 ID； Connection ID */
    uint32_t timeout_ms;                 /**< 关闭超时； Close timeout */
} core_socket_close_t;
```

Add command enum values after `CORE_CMD_PING`:

```c
    CORE_CMD_SOCKET_OPEN,                /**< 打开 socket； Open socket */
    CORE_CMD_SOCKET_SEND,                /**< 发送 socket 数据； Send socket data */
    CORE_CMD_SOCKET_RECV,                /**< 接收 socket 数据； Receive socket data */
    CORE_CMD_SOCKET_CLOSE,               /**< 关闭 socket； Close socket */
```

Add these union members to `core_cmd_t.data`:

```c
        core_socket_open_t socket_open;  /**< Socket 打开参数； Socket open args */
        core_socket_send_t socket_send;  /**< Socket 发送参数； Socket send args */
        core_socket_recv_t socket_recv;  /**< Socket 接收参数； Socket receive args */
        core_socket_close_t socket_close; /**< Socket 关闭参数； Socket close args */
```

- [ ] **Step 4: Update Core callback registration implementation**

In `src/core/core.c`, replace the existing `core_register_protocol_callback()` implementation with:

```c
esp_err_t core_register_protocol_callback(core_handle_t *me,
                                          core_protocol_t protocol,
                                          core_protocol_callback_t callback,
                                          void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock && protocol < CORE_PROTOCOL_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid protocol callback args");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    if (!destroying) {
        me->protocol_callbacks[protocol] = callback;
        me->protocol_user_ctxs[protocol] = callback ? user_ctx : NULL;
    }
    xSemaphoreGive(me->lock);

    return destroying ? ESP_ERR_INVALID_STATE : ESP_OK;
}
```

Replace `core_register_protocol_closed_callback()` with:

```c
esp_err_t core_register_protocol_closed_callback(core_handle_t *me,
                                                 core_protocol_t protocol,
                                                 core_protocol_closed_callback_t callback,
                                                 void *user_ctx)
{
    ESP_RETURN_ON_FALSE(me && me->lock && protocol < CORE_PROTOCOL_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "invalid protocol closed callback args");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool destroying = me->destroying;
    if (!destroying) {
        me->protocol_closed_callbacks[protocol] = callback;
        me->protocol_closed_user_ctxs[protocol] = callback ? user_ctx : NULL;
    }
    xSemaphoreGive(me->lock);

    return destroying ? ESP_ERR_INVALID_STATE : ESP_OK;
}
```

- [ ] **Step 5: Update MQTT service registration calls**

In `src/mqtt_client/mqtt_client.c`, replace every `core_register_protocol_callback(me->core, ...` call with protocol-indexed calls:

```c
(void)core_register_protocol_callback(me->core, CORE_PROTOCOL_MQTT, NULL, NULL);
(void)core_register_protocol_closed_callback(me->core, CORE_PROTOCOL_MQTT, NULL, NULL);
```

In `mqtt_client_create()`, use:

```c
ret = core_register_protocol_callback(core, CORE_PROTOCOL_MQTT,
                                      mqtt_protocol_data_cb, me);
```

and:

```c
ret = core_register_protocol_closed_callback(core, CORE_PROTOCOL_MQTT,
                                             mqtt_protocol_closed_cb, me);
```

- [ ] **Step 6: Clone, validate, and free socket commands**

In `src/core/core.c`, extend `clone_core_cmd()` switch with:

```c
    case CORE_CMD_SOCKET_OPEN:
        clone->data.socket_open.host = clone_optional_string(cmd->data.socket_open.host);
        if (!clone->data.socket_open.host) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_SOCKET_SEND:
        clone->data.socket_send.data = clone_payload(cmd->data.socket_send.data,
                                                     cmd->data.socket_send.len);
        if (!clone->data.socket_send.data) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
    case CORE_CMD_SOCKET_RECV:
    case CORE_CMD_SOCKET_CLOSE:
        break;
```

Extend `free_core_cmd()` with:

```c
    case CORE_CMD_SOCKET_OPEN:
        free((void *)cmd->data.socket_open.host);
        break;
    case CORE_CMD_SOCKET_SEND:
        free((void *)cmd->data.socket_send.data);
        break;
```

Update `core_cmd_type_valid()` so the valid range includes socket commands:

```c
static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_MQTT_CONFIGURE && type <= CORE_CMD_SOCKET_CLOSE;
}
```

Extend `core_cmd_valid()` with:

```c
    case CORE_CMD_SOCKET_OPEN:
        return cmd->data.socket_open.proto == CORE_SOCKET_PROTO_TCP &&
               cmd->data.socket_open.host &&
               cmd->data.socket_open.host[0] != '\0' &&
               cmd->data.socket_open.port > 0;
    case CORE_CMD_SOCKET_SEND:
        return cmd->data.socket_send.data && cmd->data.socket_send.len > 0;
    case CORE_CMD_SOCKET_RECV:
        return cmd->data.socket_recv.max_len > 0;
    case CORE_CMD_SOCKET_CLOSE:
        return true;
```

- [ ] **Step 7: Execute socket commands in Core FSM**

In `src/core/core_fsm.c`, add a helper near existing command handlers:

```c
static esp_err_t ensure_net_online(core_handle_t *me)
{
    core_net_state_t state = CORE_NET_STATE_OFFLINE;
    esp_err_t ret = core_get_net_state(me, &state);
    if (ret != ESP_OK) {
        return ret;
    }
    return state == CORE_NET_STATE_ONLINE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
```

Add socket cases inside `handle_service_cmd()`:

```c
    case CORE_CMD_SOCKET_OPEN: {
        esp_err_t ret = ensure_net_online(me);
        if (ret == ESP_OK) {
            modem_socket_open_t request = {
                .proto = MODEM_SOCKET_PROTO_TCP,
                .conn_id = cmd->data.socket_open.conn_id,
                .host = cmd->data.socket_open.host,
                .port = cmd->data.socket_open.port,
                .timeout_ms = cmd->data.socket_open.timeout_ms,
            };
            ret = modem_socket_open(me->modem, &request);
        }
        finish_service_cmd(me, cmd, result_from_esp_err(ret), NULL);
        break;
    }
    case CORE_CMD_SOCKET_SEND: {
        esp_err_t ret = ensure_net_online(me);
        if (ret == ESP_OK) {
            modem_socket_send_t request = {
                .conn_id = cmd->data.socket_send.conn_id,
                .data = cmd->data.socket_send.data,
                .len = cmd->data.socket_send.len,
                .timeout_ms = cmd->data.socket_send.timeout_ms,
            };
            ret = modem_socket_send(me->modem, &request);
        }
        finish_service_cmd(me, cmd, result_from_esp_err(ret), NULL);
        break;
    }
    case CORE_CMD_SOCKET_RECV: {
        core_socket_recv_result_t result = {0};
        esp_err_t ret = ensure_net_online(me);
        if (ret == ESP_OK) {
            modem_socket_recv_t request = {
                .conn_id = cmd->data.socket_recv.conn_id,
                .max_len = cmd->data.socket_recv.max_len,
            };
            modem_socket_recv_result_t modem_result = {0};
            ret = modem_socket_recv(me->modem, &request, &modem_result);
            if (ret == ESP_OK) {
                result.conn_id = modem_result.conn_id;
                result.payload = modem_result.payload;
                result.payload_len = modem_result.payload_len;
                result.remaining_len = modem_result.remaining_len;
                result.modem_error_code = modem_result.modem_error_code;
            }
        }
        finish_service_cmd(me, cmd, result_from_esp_err(ret), ret == ESP_OK ? &result : NULL);
        if (ret != ESP_OK) {
            free(result.payload);
        }
        break;
    }
    case CORE_CMD_SOCKET_CLOSE: {
        modem_socket_close_t request = {
            .conn_id = cmd->data.socket_close.conn_id,
            .timeout_ms = cmd->data.socket_close.timeout_ms,
        };
        esp_err_t ret = modem_socket_close(me->modem, &request);
        finish_service_cmd(me, cmd, result_from_esp_err(ret), NULL);
        break;
    }
```

- [ ] **Step 8: Route protocol events by protocol**

In the Core modem event handling path in `src/core/core_fsm.c`, use this routing pattern for `MODEM_EVENT_PROTOCOL_DATA`:

```c
core_protocol_t protocol = (core_protocol_t)sig->modem_event.data.protocol_data.protocol;
if (protocol < CORE_PROTOCOL_MAX) {
    core_protocol_callback_t cb = me->protocol_callbacks[protocol];
    void *ctx = me->protocol_user_ctxs[protocol];
    if (cb) {
        core_protocol_data_t data = {
            .protocol = protocol,
            .conn_id = sig->modem_event.data.protocol_data.conn_id,
            .topic = sig->modem_event.data.protocol_data.topic,
            .topic_len = sig->modem_event.data.protocol_data.topic_len,
            .payload = sig->modem_event.data.protocol_data.payload,
            .payload_len = sig->modem_event.data.protocol_data.payload_len,
            .reason = sig->modem_event.data.protocol_data.reason,
            .modem_error_code = sig->modem_event.data.protocol_data.modem_error_code,
        };
        cb(me, &data, ctx);
    }
}
```

For `MODEM_EVENT_PROTOCOL_CLOSED`, use:

```c
core_protocol_t protocol = (core_protocol_t)sig->modem_event.data.protocol_data.protocol;
if (protocol < CORE_PROTOCOL_MAX) {
    core_protocol_closed_callback_t cb = me->protocol_closed_callbacks[protocol];
    void *ctx = me->protocol_closed_user_ctxs[protocol];
    if (cb) {
        cb(me, protocol, ctx);
    }
}
```

- [ ] **Step 9: Run Core routing tests**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_core_protocol_callbacks_are_protocol_indexed tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_core_socket_commands_exist_and_clone_ownership -q
```

Expected: PASS for these two tests after Core and MQTT updates compile statically.

- [ ] **Step 10: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/core/core.h src/core/core_priv.h src/core/core.c src/core/core_fsm.c src/mqtt_client/mqtt_client.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add core socket command routing"
```

---

### Task 4: Modem Socket API

**Files:**

- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_priv.h`
- Modify: `src/modem/modem.c`

- [ ] **Step 1: Add protocol routing fields and socket value objects**

In `src/modem/modem.h`, replace `modem_protocol_t` with:

```c
typedef enum {
    MODEM_PROTOCOL_MQTT = 0,      /**< MQTT 协议； MQTT protocol */
    MODEM_PROTOCOL_TCP,           /**< TCP 协议； TCP protocol */
} modem_protocol_t;
```

Extend `modem_protocol_data_t` to:

```c
typedef struct {
    modem_protocol_t protocol;    /**< 协议类型； Protocol type */
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    const char *topic;            /**< 主题指针； Topic pointer */
    size_t topic_len;             /**< 主题长度； Topic length */
    const uint8_t *payload;       /**< 负载指针； Payload pointer */
    size_t payload_len;           /**< 负载长度； Payload length */
    int reason;                   /**< 事件原因； Event reason */
    int modem_error_code;         /**< 模块原始错误码； Raw modem error code */
} modem_protocol_data_t;
```

Add these socket types before `modem_event_id_t`:

```c
typedef enum {
    MODEM_SOCKET_PROTO_TCP = 0,   /**< TCP socket； TCP socket */
} modem_socket_proto_t;

typedef struct {
    modem_socket_proto_t proto;   /**< Socket 协议； Socket protocol */
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    const char *host;             /**< 目标主机； Target host */
    uint16_t port;                /**< 目标端口； Target port */
    uint32_t timeout_ms;          /**< 打开超时； Open timeout */
} modem_socket_open_t;

typedef struct {
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    const uint8_t *data;          /**< 发送数据； Send data */
    size_t len;                   /**< 发送长度； Send length */
    uint32_t timeout_ms;          /**< 发送超时； Send timeout */
} modem_socket_send_t;

typedef struct {
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    size_t max_len;               /**< 最大读取长度； Maximum read length */
} modem_socket_recv_t;

typedef struct {
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    uint8_t *payload;             /**< 堆负载，调用方拥有； Heap payload, caller owns */
    size_t payload_len;           /**< 负载长度； Payload length */
    size_t remaining_len;         /**< 模块缓存剩余长度； Remaining cached length */
    int modem_error_code;         /**< 模块错误码； Modem error code */
} modem_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;              /**< 连接 ID； Connection ID */
    uint32_t timeout_ms;          /**< 关闭超时； Close timeout */
} modem_socket_close_t;
```

- [ ] **Step 2: Add Modem public socket wrappers**

In `src/modem/modem.h`, add prototypes after `modem_mqtt_get_status()`:

```c
esp_err_t modem_socket_open(modem_handle_t *me,
                            const modem_socket_open_t *open);
esp_err_t modem_socket_send(modem_handle_t *me,
                            const modem_socket_send_t *send);
esp_err_t modem_socket_recv(modem_handle_t *me,
                            const modem_socket_recv_t *recv,
                            modem_socket_recv_result_t *result);
esp_err_t modem_socket_close(modem_handle_t *me,
                             const modem_socket_close_t *close);
```

- [ ] **Step 3: Add socket ops to Modem virtual table**

In `src/modem/modem_priv.h`, add these typedefs near `modem_ping_fn`:

```c
typedef esp_err_t (*modem_socket_open_fn)(modem_handle_t *me,
                                          const modem_socket_open_t *open);
typedef esp_err_t (*modem_socket_send_fn)(modem_handle_t *me,
                                          const modem_socket_send_t *send);
typedef esp_err_t (*modem_socket_recv_fn)(modem_handle_t *me,
                                          const modem_socket_recv_t *recv,
                                          modem_socket_recv_result_t *result);
typedef esp_err_t (*modem_socket_close_fn)(modem_handle_t *me,
                                           const modem_socket_close_t *close);
```

Add fields before diagnostics in `modem_ops_t`:

```c
    /* ── Socket 客户端； Socket client ───────────────────── */
    modem_socket_open_fn socket_open;              /**< 打开 socket； Open socket */
    modem_socket_send_fn socket_send;              /**< 发送 socket 数据； Send socket data */
    modem_socket_recv_fn socket_recv;              /**< 接收 socket 数据； Receive socket data */
    modem_socket_close_fn socket_close;            /**< 关闭 socket； Close socket */
```

- [ ] **Step 4: Implement Modem socket wrappers**

In `src/modem/modem.c`, add these functions before `modem_ping()`:

```c
esp_err_t modem_socket_open(modem_handle_t *me,
                            const modem_socket_open_t *open)
{
    ESP_RETURN_ON_FALSE(me && open && open->proto == MODEM_SOCKET_PROTO_TCP &&
                        open->host && open->host[0] && open->port > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket open args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_open,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_open not supported");

    return me->ops->socket_open(me, open);
}

esp_err_t modem_socket_send(modem_handle_t *me,
                            const modem_socket_send_t *send)
{
    ESP_RETURN_ON_FALSE(me && send && send->data && send->len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket send args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_send,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_send not supported");

    return me->ops->socket_send(me, send);
}

esp_err_t modem_socket_recv(modem_handle_t *me,
                            const modem_socket_recv_t *recv,
                            modem_socket_recv_result_t *result)
{
    ESP_RETURN_ON_FALSE(me && recv && result && recv->max_len > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid socket recv args");

    memset(result, 0, sizeof(*result));
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_recv,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_recv not supported");

    return me->ops->socket_recv(me, recv, result);
}

esp_err_t modem_socket_close(modem_handle_t *me,
                             const modem_socket_close_t *close)
{
    ESP_RETURN_ON_FALSE(me && close, ESP_ERR_INVALID_ARG, TAG,
                        "invalid socket close args");

    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->socket_close,
                        ESP_ERR_NOT_SUPPORTED, TAG, "socket_close not supported");

    return me->ops->socket_close(me, close);
}
```

- [ ] **Step 5: Run Modem socket contract test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_modem_socket_api_exists -q
```

Expected: PASS.

- [ ] **Step 6: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/modem/modem.h src/modem/modem_priv.h src/modem/modem.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add modem socket api"
```

---

### Task 5: TCP Client Service Skeleton And Facade Wiring

**Files:**

- Create: `src/tcp_client/tcp_client.h`
- Create: `src/tcp_client/tcp_client_priv.h`
- Create: `src/tcp_client/tcp_client.c`
- Modify: `src/CMakeLists.txt`
- Modify: `src/lwlte/lwlte_priv.h`
- Modify: `src/lwlte/lwlte.c`

- [ ] **Step 1: Register the new source and include directory**

In `src/CMakeLists.txt`, add source:

```cmake
         "tcp_client/tcp_client.c"
```

Add private include dir:

```cmake
tcp_client
```

- [ ] **Step 2: Create TCP client public-internal header**

Create `src/tcp_client/tcp_client.h` with this content:

```c
/**
 * @file tcp_client.h
 * @brief TCP 客户端服务层间接口
 * @details TCP client service inter-layer interface
 * @author JovisDreams
 * @date 2026-06-18
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

#include "esp_err.h"
#include "esp_event.h"
#include "core.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct tcp_client_handle tcp_client_handle_t;
typedef struct tcp_client_conn tcp_client_conn_t;

typedef enum {
    TCP_CONN_STATE_CREATED = 0,
    TCP_CONN_STATE_CONNECTING,
    TCP_CONN_STATE_CONNECTED,
    TCP_CONN_STATE_CLOSING,
    TCP_CONN_STATE_CLOSED,
    TCP_CONN_STATE_ERROR,
} tcp_conn_state_t;

typedef struct {
    uint8_t max_conns;
    int send_queue_size;
    size_t max_tx_len;
    size_t max_rx_event_len;
    uint32_t open_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t close_timeout_ms;
    int fsm_queue_size;
    int fsm_task_stack;
    int fsm_task_priority;
    esp_event_loop_handle_t loop;
} tcp_client_config_t;

typedef struct {
    const char *host;
    uint16_t port;
    void *user_ctx;
} tcp_client_open_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
tcp_client_handle_t *tcp_client_create(const tcp_client_config_t *config,
                                       core_handle_t *core);
esp_err_t tcp_client_destroy(tcp_client_handle_t *me);
esp_err_t tcp_client_open(tcp_client_handle_t *me,
                          const tcp_client_open_config_t *config,
                          tcp_client_conn_t **out_conn);
esp_err_t tcp_client_send(tcp_client_conn_t *conn,
                          const uint8_t *data,
                          size_t len);
esp_err_t tcp_client_close(tcp_client_conn_t *conn);
esp_err_t tcp_client_conn_get_state(tcp_client_conn_t *conn,
                                    tcp_conn_state_t *state);
esp_err_t tcp_client_conn_destroy(tcp_client_conn_t *conn);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
```

- [ ] **Step 3: Create TCP client private header**

Create `src/tcp_client/tcp_client_priv.h` with this content:

```c
/**
 * @file tcp_client_priv.h
 * @brief TCP 客户端服务内部接口
 * @details TCP client service internal interface
 * @author JovisDreams
 * @date 2026-06-18
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "tcp_client.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/
#define TCP_CLIENT_DEFAULT_MAX_CONNS          1
#define TCP_CLIENT_DEFAULT_SEND_QUEUE_SIZE    4
#define TCP_CLIENT_DEFAULT_MAX_TX_LEN         1460
#define TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN   730
#define TCP_CLIENT_DEFAULT_OPEN_TIMEOUT_MS    75000
#define TCP_CLIENT_DEFAULT_SEND_TIMEOUT_MS    50000
#define TCP_CLIENT_DEFAULT_CLOSE_TIMEOUT_MS   10000
#define TCP_CLIENT_DEFAULT_FSM_QUEUE_SIZE     16
#define TCP_CLIENT_DEFAULT_FSM_TASK_STACK     4096
#define TCP_CLIENT_DEFAULT_FSM_PRIORITY       8
#define TCP_CLIENT_FSM_WAIT_MS                100

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    TCP_SIG_OPEN = 0,
    TCP_SIG_CLOSE,
    TCP_SIG_SEND_READY,
    TCP_SIG_CORE_CMD_DONE,
    TCP_SIG_PROTOCOL_DATA,
    TCP_SIG_PROTOCOL_CLOSED,
    TCP_SIG_NET_OFFLINE,
} tcp_fsm_sig_type_t;

typedef struct tcp_send_item {
    uint8_t *data;
    size_t len;
} tcp_send_item_t;

typedef struct {
    bool active;
    core_cmd_type_t type;
    size_t send_len;
} tcp_pending_cmd_t;

typedef struct tcp_protocol_data_owned {
    uint8_t conn_id;
    uint8_t *payload;
    size_t payload_len;
    int reason;
    int modem_error_code;
} tcp_protocol_data_owned_t;

typedef struct {
    tcp_fsm_sig_type_t type;
    core_cmd_type_t core_cmd_type;
    core_cmd_result_t core_result;
    void *result_data;
    size_t result_size;
    int error_code;
    void *data;
    size_t data_size;
} tcp_fsm_sig_t;

struct tcp_client_handle {
    tcp_client_config_t config;
    core_handle_t *core;
    tcp_client_conn_t *conn;
    TaskHandle_t fsm_task;
    QueueHandle_t fsm_queue;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t fsm_task_done_sema;
    bool destroying;
};

struct tcp_client_conn {
    tcp_client_handle_t *client;
    QueueHandle_t send_queue;
    SemaphoreHandle_t lock;
    tcp_conn_state_t state;
    tcp_pending_cmd_t pending_cmd;
    uint8_t conn_id;
    void *user_ctx;
    bool close_requested;
    bool destroyed;
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

- [ ] **Step 4: Create TCP client implementation with lifecycle, queue, callbacks, and events**

Create `src/tcp_client/tcp_client.c` using this implementation structure. Keep function names and signal names exact so the contract test and Facade can target them:

```c
/**
 * @file tcp_client.c
 * @brief TCP 客户端服务实现
 * @details TCP client service implementation
 * @author JovisDreams
 * @date 2026-06-18
 */

/*********************
 *      INCLUDES
 *********************/
#include "tcp_client_priv.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/
#define TAG "tcp_client"

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool config_valid(const tcp_client_config_t *config, core_handle_t *core);
static esp_err_t normalize_config(const tcp_client_config_t *config, tcp_client_config_t *out);
static uint8_t *clone_payload(const uint8_t *data, size_t len);
static esp_err_t send_fsm_sig(tcp_client_handle_t *me, const tcp_fsm_sig_t *sig);
static esp_err_t set_conn_state(tcp_client_conn_t *conn, tcp_conn_state_t state);
static tcp_conn_state_t get_conn_state_value(tcp_client_conn_t *conn);
static lwlte_tcp_conn_state_t map_conn_state(tcp_conn_state_t state);
static void tcp_protocol_data_cb(core_handle_t *core, const core_protocol_data_t *data, void *user_ctx);
static void tcp_protocol_closed_cb(core_handle_t *core, core_protocol_t protocol, void *user_ctx);
static void tcp_core_cmd_done_cb(core_handle_t *core, core_cmd_type_t type, core_cmd_result_t result, const void *result_data, void *user_ctx);
static void handle_lwlte_event(void *handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void tcp_fsm_task(void *arg);
static bool tcp_fsm_should_stop(tcp_client_handle_t *me);
static void handle_signal(tcp_client_handle_t *me, tcp_fsm_sig_t *sig);
static void handle_open(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static void handle_send_ready(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static void handle_close(tcp_client_handle_t *me, tcp_client_conn_t *conn);
static void handle_core_cmd_done(tcp_client_handle_t *me, tcp_fsm_sig_t *sig);
static void handle_protocol_data(tcp_client_handle_t *me, tcp_fsm_sig_t *sig);
static void handle_protocol_closed(tcp_client_handle_t *me);
static esp_err_t submit_socket_open(tcp_client_conn_t *conn, const char *host, uint16_t port);
static esp_err_t submit_socket_send(tcp_client_conn_t *conn, const tcp_send_item_t *item);
static esp_err_t submit_socket_recv(tcp_client_conn_t *conn);
static esp_err_t submit_socket_close(tcp_client_conn_t *conn);
static esp_err_t post_tcp_event(tcp_client_conn_t *conn, lwlte_tcp_event_id_t event_id, const lwlte_tcp_event_data_t *payload);
static void post_error_event(tcp_client_conn_t *conn, esp_err_t error_code, int modem_error_code, int reason);
static void clear_send_queue(tcp_client_conn_t *conn);
static void free_fsm_sig_payload(tcp_fsm_sig_t *sig);
static void cleanup_partial_client(tcp_client_handle_t *me);
static void cleanup_conn(tcp_client_conn_t *conn);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
tcp_client_handle_t *tcp_client_create(const tcp_client_config_t *config, core_handle_t *core);
esp_err_t tcp_client_destroy(tcp_client_handle_t *me);
esp_err_t tcp_client_open(tcp_client_handle_t *me, const tcp_client_open_config_t *config, tcp_client_conn_t **out_conn);
esp_err_t tcp_client_send(tcp_client_conn_t *conn, const uint8_t *data, size_t len);
esp_err_t tcp_client_close(tcp_client_conn_t *conn);
esp_err_t tcp_client_conn_get_state(tcp_client_conn_t *conn, tcp_conn_state_t *state);
esp_err_t tcp_client_conn_destroy(tcp_client_conn_t *conn);

/**********************
 *   STATIC FUNCTIONS
 **********************/
```

Add `normalize_config()` with this exact validation and defaulting logic:

```c
static esp_err_t normalize_config(const tcp_client_config_t *config,
                                  tcp_client_config_t *out)
{
    ESP_RETURN_ON_FALSE(config && out, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    if (config->max_conns > TCP_CLIENT_DEFAULT_MAX_CONNS) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_RETURN_ON_FALSE(config->send_queue_size >= 0 &&
                        config->fsm_queue_size >= 0 &&
                        config->fsm_task_stack >= 0 &&
                        config->fsm_task_priority >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid TCP config");

    *out = *config;
    if (out->max_conns == 0) {
        out->max_conns = TCP_CLIENT_DEFAULT_MAX_CONNS;
    }
    if (out->send_queue_size == 0) {
        out->send_queue_size = TCP_CLIENT_DEFAULT_SEND_QUEUE_SIZE;
    }
    if (out->max_tx_len == 0) {
        out->max_tx_len = TCP_CLIENT_DEFAULT_MAX_TX_LEN;
    }
    if (out->max_rx_event_len == 0) {
        out->max_rx_event_len = TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN;
    }
    if (out->open_timeout_ms == 0) {
        out->open_timeout_ms = TCP_CLIENT_DEFAULT_OPEN_TIMEOUT_MS;
    }
    if (out->send_timeout_ms == 0) {
        out->send_timeout_ms = TCP_CLIENT_DEFAULT_SEND_TIMEOUT_MS;
    }
    if (out->close_timeout_ms == 0) {
        out->close_timeout_ms = TCP_CLIENT_DEFAULT_CLOSE_TIMEOUT_MS;
    }
    if (out->fsm_queue_size == 0) {
        out->fsm_queue_size = TCP_CLIENT_DEFAULT_FSM_QUEUE_SIZE;
    }
    if (out->fsm_task_stack == 0) {
        out->fsm_task_stack = TCP_CLIENT_DEFAULT_FSM_TASK_STACK;
    }
    if (out->fsm_task_priority == 0) {
        out->fsm_task_priority = TCP_CLIENT_DEFAULT_FSM_PRIORITY;
    }
    return ESP_OK;
}
```

Add the public service functions with these state and ownership transitions:

| Function | Required body behavior |
|----------|------------------------|
| `tcp_client_create()` | Allocate `tcp_client_handle_t`, call `normalize_config()`, set `me->core`, create `lock`, `fsm_queue`, and `fsm_task_done_sema`, register `CORE_PROTOCOL_TCP` data and closed callbacks, register `LWLTE_EVENT_NET_OFFLINE`, create `tcp_fsm_task`, and call `cleanup_partial_client()` on each failure path. |
| `tcp_client_destroy()` | Reject calls from the TCP FSM task, set `destroying`, unregister `LWLTE_EVENT_NET_OFFLINE`, unregister `CORE_PROTOCOL_TCP` callbacks, wait for `fsm_task_done_sema`, destroy any closed/error `me->conn`, delete `fsm_queue`, delete semaphores, delete `lock`, and free `me`. Return `ESP_ERR_INVALID_STATE` if `me->conn` is still `CONNECTING`, `CONNECTED`, or `CLOSING`. |
| `tcp_client_open()` | Validate `config`, `out_conn`, host, and port; require `core_get_net_state() == CORE_NET_STATE_ONLINE`; reject an existing `me->conn`; allocate `tcp_client_conn_t` with `conn_id = 0`; create `send_queue` and `lock`; store `user_ctx`; clone `config->host` into the `TCP_SIG_OPEN` payload; store `me->conn`; queue `TCP_SIG_OPEN`; return the conn through `out_conn`. |
| `tcp_client_send()` | Require `state == TCP_CONN_STATE_CONNECTED`; reject `len == 0` and `len > conn->client->config.max_tx_len`; deep-copy bytes into a `tcp_send_item_t`; push it into `conn->send_queue` with zero wait; return `ESP_ERR_TIMEOUT` if full; queue `TCP_SIG_SEND_READY`. |
| `tcp_client_close()` | Accept only `CONNECTED` and `ERROR`; set `close_requested`; queue `TCP_SIG_CLOSE`; return `ESP_ERR_INVALID_STATE` for `CREATED`, `CONNECTING`, `CLOSING`, and `CLOSED`. |
| `tcp_client_conn_get_state()` | Lock `conn->lock`, copy `conn->state`, unlock, return `ESP_OK`. |
| `tcp_client_conn_destroy()` | Succeed only for `CLOSED` or `ERROR`; clear `conn->client->conn` under the client lock; call `clear_send_queue()`, delete `send_queue`, delete `lock`, and free `conn`. |

Add the FSM command completion behavior exactly as this table:

| Signal | Success behavior | Error behavior |
|--------|------------------|----------------|
| `CORE_CMD_SOCKET_OPEN` | Set conn `CONNECTED`; post `LWLTE_TCP_EVENT_CONNECTED`; call `handle_send_ready()` if the TX FIFO already has data. | Set conn `ERROR`; call `post_error_event(conn, ESP_FAIL, 0, 0)`. |
| `CORE_CMD_SOCKET_SEND` | Clear pending command; post `LWLTE_TCP_EVENT_SENT` with `sent_len`; call `handle_send_ready()` for the next FIFO item. | Clear pending command; set conn `ERROR`; clear TX FIFO; post `LWLTE_TCP_EVENT_ERROR`. |
| `CORE_CMD_SOCKET_RECV` | Take ownership of `core_socket_recv_result_t.payload`; post one `LWLTE_TCP_EVENT_DATA` with `owns_payload = true`; if `remaining_len > 0`, submit another recv. | Set conn `ERROR`; post `LWLTE_TCP_EVENT_ERROR`. |
| `CORE_CMD_SOCKET_CLOSE` | Clear TX FIFO; set conn `CLOSED`; post `LWLTE_TCP_EVENT_DISCONNECTED`. | Clear TX FIFO; set conn `ERROR`; post `LWLTE_TCP_EVENT_ERROR`. |

Add lightweight protocol callbacks with this behavior:

```c
static void tcp_protocol_data_cb(core_handle_t *core,
                                 const core_protocol_data_t *data,
                                 void *user_ctx)
{
    (void)core;
    tcp_client_handle_t *me = (tcp_client_handle_t *)user_ctx;
    if (!me || !data || data->protocol != CORE_PROTOCOL_TCP) {
        return;
    }
    tcp_protocol_data_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) {
        return;
    }
    owned->conn_id = data->conn_id;
    owned->reason = data->reason;
    owned->modem_error_code = data->modem_error_code;
    tcp_fsm_sig_t sig = {
        .type = TCP_SIG_PROTOCOL_DATA,
        .data = owned,
        .data_size = sizeof(*owned),
    };
    if (send_fsm_sig(me, &sig) != ESP_OK) {
        free(owned);
    }
}

static void tcp_protocol_closed_cb(core_handle_t *core,
                                   core_protocol_t protocol,
                                   void *user_ctx)
{
    (void)core;
    tcp_client_handle_t *me = (tcp_client_handle_t *)user_ctx;
    if (!me || protocol != CORE_PROTOCOL_TCP) {
        return;
    }
    tcp_fsm_sig_t sig = { .type = TCP_SIG_PROTOCOL_CLOSED };
    (void)send_fsm_sig(me, &sig);
}
```

- [ ] **Step 5: Wire Facade private handle**

In `src/lwlte/lwlte_priv.h`, add:

```c
#include "tcp_client.h"
```

Add this field after `mqtt_client_handle_t *mqtt;`:

```c
    tcp_client_handle_t *tcp;
```

- [ ] **Step 6: Add Facade TCP wrappers**

In `src/lwlte/lwlte.c`, add a TCP state mapper prototype:

```c
static lwlte_tcp_conn_state_t map_tcp_conn_state(tcp_conn_state_t state);
```

Add this mapper after `map_mqtt_state()`:

```c
static lwlte_tcp_conn_state_t map_tcp_conn_state(tcp_conn_state_t state)
{
    switch (state) {
    case TCP_CONN_STATE_CREATED:     return LWLTE_TCP_CONN_STATE_CREATED;
    case TCP_CONN_STATE_CONNECTING:  return LWLTE_TCP_CONN_STATE_CONNECTING;
    case TCP_CONN_STATE_CONNECTED:   return LWLTE_TCP_CONN_STATE_CONNECTED;
    case TCP_CONN_STATE_CLOSING:     return LWLTE_TCP_CONN_STATE_CLOSING;
    case TCP_CONN_STATE_CLOSED:      return LWLTE_TCP_CONN_STATE_CLOSED;
    case TCP_CONN_STATE_ERROR:
    default:                         return LWLTE_TCP_CONN_STATE_ERROR;
    }
}
```

Add TCP facade functions after MQTT init/destroy functions:

```c
esp_err_t lwlte_tcp_init(lwlte_handle_t *me, const lwlte_tcp_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && config, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(non_negative_int(config->send_queue_size) &&
                        non_negative_int(config->fsm_queue_size) &&
                        non_negative_int(config->fsm_task_stack) &&
                        non_negative_int(config->fsm_task_priority),
                        ESP_ERR_INVALID_ARG, TAG, "TCP task fields must be non-negative");

    core_handle_t *core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool already_initialized = (me->tcp != NULL);
    xSemaphoreGive(me->lock);
    if (already_initialized) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    const tcp_client_config_t tcp_config = {
        .max_conns = config->max_conns,
        .send_queue_size = config->send_queue_size,
        .max_tx_len = config->max_tx_len,
        .max_rx_event_len = config->max_rx_event_len,
        .open_timeout_ms = config->open_timeout_ms,
        .send_timeout_ms = config->send_timeout_ms,
        .close_timeout_ms = config->close_timeout_ms,
        .fsm_queue_size = config->fsm_queue_size,
        .fsm_task_stack = config->fsm_task_stack,
        .fsm_task_priority = config->fsm_task_priority,
        .loop = me->event_loop,
    };
    tcp_client_handle_t *tcp = tcp_client_create(&tcp_config, core);
    if (!tcp) {
        end_api_call(me);
        return config->max_conns > 1 ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    bool lost_race = (me->tcp != NULL);
    if (!lost_race) {
        me->tcp = tcp;
    }
    xSemaphoreGive(me->lock);
    if (lost_race) {
        tcp_client_destroy(tcp);
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }

    end_api_call(me);
    return ESP_OK;
}
```

Add these Facade wrappers after `lwlte_tcp_init()`:

```c
esp_err_t lwlte_tcp_destroy(lwlte_handle_t *me)
{
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    xSemaphoreTake(me->lock, portMAX_DELAY);
    tcp_client_handle_t *tcp = me->tcp;
    me->tcp = NULL;
    xSemaphoreGive(me->lock);

    if (!tcp) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }
    ret = tcp_client_destroy(tcp);
    if (ret != ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        if (!me->tcp) {
            me->tcp = tcp;
        }
        xSemaphoreGive(me->lock);
    }
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_tcp_open(lwlte_handle_t *me,
                         const lwlte_tcp_open_config_t *config,
                         lwlte_tcp_conn_t **out_conn)
{
    ESP_RETURN_ON_FALSE(config && out_conn, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    tcp_client_handle_t *tcp = NULL;
    esp_err_t ret = begin_api_call(me, false, NULL);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");
    xSemaphoreTake(me->lock, portMAX_DELAY);
    tcp = me->tcp;
    xSemaphoreGive(me->lock);
    if (!tcp) {
        end_api_call(me);
        return ESP_ERR_INVALID_STATE;
    }
    const tcp_client_open_config_t open_config = {
        .host = config->host,
        .port = config->port,
        .user_ctx = config->user_ctx,
    };
    ret = tcp_client_open(tcp, &open_config, (tcp_client_conn_t **)out_conn);
    end_api_call(me);
    return ret;
}

esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn, const uint8_t *data, size_t len)
{
    return tcp_client_send((tcp_client_conn_t *)conn, data, len);
}

esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t *conn)
{
    return tcp_client_close((tcp_client_conn_t *)conn);
}

esp_err_t lwlte_tcp_conn_get_state(lwlte_tcp_conn_t *conn,
                                   lwlte_tcp_conn_state_t *state)
{
    ESP_RETURN_ON_FALSE(state, ESP_ERR_INVALID_ARG, TAG, "state is NULL");
    tcp_conn_state_t tcp_state = TCP_CONN_STATE_ERROR;
    esp_err_t ret = tcp_client_conn_get_state((tcp_client_conn_t *)conn, &tcp_state);
    if (ret == ESP_OK) {
        *state = map_tcp_conn_state(tcp_state);
    }
    return ret;
}

esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t *conn)
{
    return tcp_client_conn_destroy((tcp_client_conn_t *)conn);
}
```

- [ ] **Step 7: Destroy TCP before MQTT and Core**

In `destroy_owned_resources()` in `src/lwlte/lwlte.c`, add this block before MQTT destroy:

```c
    if (me->tcp) {
        ret = tcp_client_destroy(me->tcp);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "destroy TCP client failed: %s", esp_err_to_name(ret));
            return ret;
        }
        me->tcp = NULL;
    }
```

- [ ] **Step 8: Run TCP service boundary tests**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_tcp_service_layer_boundary_and_cmake tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_facade_owns_tcp_and_destroys_before_core -q
```

Expected: PASS for service boundary and Facade ownership tests.

- [ ] **Step 9: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/CMakeLists.txt src/tcp_client src/lwlte/lwlte_priv.h src/lwlte/lwlte.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add tcp client service facade"
```

---

### Task 6: Air780EP TCP Socket Mapping

**Files:**

- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add Air780EP TCP constants and prototypes**

In `src/modem/modem_air780ep.c`, add defines near MQTT constants:

```c
#define AIR780EP_TCP_CONN_ID                 0
#define AIR780EP_TCP_MAX_HEX_READ_BYTES      730
#define AIR780EP_TCP_PAYLOAD_PROMPT          ">"
#define AIR780EP_CIPRXGET_READY_PREFIX       "+CIPRXGET: 1"
#define AIR780EP_TCP_ERROR_PREFIX            "TCP ERROR:"
```

Add static prototypes:

```c
static esp_err_t air780ep_socket_open(modem_handle_t *me, const modem_socket_open_t *open);
static esp_err_t air780ep_socket_send(modem_handle_t *me, const modem_socket_send_t *send);
static esp_err_t air780ep_socket_recv(modem_handle_t *me, const modem_socket_recv_t *recv, modem_socket_recv_result_t *result);
static esp_err_t air780ep_socket_close(modem_handle_t *me, const modem_socket_close_t *close);
static esp_err_t air780ep_socket_prepare(modem_air780ep_t *self);
static void air780ep_post_tcp_readable(modem_air780ep_t *self);
static void air780ep_post_tcp_closed(modem_air780ep_t *self, int reason, int modem_error_code);
static esp_err_t decode_hex_payload(const char *hex, uint8_t **out_payload, size_t *out_len);
```

- [ ] **Step 2: Add socket ops to Air780EP ops table**

In `air780ep_ops`, add:

```c
    .socket_open = air780ep_socket_open,
    .socket_send = air780ep_socket_send,
    .socket_recv = air780ep_socket_recv,
    .socket_close = air780ep_socket_close,
```

- [ ] **Step 3: Implement Air780EP socket prepare and open**

Add:

```c
static esp_err_t air780ep_socket_prepare(modem_air780ep_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");
    ESP_RETURN_ON_ERROR(send_cmd(self, "AT+CIPMUX=0", NULL, 3000), TAG, "set CIPMUX failed");
    ESP_RETURN_ON_ERROR(send_cmd(self, "AT+CIPMODE=0", NULL, 3000), TAG, "set CIPMODE failed");
    ESP_RETURN_ON_ERROR(send_cmd(self, "AT+CIPQSEND=1", NULL, 3000), TAG, "set CIPQSEND failed");
    ESP_RETURN_ON_ERROR(send_cmd(self, "AT+CIPRXF=1", NULL, 3000), TAG, "set CIPRXF failed");
    return send_cmd(self, "AT+CIPRXGET=5", NULL, 3000);
}

static esp_err_t air780ep_socket_open(modem_handle_t *me, const modem_socket_open_t *open)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
    ESP_RETURN_ON_FALSE(open->conn_id == AIR780EP_TCP_CONN_ID && open->proto == MODEM_SOCKET_PROTO_TCP,
                        ESP_ERR_NOT_SUPPORTED, TAG, "Air780EP v1 supports one TCP socket");
    ESP_RETURN_ON_ERROR(air780ep_socket_prepare(self), TAG, "prepare socket failed");

    char cmd[192] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u", open->host, open->port);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "CIPSTART cmd too long");

    char *lines[AIR780EP_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = AIR780EP_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, cmd, &response, open->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "CIPSTART failed");
    return response_contains(&response, "CONNECT OK") || response_contains(&response, "ALREADY CONNECT")
           ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

- [ ] **Step 4: Implement Air780EP socket send and close**

Add:

```c
static esp_err_t air780ep_socket_send(modem_handle_t *me, const modem_socket_send_t *send)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
    ESP_RETURN_ON_FALSE(send->conn_id == AIR780EP_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");

    char cmd[32] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u", (unsigned)send->len);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "CIPSEND cmd too long");

    char *lines[AIR780EP_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = AIR780EP_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd_with_payload(self->base.at, cmd,
                                                    send->data, send->len,
                                                    AIR780EP_TCP_PAYLOAD_PROMPT,
                                                    &response,
                                                    send->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "CIPSEND failed");
    return response_contains(&response, "DATAACCEPT") ||
           response_contains(&response, "DATA ACCEPT") ||
           response_contains(&response, "SEND OK")
           ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t air780ep_socket_close(modem_handle_t *me, const modem_socket_close_t *close)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
    ESP_RETURN_ON_FALSE(close->conn_id == AIR780EP_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");
    char *lines[AIR780EP_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = AIR780EP_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, "AT+CIPCLOSE", &response, close->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "CIPCLOSE failed");
    return response_contains(&response, "CLOSE OK") ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

- [ ] **Step 5: Implement Air780EP HEX receive**

Add:

```c
static esp_err_t air780ep_socket_recv(modem_handle_t *me,
                                      const modem_socket_recv_t *recv,
                                      modem_socket_recv_result_t *result)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
    ESP_RETURN_ON_FALSE(recv->conn_id == AIR780EP_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");

    size_t read_len = recv->max_len;
    if (read_len > AIR780EP_TCP_MAX_HEX_READ_BYTES) {
        read_len = AIR780EP_TCP_MAX_HEX_READ_BYTES;
    }
    char cmd[32] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=3,%u", (unsigned)read_len);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "CIPRXGET cmd too long");

    char *lines[AIR780EP_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = AIR780EP_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, cmd, &response, self->config.base.timing.default_cmd_timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "CIPRXGET failed");

    const char *hex = find_ciprxget_hex_line(&response, &result->remaining_len);
    ESP_RETURN_ON_FALSE(hex, ESP_ERR_INVALID_RESPONSE, TAG, "missing CIPRXGET hex line");
    ret = decode_hex_payload(hex, &result->payload, &result->payload_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "decode CIPRXGET hex failed");
    result->conn_id = recv->conn_id;
    return ESP_OK;
}
```

Add these helper prototypes in the Air780EP static prototype section:

```c
static bool response_contains(const at_response_t *response, const char *needle);
static const char *find_ciprxget_hex_line(const at_response_t *response,
                                          size_t *out_remaining_len);
static int hex_nibble(char c);
```

Add these helper implementations in `src/modem/modem_air780ep.c`:

```c
static bool response_contains(const at_response_t *response, const char *needle)
{
    if (!response || !needle) {
        return false;
    }
    for (int i = 0; i < response->line_count; i++) {
        if (response->lines[i] && strstr(response->lines[i], needle)) {
            return true;
        }
    }
    return false;
}

static const char *find_ciprxget_hex_line(const at_response_t *response,
                                          size_t *out_remaining_len)
{
    if (!response) {
        return NULL;
    }
    for (int i = 0; i < response->line_count; i++) {
        const char *line = response->lines[i];
        if (!line || strncmp(line, "+CIPRXGET: 3,", 13) != 0) {
            continue;
        }
        const char *cursor = line + 13;
        char *end = NULL;
        (void)strtoul(cursor, &end, 10);
        if (!end || *end != ',') {
            return NULL;
        }
        cursor = end + 1;
        unsigned long remaining = strtoul(cursor, &end, 10);
        if (!end || *end != ',') {
            return NULL;
        }
        if (out_remaining_len) {
            *out_remaining_len = (size_t)remaining;
        }
        return end + 1;
    }
    return NULL;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static esp_err_t decode_hex_payload(const char *hex,
                                    uint8_t **out_payload,
                                    size_t *out_len)
{
    ESP_RETURN_ON_FALSE(hex && out_payload && out_len, ESP_ERR_INVALID_ARG,
                        TAG, "NULL argument");
    *out_payload = NULL;
    *out_len = 0;
    size_t hex_len = strlen(hex);
    ESP_RETURN_ON_FALSE((hex_len % 2U) == 0, ESP_ERR_INVALID_RESPONSE, TAG,
                        "odd hex length");
    size_t payload_len = hex_len / 2U;
    uint8_t *payload = malloc(payload_len ? payload_len : 1U);
    ESP_RETURN_ON_FALSE(payload, ESP_ERR_NO_MEM, TAG, "alloc hex payload failed");
    for (size_t i = 0; i < payload_len; i++) {
        int hi = hex_nibble(hex[i * 2U]);
        int lo = hex_nibble(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            free(payload);
            return ESP_ERR_INVALID_RESPONSE;
        }
        payload[i] = (uint8_t)((hi << 4) | lo);
    }
    *out_payload = payload;
    *out_len = payload_len;
    return ESP_OK;
}
```

- [ ] **Step 6: Add Air780EP TCP URC handling**

Register URC handlers with existing Air780EP URC registration style:

```c
AIR780EP_CIPRXGET_READY_PREFIX
"CLOSED"
AIR780EP_TCP_ERROR_PREFIX
```

The readable handler calls:

```c
air780ep_post_tcp_readable(self);
```

The close/error handlers call:

```c
air780ep_post_tcp_closed(self, 0, 0);
air780ep_post_tcp_closed(self, ESP_FAIL, parsed_error_code);
```

Implement post helpers with `MODEM_EVENT_PROTOCOL_DATA` for readable notification and `MODEM_EVENT_PROTOCOL_CLOSED` for close/error:

```c
static void air780ep_post_tcp_readable(modem_air780ep_t *self)
{
    modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_DATA,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = AIR780EP_TCP_CONN_ID,
        },
    };
    (void)modem_post_event(&self->base, &event);
}
```

- [ ] **Step 7: Run Air780EP mapping test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_air780ep_tcp_mapping_tokens -q
```

Expected: PASS.

- [ ] **Step 8: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/modem/modem_air780ep.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add air780ep tcp socket mapping"
```

---

### Task 7: ML307R TCP Socket Mapping

**Files:**

- Modify: `src/modem/modem_ml307r.c`

- [ ] **Step 1: Add ML307R TCP constants and prototypes**

In `src/modem/modem_ml307r.c`, add defines:

```c
#define ML307R_TCP_CONN_ID                 0
#define ML307R_TCP_PAYLOAD_PROMPT          ">"
#define ML307R_MIPURC_RTCP_PREFIX          "+MIPURC: \"rtcp\""
#define ML307R_MIPURC_DISCONN_PREFIX       "+MIPURC: \"disconn\""
```

Add prototypes:

```c
static esp_err_t ml307r_socket_open(modem_handle_t *me, const modem_socket_open_t *open);
static esp_err_t ml307r_socket_send(modem_handle_t *me, const modem_socket_send_t *send);
static esp_err_t ml307r_socket_recv(modem_handle_t *me, const modem_socket_recv_t *recv, modem_socket_recv_result_t *result);
static esp_err_t ml307r_socket_close(modem_handle_t *me, const modem_socket_close_t *close);
static esp_err_t ml307r_socket_prepare(modem_ml307r_t *self);
static void ml307r_post_tcp_readable(modem_ml307r_t *self, size_t recv_len, size_t total_len);
static void ml307r_post_tcp_closed(modem_ml307r_t *self, int reason, int modem_error_code);
static esp_err_t decode_hex_payload(const char *hex, uint8_t **out_payload, size_t *out_len);
```

- [ ] **Step 2: Add socket ops to ML307R ops table**

In `ml307r_ops`, add:

```c
    .socket_open = ml307r_socket_open,
    .socket_send = ml307r_socket_send,
    .socket_recv = ml307r_socket_recv,
    .socket_close = ml307r_socket_close,
```

- [ ] **Step 3: Implement ML307R prepare/open**

Add:

```c
static esp_err_t ml307r_socket_prepare(modem_ml307r_t *self)
{
    ESP_RETURN_ON_FALSE(self, ESP_ERR_INVALID_ARG, TAG, "self is NULL");
    char cmd[48] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"cid\",0,%u", self->config.primary_cid);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "cid cmd too long");
    ESP_RETURN_ON_ERROR(send_cmd(self, cmd, NULL, 3000), TAG, "set MIP cid failed");
    ESP_RETURN_ON_ERROR(send_cmd(self, "AT+MIPCFG=\"encoding\",0,0,1", NULL, 3000), TAG, "set MIP encoding failed");
    return send_cmd(self, "AT+MIPCFG=\"autofree\",0,0", NULL, 3000);
}

static esp_err_t ml307r_socket_open(modem_handle_t *me, const modem_socket_open_t *open)
{
    modem_ml307r_t *self = MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
    ESP_RETURN_ON_FALSE(open->conn_id == ML307R_TCP_CONN_ID && open->proto == MODEM_SOCKET_PROTO_TCP,
                        ESP_ERR_NOT_SUPPORTED, TAG, "ML307R v1 supports one TCP socket");
    ESP_RETURN_ON_ERROR(ml307r_socket_prepare(self), TAG, "prepare socket failed");

    char cmd[224] = {0};
    uint32_t timeout_s = open->timeout_ms ? (open->timeout_ms + 999U) / 1000U : 75U;
    int len = snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=0,\"TCP\",\"%s\",%u,%u,2",
                       open->host, open->port, (unsigned)timeout_s);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "MIPOPEN cmd too long");

    char *lines[ML307R_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = ML307R_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, cmd, &response, open->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "MIPOPEN failed");
    return response_contains(&response, "+MIPOPEN: 0,0") ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

- [ ] **Step 4: Implement ML307R send/recv/close**

Add send:

```c
static esp_err_t ml307r_socket_send(modem_handle_t *me, const modem_socket_send_t *send)
{
    modem_ml307r_t *self = MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
    ESP_RETURN_ON_FALSE(send->conn_id == ML307R_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");
    char cmd[32] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+MIPSEND=0,%u", (unsigned)send->len);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "MIPSEND cmd too long");

    char *lines[ML307R_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = ML307R_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd_with_payload(self->base.at, cmd, send->data,
                                                    send->len, ML307R_TCP_PAYLOAD_PROMPT,
                                                    &response, send->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "MIPSEND failed");
    return response_contains(&response, "+MIPSEND: 0,") ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

Add receive:

```c
static esp_err_t ml307r_socket_recv(modem_handle_t *me,
                                    const modem_socket_recv_t *recv,
                                    modem_socket_recv_result_t *result)
{
    modem_ml307r_t *self = MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
    ESP_RETURN_ON_FALSE(recv->conn_id == ML307R_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");
    char cmd[32] = {0};
    int len = snprintf(cmd, sizeof(cmd), "AT+MIPRD=0,%u", (unsigned)recv->max_len);
    ESP_RETURN_ON_FALSE(len > 0 && (size_t)len < sizeof(cmd), ESP_ERR_INVALID_ARG, TAG, "MIPRD cmd too long");

    char *lines[ML307R_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = ML307R_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, cmd, &response, self->config.base.timing.default_cmd_timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "MIPRD failed");

    const char *hex = find_miprd_hex_line(&response, &result->remaining_len);
    ESP_RETURN_ON_FALSE(hex, ESP_ERR_INVALID_RESPONSE, TAG, "missing MIPRD hex line");
    ret = decode_hex_payload(hex, &result->payload, &result->payload_len);
    ESP_RETURN_ON_ERROR(ret, TAG, "decode MIPRD hex failed");
    result->conn_id = recv->conn_id;
    return ESP_OK;
}
```

Add close:

```c
static esp_err_t ml307r_socket_close(modem_handle_t *me, const modem_socket_close_t *close)
{
    modem_ml307r_t *self = MODEM_CONTAINER_OF(me, modem_ml307r_t, base);
    ESP_RETURN_ON_FALSE(close->conn_id == ML307R_TCP_CONN_ID, ESP_ERR_NOT_SUPPORTED, TAG, "invalid conn_id");
    char *lines[ML307R_MAX_RESPONSE_LINES] = {0};
    at_response_t response = {
        .lines = lines,
        .max_lines = ML307R_MAX_RESPONSE_LINES,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, "AT+MIPCLOSE=0", &response, close->timeout_ms);
    ESP_RETURN_ON_ERROR(ret, TAG, "MIPCLOSE failed");
    return response_contains(&response, "+MIPCLOSE: 0") ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
```

- [ ] **Step 5: Add ML307R TCP URC handling**

Register handlers for:

```c
ML307R_MIPURC_RTCP_PREFIX
ML307R_MIPURC_DISCONN_PREFIX
```

The `rtcp` handler parses `+MIPURC: "rtcp",0,<recv_len>,<total_len>` and calls:

```c
ml307r_post_tcp_readable(self, recv_len, total_len);
```

The `disconn` handler parses `+MIPURC: "disconn",0,<connect_state>` and calls:

```c
ml307r_post_tcp_closed(self, connect_state, 0);
```

Readable post helper:

```c
static void ml307r_post_tcp_readable(modem_ml307r_t *self, size_t recv_len, size_t total_len)
{
    modem_event_t event = {
        .id = MODEM_EVENT_PROTOCOL_DATA,
        .data.protocol_data = {
            .protocol = MODEM_PROTOCOL_TCP,
            .conn_id = ML307R_TCP_CONN_ID,
            .payload_len = recv_len,
            .reason = (int)total_len,
        },
    };
    (void)modem_post_event(&self->base, &event);
}
```

- [ ] **Step 6: Run ML307R mapping test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_ml307r_tcp_mapping_tokens -q
```

Expected: PASS.

- [ ] **Step 7: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src/modem/modem_ml307r.c tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add ml307r tcp socket mapping"
```

---

### Task 8: TCP Examples And Kconfig

**Files:**

- Create: `example/air780ep_tcp_client.c`
- Create: `example/ml307r_tcp_client.c`
- Modify: `example/CMakeLists.txt`
- Modify: `example/example.h`
- Modify: `example/main.c`
- Modify: `example/Kconfig.projbuild`
- Modify: `example/README.md`

- [ ] **Step 1: Add example selection constants and CMake sources**

In `example/example.h`, add:

```c
#define EXAMPLE_AIR780EP_TCP_CLIENT     5
#define EXAMPLE_ML307R_TCP_CLIENT       6

void example_air780ep_tcp_client_run(void);
void example_ml307r_tcp_client_run(void);
```

In `example/main.c`, add switch cases:

```c
    case EXAMPLE_AIR780EP_TCP_CLIENT:
        example_air780ep_tcp_client_run();
        break;
    case EXAMPLE_ML307R_TCP_CLIENT:
        example_ml307r_tcp_client_run();
        break;
```

In `example/CMakeLists.txt`, add:

```cmake
         "air780ep_tcp_client.c"
         "ml307r_tcp_client.c"
```

- [ ] **Step 2: Add TCP example Kconfig settings**

In `example/Kconfig.projbuild`, add after the MQTT menu:

```kconfig
menu "Example TCP Settings"

config EXAMPLE_TCP_HOST
    string "TCP server host"
    default "tcpbin.com"

config EXAMPLE_TCP_PORT
    int "TCP server port"
    range 1 65535
    default 4242

config EXAMPLE_TCP_PAYLOAD
    string "TCP payload text"
    default "hello from esp-lwlte tcp"

config EXAMPLE_TCP_PAYLOAD_HEX
    string "TCP payload HEX bytes"
    default ""
    help
        When non-empty, decode this HEX string and send the bytes instead of EXAMPLE_TCP_PAYLOAD.

config EXAMPLE_TCP_OPEN_TIMEOUT_MS
    int "TCP open timeout milliseconds"
    range 1000 180000
    default 75000

config EXAMPLE_TCP_SEND_QUEUE_SIZE
    int "TCP send queue size"
    range 1 16
    default 4

config EXAMPLE_TCP_MAX_TX_LEN
    int "TCP max TX length"
    range 1 1460
    default 1460

config EXAMPLE_TCP_MAX_RX_EVENT_LEN
    int "TCP max RX event length"
    range 1 730
    default 730

endmenu
```

- [ ] **Step 3: Create Air780EP TCP example**

Create `example/air780ep_tcp_client.c` with this complete structure and fill only the hardware constants from the existing Air780EP examples:

```c
/**
 * @file air780ep_tcp_client.c
 * @brief Air780EP LTE TCP 客户端示例
 * @details Air780EP LTE TCP client example
 * @author JovisDreams
 * @date 2026-06-18
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"
#include "lwlte.h"

#define TAG                                      "air780ep_tcp"
#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1
#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_READY_TIMEOUT_MS                 30000
#define EXAMPLE_IDLE_DELAY_MS                    1000
#define EXAMPLE_PAYLOAD_BUF_LEN                  256

static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);
static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data);
static size_t build_payload(uint8_t *out, size_t out_len);

static lwlte_tcp_conn_t *s_conn;
static uint8_t s_payload[EXAMPLE_PAYLOAD_BUF_LEN];
static size_t s_payload_len;

void example_air780ep_tcp_client_run(void)
{
    s_conn = NULL;
    s_payload_len = build_payload(s_payload, sizeof(s_payload));
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwlte_handle_t *lte = NULL;
    const lwlte_air780ep_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .at_engine = {
                .rx_line_buf_size = 2048,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
        },
    };

    const lwlte_tcp_config_t tcp_config = {
        .send_queue_size = CONFIG_EXAMPLE_TCP_SEND_QUEUE_SIZE,
        .max_tx_len = CONFIG_EXAMPLE_TCP_MAX_TX_LEN,
        .max_rx_event_len = CONFIG_EXAMPLE_TCP_MAX_RX_EVENT_LEN,
        .open_timeout_ms = CONFIG_EXAMPLE_TCP_OPEN_TIMEOUT_MS,
    };

    ESP_ERROR_CHECK(lwlte_air780ep_init(&config, &lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID, lwlte_event_cb, lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_TCP_EVENT, ESP_EVENT_ANY_ID, tcp_event_cb, lte));
    ESP_ERROR_CHECK(lwlte_tcp_init(lte, &tcp_config));
    ESP_ERROR_CHECK(lwlte_start(lte));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}

static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_data;
    lwlte_handle_t *lte = (lwlte_handle_t *)arg;
    if ((lwlte_event_id_t)event_id != LWLTE_EVENT_NET_ONLINE || s_conn) {
        return;
    }
    const lwlte_tcp_open_config_t open_config = {
        .host = CONFIG_EXAMPLE_TCP_HOST,
        .port = CONFIG_EXAMPLE_TCP_PORT,
        .user_ctx = lte,
    };
    esp_err_t ret = lwlte_tcp_open(lte, &open_config, &s_conn);
    ESP_LOGI(TAG, "TCP open submitted: %s", esp_err_to_name(ret));
}

static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    lwlte_tcp_event_data_t *data = event_data;
    ESP_LOGI(TAG, "TCP event=%d", (int)event_id);
    switch ((lwlte_tcp_event_id_t)event_id) {
    case LWLTE_TCP_EVENT_CONNECTED:
        if (data && data->conn && s_payload_len > 0) {
            (void)lwlte_tcp_send(data->conn, s_payload, s_payload_len);
        }
        break;
    case LWLTE_TCP_EVENT_SENT:
        ESP_LOGI(TAG, "TCP sent len=%u", data ? (unsigned)data->sent_len : 0U);
        break;
    case LWLTE_TCP_EVENT_DATA:
        if (data) {
            ESP_LOGI(TAG, "TCP RX len=%u", (unsigned)data->payload_len);
            lwlte_tcp_event_data_release(data);
            (void)lwlte_tcp_close(data->conn);
        }
        break;
    case LWLTE_TCP_EVENT_DISCONNECTED:
    case LWLTE_TCP_EVENT_ERROR:
        if (s_conn) {
            (void)lwlte_tcp_conn_destroy(s_conn);
            s_conn = NULL;
        }
        break;
    default:
        break;
    }
}

static size_t build_payload(uint8_t *out, size_t out_len)
{
    const char *hex = CONFIG_EXAMPLE_TCP_PAYLOAD_HEX;
    if (hex[0] == '\0') {
        size_t len = strnlen(CONFIG_EXAMPLE_TCP_PAYLOAD, out_len);
        memcpy(out, CONFIG_EXAMPLE_TCP_PAYLOAD, len);
        return len;
    }
    size_t written = 0;
    while (hex[0] && hex[1] && written < out_len) {
        unsigned int byte = 0;
        if (sscanf(hex, "%2x", &byte) != 1) {
            break;
        }
        out[written++] = (uint8_t)byte;
        hex += 2;
    }
    return written;
}
```

- [ ] **Step 4: Create ML307R TCP example**

Create `example/ml307r_tcp_client.c` with this complete structure:

```c
/**
 * @file ml307r_tcp_client.c
 * @brief ML307R LTE TCP 客户端示例
 * @details ML307R LTE TCP client example
 * @author JovisDreams
 * @date 2026-06-18
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"
#include "lwlte.h"

#define TAG                                      "ml307r_tcp"
#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_0
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_1
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_2
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1
#define EXAMPLE_MODEM_RESET_PULSE_MS             500
#define EXAMPLE_READY_TIMEOUT_MS                 30000
#define EXAMPLE_IDLE_DELAY_MS                    1000
#define EXAMPLE_PAYLOAD_BUF_LEN                  256

static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data);
static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data);
static size_t build_payload(uint8_t *out, size_t out_len);

static lwlte_tcp_conn_t *s_conn;
static uint8_t s_payload[EXAMPLE_PAYLOAD_BUF_LEN];
static size_t s_payload_len;

void example_ml307r_tcp_client_run(void)
{
    s_conn = NULL;
    s_payload_len = build_payload(s_payload, sizeof(s_payload));
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    lwlte_handle_t *lte = NULL;

    const lwlte_ml307r_config_t config = {
        .base = {
            .uart = {
                .num = EXAMPLE_LTE_UART_NUM,
                .tx_pin = EXAMPLE_LTE_UART_TX_PIN,
                .rx_pin = EXAMPLE_LTE_UART_RX_PIN,
                .baud_rate = EXAMPLE_LTE_UART_BAUD_RATE,
            },
            .at_engine = {
                .rx_line_buf_size = 2048,
            },
            .modem = {
                .en_pin = EXAMPLE_LTE_EN_PIN,
                .ready_timeout_ms = EXAMPLE_READY_TIMEOUT_MS,
                .reset_pulse_ms = EXAMPLE_MODEM_RESET_PULSE_MS,
            },
            .core = {
                .apn = EXAMPLE_LTE_APN,
                .primary_cid = EXAMPLE_LTE_PRIMARY_CID,
            },
        },
    };

    const lwlte_tcp_config_t tcp_config = {
        .send_queue_size = CONFIG_EXAMPLE_TCP_SEND_QUEUE_SIZE,
        .max_tx_len = CONFIG_EXAMPLE_TCP_MAX_TX_LEN,
        .max_rx_event_len = CONFIG_EXAMPLE_TCP_MAX_RX_EVENT_LEN,
        .open_timeout_ms = CONFIG_EXAMPLE_TCP_OPEN_TIMEOUT_MS,
    };

    ESP_ERROR_CHECK(lwlte_ml307r_init(&config, &lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_EVENT, ESP_EVENT_ANY_ID,
                                               lwlte_event_cb, lte));
    ESP_ERROR_CHECK(esp_event_handler_register(LWLTE_TCP_EVENT, ESP_EVENT_ANY_ID,
                                               tcp_event_cb, lte));
    ESP_ERROR_CHECK(lwlte_tcp_init(lte, &tcp_config));
    ESP_ERROR_CHECK(lwlte_start(lte));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EXAMPLE_IDLE_DELAY_MS));
    }
}

static void lwlte_event_cb(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_data;
    lwlte_handle_t *lte = (lwlte_handle_t *)arg;
    if ((lwlte_event_id_t)event_id != LWLTE_EVENT_NET_ONLINE || s_conn) {
        return;
    }
    const lwlte_tcp_open_config_t open_config = {
        .host = CONFIG_EXAMPLE_TCP_HOST,
        .port = CONFIG_EXAMPLE_TCP_PORT,
        .user_ctx = lte,
    };
    esp_err_t ret = lwlte_tcp_open(lte, &open_config, &s_conn);
    ESP_LOGI(TAG, "TCP open submitted: %s", esp_err_to_name(ret));
}

static void tcp_event_cb(void *arg, esp_event_base_t base,
                         int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    lwlte_tcp_event_data_t *data = event_data;
    ESP_LOGI(TAG, "TCP event=%d", (int)event_id);
    switch ((lwlte_tcp_event_id_t)event_id) {
    case LWLTE_TCP_EVENT_CONNECTED:
        if (data && data->conn && s_payload_len > 0) {
            (void)lwlte_tcp_send(data->conn, s_payload, s_payload_len);
        }
        break;
    case LWLTE_TCP_EVENT_SENT:
        ESP_LOGI(TAG, "TCP sent len=%u", data ? (unsigned)data->sent_len : 0U);
        break;
    case LWLTE_TCP_EVENT_DATA:
        if (data) {
            ESP_LOGI(TAG, "TCP RX len=%u", (unsigned)data->payload_len);
            lwlte_tcp_event_data_release(data);
            (void)lwlte_tcp_close(data->conn);
        }
        break;
    case LWLTE_TCP_EVENT_DISCONNECTED:
    case LWLTE_TCP_EVENT_ERROR:
        if (s_conn) {
            (void)lwlte_tcp_conn_destroy(s_conn);
            s_conn = NULL;
        }
        break;
    default:
        break;
    }
}

static size_t build_payload(uint8_t *out, size_t out_len)
{
    const char *hex = CONFIG_EXAMPLE_TCP_PAYLOAD_HEX;
    if (hex[0] == '\0') {
        size_t len = strnlen(CONFIG_EXAMPLE_TCP_PAYLOAD, out_len);
        memcpy(out, CONFIG_EXAMPLE_TCP_PAYLOAD, len);
        return len;
    }
    size_t written = 0;
    while (hex[0] && hex[1] && written < out_len) {
        unsigned int byte = 0;
        if (sscanf(hex, "%2x", &byte) != 1) {
            break;
        }
        out[written++] = (uint8_t)byte;
        hex += 2;
    }
    return written;
}

```

- [ ] **Step 5: Document examples**

In `example/README.md`, add entries for:

```markdown
`EXAMPLE_AIR780EP_TCP_CLIENT` | Air780EP TCP client echo example
`EXAMPLE_ML307R_TCP_CLIENT` | ML307R TCP client echo example

## TCP Client Examples

Configure `EXAMPLE_TCP_HOST`, `EXAMPLE_TCP_PORT`, and either `EXAMPLE_TCP_PAYLOAD` or `EXAMPLE_TCP_PAYLOAD_HEX`. The TCP examples set `base.at_engine.rx_line_buf_size = 2048` because the socket RX path reads printable HEX payload lines from the modem.
```

- [ ] **Step 6: Run examples contract test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_examples_and_docs_are_wired -q
```

Expected: PASS after docs updates in Task 9, or fail only on docs tokens if Task 9 has not run yet.

- [ ] **Step 7: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add example/air780ep_tcp_client.c example/ml307r_tcp_client.c example/CMakeLists.txt example/example.h example/main.c example/Kconfig.projbuild example/README.md tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "feat: add tcp client examples"
```

---

### Task 9: Project Documentation Updates

**Files:**

- Modify: `docs/agents/directory-structure.md`
- Modify: `docs/agents/feature-roadmap.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/at_cmd_air780ep.md`
- Modify: `docs/agents/at_cmd_ml307r.md`

- [ ] **Step 1: Update directory structure**

In `docs/agents/directory-structure.md`, add entries for:

```markdown
- `src/tcp_client/` - internal asynchronous TCP client service used by Facade.
- `example/air780ep_tcp_client.c` - Air780EP TCP client example.
- `example/ml307r_tcp_client.c` - ML307R TCP client example.
```

- [ ] **Step 2: Update roadmap**

In `docs/agents/feature-roadmap.md`, add or update the TCP/UDP row with:

```markdown
TCP client v1 is implemented for Air780EP and ML307R with plain TCP, one client connection, binary-safe TX/RX, and ESP event delivery. UDP, TLS, server/listen mode, local port binding, multiple simultaneous connections, automatic reconnect, and high-throughput streaming remain planned work.
```

- [ ] **Step 3: Update architecture**

In `docs/agents/architecture.md`, add:

```markdown
TCP Client Service follows the same service boundary as MQTT: App code calls `lwlte_tcp_*`, Facade delegates to `tcp_client`, `tcp_client` submits `CORE_CMD_SOCKET_*`, Core serializes Modem access, and Modem maps socket operations to module AT commands. The TCP service does not include Modem or AT Engine headers.

Runtime path: `tcp_client -> Core -> Modem -> AT Engine`.
```

- [ ] **Step 4: Update classes doc**

In `docs/agents/classes.md`, add sections containing these tokens and descriptions:

```markdown
## TCP Client Service

`tcp_client_handle_t` owns the TCP FSM task, one `tcp_client_conn_t`, Core protocol callbacks for `CORE_PROTOCOL_TCP`, and event posting to `LWLTE_TCP_EVENT`.

`tcp_client_conn_t` is the internal object behind public `lwlte_tcp_conn_t`. It owns connection state, `conn_id=0`, user context, pending Core command metadata, and a FIFO of copied TX payloads.

TCP event handlers must call `lwlte_tcp_event_data_release()` before returning when `owns_event` or `owns_payload` is true. `LWLTE_TCP_EVENT_DATA` payloads additionally carry heap-owned bytes when `owns_payload` is true.

## Socket Commands

Core socket commands are `CORE_CMD_SOCKET_OPEN`, `CORE_CMD_SOCKET_SEND`, `CORE_CMD_SOCKET_RECV`, and `CORE_CMD_SOCKET_CLOSE`. Core deep-copies socket hosts and TX payloads before enqueueing commands.
```

- [ ] **Step 5: Update AT command docs**

In `docs/agents/at_cmd_air780ep.md`, add:

```markdown
TCP client v1 uses `AT+CIPSTART`, `AT+CIPSEND`, `AT+CIPRXGET=5`, `AT+CIPRXGET=3,<len>`, and `AT+CIPCLOSE`. RX is read in HEX/manual mode and decoded by the Air780EP Modem implementation.
```

In `docs/agents/at_cmd_ml307r.md`, add:

```markdown
TCP client v1 uses `AT+MIPCFG="cid"`, `AT+MIPCFG="encoding",0,0,1`, `AT+MIPOPEN`, `AT+MIPSEND`, `AT+MIPRD`, and `AT+MIPCLOSE`. RX cache notifications use `+MIPURC: "rtcp"`; disconnect notifications use `+MIPURC: "disconn"`.
```

- [ ] **Step 6: Run docs portion of contract test**

Run:

```bash
python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py::TcpClientEndToEndContractTest::test_examples_and_docs_are_wired -q
```

Expected: PASS.

- [ ] **Step 7: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add docs/agents/directory-structure.md docs/agents/feature-roadmap.md docs/agents/architecture.md docs/agents/classes.md docs/agents/at_cmd_air780ep.md docs/agents/at_cmd_ml307r.md tests/host/test_tcp_client_end_to_end_contract.py
git commit -m "docs: document tcp client service"
```

---

### Task 10: Verification And Build

**Files:**

- Verify: all modified files

- [ ] **Step 1: Run host contract tests**

Run:

```bash
python3 -m pytest tests/host -q
```

Expected: all host tests pass.

- [ ] **Step 2: Build with ESP-IDF MCP**

Use the ESP-IDF MCP build tool:

```text
esp-idf-eim_build_project
```

Expected: project builds successfully for the currently configured target.

If MCP is unavailable, run the documented fallback command:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: `Project build complete`.

- [ ] **Step 3: Verify Air780EP example selection builds**

Temporarily set this in `example/main.c`:

```c
#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_TCP_CLIENT
```

Run the ESP-IDF MCP build tool again.

Expected: build succeeds.

Restore the previous default example selection unless the user asks to keep TCP selected.

- [ ] **Step 4: Verify ML307R example selection builds**

Temporarily set this in `example/main.c`:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_TCP_CLIENT
```

Run the ESP-IDF MCP build tool again.

Expected: build succeeds.

Restore the previous default example selection unless the user asks to keep TCP selected.

- [ ] **Step 5: Optional real-device Air780EP verification**

When Air780EP hardware and an echo server are available, flash the Air780EP TCP example and capture logs. Expected event order:

```text
LWLTE_EVENT_NET_ONLINE
LWLTE_TCP_EVENT_CONNECTED
LWLTE_TCP_EVENT_SENT
LWLTE_TCP_EVENT_DATA
LWLTE_TCP_EVENT_DISCONNECTED
```

Expected payload result: received byte length equals sent byte length.

- [ ] **Step 6: Optional real-device ML307R verification**

When ML307R hardware and an echo server are available, flash the ML307R TCP example and capture logs. Expected event order:

```text
LWLTE_EVENT_NET_ONLINE
LWLTE_TCP_EVENT_CONNECTED
LWLTE_TCP_EVENT_SENT
LWLTE_TCP_EVENT_DATA
LWLTE_TCP_EVENT_DISCONNECTED
```

Expected payload result: received byte length equals sent byte length.

- [ ] **Step 7: Final status inspection**

Run:

```bash
git status --short
```

Expected: only intended TCP implementation, examples, tests, and docs files are modified or untracked.

- [ ] **Step 8: Commit if explicitly authorized**

Only if commits are authorized:

```bash
git add src include example docs tests
git commit -m "feat: add tcp client v1"
```

If using task-level commits, skip this aggregate commit.

---

## Self-Review Notes

Spec coverage:

- Public `lwlte_tcp_*` API: Task 2 and Task 5.
- Opaque connection handle: Task 2 and Task 5.
- Async events and binary-safe DATA ownership: Task 2 and Task 5.
- Single connection with `max_conns > 1` returning `ESP_ERR_NOT_SUPPORTED`: Task 5.
- Network-online requirement before open: Task 5.
- Core generic socket commands: Task 3.
- Protocol callback coexistence for MQTT/TCP: Task 3.
- Modem socket API: Task 4.
- Air780EP AT mapping: Task 6.
- ML307R AT mapping: Task 7.
- Examples and Kconfig: Task 8.
- Docs and verification: Task 9 and Task 10.

Type consistency:

- Public connection handle is `lwlte_tcp_conn_t *`.
- Internal connection handle is `tcp_client_conn_t *`.
- Core protocol enum is `core_protocol_t` with `CORE_PROTOCOL_MAX`.
- Modem protocol enum is `modem_protocol_t` with `MODEM_PROTOCOL_TCP`.
- Socket command names consistently use `CORE_CMD_SOCKET_*` and `modem_socket_*`.

Verification commands:

- `python3 -m pytest tests/host/test_tcp_client_end_to_end_contract.py -q`
- `python3 -m pytest tests/host -q`
- `esp-idf-eim_build_project`
