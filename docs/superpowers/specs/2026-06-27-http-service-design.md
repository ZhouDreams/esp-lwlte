# HTTP Service v1 设计

## 1. 概述

为 esp-lwlte 增加 HTTP/HTTPS 请求能力。v1 采用 **Core 同步命令**范式（类比 `lwlte_ping()` / SSL provisioning），暴露单一同步 API `lwlte_http_request()`，支持 GET / POST、明文 HTTP 与 HTTPS，返回 HTTP 状态码与完整响应体。

本设计对齐 [功能 Roadmap](../../agents/feature-roadmap.md)「P4 HTTP/HTTPS —— Core 同步命令（v1）」。

### 目标

- 同步 `lwlte_http_request()`：阻塞调用线程，直到收到 HTTP 响应或超时。
- 支持 `GET` / `POST` 方法。
- 支持 `HTTP`（明文）与 `HTTPS`（TLS，引用已 provision 的 SSL context）。
- 返回 HTTP 状态码、响应体（库分配堆 buffer）、模块原始错误码。
- Air780EP + ML307R 双模块实现。

### 非目标（v1 不做）

- 独立 `http_client` service、自有 FSM、事件 base。
- 流式响应 / 大文件分块下载到文件系统。
- 多请求并发或会话复用（每次请求独立 init→term）。
- 自定义多 header（`USERDATA` / `MHTTPHEADER` 多行），v1 仅支持单个 `content_type`。
- `HEAD` / `PUT` / `DELETE` 等其他方法。
- 严格二进制安全的响应体读取（见「已知限制」）。
- 实机验证（本轮仅编译验证，实机验证由用户后续触发）。

---

## 2. 架构边界与编排模型

### 归属

HTTP v1 作为 **Core 同步命令**，不新建独立 service、不引入新 FSM 或事件 base。与 Roadmap「P4 HTTP/HTTPS —— Core 同步命令（v1）」一致。

### 跨层调用链

```
App:  lwlte_http_request(lte, &req, &resp)          // 阻塞
        │
Facade: 构造 CORE_CMD_HTTP_REQUEST，submit 后等 done sema
        │
Core:  FSM 串行执行，调 modem_http_request(modem, &req, &resp)
        │
Modem: Air780EP/ML307R 完成完整 HTTP 会话（init→para→[data]→action→read→term）
        │
AT Engine: at_engine_send_cmd[_with_options]()
```

### Modem ops 粒度决策

Roadmap 列了 6 个细 ops（`http_create` / `http_set_param` / `http_set_content` / `http_request` / `http_read` / `http_term`），但 v1 采用**单一粗粒度 `http_request` ops**，在 modem 实现内部串行完成完整 HTTP 会话。理由：

- HTTP 是无状态请求-响应模型，没有跨命令的会话状态需要 Core 维护（与 MQTT 长会话不同）。
- Core command queue 已经是串行化层，再在 Core FSM 里编排 6 个子命令会让 HTTP 命令长时间占用 FSM 且中间态难管理。
- 失败时 modem 实现内部统一 `HTTPTERM` / `MHTTPDEL` 清理，Core 无需感知会话半状态。

细分 ops 留给后续「独立 http_client service + 流式/多请求」阶段再拆。

### 模块覆盖

Air780EP + ML307R 双模块（与 TCP / MQTT / SSL 现状一致）。ML307R 是实例模型（`MHTTPCREATE` 返回 httpid），v1 局部持有 httpid，不跨请求缓存，不暴露给上层。

### 串行化

v1 单请求无并发，由 Core command queue 自然串行；`lwlte_http_request()` 阻塞调用线程直到完成或超时，不应在事件回调中调用。

---

## 3. 公开门面 API（`src/include/lwlte.h`）

### 类型定义

```c
typedef enum {
    LWLTE_HTTP_METHOD_GET = 0,   /**< GET 方法； GET method */
    LWLTE_HTTP_METHOD_POST,      /**< POST 方法； POST method */
} lwlte_http_method_t;

typedef enum {
    LWLTE_HTTP_TRANSPORT_HTTP = 0,   /**< 明文 HTTP； Plain HTTP */
    LWLTE_HTTP_TRANSPORT_HTTPS,      /**< HTTPS (TLS)； HTTPS over TLS */
} lwlte_http_transport_t;
```

### 请求结构

调用方构造，字符串 / 缓冲区为借用，函数返回前必须保持有效：

```c
typedef struct {
    lwlte_http_method_t method;       /**< HTTP 方法； HTTP method */
    const char *url;                  /**< 完整 URL，含 http(s)://； Full URL */
    lwlte_http_transport_t transport; /**< 传输类型； Transport type */
    uint8_t ssl_context_id;           /**< HTTPS 引用的 SSL context ID； SSL context for HTTPS */
    const char *content_type;         /**< POST content-type，可空； POST content-type, optional */
    const uint8_t *body;              /**< POST 请求体； POST body */
    size_t body_len;                  /**< POST 请求体长度； POST body length */
    uint32_t timeout_ms;              /**< 总超时，0 用默认； Total timeout, 0=default */
} lwlte_http_request_t;
```

### 响应结构

body 由库按服务器响应实际大小 malloc，应用用完后调 `lwlte_http_response_release()` 释放。与 MQTT_DATA / TCP_DATA 的库分配 + release 模型一致。

```c
typedef struct {
    int status_code;            /**< HTTP 状态码，如 200； HTTP status code */
    uint8_t *body;              /**< 库分配的响应体，须由 lwlte_http_response_release() 释放；
                                     Library-allocated body, free via lwlte_http_response_release() */
    size_t body_len;            /**< 响应体字节数； Body length in bytes */
    int modem_error_code;       /**< 模块原始错误码； Raw modem error code */
} lwlte_http_response_t;
```

### 释放函数

```c
void lwlte_http_response_release(lwlte_http_response_t *response);
```

释放 body 并置 NULL（`free(NULL)` 安全，无 body 或已释放时也是 no-op）。同步单消费者场景，不需要 `owns_payload` 标志（与 MQTT / TCP 多 handler 模型不同）。

### 请求函数

```c
esp_err_t lwlte_http_request(lwlte_handle_t me,
                             const lwlte_http_request_t *request,
                             lwlte_http_response_t *response);
```

### 关键语义

- **阻塞**调用，直到收到 HTTP 响应或超时；不应在事件回调中调用。
- **返回值**：transport 层成功收到 HTTP 响应即返回 `ESP_OK`（此时 `status_code` 有效）；HTTP 4xx / 5xx **仍返回** `ESP_OK`，应用自行判断 `status_code`。网络 / 模块 / 超时错误返回对应 `esp_err_t`。
- **body 所有权**：成功时 `body` 为库分配堆 buffer（可能为 NULL，如 0 长度响应），应用须调 `lwlte_http_response_release()` 释放；失败时 `body` 恒为 NULL，调用 release 也是安全 no-op。响应体大小受可用堆内存限制，malloc 失败返回 `ESP_ERR_NO_MEM`。
- **response 零初始化**：facade 在入口主动将 `response->body = NULL`、`body_len = 0`、`status_code = 0`、`modem_error_code = 0`，保证任何失败返回路径 release 都安全；调用方无需自行初始化。
- **HTTPS**：`transport=HTTPS` 时使用 `ssl_context_id`，该 context 须已由 `lwlte_ssl_provision()` 配置。Air780EP 内部固定 SSL context 153，该字段值被忽略；ML307R 映射到自身 `ssl_id`。
- **校验**：`url` 必填且非空；GET 时 `body` 应为 NULL；POST 时 `body` 非空且 `body_len > 0`。

---

## 4. Core 层命令（`src/core/`）

### 新增命令类型

`CORE_CMD_HTTP_REQUEST`，追加在 `CORE_CMD_SOCKET_CLOSE` 之后；`core_cmd_type_t` 上限与 `core_cmd_type_valid()` 同步更新。

### 命令输入参数

`core_cmd_t.data.http_request`，clone 时深拷贝字符串 / 缓冲：

```c
struct {
    lwlte_http_method_t method;
    const char *url;                    /* 深拷贝 */
    lwlte_http_transport_t transport;
    uint8_t ssl_context_id;
    const char *content_type;           /* 深拷贝，可空 */
    const uint8_t *body;                /* 深拷贝，POST 请求体 */
    size_t body_len;
} http_request;
```

超时复用 `cmd->timeout_ms`。

### 结果结构

`core_http_result_t`，通过 `finish_service_cmd` 的 `result_data` 传递，body 零拷贝转移：

```c
typedef struct {
    esp_err_t error_code;     /**< ESP 错误码 */
    int status_code;          /**< HTTP 状态码，成功时有效 */
    uint8_t *body;            /**< 堆 body，所有权转移给最终应用 release */
    size_t body_len;
    int modem_error_code;
} core_http_result_t;
```

### body 所有权链（零拷贝，全程只 malloc 一次）

```
modem_http_request() 分配 body
  → modem_http_response_t.body
  → core 组装进 core_http_result_t.body（指针转移，不拷贝）
  → finish_service_cmd 同步调 done_cb，result_data = &core_http_result_t
  → facade HTTP 专用 done_cb 把 body 指针转移到调用方 ctx
  → lwlte_http_request() 把 ctx.body 放入 lwlte_http_response_t.body
  → 应用调 lwlte_http_response_release() 释放
```

### 关键约束

- `modem_http_request()` 返回非 `ESP_OK` 时，`modem_http_response_t.body` **必须为 NULL**（modem 实现内部已 free 任何已分配 body）。这样 core 失败路径无需处理 body 释放。
- 成功（`ESP_OK`）时 body 可非 NULL（有响应体）或 NULL（0 长度，如 204 No Content）。
- facade 为 HTTP 使用**专用 done_cb**（`facade_http_cmd_done_cb`），不复用通用 `facade_core_cmd_done_cb`（后者假设 `result_data` 是 `esp_err_t *`）。

### core_fsm `handle_service_cmd` HTTP 分支

类比 socket 分支，前置 `ensure_net_online`：

```c
case CORE_CMD_HTTP_REQUEST: {
    core_http_result_t result = {0};
    esp_err_t http_ret = ensure_net_online(me);
    if (http_ret == ESP_OK) {
        modem_http_request_t req = { /* 映射 cmd 参数 */ };
        modem_http_response_t modem_resp = {0};
        http_ret = modem_http_request(me->modem, &req, &modem_resp);
        result.status_code = modem_resp.status_code;
        result.body = modem_resp.body;        /* 转移 */
        result.body_len = modem_resp.body_len;
    }
    result.error_code = http_ret;
    finish_service_cmd(me, cmd, result_from_esp_err(http_ret), &result);
    return;  /* body 已在 done_cb 转移；失败时 body==NULL（modem 约束） */
}
```

### clone / free / valid

- `clone_core_cmd`：深拷贝 `url` / `content_type` / `body`（POST 请求体），任一分配失败释放已分配并返回 NULL。
- `free_core_cmd`：释放上述三个副本。注意此处是**请求体**副本，不是 response body。
- `core_cmd_valid`：`url` 非空；`method` / `transport` 合法；POST 时 `body` 非空且 `body_len > 0`。

---

## 5. Modem 层 ops 与数据结构（`src/modem/`）

### Modem 层类型

modem 层自有枚举，core 做映射，与 MQTT transport 模式一致：

```c
typedef enum {
    MODEM_HTTP_METHOD_GET = 0,
    MODEM_HTTP_METHOD_POST,
} modem_http_method_t;

typedef enum {
    MODEM_HTTP_TRANSPORT_HTTP = 0,
    MODEM_HTTP_TRANSPORT_HTTPS,
} modem_http_transport_t;

typedef struct {
    modem_http_method_t method;
    const char *url;
    modem_http_transport_t transport;
    uint8_t ssl_context_id;
    const char *content_type;       /* 可空 */
    const uint8_t *body;            /* POST 请求体 */
    size_t body_len;
    uint32_t timeout_ms;
    int *modem_error_code;          /* 输出，可空 */
} modem_http_request_t;

typedef struct {
    int status_code;
    uint8_t *body;                  /* 堆分配，成功时调用方拥有；失败时必须为 NULL */
    size_t body_len;
} modem_http_response_t;
```

### ops 新增

`modem_ops_t` 追加函数指针：

```c
esp_err_t (*http_request)(modem_handle_t me,
                          const modem_http_request_t *request,
                          modem_http_response_t *response);
```

`modem.h` 增加公共包装 `modem_http_request()`，做参数 / 状态校验（`check_ready`）后转发 `me->ops->http_request(me, ...)`，与现有 socket / mqtt 包装一致。模块未实现时返回 `ESP_ERR_NOT_SUPPORTED`。

### 子类结构体

v1 不在 `modem_air780ep_t` / `modem_ml307r_t` 缓存 HTTP 会话状态（每次请求独立 init→term），无需新增持久字段。

---

## 6. Air780EP 实现要点（`src/modem/modem_air780ep.c`）

完整会话在 `air780ep_http_request()` 内串行，`HTTPTERM` 在 finally 路径无条件清理：

1. `AT+HTTPINIT`
2. HTTPS：`AT+HTTPSSL=1`（SSL 证书已由 P3 经 `SSLCFG` 写入 context 153，`ssl_context_id` 字段被忽略）；HTTP：`AT+HTTPSSL=0`
3. `AT+HTTPPARA="CID",1`
4. `AT+HTTPPARA="URL","<url>"`
5. `content_type` 非空时 `AT+HTTPPARA="CONTENT","<content_type>"`
6. POST 时 `AT+HTTPDATA=<len>,<time>` → `DOWNLOAD` prompt → 写 body（复用现有 prompt + 数据发送模式，与 `CIPSEND` / `MPUBEX` 一致）
7. `AT+HTTPACTION=<method>`：**用 `at_engine_send_cmd_with_options()`**，`success_matches` 配 `+HTTPACTION:` 前缀匹配，`AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` 跳过立即返回的中间 `OK`，阻塞直到收到 `+HTTPACTION: <method>,<status>,<len>` 行或 `timeout_ms` 超时。从中解析 `status_code` 和响应体长度。与 MQTT `MCONNECT` 的 `CONNACK OK` 模式一致。
8. 响应体长度 > 0 时 `AT+HTTPREAD` 读取并组装 body（按 `+HTTPREAD:<len>` 报告长度拼接数据行）
9. `AT+HTTPTERM`（无论成功 / 失败，finally 清理）

### HTTPACTION URC 可靠性

依赖 at_engine 现有约束——命令等待期间的非最终响应行优先归入当前命令响应（见 [classes.md](../../agents/classes.md) §1.6）。这使 `+HTTPACTION:` 在命令等待期被捕获为成功终止行，无需 at_engine 改动。

### HTTPACTION status 分类

- `100..505`：标准 HTTP 状态码，正常返回，填入 `status_code`。
- `600..606`：模块侧错误（601 网络 / 602 内存 / 603 DNS / 604 协议栈忙 / 605 SSL 建立失败 / 606 SSL 通信错误），映射为 `ESP_ERR_INVALID_RESPONSE`，`*modem_error_code = status`，返回非 `ESP_OK`。

---

## 7. ML307R 实现要点（`src/modem/modem_ml307r.c`）

完整会话在 `ml307r_http_request()` 内串行，`MHTTPDEL` 在 finally 路径清理：

1. `AT+MHTTPCREATE` 创建实例，解析返回 `httpid`（v1 局部持有，不跨请求缓存）
2. `AT+MHTTPCFG=<httpid>,...` 配置 URL / method / SSL；HTTPS 时映射 `ssl_context_id` 到 ML307R `ssl_id`
3. `content_type` 非空时 `AT+MHTTPHEADER=<httpid>,"Content-Type:<value>"`
4. POST 时 `AT+MHTTPCONTENT=<httpid>,<len>` 写 body
5. `AT+MHTTPREQUEST=<httpid>`：**用 `at_engine_send_cmd_with_options()`**，把 `+MHTTPURC: "rsp",<httpid>` 作为成功终止匹配，阻塞等结果或超时。解析 `+MHTTPURC: "rsp",<httpid>,<status>,<len>` 得 `status_code` 和长度。错误识别 `+MHTTPURC: "err",<httpid>,<err>`，映射为 `ESP_ERR_INVALID_RESPONSE` 并保留 `err`。
6. 响应体长度 > 0 时 `AT+MHTTPREAD=<httpid>` 读取（可能多次按 len 读）
7. `AT+MHTTPDEL=<httpid>`（finally 清理）

---

## 8. 错误处理与错误码映射

### ESP 错误码总览

| 场景 | 返回 |
|------|------|
| 参数无效 | `ESP_ERR_INVALID_ARG` |
| 网络未 online / 门面 destroying | `ESP_ERR_INVALID_STATE` |
| HTTPACTION / MHTTPURC 超时 | `ESP_ERR_TIMEOUT` |
| 模块 600..606 / CME ERROR / 响应格式无效 | `ESP_ERR_INVALID_RESPONSE`（含 `modem_error_code`） |
| body malloc 失败 | `ESP_ERR_NO_MEM` |
| 模块未实现 http ops | `ESP_ERR_NOT_SUPPORTED` |
| 其他模块错误 | `ESP_FAIL`（含 `modem_error_code`） |

### 失败清理约束（modem 实现）

任一步骤失败时，实现必须按以下顺序清理：

1. 释放已分配的 body（`free` 并置 `response->body = NULL`、`body_len = 0`）
2. `HTTPTERM` / `MHTTPDEL` 清理模块会话
3. 填 `*modem_error_code`（若指针非空）
4. 返回对应 `esp_err_t`（非 `ESP_OK`）

保证 `modem_http_response_t.body` 在非 `ESP_OK` 返回时恒为 NULL。

---

## 9. Facade 装配与生命周期

### 不引入独立 service

HTTP v1 是同步命令：

- `lwlte_handle_t` **不新增** `http` 字段（对比 `mqtt` / `tcp` / `ping` 句柄）。
- **不新增** `lwlte_http_init()` / `lwlte_http_destroy()`。
- `lwlte_http_request()` 直接经 `begin_api_call(require_core=true)` + `core_submit_cmd()`，无装配步骤。
- `lwlte_destroy()` 无需特殊 HTTP 清理（HTTP 无持久资源，每次请求自洽 init/term）。

### 不新增源文件

modem 实现加入现有 `modem_air780ep.c` / `modem_ml307r.c`；core 加入 `core.h` / `core.c` / `core_fsm.c`；facade 加入 `lwlte.h` / `lwlte.c`。`CMakeLists.txt` 不变。

### Facade 实现骨架

```c
esp_err_t lwlte_http_request(lwlte_handle_t me,
                             const lwlte_http_request_t *request,
                             lwlte_http_response_t *response)
{
    /* 参数校验（url 非空、method/transport 合法、POST body 非空） */
    /* response 入口零初始化（body=NULL, body_len=0, status_code=0, modem_error_code=0）
       —— 保证任何失败返回路径 release 都安全 */
    /* begin_api_call(require_core=true) */
    /* 构造 lwlte_http_cmd_ctx_t + done_sema */
    /* 构造 CORE_CMD_HTTP_REQUEST，done_cb = facade_http_cmd_done_cb */
    /* core_submit_cmd(); 成功则 wait done_sema */
    /* 把 ctx 的 status_code/body/body_len/modem_error_code 填入 response */
    /* 释放 done_sema；end_api_call */
}
```

### HTTP 专用 done_cb

`facade_http_cmd_done_cb` 从 `core_http_result_t` 转移 body 指针到 ctx，失败时 body 应为 NULL（modem 约束），不转移。

---

## 10. 已知限制（v1）

### 响应体二进制安全

v1 响应体读取基于 AT Engine 当前行解析模式（按 `\r\n` 分行）。对包含裸 `\r\n` 或可被误判为终止行（如内嵌 `OK\r\n`）的字节序列的响应体，可能截断或错乱。**v1 假设响应体为文本类内容（JSON / XML / 纯文本）**。

严格二进制安全（任意字节）留后续，需 AT Engine 增强按长度读取原始字节的能力（类比 TCP 的 `CIPRXGET` HEX 手动读取模式）。此为 Roadmap「开放问题：响应体读取按长度（可能二进制），不能按行解析」的 v1 取舍。

### 其他限制

- 无自定义多 header（仅 `content_type`）。
- 无请求 / 会话复用（每次独立 init→term）。
- 无流式 / 大文件下载。
- 无 `HEAD` / `PUT` / `DELETE`。
- 响应体大小受可用堆内存限制。

---

## 11. 验证策略

### 本轮验证（编译 + host 契约测试）

- **编译验证**：`idf.py build`（ESP32-C3 target，Air780EP + ML307R 双工程）。
- **host 契约测试**：项目已有 `tests/host/` pytest 契约测试框架（基于 `unittest`，静态源码契约检查，现有 182 测试通过）。新增 `tests/host/test_http_end_to_end_contract.py`，类比 `test_ping_end_to_end_contract.py` / `test_tcp_tls_ssl_socket_contract.py`，静态校验：
  - 公开 API 结构与函数原型（`lwlte.h` 里的 `lwlte_http_request_t` / `lwlte_http_response_t` / `lwlte_http_request()` / `lwlte_http_response_release()` 字段与签名）。
  - Core 命令串接（`CORE_CMD_HTTP_REQUEST` 枚举、`core_cmd_t.data.http_request`、`clone_core_cmd` / `free_core_cmd` / `core_cmd_valid` 的 HTTP 分支、`core_fsm.c` 的 HTTP 映射）。
  - Modem ops 注册（`modem_ops_t.http_request`、`modem_http_request()` 包装、Air780EP / ML307R ops 表注册）。
  - AT 命令字符串契约（Air780EP `AT+HTTPINIT` / `AT+HTTPPARA` / `AT+HTTPACTION` / `AT+HTTPREAD` / `AT+HTTPTERM`；ML307R `AT+MHTTPCREATE` / `AT+MHTTPCFG` / `AT+MHTTPREQUEST` / `AT+MHTTPREAD` / `AT+MHTTPDEL`）。
  - 运行：`pytest tests/host/`，确认新测试通过且不破坏现有测试。

### 后续实机验证（由用户触发）

实机验证本轮不做，届时再执行：

- GET 明文（200 正常响应，校验 body 完整性）
- GET HTTPS（引用已 provision 的 SSL context，校验 TLS 握手 + 响应）
- GET 404（校验返回 `ESP_OK` + `status_code=404`）
- POST 明文（带 content_type + body，校验请求体送达）
- 超时场景（无效 host，校验 `ESP_ERR_TIMEOUT`）
- release 无泄漏（多次请求后堆内存稳定）

---

## 12. 落地范围汇总

| 层 | 文件 | 改动 |
|----|------|------|
| Facade 公开 API | `src/include/lwlte.h` | 新增 `lwlte_http_method_t` / `lwlte_http_transport_t` / `lwlte_http_request_t` / `lwlte_http_response_t` / `lwlte_http_request()` / `lwlte_http_response_release()` |
| Facade 实现 | `src/lwlte/lwlte.c` | `lwlte_http_request()` + `lwlte_http_response_release()` + `facade_http_cmd_done_cb` + `lwlte_http_cmd_ctx_t` |
| Core 类型 | `src/core/core.h` | 新增 `CORE_CMD_HTTP_REQUEST` / `core_http_result_t` / `core_cmd_t.data.http_request` |
| Core 实现 | `src/core/core.c` | `clone_core_cmd` / `free_core_cmd` / `core_cmd_valid` / `core_cmd_type_valid` HTTP 分支 |
| Core FSM | `src/core/core_fsm.c` | `handle_service_cmd` HTTP 分支 |
| Modem 类型 / 包装 | `src/modem/modem.h` / `modem.c` | `modem_http_method_t` / `modem_http_transport_t` / `modem_http_request_t` / `modem_http_response_t` / `modem_ops_t.http_request` / `modem_http_request()` |
| Air780EP 实现 | `src/modem/modem_air780ep.c` / `modem_priv.h` | `air780ep_http_request()` + ops 表注册 |
| ML307R 实现 | `src/modem/modem_ml307r.c` / `modem_priv.h` | `ml307r_http_request()` + ops 表注册 |
| Host 契约测试 | `tests/host/test_http_end_to_end_contract.py` | 新增：公开 API 结构 / Core 命令串接 / modem ops / AT 命令字符串静态契约 |

不新增 C 源文件（`CMakeLists.txt` 不变）；新增一个 Python host 契约测试文件。
