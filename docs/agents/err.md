# 错误处理机制

本项目仅面向 ESP-IDF 平台，直接使用 ESP-IDF 内置的 `esp_err_t` 和 `esp_check.h` 错误检查宏，不自定义错误码体系。

---

## 1. 返回值约定

### 1.1 所有公共 API 返回 `esp_err_t`

```c
esp_err_t lwlte_start(lwlte_handle_t me);
esp_err_t lwlte_stop(lwlte_handle_t me);
esp_err_t lwlte_destroy(lwlte_handle_t me);
```

### 1.2 Modem 公共包装 API 和内部 ops 方法统一返回 `esp_err_t`

```c
/* src/modem/modem.h：Core 调用层间 modem_* 包装 API */
esp_err_t modem_start(modem_handle_t me);
esp_err_t modem_get_signal(modem_handle_t me, modem_signal_t *signal);
esp_err_t modem_set_apn(modem_handle_t me, uint8_t cid, const char *apn);

/* Modem 层内部：wrapper 实现再分发到具体模块 ops */
struct modem_ops {
    esp_err_t (*start)(modem_handle_t me);
    esp_err_t (*get_signal)(modem_handle_t me, modem_signal_t *signal);
    esp_err_t (*set_apn)(modem_handle_t me, uint8_t cid, const char *apn);
    esp_err_t (*activate_pdp)(modem_handle_t me, uint8_t cid);
};
```

### 1.3 常用错误码

| 错误码 | 含义 |
|--------|------|
| `ESP_OK` (0) | 成功 |
| `ESP_FAIL` | 通用失败 |
| `ESP_ERR_INVALID_ARG` | 参数无效 |
| `ESP_ERR_NO_MEM` | 内存不足 |
| `ESP_ERR_TIMEOUT` | 超时 |
| `ESP_ERR_INVALID_STATE` | 状态错误 |
| `ESP_ERR_NOT_SUPPORTED` | 不支持的操作 |
| `ESP_ERR_NOT_FOUND` | 未找到 |

完整列表见 ESP-IDF 文档 [Error Codes Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/error-codes.html)。

---

## 2. ESP-IDF 内置错误检查宏

以下宏定义在 `<esp_check.h>`，本项目直接使用。**核心约定**：`ESP_GOTO_ON_*` 系列依赖的局部变量必须命名为 `ret`。

### 2.1 宏一览

| 宏 | 行为 | 适用场景 |
|----|------|---------|
| `ESP_ERROR_CHECK(x)` | `x != ESP_OK` 时打印并 `abort()` | 致命错误，无法继续运行 |
| `ESP_ERROR_CHECK_WITHOUT_ABORT(x)` | `x != ESP_OK` 时打印，不终止 | 调试期临时检查 |
| `ESP_RETURN_ON_ERROR(x, TAG, fmt, ...)` | `x != ESP_OK` 时打印并返回 `x` | 无资源需清理，失败直接返回 |
| `ESP_RETURN_ON_FALSE(a, err_code, TAG, fmt, ...)` | `a == false` 时打印并返回 `err_code` | 参数/状态检查 |
| `ESP_GOTO_ON_ERROR(x, goto_tag, TAG, fmt, ...)` | `x != ESP_OK` 时打印、设 `ret`、跳转 | 有资源需清理 |
| `ESP_GOTO_ON_FALSE(a, err_code, goto_tag, TAG, fmt, ...)` | `a == false` 时打印、设 `ret`、跳转 | 资源创建失败需清理 |
| `ESP_RETURN_ON_ERROR_CLEANUP(x, ...)` | `x != ESP_OK` 时执行清理代码后返回 | 轻量清理，无需完整 goto 标签 |
| `ESP_RETURN_VOID_ON_ERROR(x, TAG, fmt, ...)` | `x != ESP_OK` 时打印并返回 | void 函数中的错误检查 |
| `ESP_RETURN_VOID_ON_FALSE(a, TAG, fmt, ...)` | `a == false` 时打印并返回 | void 函数中的条件检查 |

### 2.2 `ESP_ERROR_CHECK` — 致命错误

仅用于硬件初始化失败等不可恢复场景：

```c
/* UART 初始化失败是致命错误 */
ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE, 0, 0, NULL, 0));
ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
```

**规则**：只能在 Board Init 这类最外层硬件装配入口中使用。Core 层及以上禁止调用 `ESP_ERROR_CHECK`——应用层不应有 `abort()` 调用。

### 2.3 `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` — 错误返回

用于无需清理资源的场景：

```c
esp_err_t core_start(core_handle_t me)
{
    ESP_RETURN_ON_FALSE(me && me->lock, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(api_state_allows(me, CORE_SIG_START),
                        ESP_ERR_INVALID_STATE, TAG, "start not allowed");

    core_fsm_sig_t sig = {
        .type = CORE_SIG_START,
    };

    xSemaphoreTake(me->lock, portMAX_DELAY);
    if (me->destroying || me->state != CORE_STATE_STOPPED ||
        me->fsm.stop_requested || !me->fsm.running ||
        !me->fsm.task || !me->fsm.queue) {
        xSemaphoreGive(me->lock);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t send_ret = xQueueSend(me->fsm.queue, &sig, 0);
    if (send_ret == pdTRUE) {
        me->state = CORE_STATE_STARTING;
        me->stop_pending = false;
    }
    xSemaphoreGive(me->lock);

    if (send_ret != pdTRUE) {
        ESP_LOGE(TAG, "send start signal failed: %s",
                 esp_err_to_name(ESP_ERR_TIMEOUT));
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
```

### 2.4 `ESP_GOTO_ON_ERROR` / `ESP_GOTO_ON_FALSE` — 跳转清理

用于有资源需释放的场景。**硬性要求**：使用这些宏的函数必须在开头定义 `esp_err_t ret = ESP_OK;`，标签统一用 `err`。

```c
esp_err_t core_init_resources(core_handle_t me, modem_handle_t modem)
{
    ESP_RETURN_ON_FALSE(me && modem, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = ESP_OK;

    me->fsm_queue = xQueueCreate(CONFIG_LWLTE_FSM_QUEUE_SIZE, sizeof(sig_item_t));
    ESP_GOTO_ON_FALSE(me->fsm_queue, ESP_ERR_NO_MEM, err, TAG, "xQueueCreate fsm_queue failed");

    ret = modem_register_event_callback(modem, core_modem_event_handler, me);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register modem event callback failed");

    return ESP_OK;

err:
    if (me->fsm_queue) vQueueDelete(me->fsm_queue);
    me->fsm_queue = NULL;
    return ret;
}
```

### 2.5 `ESP_RETURN_ON_ERROR_CLEANUP` — 轻量清理

当清理动作简单（1-2 行），不值得单独写 goto 标签时使用：

```c
esp_err_t modem_sample_signal(modem_handle_t me, int *rssi)
{
    ESP_RETURN_ON_FALSE(me && rssi, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    void *buf = malloc(128);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc failed");

    modem_signal_t signal = {0};
    ESP_RETURN_ON_ERROR_CLEANUP(
        modem_get_signal(me, &signal),
        free(buf)
    );

    *rssi = signal.rssi;
    free(buf);
    return ESP_OK;
}
```

---

## 3. 项目自定义宏

### 3.1 `ESP_LOG_ON_ERROR` — 失败可忽略

ESP-IDF 没有内置"仅记录日志"的宏，项目自定义如下：

```c
#define ESP_LOG_ON_ERROR(x, tag, fmt, ...)                                 \
    do {                                                                   \
        esp_err_t _err_ = (x);                                            \
        if (unlikely(_err_ != ESP_OK)) {                                   \
            ESP_LOGW(tag, "%s:%d: " fmt " (err=%s)",                      \
                     __FUNCTION__, __LINE__, ##__VA_ARGS__,                \
                     esp_err_to_name(_err_));                              \
        }                                                                  \
    } while (0)
```

**适用场景**：失败不影响主流程的操作——deinit、事件通知、信号量释放等：

```c
/* deinit 失败不影响主流程 */
ESP_LOG_ON_ERROR(uart_driver_delete(UART_NUM), TAG, "uart deinit failed");

/* 事件发送失败可忽略 */
ESP_LOG_ON_ERROR(core_post_event(me, LWLTE_EVENT_STARTED, NULL), TAG, "post event failed");
```

---

## 4. 宏使用规则速查

| 场景 | 使用的宏 | 说明 |
|------|---------|------|
| 有资源需清理 | `ESP_GOTO_ON_ERROR` / `ESP_GOTO_ON_FALSE` | 跳转到 `err` 标签 |
| 无资源需清理 | `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` | 直接返回 |
| 清理简单（1-2行） | `ESP_RETURN_ON_ERROR_CLEANUP` | 无需 goto 标签 |
| 失败可忽略 | `ESP_LOG_ON_ERROR` | 仅记录警告日志 |
| 致命错误 | `ESP_ERROR_CHECK` | abort() — 仅 Board Init |
| 直接 `return func()` | 不用宏 | 上层来检查返回值 |

### 不应用宏的情况

| 情况 | 做法 | 示例 |
|------|------|------|
| 需要根据错误码分支处理 | 手动 `if` | `if (ret == ESP_ERR_TIMEOUT) { ... }` |
| 将错误码传播给调用者 | 直接 `return func()` | `return modem_set_apn(modem, 1, apn)` |

---

## 5. goto cleanup 模式规范

### 5.1 硬性规则

- 局部变量名**必须**用 `ret`（`ESP_GOTO_ON_*` 宏内部固定操作 `ret`）
- 跳转标签**统一**用 `err`
- `ret` 必须在函数开头初始化为 `ESP_OK`
- cleanup 区按资源创建的**逆序**释放
- 每个 `free`/`delete` 后置 `NULL`（防御 double-free）

### 5.2 完整模板

```c
esp_err_t service_create(const service_config_t *config, service_t **out)
{
    ESP_RETURN_ON_FALSE(config && out, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = ESP_OK;

    /* 1. 分配自身 */
    service_t *me = calloc(1, sizeof(service_t));
    ESP_GOTO_ON_FALSE(me, ESP_ERR_NO_MEM, err, TAG, "calloc service failed");

    /* 2. 创建资源 A */
    me->resource_a = create_resource_a();
    ESP_GOTO_ON_FALSE(me->resource_a, ESP_ERR_NO_MEM, err, TAG, "create resource_a failed");

    /* 3. 创建资源 B */
    me->resource_b = create_resource_b();
    ESP_GOTO_ON_FALSE(me->resource_b, ESP_ERR_NO_MEM, err, TAG, "create resource_b failed");

    /* 4. 操作可能失败 */
    ret = do_something(me);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "do_something failed");

    *out = me;
    return ESP_OK;

err:
    if (me->resource_b) destroy_resource_b(me->resource_b);
    if (me->resource_a) destroy_resource_a(me->resource_a);
    free(me);
    return ret;
}
```

---

## 6. `modem_*` 包装 API 与 ops 表调用中的错误处理

Core 层只调用 `modem_*` 包装 API，内部多态机制不向 Core 暴露。
`modem_ops` 仅在 Modem 层 wrapper 实现内部使用。二者都返回 `esp_err_t`，调用时遵循统一接口守卫模式（详见 `oop-design.md` 第 4 章）：

### 6.1 Core 调用 `modem_*` 包装 API — 直接传播

```c
/* Core 层通过 modem_* 包装 API 调用，错误向上传播 */
esp_err_t core_refresh_signal(core_handle_t me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && me->modem && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    return modem_get_signal(me->modem, signal);
}
```

### 6.2 Modem wrapper 选填方法 — NULL 检查后提供默认行为

```c
esp_err_t modem_sleep(modem_handle_t me)
{
    ESP_RETURN_ON_FALSE(me && me->ops, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    if (!me->ops->sleep) {
        /* 默认行为：不支持休眠，安静跳过 */
        ESP_LOGD(TAG, "sleep not supported, skip");
        return ESP_OK;
    }
    return me->ops->sleep(me);
}
```

### 6.3 严格接口 — assert 守卫

```c
esp_err_t lwlte_transport_send(transport_t *me, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(me && data, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    assert(me->ops && me->ops->send);  /* 接口契约：子类必须实现 */
    if (!me->ops || !me->ops->send)    /* release 兜底 */
        return ESP_ERR_NOT_SUPPORTED;
    return me->ops->send(me, data, len);
}
```

---

## 7. 禁止事项

| 禁止项 | 替代方案 |
|--------|---------|
| 自定义项目错误码体系 | 使用 ESP-IDF 内置 `esp_err_t` 和标准错误码 |
| 自定义错误检查宏（`LWLTE_RETURN_ON_*` 等） | 使用 `esp_check.h` 的 `ESP_RETURN_ON_*` / `ESP_GOTO_ON_*` |
| `ESP_GOTO_ON_*` 用 `ret` 之外的变量名 | 宏内部固定写 `ret`，否则编译不过 |
| goto 标签不用 `err` | 统一用 `err`，全项目一致 |
| Core 层及以上调用 `ESP_ERROR_CHECK` | 致命 abort 只允许在 Board Init |
| 返回 `int` 表示错误 | 统一返回 `esp_err_t` |

## 8. 事件数据中的 `error_code` 语义

`error_code` is a best-effort diagnostic number carried in event data structs.
For non-error events it is 0. For error events, it may be a CME/CMS code (positive)
from the AT layer or an esp_err_t value (negative). Callers MUST NOT branch on
specific values for control flow.
