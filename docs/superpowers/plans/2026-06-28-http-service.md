# HTTP Service v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Commit policy:** Per AGENTS.md, never auto-commit. Each task's commit step requires explicit user authorization before running `git commit`.

**Goal:** Add synchronous HTTP/HTTPS request capability (`lwlte_http_request()`) supporting GET/POST over plain HTTP and HTTPS, returning status code and library-allocated response body.

**Architecture:** Core synchronous command (`CORE_CMD_HTTP_REQUEST`), analogous to `lwlte_ping()` / SSL provisioning. Single coarse-grained modem ops `http_request` runs the full HTTP session (init→para→[data]→action→read→term) inside the modem implementation. Body is malloc'd once in modem layer, transferred zero-copy through core to facade, released by application via `lwlte_http_response_release()`.

**Tech Stack:** ESP-IDF (ESP32-C3), FreeRTOS, C99, Python unittest host contract tests.

**Spec:** `docs/superpowers/specs/2026-06-27-http-service-design.md`

---

## File Structure

| File | Responsibility | Action |
|------|---------------|--------|
| `src/modem/modem.h` | Public modem types + `modem_http_request()` wrapper prototype | Modify |
| `src/modem/modem_priv.h` | `modem_ops_t.http_request` field + `modem_http_request_fn` typedef | Modify |
| `src/modem/modem.c` | `modem_http_request()` wrapper implementation | Modify |
| `src/core/core.h` | `CORE_CMD_HTTP_REQUEST` + `core_http_result_t` + `core_cmd_t.data.http_request` | Modify |
| `src/core/core.c` | clone/free/valid HTTP branches + `core_cmd_type_valid` upper bound | Modify |
| `src/core/core_fsm.c` | `handle_service_cmd` HTTP branch | Modify |
| `src/modem/modem_air780ep.c` | `air780ep_http_request()` + forward decl + ops registration | Modify |
| `src/modem/modem_ml307r.c` | `ml307r_http_request()` + forward decl + ops registration | Modify |
| `src/include/lwlte.h` | Public HTTP types + `lwlte_http_request()` / `lwlte_http_response_release()` prototypes | Modify |
| `src/lwlte/lwlte.c` | `lwlte_http_request()` + release + `facade_http_cmd_done_cb` + ctx typedef | Modify |
| `tests/host/test_http_end_to_end_contract.py` | Static contract tests across all layers | Create |

No new C source files; `CMakeLists.txt` unchanged. One new Python test file.

---

## Task 1: Modem Layer Type Declarations

**Files:**
- Modify: `src/modem/modem.h` (after socket types, before event types)
- Modify: `src/modem/modem_priv.h` (after `modem_socket_close_fn`, inside `modem_ops_t`)

- [ ] **Step 1: Add HTTP enums and request/response structs to `modem.h`**

Insert after the `modem_socket_close_t` typedef block (before the `modem_event_id_t` enum), in the TYPEDEFS section:

```c
/**
 * @brief HTTP 方法
 * @details HTTP method
 */
typedef enum {
    MODEM_HTTP_METHOD_GET = 0,      /**< GET 方法； GET method */
    MODEM_HTTP_METHOD_POST,         /**< POST 方法； POST method */
} modem_http_method_t;

/**
 * @brief HTTP 传输类型
 * @details HTTP transport type
 */
typedef enum {
    MODEM_HTTP_TRANSPORT_HTTP = 0,  /**< 明文 HTTP； Plain HTTP */
    MODEM_HTTP_TRANSPORT_HTTPS,     /**< HTTPS (TLS)； HTTPS over TLS */
} modem_http_transport_t;

/**
 * @brief HTTP 请求参数
 * @details HTTP request parameters
 */
typedef struct {
    modem_http_method_t method;         /**< HTTP 方法； HTTP method */
    const char *url;                    /**< 完整 URL； Full URL */
    modem_http_transport_t transport;   /**< 传输类型； Transport type */
    uint8_t ssl_context_id;             /**< HTTPS SSL context ID； SSL context for HTTPS */
    const char *content_type;           /**< POST content-type，可空； POST content-type, optional */
    const uint8_t *body;                /**< POST 请求体； POST body */
    size_t body_len;                    /**< POST 请求体长度； POST body length */
    uint32_t timeout_ms;                /**< 总超时； Total timeout */
    int *modem_error_code;              /**< 模块错误码输出，可空； Raw modem error code output, optional */
} modem_http_request_t;

/**
 * @brief HTTP 响应结果
 * @details HTTP response result
 * @note body 成功时为堆分配，调用方拥有；失败时必须为 NULL。
 */
typedef struct {
    int status_code;                    /**< HTTP 状态码； HTTP status code */
    uint8_t *body;                      /**< 堆 body，成功时调用方拥有； Heap body, caller owns on success */
    size_t body_len;                    /**< 响应体长度； Body length */
} modem_http_response_t;
```

- [ ] **Step 2: Add `modem_http_request_fn` typedef and ops field to `modem_priv.h`**

Add the typedef after `modem_socket_close_fn` (before the `modem_ops_t` definition):

```c
/**
 * @brief HTTP 请求操作函数
 * @details HTTP request operation function
 */
typedef esp_err_t (*modem_http_request_fn)(modem_handle_t me,
                                           const modem_http_request_t *request,
                                           modem_http_response_t *response);
```

Add the field to `modem_ops_t`, after the `ping` field in the diagnostics section:

```c
    /* ── 诊断； Diagnostics ──────────────────────────────── */
    modem_ping_fn ping;                              /**< 执行 Ping 诊断； Execute ping diagnostic */

    /* ── HTTP 客户端； HTTP client ───────────────────────── */
    modem_http_request_fn http_request;              /**< 执行 HTTP 请求； Execute HTTP request */
```

- [ ] **Step 3: Add `modem_http_request()` prototype to `modem.h`**

Add after `modem_ping()` prototype (in GLOBAL PROTOTYPES):

```c
/**
 * @brief 执行 HTTP 请求
 * @details Execute HTTP request
 * @param[in] me 调制解调器句柄
 * @param[in] request HTTP 请求参数
 * @param[out] response HTTP 响应结果，成功时 body 为堆分配由调用方释放
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 请求超时
 *         - ESP_ERR_INVALID_RESPONSE: 响应无效
 *         - 其他 esp_err_t: 下层错误
 */
esp_err_t modem_http_request(modem_handle_t me,
                             const modem_http_request_t *request,
                             modem_http_response_t *response);
```

- [ ] **Step 4: Verify build compiles (ops tables auto-NULL the new field)**

Run: `idf.py build`
Expected: PASS (designated initializers leave `.http_request = NULL` automatically; wrapper not yet implemented but prototype matches)

- [ ] **Step 5: Commit (with user authorization)**

```bash
git add src/modem/modem.h src/modem/modem_priv.h
git commit -m "feat(modem): add HTTP request types and ops declaration"
```

---

## Task 2: Modem Layer Public Wrapper

**Files:**
- Modify: `src/modem/modem.c` (after `modem_ping()`, before STATIC FUNCTIONS)

- [ ] **Step 1: Implement `modem_http_request()` wrapper**

Add after the `modem_ping()` function (in GLOBAL FUNCTIONS section), following the exact pattern of `modem_socket_open()` / `modem_ping()`:

```c
esp_err_t modem_http_request(modem_handle_t me,
                             const modem_http_request_t *request,
                             modem_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(me && request && response, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(request->url && request->url[0], ESP_ERR_INVALID_ARG,
                        TAG, "invalid http request url");
    ESP_RETURN_ON_FALSE(request->method == MODEM_HTTP_METHOD_GET ||
                        request->method == MODEM_HTTP_METHOD_POST,
                        ESP_ERR_INVALID_ARG, TAG, "invalid http method");
    ESP_RETURN_ON_FALSE(request->transport == MODEM_HTTP_TRANSPORT_HTTP ||
                        request->transport == MODEM_HTTP_TRANSPORT_HTTPS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid http transport");
    ESP_RETURN_ON_FALSE(request->method != MODEM_HTTP_METHOD_POST ||
                        (request->body && request->body_len > 0),
                        ESP_ERR_INVALID_ARG, TAG, "POST requires non-empty body");

    memset(response, 0, sizeof(*response));
    esp_err_t ret = check_ready(me, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "modem not ready");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->http_request,
                        ESP_ERR_NOT_SUPPORTED, TAG, "http_request not supported");

    return me->ops->http_request(me, request, response);
}
```

- [ ] **Step 2: Verify build**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 3: Commit (with user authorization)**

```bash
git add src/modem/modem.c
git commit -m "feat(modem): add modem_http_request wrapper"
```

---

## Task 3: Core Layer Type Declarations

**Files:**
- Modify: `src/core/core.h`

- [ ] **Step 1: Add `CORE_CMD_HTTP_REQUEST` to `core_cmd_type_t`**

In the `core_cmd_type_t` enum, after `CORE_CMD_SOCKET_CLOSE`:

```c
    CORE_CMD_SOCKET_CLOSE,               /**< 关闭 socket； Close socket */
    CORE_CMD_HTTP_REQUEST,               /**< 执行 HTTP 请求； Execute HTTP request */
```

- [ ] **Step 2: Add `core_http_result_t`**

Add after the `core_socket_result_t` typedef (before `core_cmd_done_callback_t`):

```c
typedef struct {
    esp_err_t error_code;                /**< ESP 错误码； ESP error code */
    int status_code;                     /**< HTTP 状态码，成功时有效； HTTP status code */
    uint8_t *body;                       /**< 堆 body，所有权转移给应用 release； Heap body, ownership transfers to app */
    size_t body_len;                     /**< 响应体长度； Body length */
    int modem_error_code;                /**< 模块原始错误码； Raw modem error code */
} core_http_result_t;
```

- [ ] **Step 3: Add `http_request` member to `core_cmd_t.data` union**

Add after `core_socket_close_t socket_close;` in the union:

```c
        core_socket_close_t socket_close; /**< Socket 关闭参数； Socket close args */
        struct {
            lwlte_http_method_t method;   /**< HTTP 方法； HTTP method */
            const char *url;              /**< URL，clone 深拷贝； URL, deep-copied */
            lwlte_http_transport_t transport; /**< 传输类型； Transport */
            uint8_t ssl_context_id;       /**< SSL context ID； SSL context ID */
            const char *content_type;     /**< content-type，clone 深拷贝； content-type, deep-copied */
            const uint8_t *body;          /**< POST 请求体，clone 深拷贝； POST body, deep-copied */
            size_t body_len;              /**< 请求体长度； Body length */
        } http_request;                   /**< HTTP 请求参数； HTTP request args */
```

- [ ] **Step 4: Verify build**

Run: `idf.py build`
Expected: PASS (clone/free/valid not yet handle the new type but union member exists)

- [ ] **Step 5: Commit (with user authorization)**

```bash
git add src/core/core.h
git commit -m "feat(core): add CORE_CMD_HTTP_REQUEST and core_http_result_t"
```

---

## Task 4: Core Layer Implementation

**Files:**
- Modify: `src/core/core.c` (clone_core_cmd, free_core_cmd, core_cmd_valid, core_cmd_type_valid)
- Modify: `src/core/core_fsm.c` (handle_service_cmd)

- [ ] **Step 1: Update `core_cmd_type_valid` upper bound in `core.c`**

```c
static bool core_cmd_type_valid(core_cmd_type_t type)
{
    return type >= CORE_CMD_SSL_PROVISION && type <= CORE_CMD_HTTP_REQUEST;
}
```

- [ ] **Step 2: Add HTTP validation case to `core_cmd_valid` in `core.c`**

Add before the `default:` case:

```c
    case CORE_CMD_HTTP_REQUEST:
        return cmd->data.http_request.url != NULL &&
               cmd->data.http_request.url[0] != '\0' &&
               (cmd->data.http_request.method == LWLTE_HTTP_METHOD_GET ||
                cmd->data.http_request.method == LWLTE_HTTP_METHOD_POST) &&
               (cmd->data.http_request.transport == LWLTE_HTTP_TRANSPORT_HTTP ||
                cmd->data.http_request.transport == LWLTE_HTTP_TRANSPORT_HTTPS) &&
               (cmd->data.http_request.method != LWLTE_HTTP_METHOD_POST ||
                (cmd->data.http_request.body != NULL &&
                 cmd->data.http_request.body_len > 0));
```

- [ ] **Step 3: Add HTTP clone case to `clone_core_cmd` in `core.c`**

Add before `default:`:

```c
    case CORE_CMD_HTTP_REQUEST:
        clone->data.http_request.url = clone_optional_string(cmd->data.http_request.url);
        clone->data.http_request.content_type =
            clone_optional_string(cmd->data.http_request.content_type);
        clone->data.http_request.body =
            clone_payload(cmd->data.http_request.body, cmd->data.http_request.body_len);
        if (!clone->data.http_request.url ||
            (cmd->data.http_request.content_type && !clone->data.http_request.content_type) ||
            (cmd->data.http_request.body && !clone->data.http_request.body)) {
            free_core_cmd(clone);
            return NULL;
        }
        break;
```

- [ ] **Step 4: Add HTTP free case to `free_core_cmd` in `core.c`**

Add before `default:`:

```c
    case CORE_CMD_HTTP_REQUEST:
        free((void *)cmd->data.http_request.url);
        free((void *)cmd->data.http_request.content_type);
        free((void *)cmd->data.http_request.body);
        break;
```

- [ ] **Step 5: Add HTTP branch to `handle_service_cmd` in `core_fsm.c`**

Add the `CORE_CMD_HTTP_REQUEST` case before `default:`. This follows the socket recv pattern (early return after finish_service_cmd):

```c
    case CORE_CMD_HTTP_REQUEST: {
        core_http_result_t result = {0};
        esp_err_t http_ret = ensure_net_online(me);
        if (http_ret == ESP_OK) {
            modem_http_request_t req = {
                .method = (modem_http_method_t)cmd->data.http_request.method,
                .url = cmd->data.http_request.url,
                .transport = (modem_http_transport_t)cmd->data.http_request.transport,
                .ssl_context_id = cmd->data.http_request.ssl_context_id,
                .content_type = cmd->data.http_request.content_type,
                .body = cmd->data.http_request.body,
                .body_len = cmd->data.http_request.body_len,
                .timeout_ms = cmd->timeout_ms,
                .modem_error_code = &result.modem_error_code,
            };
            modem_http_response_t modem_resp = {0};
            http_ret = modem_http_request(me->modem, &req, &modem_resp);
            result.status_code = modem_resp.status_code;
            result.body = modem_resp.body;
            result.body_len = modem_resp.body_len;
        }
        result.error_code = http_ret;
        finish_service_cmd(me, cmd, result_from_esp_err(http_ret), &result);
        return;
    }
```

Note: The `(modem_http_method_t)` and `(modem_http_transport_t)` casts are needed because `lwlte_http_method_t` and `modem_http_method_t` are distinct enum types with identical values (same pattern as `lwlte_mqtt_transport_t` / `MODEM_MQTT_TRANSPORT_*` in the existing `CORE_CMD_MQTT_CONFIGURE` handler).

- [ ] **Step 6: Verify build**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 7: Commit (with user authorization)**

```bash
git add src/core/core.c src/core/core_fsm.c
git commit -m "feat(core): handle CORE_CMD_HTTP_REQUEST in clone/free/valid/fsm"
```

---

## Task 5: Air780EP HTTP Implementation

**Files:**
- Modify: `src/modem/modem_air780ep.c`

- [ ] **Step 1: Add HTTP defines and forward declaration**

In the DEFINES section (after `AIR780EP_CIPPING_CMD_OVERHEAD_MS`):

```c
#define AIR780EP_HTTP_SSL_CONTEXT_ID    153
#define AIR780EP_HTTP_CMD_TIMEOUT_MS    9000
#define AIR780EP_HTTP_ACTION_TIMEOUT_MS 120000
#define AIR780EP_HTTPDATA_PROMPT_MS     10000
#define AIR780EP_HTTPDATA_BODY_MAX      3356
#define AIR780EP_HTTPREAD_BODY_MAX      3356
```

In STATIC PROTOTYPES, add (after `air780ep_ping` prototype):

```c
/**
 * @brief 执行 Air780EP HTTP 请求
 * @details Execute Air780EP HTTP request
 * @param[in] me 调制解调器句柄
 * @param[in] request HTTP 请求参数
 * @param[out] response HTTP 响应结果
 * @return ESP_OK 成功，其它为错误码
 */
static esp_err_t air780ep_http_request(modem_handle_t me,
                                       const modem_http_request_t *request,
                                       modem_http_response_t *response);
```

- [ ] **Step 2: Implement `air780ep_http_request()`**

Add the implementation before the `s_air780ep_ops` table (in STATIC FUNCTIONS section). This function runs the full HTTP session and uses the existing `send_cmd` / `send_cmd_with_options` / `ensure_at_ok` / `find_line_with_prefix` / `response_contains` helpers:

```c
static esp_err_t air780ep_http_request(modem_handle_t me,
                                       const modem_http_request_t *request,
                                       modem_http_response_t *response)
{
    modem_air780ep_t *self = to_air780ep(me);
    esp_err_t ret = ESP_OK;
    bool http_initialized = false;

    if (request->modem_error_code) {
        *request->modem_error_code = 0;
    }

    /* 1. AT+HTTPINIT */
    air780ep_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    ret = send_cmd(self, "AT+HTTPINIT", &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPINIT send failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = ensure_at_ok(&ctx.response, "AT+HTTPINIT");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPINIT failed");
        return ret;
    }
    http_initialized = true;

    /* 2. SSL toggle + 3-5. parameters; on failure jump to cleanup */
#define HTTP_CLEANUP()                                          \
    do {                                                        \
        if (http_initialized) {                                \
            air780ep_cmd_ctx_t term_ctx;                        \
            init_cmd_ctx(&term_ctx);                            \
            (void)send_cmd(self, "AT+HTTPTERM", &term_ctx,      \
                           AIR780EP_HTTP_CMD_TIMEOUT_MS);       \
            http_initialized = false;                           \
        }                                                       \
    } while (0)

    /* 2. AT+HTTPSSL */
    init_cmd_ctx(&ctx);
    const char *ssl_cmd = (request->transport == MODEM_HTTP_TRANSPORT_HTTPS) ?
                          "AT+HTTPSSL=1" : "AT+HTTPSSL=0";
    ret = send_cmd(self, ssl_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPSSL");
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPSSL failed: %s", esp_err_to_name(ret));
        HTTP_CLEANUP();
        return ret;
    }

    /* 3. AT+HTTPPARA="CID",1 */
    init_cmd_ctx(&ctx);
    ret = send_cmd(self, "AT+HTTPPARA=\"CID\",1", &ctx,
                   AIR780EP_HTTP_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA CID");
    }
    if (ret != ESP_OK) {
        HTTP_CLEANUP();
        return ret;
    }

    /* 4. AT+HTTPPARA="URL",<url> */
    init_cmd_ctx(&ctx);
    int needed = snprintf(NULL, 0, "AT+HTTPPARA=\"URL\",\"%s\"", request->url);
    if (needed < 0) {
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_ARG;
    }
    char *url_cmd = malloc((size_t)needed + 1U);
    if (!url_cmd) {
        HTTP_CLEANUP();
        return ESP_ERR_NO_MEM;
    }
    snprintf(url_cmd, (size_t)needed + 1U, "AT+HTTPPARA=\"URL\",\"%s\"",
             request->url);
    ret = send_cmd(self, url_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
    free(url_cmd);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA URL");
    }
    if (ret != ESP_OK) {
        HTTP_CLEANUP();
        return ret;
    }

    /* 5. AT+HTTPPARA="CONTENT",<content_type> (optional) */
    if (request->content_type && request->content_type[0]) {
        init_cmd_ctx(&ctx);
        needed = snprintf(NULL, 0, "AT+HTTPPARA=\"CONTENT\",\"%s\"",
                          request->content_type);
        if (needed < 0) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        char *ct_cmd = malloc((size_t)needed + 1U);
        if (!ct_cmd) {
            HTTP_CLEANUP();
            return ESP_ERR_NO_MEM;
        }
        snprintf(ct_cmd, (size_t)needed + 1U,
                 "AT+HTTPPARA=\"CONTENT\",\"%s\"", request->content_type);
        ret = send_cmd(self, ct_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
        free(ct_cmd);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPPARA CONTENT");
        }
        if (ret != ESP_OK) {
            HTTP_CLEANUP();
            return ret;
        }
    }

    /* 6. POST: AT+HTTPDATA=<len>,<time> + body */
    if (request->method == MODEM_HTTP_METHOD_POST && request->body &&
        request->body_len > 0) {
        if (request->body_len > AIR780EP_HTTPDATA_BODY_MAX) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_SIZE;
        }
        char data_cmd[48];
        int written = snprintf(data_cmd, sizeof(data_cmd),
                               "AT+HTTPDATA=%u,%u",
                               (unsigned int)request->body_len,
                               (unsigned int)AIR780EP_HTTPDATA_PROMPT_MS);
        if (written < 0 || (size_t)written >= sizeof(data_cmd)) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        const at_cmd_options_t data_options = {
            .timeout_ms = AIR780EP_HTTPDATA_PROMPT_MS,
            .flags = 0,
        };
        init_cmd_ctx(&ctx);
        ret = at_engine_send_cmd_with_payload(self->base.at, data_cmd,
                                              request->body, request->body_len,
                                              "DOWNLOAD", &ctx.response,
                                              &data_options);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPDATA");
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AT+HTTPDATA failed: %s", esp_err_to_name(ret));
            HTTP_CLEANUP();
            return ret;
        }
    }

    /* 7. AT+HTTPACTION=<method> — wait for +HTTPACTION URC as success line */
    const char *action_cmd = (request->method == MODEM_HTTP_METHOD_POST) ?
                             "AT+HTTPACTION=1" : "AT+HTTPACTION=0";
    const at_cmd_success_match_t action_match = {
        .type = AT_CMD_SUCCESS_MATCH_PREFIX,
        .value = "+HTTPACTION:",
    };
    uint32_t action_timeout = request->timeout_ms > 0 ?
                              request->timeout_ms : AIR780EP_HTTP_ACTION_TIMEOUT_MS;
    const at_cmd_options_t action_options = {
        .timeout_ms = action_timeout,
        .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
        .success_matches = &action_match,
        .success_match_count = 1,
    };
    init_cmd_ctx(&ctx);
    ret = send_cmd_with_options(self, action_cmd, &ctx, &action_options);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT+HTTPACTION failed: %s", esp_err_to_name(ret));
        if (request->modem_error_code) {
            *request->modem_error_code = ctx.response.error_code;
        }
        HTTP_CLEANUP();
        return (ret == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT
                                        : ESP_ERR_INVALID_RESPONSE;
    }

    /* Parse +HTTPACTION: <method>,<status>,<len> */
    const char *action_line = find_line_with_prefix(&ctx.response, "+HTTPACTION:");
    if (!action_line) {
        ESP_LOGW(TAG, "missing +HTTPACTION line");
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }
    int method_val = 0, status_val = 0, data_len = 0;
    int parsed = sscanf(action_line, "+HTTPACTION: %d,%d,%d",
                        &method_val, &status_val, &data_len);
    if (parsed < 2) {
        ESP_LOGW(TAG, "parse +HTTPACTION failed: %s", action_line);
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Module-side errors: 600..606 */
    if (status_val >= 600 && status_val <= 606) {
        ESP_LOGW(TAG, "HTTPACTION module error %d", status_val);
        if (request->modem_error_code) {
            *request->modem_error_code = status_val;
        }
        HTTP_CLEANUP();
        return ESP_ERR_INVALID_RESPONSE;
    }

    response->status_code = status_val;

    /* 8. AT+HTTPREAD (if body expected) */
    if (data_len > 0) {
        size_t read_len = (size_t)data_len;
        if (read_len > AIR780EP_HTTPREAD_BODY_MAX) {
            read_len = AIR780EP_HTTPREAD_BODY_MAX;
        }
        char read_cmd[40];
        int r_written = snprintf(read_cmd, sizeof(read_cmd),
                                 "AT+HTTPREAD=0,%u", (unsigned int)read_len);
        if (r_written < 0 || (size_t)r_written >= sizeof(read_cmd)) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_ARG;
        }
        init_cmd_ctx(&ctx);
        ret = send_cmd(self, read_cmd, &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+HTTPREAD");
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AT+HTTPREAD failed: %s", esp_err_to_name(ret));
            HTTP_CLEANUP();
            return ret;
        }
        /* Find +HTTPREAD:<len> then concatenate subsequent data lines */
        const char *read_hdr = find_line_with_prefix(&ctx.response, "+HTTPREAD:");
        if (!read_hdr) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_RESPONSE;
        }
        int hdr_len = 0;
        if (sscanf(read_hdr, "+HTTPREAD: %d", &hdr_len) != 1 || hdr_len <= 0) {
            HTTP_CLEANUP();
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint8_t *body_buf = malloc((size_t)hdr_len);
        if (!body_buf) {
            HTTP_CLEANUP();
            return ESP_ERR_NO_MEM;
        }
        size_t copied = 0;
        for (int i = 0; i < ctx.response.line_count && (int)copied < hdr_len; i++) {
            const char *line = ctx.response.lines[i];
            if (!line) {
                continue;
            }
            /* Skip the +HTTPREAD: header line and OK */
            if (strncmp(line, "+HTTPREAD:", 10) == 0) {
                continue;
            }
            size_t line_len = strlen(line);
            size_t remain = (size_t)hdr_len - copied;
            /* Reconstruct original bytes: each line was split by \r\n in the
             * AT engine line parser. Lines after the first need \r\n restored. */
            if (copied > 0 && remain >= 2) {
                body_buf[copied++] = '\r';
                body_buf[copied++] = '\n';
            }
            size_t to_copy = line_len < remain ? line_len : remain;
            memcpy(body_buf + copied, line, to_copy);
            copied += to_copy;
        }
        response->body = body_buf;
        response->body_len = copied;
    }

#undef HTTP_CLEANUP

    /* 9. AT+HTTPTERM (finally cleanup) */
    init_cmd_ctx(&ctx);
    (void)send_cmd(self, "AT+HTTPTERM", &ctx, AIR780EP_HTTP_CMD_TIMEOUT_MS);

    return ESP_OK;
}
```

- [ ] **Step 3: Register `http_request` in the ops table**

In `s_air780ep_ops`, after `.ping = air780ep_ping,`:

```c
    .ping = air780ep_ping,
    .http_request = air780ep_http_request,
```

- [ ] **Step 4: Verify build**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 5: Commit (with user authorization)**

```bash
git add src/modem/modem_air780ep.c
git commit -m "feat(air780ep): implement HTTP request ops"
```

---

## Task 6: ML307R HTTP Implementation

**Files:**
- Modify: `src/modem/modem_ml307r.c`

- [ ] **Step 1: Add HTTP defines and forward declaration**

In the DEFINES section, add:

```c
#define ML307R_HTTP_CMD_TIMEOUT_MS      9000
#define ML307R_HTTP_REQUEST_TIMEOUT_MS  120000
#define ML307R_HTTP_CONTENT_PROMPT_MS   10000
#define ML307R_HTTP_BODY_MAX            4096
```

In STATIC PROTOTYPES, add (after `ml307r_ping` prototype):

```c
/**
 * @brief 执行 ML307R HTTP 请求
 * @details Execute ML307R HTTP request
 */
static esp_err_t ml307r_http_request(modem_handle_t me,
                                     const modem_http_request_t *request,
                                     modem_http_response_t *response);
```

- [ ] **Step 2: Implement `ml307r_http_request()`**

Add before the `s_ml307r_ops` table. ML307R uses an instance model: `MHTTPCREATE` returns an httpid; configure, request, read, then `MHTTPDEL`:

```c
static esp_err_t ml307r_http_request(modem_handle_t me,
                                     const modem_http_request_t *request,
                                     modem_http_response_t *response)
{
    modem_ml307r_t *self = to_ml307r(me);
    esp_err_t ret = ESP_OK;
    int http_id = -1;

    if (request->modem_error_code) {
        *request->modem_error_code = 0;
    }

    /* 1. AT+MHTTPCREATE -> +MHTTPCREATE: <id> */
    ml307r_cmd_ctx_t ctx;
    init_cmd_ctx(&ctx);
    ret = send_cmd(self, "AT+MHTTPCREATE", &ctx,
                          ML307R_HTTP_CMD_TIMEOUT_MS);
    if (ret == ESP_OK) {
        ret = ensure_at_ok(&ctx.response, "AT+MHTTPCREATE");
    }
    if (ret != ESP_OK) {
        return ret;
    }
    const char *create_line = find_line_with_prefix(&ctx.response,
                                                      "+MHTTPCREATE:");
    if (!create_line ||
        sscanf(create_line, "+MHTTPCREATE: %d", &http_id) != 1 ||
        http_id < 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

#define HTTP_CLEANUP_ML()                                            \
    do {                                                             \
        if (http_id >= 0) {                                          \
            char _del_cmd[32];                                       \
            snprintf(_del_cmd, sizeof(_del_cmd),                     \
                     "AT+MHTTPDEL=%d", http_id);                     \
            ml307r_cmd_ctx_t _dc;                                    \
            init_cmd_ctx(&_dc);                               \
            (void)send_cmd(self, _del_cmd, &_dc,              \
                                  ML307R_HTTP_CMD_TIMEOUT_MS);       \
            http_id = -1;                                            \
        }                                                            \
    } while (0)

    /* 2. AT+MHTTPCFG=<id>,"<url>",<method>,<ssl_enable>[,<ssl_id>] */
    {
        int needed_cfg = snprintf(NULL, 0, "AT+MHTTPCFG=%d,\"%s\",%d,%d",
                                  http_id, request->url,
                                  (int)request->method,
                                  request->transport == MODEM_HTTP_TRANSPORT_HTTPS ? 1 : 0);
        if (needed_cfg < 0) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_ARG;
        }
        char *cfg_cmd = malloc((size_t)needed_cfg + 1U);
        if (!cfg_cmd) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_NO_MEM;
        }
        snprintf(cfg_cmd, (size_t)needed_cfg + 1U,
                 "AT+MHTTPCFG=%d,\"%s\",%d,%d", http_id, request->url,
                 (int)request->method,
                 request->transport == MODEM_HTTP_TRANSPORT_HTTPS ? 1 : 0);
        init_cmd_ctx(&ctx);
        ret = send_cmd(self, cfg_cmd, &ctx,
                              ML307R_HTTP_CMD_TIMEOUT_MS);
        free(cfg_cmd);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+MHTTPCFG");
        }
        if (ret != ESP_OK) {
            HTTP_CLEANUP_ML();
            return ret;
        }
    }

    /* 3. content_type header (optional) */
    if (request->content_type && request->content_type[0]) {
        int needed_hdr = snprintf(NULL, 0,
                                  "AT+MHTTPHEADER=%d,\"Content-Type:%s\"",
                                  http_id, request->content_type);
        if (needed_hdr < 0) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_ARG;
        }
        char *hdr_cmd = malloc((size_t)needed_hdr + 1U);
        if (!hdr_cmd) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_NO_MEM;
        }
        snprintf(hdr_cmd, (size_t)needed_hdr + 1U,
                 "AT+MHTTPHEADER=%d,\"Content-Type:%s\"",
                 http_id, request->content_type);
        init_cmd_ctx(&ctx);
        ret = send_cmd(self, hdr_cmd, &ctx,
                              ML307R_HTTP_CMD_TIMEOUT_MS);
        free(hdr_cmd);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+MHTTPHEADER");
        }
        if (ret != ESP_OK) {
            HTTP_CLEANUP_ML();
            return ret;
        }
    }

    /* 4. POST body via AT+MHTTPCONTENT=<id>,<len> */
    if (request->method == MODEM_HTTP_METHOD_POST && request->body &&
        request->body_len > 0) {
        if (request->body_len > ML307R_HTTP_BODY_MAX) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_SIZE;
        }
        char content_cmd[40];
        int written = snprintf(content_cmd, sizeof(content_cmd),
                               "AT+MHTTPCONTENT=%d,%u", http_id,
                               (unsigned int)request->body_len);
        if (written < 0 || (size_t)written >= sizeof(content_cmd)) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_ARG;
        }
        const at_cmd_options_t content_options = {
            .timeout_ms = ML307R_HTTP_CONTENT_PROMPT_MS,
            .flags = 0,
        };
        init_cmd_ctx(&ctx);
        ret = at_engine_send_cmd_with_payload(self->base.at, content_cmd,
                                              request->body, request->body_len,
                                              ">", &ctx.response,
                                              &content_options);
        if (ret == ESP_OK) {
            ret = ensure_at_ok(&ctx.response, "AT+MHTTPCONTENT");
        }
        if (ret != ESP_OK) {
            HTTP_CLEANUP_ML();
            return ret;
        }
    }

    /* 5. AT+MHTTPREQUEST=<id> — wait for +MHTTPURC: "rsp" */
    {
        char req_cmd[32];
        int written = snprintf(req_cmd, sizeof(req_cmd),
                               "AT+MHTTPREQUEST=%d", http_id);
        if (written < 0 || (size_t)written >= sizeof(req_cmd)) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_ARG;
        }
        char rsp_prefix[48];
        snprintf(rsp_prefix, sizeof(rsp_prefix),
                 "+MHTTPURC: \"rsp\",%d", http_id);
        const at_cmd_success_match_t rsp_match = {
            .type = AT_CMD_SUCCESS_MATCH_PREFIX,
            .value = rsp_prefix,
        };
        uint32_t req_timeout = request->timeout_ms > 0 ?
                               request->timeout_ms :
                               ML307R_HTTP_REQUEST_TIMEOUT_MS;
        const at_cmd_options_t req_options = {
            .timeout_ms = req_timeout,
            .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
            .success_matches = &rsp_match,
            .success_match_count = 1,
        };
        init_cmd_ctx(&ctx);
        ret = send_cmd_with_options(self, req_cmd, &ctx, &req_options);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "AT+MHTTPREQUEST failed: %s", esp_err_to_name(ret));
            if (request->modem_error_code) {
                *request->modem_error_code = ctx.response.error_code;
            }
            HTTP_CLEANUP_ML();
            return (ret == ESP_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT
                                            : ESP_ERR_INVALID_RESPONSE;
        }
        /* Check for error URC */
        if (find_line_with_prefix(&ctx.response, "+MHTTPURC: \"err\"")) {
            ESP_LOGW(TAG, "MHTTPURC err received");
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_RESPONSE;
        }
        /* Parse +MHTTPURC: "rsp",<id>,<status>,<len> */
        const char *rsp_line = find_line_with_prefix(&ctx.response, rsp_prefix);
        if (!rsp_line) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_RESPONSE;
        }
        int rsp_id = 0, status_val = 0, data_len = 0;
        int parsed = sscanf(rsp_line, "+MHTTPURC: \"rsp\",%d,%d,%d",
                            &rsp_id, &status_val, &data_len);
        if (parsed < 3) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_INVALID_RESPONSE;
        }
        response->status_code = status_val;
    }

    /* 6. AT+MHTTPREAD=<id>[,<len>] — read response body */
    if (response->status_code >= 200 && response->status_code < 300) {
        /* Read loop using MHTTPREAD until no more data */
        size_t total_copied = 0;
        size_t buf_cap = 4096;
        uint8_t *body_buf = malloc(buf_cap);
        if (!body_buf) {
            HTTP_CLEANUP_ML();
            return ESP_ERR_NO_MEM;
        }
        while (true) {
            char read_cmd[32];
            snprintf(read_cmd, sizeof(read_cmd), "AT+MHTTPREAD=%d,%u",
                     http_id, (unsigned int)1024);
            init_cmd_ctx(&ctx);
            ret = send_cmd(self, read_cmd, &ctx,
                                  ML307R_HTTP_CMD_TIMEOUT_MS);
            if (ret != ESP_OK) {
                free(body_buf);
                HTTP_CLEANUP_ML();
                return ret;
            }
            const char *read_hdr = find_line_with_prefix(&ctx.response,
                                                           "+MHTTPREAD:");
            if (!read_hdr) {
                /* No more data */
                break;
            }
            int read_len = 0;
            if (sscanf(read_hdr, "+MHTTPREAD: %d", &read_len) != 1 ||
                read_len <= 0) {
                break;
            }
            /* Concatenate data lines after the header */
            for (int i = 0; i < ctx.response.line_count; i++) {
                const char *line = ctx.response.lines[i];
                if (!line || strncmp(line, "+MHTTPREAD:", 11) == 0) {
                    continue;
                }
                size_t line_len = strlen(line);
                if (total_copied + line_len > buf_cap) {
                    size_t new_cap = buf_cap * 2;
                    while (total_copied + line_len > new_cap) {
                        new_cap *= 2;
                    }
                    uint8_t *new_buf = realloc(body_buf, new_cap);
                    if (!new_buf) {
                        free(body_buf);
                        HTTP_CLEANUP_ML();
                        return ESP_ERR_NO_MEM;
                    }
                    body_buf = new_buf;
                    buf_cap = new_cap;
                }
                memcpy(body_buf + total_copied, line, line_len);
                total_copied += line_len;
            }
        }
        response->body = body_buf;
        response->body_len = total_copied;
    }

#undef HTTP_CLEANUP_ML

    /* 7. AT+MHTTPDEL (finally cleanup) */
    {
        char del_cmd[32];
        snprintf(del_cmd, sizeof(del_cmd), "AT+MHTTPDEL=%d", http_id);
        init_cmd_ctx(&ctx);
        (void)send_cmd(self, del_cmd, &ctx,
                              ML307R_HTTP_CMD_TIMEOUT_MS);
    }

    return ESP_OK;
}
```

**Note on ML307R helpers:** ML307R uses the **same** helper names as Air780EP (`init_cmd_ctx`, `send_cmd`, `send_cmd_with_options`, `ensure_at_ok`, `find_line_with_prefix`, `response_contains`, `to_ml307r`) — confirmed via grep. The ctx type is `ml307r_cmd_ctx_t`. The only difference is the `self` type (`modem_ml307r_t *` vs `modem_air780ep_t *`).

- [ ] **Step 3: Register `http_request` in the ops table**

In `s_ml307r_ops`, after `.ping = ml307r_ping,`:

```c
    .ping = ml307r_ping,
    .http_request = ml307r_http_request,
```

- [ ] **Step 4: Verify build**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 5: Commit (with user authorization)**

```bash
git add src/modem/modem_ml307r.c
git commit -m "feat(ml307r): implement HTTP request ops"
```

---

## Task 7: Facade Public API and Implementation

**Files:**
- Modify: `src/include/lwlte.h`
- Modify: `src/lwlte/lwlte.c`

- [ ] **Step 1: Add public HTTP types to `lwlte.h`**

In the TYPEDEFS section, after the SSL types and before the config structs (or after `lwlte_tcp_event_data_t`):

```c
/**
 * @brief LTE HTTP 方法
 * @details LTE HTTP method
 */
typedef enum {
    LWLTE_HTTP_METHOD_GET = 0,   /**< GET 方法； GET method */
    LWLTE_HTTP_METHOD_POST,      /**< POST 方法； POST method */
} lwlte_http_method_t;

/**
 * @brief LTE HTTP 传输类型
 * @details LTE HTTP transport type
 */
typedef enum {
    LWLTE_HTTP_TRANSPORT_HTTP = 0,   /**< 明文 HTTP； Plain HTTP */
    LWLTE_HTTP_TRANSPORT_HTTPS,      /**< HTTPS (TLS)； HTTPS over TLS */
} lwlte_http_transport_t;

/**
 * @brief HTTP 请求参数
 * @details HTTP request parameters
 * @note 字符串和缓冲区为借用，在 lwlte_http_request() 返回前必须有效。
 */
typedef struct {
    lwlte_http_method_t method;       /**< HTTP 方法； HTTP method */
    const char *url;                  /**< 完整 URL，含 http(s)://； Full URL */
    lwlte_http_transport_t transport; /**< 传输类型； Transport type */
    uint8_t ssl_context_id;           /**< HTTPS SSL context ID； SSL context for HTTPS */
    const char *content_type;         /**< POST content-type，可空； POST content-type, optional */
    const uint8_t *body;              /**< POST 请求体； POST body */
    size_t body_len;                  /**< POST 请求体长度； POST body length */
    uint32_t timeout_ms;              /**< 总超时，0 用默认； Total timeout, 0=default */
} lwlte_http_request_t;

/**
 * @brief HTTP 响应结果
 * @details HTTP response result
 * @note body 成功时为库分配堆 buffer，须由 lwlte_http_response_release() 释放。
 */
typedef struct {
    int status_code;            /**< HTTP 状态码，如 200； HTTP status code */
    uint8_t *body;              /**< 库分配的响应体； Library-allocated body */
    size_t body_len;            /**< 响应体长度； Body length in bytes */
    int modem_error_code;       /**< 模块原始错误码； Raw modem error code */
} lwlte_http_response_t;
```

- [ ] **Step 2: Add public API prototypes to `lwlte.h`**

In GLOBAL PROTOTYPES, after the MQTT publish prototype:

```c
/**
 * @brief 执行同步 HTTP 请求
 * @details Perform synchronous HTTP request
 * @note 阻塞调用，直到收到响应或超时；不应在事件回调中调用。
 * @note HTTP 4xx/5xx 仍返回 ESP_OK，应用自行判断 status_code。
 * @note 成功时 response->body 为库分配堆 buffer，须调 lwlte_http_response_release() 释放。
 * @param[in] me LTE 用户门面句柄
 * @param[in] request HTTP 请求参数
 * @param[out] response HTTP 响应结果
 * @return
 *         - ESP_OK: 收到 HTTP 响应
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 网络未 online 或门面正在销毁
 *         - ESP_ERR_TIMEOUT: 请求超时
 *         - ESP_ERR_INVALID_RESPONSE: 模块响应无效
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_NOT_SUPPORTED: 模块不支持
 *         - 其他 esp_err_t: 下层错误
 */
esp_err_t lwlte_http_request(lwlte_handle_t me,
                             const lwlte_http_request_t *request,
                             lwlte_http_response_t *response);

/**
 * @brief 释放 HTTP 响应资源
 * @details Release HTTP response resources
 * @note 处理 lwlte_http_request() 返回后应调用；失败时 body 为 NULL 也是安全 no-op。
 * @param[in] response HTTP 响应结果指针，可为 NULL
 */
void lwlte_http_response_release(lwlte_http_response_t *response);
```

- [ ] **Step 3: Add HTTP ctx typedef and done_cb to `lwlte.c`**

In the TYPEDEFS section (after `lwlte_sync_cmd_ctx_t`):

```c
typedef struct {
    SemaphoreHandle_t done;
    core_cmd_result_t result;
    esp_err_t error_code;
    int status_code;
    uint8_t *body;
    size_t body_len;
    int modem_error_code;
} lwlte_http_cmd_ctx_t;
```

In STATIC PROTOTYPES, add:

```c
/**
 * @brief 处理同步 HTTP 命令完成回调
 * @details Handle synchronous HTTP command done callback
 */
static void facade_http_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx);
```

- [ ] **Step 4: Implement `lwlte_http_response_release()` and `facade_http_cmd_done_cb()`**

In GLOBAL FUNCTIONS (after `lwlte_tcp_event_data_release`):

```c
void lwlte_http_response_release(lwlte_http_response_t *response)
{
    if (!response) {
        return;
    }
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
}
```

In STATIC FUNCTIONS (after `facade_core_cmd_done_cb`):

```c
static void facade_http_cmd_done_cb(core_handle_t core,
                                    core_cmd_type_t type,
                                    core_cmd_result_t result,
                                    const void *result_data,
                                    void *user_ctx)
{
    (void)core;
    (void)type;
    lwlte_http_cmd_ctx_t *ctx = (lwlte_http_cmd_ctx_t *)user_ctx;
    if (!ctx) {
        return;
    }
    ctx->result = result;
    if (result == CORE_CMD_RESULT_OK && result_data) {
        const core_http_result_t *hr = (const core_http_result_t *)result_data;
        ctx->error_code = hr->error_code;
        ctx->status_code = hr->status_code;
        ctx->body = hr->body;
        ctx->body_len = hr->body_len;
        ctx->modem_error_code = hr->modem_error_code;
    } else {
        ctx->error_code = ESP_FAIL;
        ctx->status_code = 0;
        ctx->body = NULL;
        ctx->body_len = 0;
        ctx->modem_error_code = 0;
        if (result_data) {
            const core_http_result_t *hr = (const core_http_result_t *)result_data;
            ctx->error_code = hr->error_code;
            ctx->modem_error_code = hr->modem_error_code;
            free(hr->body);
        }
    }
    if (ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}
```

- [ ] **Step 5: Implement `lwlte_http_request()`**

In GLOBAL FUNCTIONS (after `lwlte_ssl_get_context_status` or near `lwlte_ping`):

```c
esp_err_t lwlte_http_request(lwlte_handle_t me,
                             const lwlte_http_request_t *request,
                             lwlte_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(request && response, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");
    ESP_RETURN_ON_FALSE(request->url && request->url[0], ESP_ERR_INVALID_ARG,
                        TAG, "HTTP url is required");
    ESP_RETURN_ON_FALSE(request->method == LWLTE_HTTP_METHOD_GET ||
                        request->method == LWLTE_HTTP_METHOD_POST,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HTTP method");
    ESP_RETURN_ON_FALSE(request->transport == LWLTE_HTTP_TRANSPORT_HTTP ||
                        request->transport == LWLTE_HTTP_TRANSPORT_HTTPS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid HTTP transport");
    ESP_RETURN_ON_FALSE(request->method != LWLTE_HTTP_METHOD_POST ||
                        (request->body && request->body_len > 0),
                        ESP_ERR_INVALID_ARG, TAG,
                        "POST requires non-empty body");

    /* Zero-init response so release() is safe on any return path */
    response->status_code = 0;
    response->body = NULL;
    response->body_len = 0;
    response->modem_error_code = 0;

    core_handle_t core = NULL;
    esp_err_t ret = begin_api_call(me, true, &core);
    ESP_RETURN_ON_ERROR(ret, TAG, "facade not usable");

    lwlte_http_cmd_ctx_t ctx = {
        .done = xSemaphoreCreateBinary(),
        .result = CORE_CMD_RESULT_ERROR,
        .error_code = ESP_FAIL,
    };
    if (!ctx.done) {
        end_api_call(me);
        return ESP_ERR_NO_MEM;
    }

    uint32_t timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 120000;
    core_cmd_t cmd = {
        .type = CORE_CMD_HTTP_REQUEST,
        .done_cb = facade_http_cmd_done_cb,
        .user_ctx = &ctx,
        .timeout_ms = timeout_ms,
        .data.http_request = {
            .method = request->method,
            .url = request->url,
            .transport = request->transport,
            .ssl_context_id = request->ssl_context_id,
            .content_type = request->content_type,
            .body = request->body,
            .body_len = request->body_len,
        },
    };

    ret = core_submit_cmd(core, &cmd);
    if (ret == ESP_OK) {
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        ret = ctx.error_code;
        response->status_code = ctx.status_code;
        response->body = ctx.body;
        response->body_len = ctx.body_len;
        response->modem_error_code = ctx.modem_error_code;
    }

    vSemaphoreDelete(ctx.done);
    end_api_call(me);
    return ret;
}
```

- [ ] **Step 6: Verify build**

Run: `idf.py build`
Expected: PASS

- [ ] **Step 7: Commit (with user authorization)**

```bash
git add src/include/lwlte.h src/lwlte/lwlte.c
git commit -m "feat(facade): add lwlte_http_request and response release"
```

---

## Task 8: Host Contract Test

**Files:**
- Create: `tests/host/test_http_end_to_end_contract.py`

- [ ] **Step 1: Create the contract test file**

This follows the pattern of `test_tcp_tls_ssl_socket_contract.py` and `test_ping_end_to_end_contract.py` — static source inspection via regex/token checks.

```python
#!/usr/bin/env python3
"""Static end-to-end contract checks for HTTP Service v1 implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class HttpEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.modem_c = read("src/modem/modem.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")

    def assert_has_all(self, text: str, tokens: list, label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_public_http_api_exists(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_HTTP_METHOD_GET",
            "LWLTE_HTTP_METHOD_POST",
            "LWLTE_HTTP_TRANSPORT_HTTP",
            "LWLTE_HTTP_TRANSPORT_HTTPS",
            "lwlte_http_method_t method;",
            "const char *url;",
            "lwlte_http_transport_t transport;",
            "uint8_t ssl_context_id;",
            "int status_code;",
            "uint8_t *body;",
            "size_t body_len;",
            "int modem_error_code;",
            "esp_err_t lwlte_http_request(lwlte_handle_t me,",
            "void lwlte_http_response_release(",
        ], "lwlte.h HTTP API")

    def test_facade_implementation_threads_http(self):
        self.assert_has_all(self.lwlte_c, [
            "typedef struct {",
            "SemaphoreHandle_t done;",
            "facade_http_cmd_done_cb",
            "CORE_CMD_HTTP_REQUEST",
            ".done_cb = facade_http_cmd_done_cb",
            "lwlte_http_response_release",
            "free(response->body)",
        ], "lwlte.c HTTP facade")

    def test_core_command_and_result_types(self):
        self.assert_has_all(self.core_h, [
            "CORE_CMD_HTTP_REQUEST",
            "core_http_result_t",
            "lwlte_http_method_t method;",
            "lwlte_http_transport_t transport;",
        ], "core.h HTTP types")
        self.assert_has_all(self.core_c, [
            "CORE_CMD_HTTP_REQUEST",
            "clone->data.http_request.url",
            "clone->data.http_request.content_type",
            "clone->data.http_request.body",
        ], "core.c HTTP clone")
        self.assert_has_all(self.core_fsm_c, [
            "CORE_CMD_HTTP_REQUEST",
            "modem_http_request(me->modem",
            ".method = (modem_http_method_t)",
            ".transport = (modem_http_transport_t)",
            "core_http_result_t result",
        ], "core_fsm.c HTTP mapping")

    def test_modem_types_and_wrapper(self):
        self.assert_has_all(self.modem_h, [
            "MODEM_HTTP_METHOD_GET",
            "MODEM_HTTP_METHOD_POST",
            "MODEM_HTTP_TRANSPORT_HTTP",
            "MODEM_HTTP_TRANSPORT_HTTPS",
            "modem_http_request_t",
            "modem_http_response_t",
            "esp_err_t modem_http_request(modem_handle_t me,",
        ], "modem.h HTTP types")
        self.assert_has_all(self.modem_c, [
            "modem_http_request(modem_handle_t me,",
            "me->ops->http_request",
        ], "modem.c HTTP wrapper")

    def test_air780ep_http_ops_and_at_commands(self):
        self.assert_has_all(self.air_c, [
            ".http_request = air780ep_http_request",
            "AT+HTTPINIT",
            "AT+HTTPSSL=1",
            "AT+HTTPSSL=0",
            'AT+HTTPPARA="CID"',
            'AT+HTTPPARA="URL"',
            "AT+HTTPDATA=",
            "AT+HTTPACTION=",
            "+HTTPACTION:",
            "AT+HTTPREAD",
            "AT+HTTPTERM",
        ], "air780ep HTTP AT commands")

    def test_ml307r_http_ops_and_at_commands(self):
        self.assert_has_all(self.ml_c, [
            ".http_request = ml307r_http_request",
            "AT+MHTTPCREATE",
            "AT+MHTTPCFG=",
            "AT+MHTTPHEADER=",
            "AT+MHTTPCONTENT=",
            "AT+MHTTPREQUEST=",
            "+MHTTPURC: \"rsp\"",
            "AT+MHTTPREAD",
            "AT+MHTTPDEL=",
        ], "ml307r HTTP AT commands")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the contract test — verify it passes**

Run: `python3 -m pytest tests/host/test_http_end_to_end_contract.py -v`
Expected: PASS (all 6 test methods)

If any test fails, inspect the corresponding source file for missing tokens and fix the implementation before proceeding.

- [ ] **Step 3: Run the full host test suite — verify no regressions**

Run: `python3 -m pytest tests/host/ -v`
Expected: All tests PASS (existing 182 + new 6 = 188), no regressions.

- [ ] **Step 4: Commit (with user authorization)**

```bash
git add tests/host/test_http_end_to_end_contract.py
git commit -m "test: add HTTP service end-to-end contract tests"
```

---

## Task 9: Final Build Verification

**Files:** None (verification only)

- [ ] **Step 1: Clean build for Air780EP target**

Run: `idf.py fullclean && idf.py build`
Expected: PASS, no warnings related to HTTP code

- [ ] **Step 2: Full host test suite**

Run: `python3 -m pytest tests/host/ -v`
Expected: All PASS

- [ ] **Step 3: Verify no docs need updating**

The spec `docs/superpowers/specs/2026-06-27-http-service-design.md` is already written. Check if `docs/agents/feature-roadmap.md` P4 status should change from "规划" to "已完成" — **only update if user confirms HTTP v1 is considered complete** (实机验证 pending, so likely keep as-is or note "v1 编译完成，实机待验").

---

## Self-Review Notes

**Spec coverage:** All 12 sections of the spec map to tasks:
- §2 architecture → Tasks 1-7 (layered implementation)
- §3 public API → Task 7
- §4 core command → Tasks 3-4
- §5 modem ops → Tasks 1-2, 5-6
- §6 Air780EP → Task 5
- §7 ML307R → Task 6
- §8 error handling → embedded in Tasks 5-6 (status 600..606, cleanup macros)
- §9 facade lifecycle → Task 7
- §10 known limitations → documented in spec, reflected in body assembly code
- §11 verification → Tasks 8-9
- §12 file scope → File Structure table above

**Key implementation note:** ML307R helper function names are identical to Air780EP's (`init_cmd_ctx`, `send_cmd`, `send_cmd_with_options`, `ensure_at_ok`, `find_line_with_prefix`, `response_contains`, `to_ml307r`), confirmed via grep against `src/modem/modem_ml307r.c`. Task 6 code uses the correct names directly — no substitution needed.
