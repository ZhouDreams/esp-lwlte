# AT Engine Command Options Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-command AT Engine options so callers can keep the default `OK` command path while supporting custom successful final responses such as `SHUT OK`, pure IP address lines, `CONNACK OK`, and `CONNECT OK`.

**Architecture:** Keep `at_engine_send_cmd()` as the simple default API and add `at_engine_send_cmd_with_options()` for commands that need a custom success-final strategy. The AT Engine remains generic: it matches caller-provided exact, prefix, or any-line success rules and never hard-codes Air780EP strings. After code changes, update `docs/agents/classes.md` so the architecture documentation reflects the new command options API.

**Tech Stack:** C99, ESP-IDF 6.0, FreeRTOS semaphores/tasks, ESP-IDF UART driver, `esp_err_t`, `esp_check.h`, project Doxygen/style templates, Markdown docs.

---

## User Constraints

- Do not create git commits unless the user explicitly asks for a commit.
- Keep `at_engine_send_cmd()` as the default/simple API.
- Add a configurable API so future command behavior only requires extending the options struct.
- Update `docs/agents/classes.md` after the code changes.
- Do not hard-code Air780EP command strings such as `SHUT OK`, `CONNACK OK`, or IP address parsing into the generic AT Engine.

## Scope Check

This plan covers one subsystem: AT Engine command completion behavior. It does not implement the Modem module yet. It prepares AT Engine for the later Modem implementation by supporting command-specific success finals.

## File Structure

- Modify: `src/include/at_engine.h`
  - Add command success match types, success match descriptors, option flags, `at_cmd_options_t`, and `at_engine_send_cmd_with_options()`.
  - Keep `at_engine_send_cmd()` unchanged for existing callers.
- Modify: `src/at_engine/at_engine.c`
  - Store a per-command options snapshot in `at_cmd_ctx_t`.
  - Route `at_engine_send_cmd()` through `at_engine_send_cmd_with_options()`.
  - Add option validation and line-completion matching helpers.
  - Preserve error finals (`ERROR`, `+CME ERROR:`, `+CMS ERROR:`) as always-terminal failures.
- Modify: `docs/agents/classes.md`
  - Document `at_cmd_options_t` and the new `at_engine_send_cmd_with_options()` API.
  - Replace the stale special-response warning for `AT+CIFSR` and `AT+CIPSHUT` with the new options-based solution.
- Verify: `src/CMakeLists.txt`
  - No expected change, but build verification depends on this component registration.

## Behavior Contract

- Default calls to `at_engine_send_cmd()` behave exactly as before: `OK` ends successfully; `ERROR`, `+CME ERROR:`, and `+CMS ERROR:` end unsuccessfully.
- `at_engine_send_cmd_with_options()` accepts caller-provided success finals.
- `AT_CMD_FLAG_NO_STANDARD_OK_FINAL` makes `OK` an intermediate response instead of a successful final.
- `AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` drops that intermediate `OK` from `response.lines`.
- `ERROR`, `+CME ERROR:`, and `+CMS ERROR:` always terminate the command as failures, regardless of options.
- A custom successful final is appended to `response.lines` before completing the command, so callers can read values such as a pure IP address from `AT+CIFSR`.
- If `AT_CMD_FLAG_NO_STANDARD_OK_FINAL` is set, a line equal to `OK` never matches `AT_CMD_SUCCESS_MATCH_ANY_LINE`; it is treated as intermediate `OK`.

## Task 1: Public API Options

**Files:**
- Modify: `src/include/at_engine.h:21-105`
- Modify: `src/include/at_engine.h:132-181`

- [ ] **Step 1: Add command option flags in the DEFINES section**

In `src/include/at_engine.h`, replace the empty `DEFINES` section with:

```c
/*********************
 *      DEFINES
 *********************/

/**
 * @brief 不把标准 OK 作为最终成功响应
 * @details Do not treat standard OK as a final success response
 */
#define AT_CMD_FLAG_NO_STANDARD_OK_FINAL        (1U << 0)

/**
 * @brief 跳过中间 OK 响应行
 * @details Skip intermediate OK response line
 */
#define AT_CMD_FLAG_SKIP_INTERMEDIATE_OK        (1U << 1)
```

- [ ] **Step 2: Add option-related public types after `at_response_t`**

In `src/include/at_engine.h`, insert this block immediately after the `at_response_t` definition and before `at_urc_callback_t`:

```c
/**
 * @brief AT 命令成功匹配类型
 * @details AT command success match type
 */
typedef enum {
    AT_CMD_SUCCESS_MATCH_EXACT = 0,      /**< 完整匹配； Exact match */
    AT_CMD_SUCCESS_MATCH_PREFIX,         /**< 前缀匹配； Prefix match */
    AT_CMD_SUCCESS_MATCH_ANY_LINE,       /**< 任意非错误响应行； Any non-error response line */
} at_cmd_success_match_type_t;

/**
 * @brief AT 命令成功匹配规则
 * @details AT command success match rule
 */
typedef struct {
    at_cmd_success_match_type_t type;     /**< 匹配类型； Match type */
    const char *value;                    /**< 匹配文本，ANY_LINE 时忽略； Match text, ignored for ANY_LINE */
} at_cmd_success_match_t;

/**
 * @brief AT 命令选项
 * @details AT command options
 * @note success_matches 指向的数组在 at_engine_send_cmd_with_options() 返回前必须保持有效。
 */
typedef struct {
    uint32_t timeout_ms;                                  /**< 超时时间，0 表示使用默认值； Timeout, 0 uses default */
    uint32_t flags;                                       /**< AT_CMD_FLAG_* 标志； AT_CMD_FLAG_* flags */
    const at_cmd_success_match_t *success_matches;        /**< 自定义成功匹配规则数组； Custom success match rules */
    int success_match_count;                              /**< 自定义成功匹配规则数量； Custom success match count */
} at_cmd_options_t;
```

- [ ] **Step 3: Add the configurable send prototype**

In `src/include/at_engine.h`, keep the existing `at_engine_send_cmd()` declaration, then add this declaration immediately after it:

```c
/**
 * @brief 使用选项发送 AT 命令
 * @details Send AT command with options
 * @param[in] me AT 引擎句柄
 * @param[in] cmd AT 命令，不要求包含 CRLF
 * @param[out] response 响应对象
 * @param[in] options 单次命令选项
 * @note options->success_matches 指向的数组在函数返回前必须保持有效。
 * @return
 *         - ESP_OK: 命令流程完成，AT 业务结果见 response->status
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待响应超时
 *         - ESP_FAIL: UART 写入失败
 */
esp_err_t at_engine_send_cmd_with_options(at_engine_t *me, const char *cmd,
                                          at_response_t *response,
                                          const at_cmd_options_t *options);
```

- [ ] **Step 4: Run a build to capture the expected first failure**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project` from the workspace root.

Expected: build fails because `at_engine_send_cmd_with_options()` is declared but not defined, or because implementation has not yet been updated. Do not edit unrelated files.

## Task 2: Command Context and Option Validation

**Files:**
- Modify: `src/at_engine/at_engine.c:54-116`
- Modify: `src/at_engine/at_engine.c:214-299`

- [ ] **Step 1: Extend `at_cmd_ctx_t`**

In `src/at_engine/at_engine.c`, change `at_cmd_ctx_t` to include an options snapshot:

```c
typedef struct {
    const char *cmd;
    uint32_t timeout_ms;
    at_response_t *response;
    at_cmd_options_t options;
    int echo_consumed;
    int data_line_index;
    bool result_received;
} at_cmd_ctx_t;
```

- [ ] **Step 2: Replace static prototypes for final parsing helpers**

In the `STATIC PROTOTYPES` section, replace:

```c
static bool parse_final_result(at_response_t *response, const char *line);
static int parse_error_code(const char *line);
```

with:

```c
static esp_err_t validate_options(const at_cmd_options_t *options);
static bool parse_error_result(at_response_t *response, const char *line);
static bool match_custom_success(const at_cmd_ctx_t *ctx, const char *line);
static bool match_success_rule(const at_cmd_success_match_t *rule, const char *line);
static bool is_intermediate_ok(const at_cmd_ctx_t *ctx, const char *line);
static int parse_error_code(const char *line);
```

- [ ] **Step 3: Replace `at_engine_send_cmd()` with a wrapper**

Replace the body of `at_engine_send_cmd()` with:

```c
esp_err_t at_engine_send_cmd(at_engine_t *me, const char *cmd,
                             at_response_t *response, uint32_t timeout_ms)
{
    const at_cmd_options_t options = {
        .timeout_ms = timeout_ms,
        .flags = 0,
        .success_matches = NULL,
        .success_match_count = 0,
    };

    return at_engine_send_cmd_with_options(me, cmd, response, &options);
}
```

- [ ] **Step 4: Add `at_engine_send_cmd_with_options()` using the old send implementation**

Immediately after `at_engine_send_cmd()`, add `at_engine_send_cmd_with_options()` by moving the old `at_engine_send_cmd()` implementation into the new function and applying these concrete changes:

```c
esp_err_t at_engine_send_cmd_with_options(at_engine_t *me, const char *cmd,
                                          at_response_t *response,
                                          const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(me && cmd && response && options,
                        ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(response->lines && response->max_lines > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid response lines");

    esp_err_t ret = validate_options(options);
    ESP_RETURN_ON_ERROR(ret, TAG, "invalid command options");

    ret = begin_send_call(me);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t wait_ms = options->timeout_ms ? options->timeout_ms :
                       (uint32_t)me->config.cmd_default_timeout_ms;
    if (wait_ms == 0) {
        end_send_call(me);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(me->cmd_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        end_send_call(me);
        return ESP_ERR_TIMEOUT;
    }

    reset_response(response);
    clear_done_signal(me);

    at_cmd_ctx_t *ctx = &me->cmd_ctx_storage;

    xSemaphoreTake(me->lock, portMAX_DELAY);
    clear_response_pool(me);
    *ctx = (at_cmd_ctx_t) {
        .cmd = cmd,
        .timeout_ms = wait_ms,
        .response = response,
        .options = *options,
        .echo_consumed = 0,
        .data_line_index = 0,
        .result_received = false,
    };
    me->cmd_ctx = ctx;
    me->state = AT_STATE_SENDING;
    xSemaphoreGive(me->lock);

    ret = write_cmd(me, cmd);
    if (ret != ESP_OK) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
        xSemaphoreGive(me->lock);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ret;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->state = AT_STATE_WAITING;
    }
    xSemaphoreGive(me->lock);

    if (xSemaphoreTake(me->cmd_done_sema, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        xSemaphoreTake(me->lock, portMAX_DELAY);
        response->status = AT_RESP_TIMEOUT;
        response->error_code = 0;
        if (me->cmd_ctx == ctx) {
            me->cmd_ctx = NULL;
        }
        flush_rx_input_locked(me);
        me->state = AT_STATE_IDLE;
        xSemaphoreGive(me->lock);
        clear_done_signal(me);
        xSemaphoreGive(me->cmd_mutex);
        end_send_call(me);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->cmd_ctx == ctx) {
        me->cmd_ctx = NULL;
        me->state = AT_STATE_IDLE;
    }
    xSemaphoreGive(me->lock);

    xSemaphoreGive(me->cmd_mutex);
    end_send_call(me);
    return ESP_OK;
}
```

- [ ] **Step 5: Add `validate_options()`**

Add this static function in the `STATIC FUNCTIONS` section before `begin_send_call()`:

```c
static esp_err_t validate_options(const at_cmd_options_t *options)
{
    ESP_RETURN_ON_FALSE(options, ESP_ERR_INVALID_ARG, TAG, "options is NULL");
    ESP_RETURN_ON_FALSE(options->success_match_count >= 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid success_match_count");
    ESP_RETURN_ON_FALSE(options->success_match_count == 0 || options->success_matches,
                        ESP_ERR_INVALID_ARG, TAG, "success_matches is NULL");

    for (int i = 0; i < options->success_match_count; i++) {
        const at_cmd_success_match_t *rule = &options->success_matches[i];
        if (rule->type == AT_CMD_SUCCESS_MATCH_EXACT ||
            rule->type == AT_CMD_SUCCESS_MATCH_PREFIX) {
            ESP_RETURN_ON_FALSE(rule->value && rule->value[0] != '\0',
                                ESP_ERR_INVALID_ARG, TAG, "empty success match value");
        } else {
            ESP_RETURN_ON_FALSE(rule->type == AT_CMD_SUCCESS_MATCH_ANY_LINE,
                                ESP_ERR_INVALID_ARG, TAG, "invalid success match type");
        }
    }

    return ESP_OK;
}
```

- [ ] **Step 6: Build after the API route exists**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build may still fail until line parsing is updated in Task 3. Any failure should be limited to references to the old `parse_final_result()` path or new helper definitions.

## Task 3: Line Completion Strategy

**Files:**
- Modify: `src/at_engine/at_engine.c:486-620`

- [ ] **Step 1: Replace command-line handling in `handle_line()`**

In `handle_line()`, replace the `if (ctx) { ... }` block with:

```c
    if (ctx) {
        if (!ctx->echo_consumed && is_echo_line(ctx, line)) {
            ctx->echo_consumed = 1;
            xSemaphoreGive(me->lock);
            return;
        }

        if (parse_error_result(ctx->response, line)) {
            finish_cmd_locked(me, ctx->response->status, ctx->response->error_code);
            xSemaphoreGive(me->lock);
            return;
        }

        if (strcmp(line, "OK") == 0) {
            if (!is_intermediate_ok(ctx, line)) {
                finish_cmd_locked(me, AT_RESP_OK, 0);
                xSemaphoreGive(me->lock);
                return;
            }
            if ((ctx->options.flags & AT_CMD_FLAG_SKIP_INTERMEDIATE_OK) != 0) {
                xSemaphoreGive(me->lock);
                return;
            }
            append_response_line_locked(me, ctx, line);
            xSemaphoreGive(me->lock);
            return;
        }

        if (match_custom_success(ctx, line)) {
            append_response_line_locked(me, ctx, line);
            finish_cmd_locked(me, AT_RESP_OK, 0);
            xSemaphoreGive(me->lock);
            return;
        }

        append_response_line_locked(me, ctx, line);
        xSemaphoreGive(me->lock);
        return;
    }
```

- [ ] **Step 2: Replace `parse_final_result()` with error-only parsing**

Replace the old `parse_final_result()` function with:

```c
static bool parse_error_result(at_response_t *response, const char *line)
{
    if (strcmp(line, "ERROR") == 0) {
        response->status = AT_RESP_ERROR;
        response->error_code = 0;
        return true;
    }
    if (starts_with(line, "+CME ERROR:")) {
        response->status = AT_RESP_CME_ERROR;
        response->error_code = parse_error_code(line);
        return true;
    }
    if (starts_with(line, "+CMS ERROR:")) {
        response->status = AT_RESP_CMS_ERROR;
        response->error_code = parse_error_code(line);
        return true;
    }
    return false;
}
```

- [ ] **Step 3: Add intermediate OK and custom success helpers**

Add these functions after `parse_error_result()` and before `parse_error_code()`:

```c
static bool is_intermediate_ok(const at_cmd_ctx_t *ctx, const char *line)
{
    if (!ctx || strcmp(line, "OK") != 0) {
        return false;
    }
    return (ctx->options.flags & AT_CMD_FLAG_NO_STANDARD_OK_FINAL) != 0;
}

static bool match_custom_success(const at_cmd_ctx_t *ctx, const char *line)
{
    if (!ctx || !line) {
        return false;
    }

    for (int i = 0; i < ctx->options.success_match_count; i++) {
        if (match_success_rule(&ctx->options.success_matches[i], line)) {
            return true;
        }
    }

    return false;
}

static bool match_success_rule(const at_cmd_success_match_t *rule, const char *line)
{
    if (!rule || !line) {
        return false;
    }

    switch (rule->type) {
    case AT_CMD_SUCCESS_MATCH_EXACT:
        return strcmp(line, rule->value) == 0;
    case AT_CMD_SUCCESS_MATCH_PREFIX:
        return starts_with(line, rule->value);
    case AT_CMD_SUCCESS_MATCH_ANY_LINE:
        return true;
    default:
        return false;
    }
}
```

- [ ] **Step 4: Verify old default behavior still builds**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build succeeds. If it fails, fix only compile errors caused by the new API and helper names.

- [ ] **Step 5: Manually inspect the behavior against examples**

Confirm the code path supports these caller examples without adding them to source yet:

```c
static const at_cmd_success_match_t cipshut_final[] = {
    { AT_CMD_SUCCESS_MATCH_EXACT, "SHUT OK" },
};
const at_cmd_options_t cipshut_options = {
    .timeout_ms = 90000,
    .flags = 0,
    .success_matches = cipshut_final,
    .success_match_count = 1,
};
```

```c
static const at_cmd_success_match_t cifsr_final[] = {
    { AT_CMD_SUCCESS_MATCH_ANY_LINE, NULL },
};
const at_cmd_options_t cifsr_options = {
    .timeout_ms = 9000,
    .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL,
    .success_matches = cifsr_final,
    .success_match_count = 1,
};
```

```c
static const at_cmd_success_match_t mconnect_final[] = {
    { AT_CMD_SUCCESS_MATCH_EXACT, "CONNACK OK" },
    { AT_CMD_SUCCESS_MATCH_EXACT, "CONNECT OK" },
};
const at_cmd_options_t mconnect_options = {
    .timeout_ms = 30000,
    .flags = AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK,
    .success_matches = mconnect_final,
    .success_match_count = 2,
};
```

Expected inspection results:
- `AT+CIPSHUT` can complete on `SHUT OK`.
- `AT+CIFSR` can complete on the first non-error data line and that line is available in `response.lines[0]`.
- `MCONNECT` ignores the first `OK` as a final and completes on `CONNACK OK` or `CONNECT OK`.

## Task 4: Documentation Sync

**Files:**
- Modify: `docs/agents/classes.md:17-240`
- Modify: `docs/agents/classes.md:409-428`
- Modify: `docs/agents/classes.md:648-668`

- [ ] **Step 1: Update AT Engine class overview**

In `docs/agents/classes.md`, update the AT Engine class overview table so it includes the new options types:

```markdown
| `at_cmd_options_t` | 层间 API | Modem 层 | 值对象 | 单次命令的超时、成功终止匹配和 OK 处理选项 |
| `at_cmd_success_match_t` | 层间 API | Modem 层 | 值对象 | 自定义成功响应匹配规则 |
```

- [ ] **Step 2: Update public method listing**

In section `1.3 at_engine_t`, replace the send command prototype block with:

```c
/* 发送普通 AT 命令（阻塞调用，直到 OK/ERROR/CME/CMS 或超时） */
esp_err_t    at_engine_send_cmd(at_engine_t *me, const char *cmd,
                                at_response_t *response, uint32_t timeout_ms);

/* 使用单次命令选项发送 AT 命令，支持自定义成功终止响应 */
esp_err_t    at_engine_send_cmd_with_options(at_engine_t *me, const char *cmd,
                                             at_response_t *response,
                                             const at_cmd_options_t *options);
```

- [ ] **Step 3: Add a short section after `at_response_t`**

After section `1.4 at_response_t`, add a new section named `1.5 at_cmd_options_t — 单次命令选项` and renumber the following AT Engine sections if needed. The new section content should be:

```markdown
### 1.5 `at_cmd_options_t` — 单次命令选项

**所属层**：AT Engine  
**可见性**：层间 API — Modem 层在调用特殊 AT 命令时构造后传给 AT Engine  
**OOP 角色**：值对象 — 描述单次命令的成功终止策略

`at_engine_send_cmd()` 继续覆盖普通 `OK/ERROR` 命令。`at_engine_send_cmd_with_options()` 用于特殊命令，例如 `AT+CIPSHUT` 的 `SHUT OK`、`AT+CIFSR` 的纯 IP 行、以及先返回中间 `OK` 再返回 `CONNACK OK` / `CONNECT OK` 的连接类命令。

关键规则：
- `ERROR`、`+CME ERROR:`、`+CMS ERROR:` 永远作为失败终止响应。
- 默认 `OK` 仍作为成功终止响应。
- `AT_CMD_FLAG_NO_STANDARD_OK_FINAL` 可把 `OK` 降级为中间响应。
- `AT_CMD_FLAG_SKIP_INTERMEDIATE_OK` 可丢弃这个中间 `OK`，不写入 `response.lines`。
- 自定义成功终止行会先写入 `response.lines`，再以 `AT_RESP_OK` 完成命令。
```

- [ ] **Step 4: Replace the stale Air780EP special-response constraint**

In the Modem Adapter `ops 功能与 Air780EP AT 指令映射` section, replace the current text that says `AT+CIFSR` and `AT+CIPSHUT` cannot use the normal model with:

```markdown
**特殊响应约束**：`AT+CIFSR` 成功时返回纯 IP 地址且不以 `OK` 结束，`AT+CIPSHUT` 成功终止行为 `SHUT OK`，`MCONNECT` 类命令可能先返回中间 `OK` 再返回 `CONNACK OK` 或 `CONNECT OK`。这些命令应使用 `at_engine_send_cmd_with_options()` 配置自定义成功终止规则，而不是把模块私有响应硬编码进 AT Engine。
```

- [ ] **Step 5: Update Air780EP command context usage**

In section `2.13 air780ep_cmd_ctx_t`, replace the usage note with:

```markdown
**使用模式**：Air780EP 普通 `OK/ERROR` 命令可继续使用 `at_engine_send_cmd()`。特殊成功终止命令在栈上创建 `air780ep_cmd_ctx_t` 和 `at_cmd_options_t`，调用 `at_engine_send_cmd_with_options()`，再解析 `response.lines`。该上下文不跨命令保存，不暴露给 Core。
```

- [ ] **Step 6: Check documentation-only diff for docs task**

Run: `git diff -- docs/agents/classes.md`

Expected: diff only documents the new AT Engine options API and removes stale statements that said Air780EP must use a future helper or AT Engine extension.

## Task 5: Final Verification

**Files:**
- Verify: `src/include/at_engine.h`
- Verify: `src/at_engine/at_engine.c`
- Verify: `docs/agents/classes.md`
- Verify: `src/CMakeLists.txt`

- [ ] **Step 1: Run whitespace check**

Run: `git diff --check`

Expected: no output and exit status 0.

- [ ] **Step 2: Run ESP-IDF build**

Run: ESP-IDF MCP build tool `esp-idf-eim_build_project`.

Expected: build succeeds.

- [ ] **Step 3: Search for stale API warnings**

Run: `rg -n "内部专用 helper|不能把这两条命令当作普通|parse_final_result|send_cmd_ex" src docs/agents docs/superpowers/plans/2026-05-23-at-engine-command-options-plan.md`

Expected: no matches for stale names or stale warnings, except this plan may appear in the command itself if the shell reports command text. If matches appear in older historical plan/spec files under `docs/superpowers/`, do not rewrite them unless they are current active docs.

- [ ] **Step 4: Review final source diff**

Run: `git diff -- src/include/at_engine.h src/at_engine/at_engine.c docs/agents/classes.md src/CMakeLists.txt`

Expected:
- `src/include/at_engine.h` adds options types and `at_engine_send_cmd_with_options()`.
- `src/at_engine/at_engine.c` routes default send through options and implements custom final matching.
- `docs/agents/classes.md` documents the new API and special response strategy.
- `src/CMakeLists.txt` is unchanged unless the build system required no-op formatting, which should be avoided.

- [ ] **Step 5: Report verification results**

Report the exact verification commands run and whether each passed. Explicitly state that this validates build/static behavior only; it does not prove real modem behavior until Air780EP commands are exercised on hardware.

## Self-Review Result

- Spec coverage: The plan covers the approved default API plus configurable API, exact/prefix/any-line custom success matching, intermediate `OK` handling, MCONNECT-style completion, and `classes.md` sync.
- Placeholder scan: No unfinished placeholder markers or unspecified implementation steps remain.
- Type consistency: Public names are consistently `at_cmd_options_t`, `at_cmd_success_match_t`, `at_cmd_success_match_type_t`, and `at_engine_send_cmd_with_options()`.
- Scope: The plan intentionally stops before Modem implementation and only prepares AT Engine behavior needed by Modem.
