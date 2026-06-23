# TCP Client Design

## Context

`esp-lwlte` already has a layered service architecture:

- App code only uses public Facade APIs from `src/include/lwlte.h`.
- Facade owns composition and delegates runtime work to internal services.
- MQTT Client Service sits above Core and submits protocol commands through `core_submit_cmd()`.
- Core FSM is the serialized executor for Modem operations.
- Modem Adapter translates semantic operations into Air780EP or ML307R AT commands.
- AT Engine sends commands, handles prompt payload writes, and dispatches idle URCs by prefix.

TCP is planned as the next transport capability after the existing network/PDP, MQTT, and Ping features. The current implementation already has useful pieces, but TCP needs two architectural prerequisites:

- Protocol data events are MQTT-shaped today: they carry `topic` and `payload` but no connection id.
- Core protocol callbacks are single global slots, so enabling TCP and MQTT together would otherwise cause one service to overwrite the other's callback.

This design defines the first TCP client feature and the minimum shared event-routing changes needed to support it cleanly.

## Decisions

| Topic | Decision |
|-------|----------|
| Public API | TCP-specific `lwlte_tcp_*` API |
| Internal model | Generic socket command/Modem ops family, exposed publicly only as TCP in v1 |
| Modules | Air780EP and ML307R |
| Connection count | Single TCP client connection in v1 |
| Connection identity | Opaque `lwlte_tcp_conn_t *` |
| API style | Asynchronous operations with ESP-IDF events |
| RX model | Event-driven `LWLTE_TCP_EVENT_DATA` |
| Data model | Binary-safe `uint8_t * + len` for TX and RX |
| Init model | Explicit `lwlte_tcp_init(lte, &config)` |
| Reconnect | No automatic reconnect in v1 |
| Transport | Plain TCP client only |
| MQTT coexistence | Must support TCP and MQTT enabled at the same time |
| Examples | Air780EP TCP client example and ML307R TCP client example |
| Verification | Build verification plus optional real-device echo/client logs |

## Goals

- Add an asynchronous public TCP client API through the LWLTE Facade.
- Support Air780EP and ML307R plain TCP client connections.
- Keep the public API TCP-specific and easy to use.
- Keep the internal Modem/Core command boundary generic enough for future UDP and multi-connection support.
- Make TX and RX binary-safe, including payloads containing `0x00`, `\r\n`, and arbitrary bytes.
- Allow TCP and MQTT services to be initialized and run on the same `lwlte_handle_t`.
- Use conservative defaults while allowing configuration of queue sizes and per-event/per-send lengths.
- Add examples for both supported modules.

## Non-Goals

- Do not implement UDP.
- Do not implement TLS or expose TLS config fields in the first public TCP API.
- Do not implement TCP server/listen mode.
- Do not implement local port binding.
- Do not implement multiple simultaneous TCP connections.
- Do not implement automatic reconnect.
- Do not promise high-throughput or large-file transfer performance in v1.
- Do not add a host-side modem simulator or unit-test framework as part of this feature.
- Do not let TCP Client Service call Modem or AT Engine directly.

## Public Facade API

The public API is added to `src/include/lwlte.h`.

```c
typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;

typedef enum {
    LWLTE_TCP_CONN_STATE_CREATED = 0,
    LWLTE_TCP_CONN_STATE_CONNECTING,
    LWLTE_TCP_CONN_STATE_CONNECTED,
    LWLTE_TCP_CONN_STATE_CLOSING,
    LWLTE_TCP_CONN_STATE_CLOSED,
    LWLTE_TCP_CONN_STATE_ERROR,
} lwlte_tcp_conn_state_t;

typedef enum {
    LWLTE_TCP_EVENT_STARTED = 0,
    LWLTE_TCP_EVENT_STOPPED,
    LWLTE_TCP_EVENT_CONNECTED,
    LWLTE_TCP_EVENT_DISCONNECTED,
    LWLTE_TCP_EVENT_SENT,
    LWLTE_TCP_EVENT_DATA,
    LWLTE_TCP_EVENT_ERROR,
} lwlte_tcp_event_id_t;

ESP_EVENT_DECLARE_BASE(LWLTE_TCP_EVENT);
```

TCP service configuration:

```c
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
} lwlte_tcp_config_t;
```

Open request:

```c
typedef struct {
    const char *host;
    uint16_t port;
    void *user_ctx;
} lwlte_tcp_open_config_t;
```

Event data:

```c
typedef struct {
    lwlte_tcp_conn_t *conn;
    void *user_ctx;
    lwlte_tcp_conn_state_t conn_state;
    esp_err_t error_code;
    int modem_error_code;
    int reason;
    size_t sent_len;
    const uint8_t *payload;
    size_t payload_len;
    bool owns_payload;
    bool owns_event;
} lwlte_tcp_event_data_t;
```

Lifecycle and operations:

```c
esp_err_t lwlte_tcp_init(lwlte_handle_t *me,
                         const lwlte_tcp_config_t *config);
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

void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data);
```

Public API rules:

- `lwlte_tcp_init()` must be called before `lwlte_tcp_open()`.
- `max_conns` defaults to `1`; v1 only supports `1`. `max_conns > 1` returns `ESP_ERR_NOT_SUPPORTED`. Other malformed config values return `ESP_ERR_INVALID_ARG`.
- `lwlte_tcp_open()` requires the Core network state to be online. If the network is not online, it returns `ESP_ERR_INVALID_STATE` and does not create a connection object.
- `lwlte_tcp_open()` creates an opaque connection object and asynchronously starts connection establishment.
- `lwlte_tcp_send()` copies the caller's payload into a service-owned FIFO before returning. The caller may reuse or free its buffer immediately after `lwlte_tcp_send()` returns.
- `lwlte_tcp_send()` rejects `len == 0` and `len > max_tx_len` with `ESP_ERR_INVALID_ARG` in v1.
- The TCP service sends FIFO items in submission order and posts `LWLTE_TCP_EVENT_SENT` after each successful send.
- Every `LWLTE_TCP_EVENT` handler must call `lwlte_tcp_event_data_release()` before returning when `owns_event` or `owns_payload` is true. `LWLTE_TCP_EVENT_DATA` additionally carries a heap-owned payload copy when `owns_payload` is true.
- `lwlte_tcp_conn_destroy()` is explicit. Connection failure, remote close, and `lwlte_tcp_close()` completion do not automatically free the object.
- `user_ctx` is captured from `lwlte_tcp_open_config_t` and returned unchanged in all events for that connection.

## Defaults

Recommended defaults:

| Field | Default |
|-------|---------|
| `max_conns` | `1` |
| `send_queue_size` | `4` |
| `max_tx_len` | `1460` |
| `max_rx_event_len` | `730` |
| `open_timeout_ms` | `75000` |
| `send_timeout_ms` | `50000` |
| `close_timeout_ms` | `10000` |
| `fsm_queue_size` | `16` |
| `fsm_task_stack` | `4096` |
| `fsm_task_priority` | `8` |

These defaults target control, telemetry, and low-to-medium throughput TCP traffic. `max_rx_event_len` defaults to `730` because Air780EP HEX receive reads at most 730 raw bytes per command. Larger values are accepted only when the selected module path can honor them safely; v1 does not promise high-throughput streaming.

## Internal Architecture

The runtime stack is:

```text
App
  -> lwlte_tcp_* public API
       -> Facade
            -> tcp_client service
                 -> core_submit_cmd(CORE_CMD_SOCKET_*)
                      -> Core FSM
                           -> modem_socket_*()
                                -> Air780EP / ML307R AT commands
                                     -> AT Engine
```

Responsibilities:

- Facade owns the public API, stores `tcp_client_handle_t *tcp`, and destroys TCP before Core.
- TCP Client Service owns the TCP FSM, connection object, send FIFO, event posting, and user-facing state transitions.
- Core serializes all socket commands with MQTT, Ping, network activation, and other Modem operations.
- Modem Adapter exposes semantic socket operations and translates them per module.
- AT Engine remains generic. It sends commands and raw payloads but does not understand TCP.

The TCP service must not include `modem.h`, `modem_air780ep.h`, `modem_ml307r.h`, `at_engine.h`, or any other lower-layer header except `core.h`.

## TCP Client Service

Add internal layer API under `src/tcp_client/`:

```c
typedef struct tcp_client_handle tcp_client_handle_t;
typedef struct tcp_client_conn tcp_client_conn_t;

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
```

Core-facing behavior:

- `tcp_client_create()` receives a borrowed `core_handle_t *` and a config snapshot.
- `tcp_client_open()` checks Core network state before creating a connection.
- The FSM submits `CORE_CMD_SOCKET_OPEN`, `CORE_CMD_SOCKET_SEND`, `CORE_CMD_SOCKET_RECV`, and `CORE_CMD_SOCKET_CLOSE`.
- Core command completion callbacks only enqueue TCP FSM signals; they do not mutate connection state directly.
- Protocol data/closed callbacks only copy or enqueue lightweight signals.

Internal connection state:

```c
typedef enum {
    TCP_CONN_STATE_CREATED = 0,
    TCP_CONN_STATE_CONNECTING,
    TCP_CONN_STATE_CONNECTED,
    TCP_CONN_STATE_CLOSING,
    TCP_CONN_STATE_CLOSED,
    TCP_CONN_STATE_ERROR,
} tcp_conn_state_t;
```

FSM flow:

1. `tcp_client_open()` creates a conn and queues `TCP_SIG_OPEN`.
2. FSM sets conn to `CONNECTING` and submits `CORE_CMD_SOCKET_OPEN`.
3. Open success sets conn to `CONNECTED` and posts `LWLTE_TCP_EVENT_CONNECTED`.
4. `tcp_client_send()` copies payload into the send FIFO and queues `TCP_SIG_SEND_READY`.
5. FSM drains one FIFO item at a time through `CORE_CMD_SOCKET_SEND` and posts `LWLTE_TCP_EVENT_SENT` on success.
6. RX-readable events queue `TCP_SIG_RX_READY`; FSM submits `CORE_CMD_SOCKET_RECV` with `max_rx_event_len` and posts one `LWLTE_TCP_EVENT_DATA` for each returned payload chunk.
7. `tcp_client_close()` queues `TCP_SIG_CLOSE`; FSM rejects new sends, clears unsent FIFO entries, submits `CORE_CMD_SOCKET_CLOSE`, and transitions to `CLOSED` on completion.
8. Network offline, remote close, PDP loss, or socket error clears pending sends and posts `DISCONNECTED` or `ERROR` without reconnecting.

Network offline behavior:

- Any active connection becomes closed or error.
- Pending send FIFO entries are released without sending.
- TCP posts `LWLTE_TCP_EVENT_DISCONNECTED` for normal loss and `LWLTE_TCP_EVENT_ERROR` when a module/network error reason is available.
- The application must wait for `LWLTE_EVENT_NET_ONLINE` and call `lwlte_tcp_open()` again if it wants to reconnect.

## Core Changes

Core command types gain a socket command family:

```c
typedef enum {
    CORE_SOCKET_PROTO_TCP = 0,
} core_socket_proto_t;

typedef struct {
    core_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
} core_socket_open_t;

typedef struct {
    uint8_t conn_id;
    const uint8_t *data;
    size_t len;
} core_socket_send_t;

typedef struct {
    uint8_t conn_id;
    size_t max_len;
} core_socket_recv_t;

typedef struct {
    uint8_t conn_id;
} core_socket_close_t;
```

Command enum additions:

```c
CORE_CMD_SOCKET_OPEN,
CORE_CMD_SOCKET_SEND,
CORE_CMD_SOCKET_RECV,
CORE_CMD_SOCKET_CLOSE,
```

Core command ownership rules:

- `core_submit_cmd()` deep-copies socket `host` and TX payload data.
- `CORE_CMD_SOCKET_RECV` returns a heap-owned payload result to the done callback; the TCP service takes ownership on success and frees it on error.
- All socket commands require Core network state `ONLINE`; otherwise Core completes them with `ESP_ERR_INVALID_STATE`.
- Core remains the only layer that calls `modem_socket_*()`.

## Protocol Event Routing

Protocol data and closed events are generalized at Modem and Core boundaries.

Modem protocol types:

```c
typedef enum {
    MODEM_PROTOCOL_MQTT = 0,
    MODEM_PROTOCOL_TCP,
} modem_protocol_t;
```

Modem protocol data:

```c
typedef struct {
    modem_protocol_t protocol;
    uint8_t conn_id;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
    int reason;
    int modem_error_code;
} modem_protocol_data_t;
```

Core mirrors the same protocol and connection routing fields:

```c
typedef enum {
    CORE_PROTOCOL_MQTT = 0,
    CORE_PROTOCOL_TCP,
} core_protocol_t;

typedef struct {
    core_protocol_t protocol;
    uint8_t conn_id;
    const char *topic;
    size_t topic_len;
    const uint8_t *payload;
    size_t payload_len;
    int reason;
    int modem_error_code;
} core_protocol_data_t;
```

Closed events must carry protocol identity and connection id. `MODEM_EVENT_PROTOCOL_CLOSED` and Core closed callback can no longer hard-code MQTT.

Core protocol callback registration changes from one global slot to protocol-aware registration. The exact structure can be an array keyed by `core_protocol_t` because the number of protocols is small:

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

Rules:

- MQTT service registers only `CORE_PROTOCOL_MQTT`.
- TCP service registers only `CORE_PROTOCOL_TCP`.
- Registering TCP must not overwrite MQTT callbacks, and registering MQTT must not overwrite TCP callbacks.
- Callback execution remains synchronous on the Core FSM path and must remain lightweight.

## Modem Adapter Socket API

Modem adds semantic socket types and wrappers:

```c
typedef enum {
    MODEM_SOCKET_PROTO_TCP = 0,
} modem_socket_proto_t;

typedef struct {
    modem_socket_proto_t proto;
    uint8_t conn_id;
    const char *host;
    uint16_t port;
    uint32_t timeout_ms;
} modem_socket_open_t;

typedef struct {
    uint8_t conn_id;
    const uint8_t *data;
    size_t len;
    uint32_t timeout_ms;
} modem_socket_send_t;

typedef struct {
    uint8_t conn_id;
    size_t max_len;
} modem_socket_recv_t;

typedef struct {
    uint8_t conn_id;
    uint8_t *payload;
    size_t payload_len;
    size_t remaining_len;
    int modem_error_code;
} modem_socket_recv_result_t;

typedef struct {
    uint8_t conn_id;
    uint32_t timeout_ms;
} modem_socket_close_t;

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

`modem_ops_t` gains `socket_open`, `socket_send`, `socket_recv`, and `socket_close` methods. Unsupported modules return `ESP_ERR_NOT_SUPPORTED`.

## Air780EP Mapping

Air780EP v1 uses plain TCP single-connection non-transparent mode.

Setup before opening:

- `AT+CIPMUX=0`
- `AT+CIPMODE=0`
- `AT+CIPQSEND=1`
- `AT+CIPRXF=1`
- Enable manual receive notifications with `AT+CIPRXGET=5` so payload does not arrive as naked AT text.

Open:

```text
AT+CIPSTART="TCP","<host>",<port>
```

Success accepts:

- `CONNECT OK`
- `ALREADY CONNECT`

Failure handles:

- `CONNECT FAIL`
- `TCP ERROR:<err>`
- `+CME ERROR:<err>`
- timeout

The connection result budget is `open_timeout_ms`, default `75000`.

Send:

```text
AT+CIPSEND=<len>
> <raw bytes>
DATAACCEPT:<len> or DATA ACCEPT:<len> or SEND OK
```

The implementation uses `at_engine_send_cmd_with_payload()` so the payload is written as raw bytes and no C-string processing is applied. The send result budget is `send_timeout_ms`, default `50000`.

Receive:

- A `+CIPRXGET:1` notification means data is available.
- The URC handler posts a lightweight event; it does not call AT Engine commands.
- TCP/Core later submits `modem_socket_recv()`.
- Air780EP v1 uses HEX read mode: `AT+CIPRXGET=3,<read_len>`.
- `read_len` is `min(max_rx_event_len, 730)` because Air780EP HEX read accepts at most 730 raw bytes.
- The Modem implementation decodes the returned HEX text into a heap buffer before returning DATA.

Close:

```text
AT+CIPCLOSE
```

Success accepts `CLOSE OK`. Remote close (`CLOSED`) and `TCP ERROR:<err>` produce protocol closed/error events with `MODEM_PROTOCOL_TCP`.

## ML307R Mapping

ML307R v1 uses `connect_id=0` and TCP stream cache mode.

Setup before opening:

- Bind `connect_id=0` to the active primary CID: `AT+MIPCFG="cid",0,<cid>`.
- Configure raw TX and HEX RX for binary-safe receive parsing: `AT+MIPCFG="encoding",0,0,1`.
- Configure abnormal disconnect auto-release: `AT+MIPCFG="autofree",0,0`.
- Open with access mode `2` for TCP stream cache.

Open:

```text
AT+MIPOPEN=0,"TCP","<host>",<port>,<timeout>,2
```

Success is `+MIPOPEN: 0,0`. Non-zero result maps to a standard `esp_err_t` and preserves the raw result code in event data.

Send:

```text
AT+MIPSEND=0,<len>
> <raw bytes>
+MIPSEND: 0,<len>
```

The implementation uses `at_engine_send_cmd_with_payload()` for raw bytes. `+MIPSEND` means the data entered the module protocol stack cache, not that the remote peer ACKed it.

Receive:

- Cache notification: `+MIPURC: "rtcp",0,<recv_len>,<total_len>`.
- The URC handler posts a lightweight readable event.
- TCP/Core later submits `modem_socket_recv()`.
- `modem_socket_recv()` executes `AT+MIPRD=0,<read_len>` with `read_len <= max_rx_event_len`.
- Because ML307R RX encoding is HEX, the Modem implementation decodes the returned HEX text into a heap buffer before returning DATA.
- Returned payload is heap-owned and binary-safe.

Close:

```text
AT+MIPCLOSE=0
```

Success is `+MIPCLOSE: 0` or `+MIPCLOSE: 0,<ret_code>`. Runtime disconnect is `+MIPURC: "disconn",0,<connect_state>` and maps to TCP disconnected/error without reconnecting.

## AT Engine Impact

TX reuses existing `at_engine_send_cmd_with_payload()`.

RX v1 deliberately avoids arbitrary raw payload parsing in AT Engine. Both supported modules use cached/manual receive plus HEX output, so socket payload bytes are represented as printable ASCII HEX inside normal AT response lines and decoded in Modem code.

The implementation must ensure the AT line buffer can hold the longest HEX response line it requests. Each `modem_socket_recv()` computes `read_len` as the minimum of the TCP `max_rx_event_len`, the module receive limit, and the effective AT Engine response-line capacity. The first implementation keeps Air780EP RX chunks at or below 730 raw bytes and sets TCP examples' `base.at_engine.rx_line_buf_size` to at least `2048`.

## Error Handling

API errors:

| Situation | Return |
|-----------|--------|
| Null argument or invalid field | `ESP_ERR_INVALID_ARG` |
| TCP service not initialized | `ESP_ERR_INVALID_STATE` |
| Network not online at `open` | `ESP_ERR_INVALID_STATE` |
| Connection not connected at `send` | `ESP_ERR_INVALID_STATE` |
| Unsupported config, such as `max_conns > 1` | `ESP_ERR_NOT_SUPPORTED` |
| Send FIFO full | `ESP_ERR_TIMEOUT` |
| Allocation failure | `ESP_ERR_NO_MEM` |

Runtime events:

- Remote normal close posts `LWLTE_TCP_EVENT_DISCONNECTED`.
- Module/network/protocol errors post `LWLTE_TCP_EVENT_ERROR` with `error_code`, `modem_error_code`, and `reason`.
- Network offline clears pending sends and posts a connection loss event.
- No automatic reconnect is attempted.
- Malformed length-delimited RX data maps to `ESP_ERR_INVALID_RESPONSE` and an ERROR event.

Partial send semantics:

- v1 reports `SENT` when the module accepts the submitted bytes into its TCP/IP stack.
- v1 does not report remote TCP ACK completion.
- If the module reports send failure, the corresponding FIFO item completes with ERROR and the connection transitions to `ERROR`.

## Threading And Lifetime

Threading model:

```text
App task
  -> lwlte_tcp_open/send/close
       -> TCP service queue

TCP FSM task
  -> serial connection/send/recv/close state handling
       -> core_submit_cmd()

Core FSM task
  -> modem_socket_*()
       -> AT Engine command path

AT Engine RX task
  -> idle URC prefix dispatch
       -> Modem event queue
            -> Core protocol callback
                 -> TCP service FSM signal
```

Lifetime rules:

- Facade destroys TCP service before Core.
- TCP service unregisters Core protocol callbacks before releasing its object.
- Connection objects are explicitly destroyed by the user.
- `lwlte_tcp_conn_destroy()` is valid after `CLOSED` or `ERROR`.
- Destroying a connection while it is `CONNECTING`, `CONNECTED`, or `CLOSING` returns `ESP_ERR_INVALID_STATE` in v1; the application closes first or waits for completion/error.
- `lwlte_tcp_destroy()` closes/stops the service only when no active connection object remains, or it force-cleans only as part of `lwlte_destroy()` with documented best-effort cleanup.

## Examples

Add two example files:

- `example/air780ep_tcp_client.c`
- `example/ml307r_tcp_client.c`

Both examples use the unified example entry pattern:

1. Initialize the selected module facade.
2. Register `LWLTE_EVENT` and `LWLTE_TCP_EVENT` handlers.
3. Call `lwlte_tcp_init()`.
4. Call `lwlte_start()`.
5. On `LWLTE_EVENT_NET_ONLINE`, call `lwlte_tcp_open()`.
6. On `LWLTE_TCP_EVENT_CONNECTED`, send the configured payload.
7. On `LWLTE_TCP_EVENT_SENT`, log sent length.
8. On `LWLTE_TCP_EVENT_DATA`, log length plus text/HEX preview, close the connection, and release the event data before returning.
9. On disconnect/error, destroy the connection object, stop LTE, and destroy the TCP service.

Kconfig adds TCP example settings:

- TCP host
- TCP port
- TCP payload string
- Optional TCP payload HEX string; when non-empty, the example decodes it and sends those bytes instead of the text payload
- Open timeout
- Send queue size
- Max TX length
- Max RX event length

## Verification

Required verification:

- ESP-IDF build passes for the selected target.
- Air780EP TCP example builds.
- ML307R TCP example builds.

Real-device verification when hardware and a TCP echo server are available:

- Air780EP log shows `NET_ONLINE -> TCP CONNECTED -> TCP SENT -> TCP DATA -> TCP DISCONNECTED`.
- ML307R log shows `NET_ONLINE -> TCP CONNECTED -> TCP SENT -> TCP DATA -> TCP DISCONNECTED`.
- Text payload round-trip length matches the sent length.
- If the echo server supports binary payloads, a payload containing `0x00` and `\r\n` round-trips with identical length and bytes.

## Documentation Updates

After implementation, update project docs consistently:

- `docs/agents/feature-roadmap.md`: mark TCP/UDP Socket status appropriately for TCP client v1 and note UDP remains planned.
- `docs/agents/architecture.md`: replace future TCP boundary notes with the `tcp_client -> Core -> Modem` boundary.
- `docs/agents/classes.md`: add TCP Client Service and socket command/value-object sections.
- `docs/agents/at_cmd_air780ep.md`: mark the chosen Air780EP TCP RX/TX path as implemented.
- `docs/agents/at_cmd_ml307r.md`: mark the chosen ML307R TCP RX/TX path as implemented.
- `docs/agents/directory-structure.md`: add `src/tcp_client/` if the implementation introduces that directory.

## Implementation Notes

- Keep the first implementation small: one connection, one TCP protocol, no TLS fields.
- Use `conn_id=0` internally for v1, but route events through `conn_id` so multi-connection support has a stable path later.
- Do not expose `conn_id` in the public TCP API; public code uses `lwlte_tcp_conn_t *`.
- Do not add backward compatibility shims for old callback signatures; update existing MQTT/Core call sites in the same refactor.
- Preserve existing MQTT behavior while changing Core protocol callback registration.
- Use cache/manual RX modes for v1; direct URC payload modes are out of scope.
