# 错误处理机制

本项目仅面向 ESP-IDF 平台，直接使用 ESP-IDF 内置的 `esp_err_t` 和 `esp_check.h` 错误检查宏，不自定义错误码体系。

---

## 1. 返回值约定

### 1.1 所有公共 API 返回 `esp_err_t`

```c
esp_err_t lwlte_core_start(lwlte_core_t *me);
esp_err_t lwlte_mqtt_publish(lwlte_mqtt_t *me, const lwlte_mqtt_req_t *req);
esp_err_t lwlte_net_get_signal(lwlte_net_t *me, int *rssi);
```

### 1.2 ops 表方法统一返回 `esp_err_t`

```c
struct modem_ops {
    esp_err_t (*init)(modem_t *me);
    esp_err_t (*get_signal)(modem_t *me, int *rssi, int *ber);
    esp_err_t (*set_apn)(modem_t *me, const char *apn);
    esp_err_t (*connect)(modem_t *me);
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

**规则**：只能在 Board Init 或 Port 层初始化中使用。Core 层及以上禁止调用 `ESP_ERROR_CHECK`——应用层不应有 `abort()` 调用。

### 2.3 `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` — 错误返回

用于无需清理资源的场景：

```c
esp_err_t lwlte_core_send_cmd(lwlte_core_t *me, const char *cmd)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "me is NULL");
    ESP_RETURN_ON_FALSE(cmd, ESP_ERR_INVALID_ARG, TAG, "cmd is NULL");

    esp_err_t ret = me->modem->ops->send_cmd(me->modem, cmd, 3000);
    ESP_RETURN_ON_ERROR(ret, TAG, "send_cmd failed: %s", cmd);

    return ESP_OK;
}
```

### 2.4 `ESP_GOTO_ON_ERROR` / `ESP_GOTO_ON_FALSE` — 跳转清理

用于有资源需释放的场景。**硬性要求**：使用这些宏的函数必须在开头定义 `esp_err_t ret = ESP_OK;`，标签统一用 `err`。

```c
esp_err_t lwlte_core_create(const lwlte_core_config_t *config,
                             lwlte_modem_t *modem,
                             lwlte_core_t **out_core)
{
    ESP_RETURN_ON_FALSE(config && modem && out_core, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = ESP_OK;

    lwlte_core_t *me = calloc(1, sizeof(lwlte_core_t));
    ESP_GOTO_ON_FALSE(me, ESP_ERR_NO_MEM, err, TAG, "calloc core failed");

    me->fsm_queue = xQueueCreate(CONFIG_LWLTE_FSM_QUEUE_SIZE, sizeof(sig_item_t));
    ESP_GOTO_ON_FALSE(me->fsm_queue, ESP_ERR_NO_MEM, err, TAG, "xQueueCreate fsm_queue failed");

    ret = lwlte_core_register_urc(me);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "register URC failed");

    *out_core = me;
    return ESP_OK;

err:
    if (me->fsm_queue) vQueueDelete(me->fsm_queue);
    free(me);
    return ret;
}
```

### 2.5 `ESP_RETURN_ON_ERROR_CLEANUP` — 轻量清理

当清理动作简单（1-2 行），不值得单独写 goto 标签时使用：

```c
esp_err_t lwlte_net_get_signal(lwlte_net_t *me, int *rssi)
{
    ESP_RETURN_ON_FALSE(me && rssi, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    void *buf = malloc(128);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "malloc failed");

    ESP_RETURN_ON_ERROR_CLEANUP(
        me->modem->ops->get_signal(me->modem, rssi, NULL),
        free(buf)
    );

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
ESP_LOG_ON_ERROR(lwlte_core_post_event(me, LWLTE_EVENT_STARTED), TAG, "post event failed");
```

---

## 4. 宏使用规则速查

| 场景 | 使用的宏 | 说明 |
|------|---------|------|
| 有资源需清理 | `ESP_GOTO_ON_ERROR` / `ESP_GOTO_ON_FALSE` | 跳转到 `err` 标签 |
| 无资源需清理 | `ESP_RETURN_ON_ERROR` / `ESP_RETURN_ON_FALSE` | 直接返回 |
| 清理简单（1-2行） | `ESP_RETURN_ON_ERROR_CLEANUP` | 无需 goto 标签 |
| 失败可忽略 | `ESP_LOG_ON_ERROR` | 仅记录警告日志 |
| 致命错误 | `ESP_ERROR_CHECK` | abort() — 仅 Port/Boad Init |
| 直接 `return func()` | 不用宏 | 上层来检查返回值 |

### 不应用宏的情况

| 情况 | 做法 | 示例 |
|------|------|------|
| 需要根据错误码分支处理 | 手动 `if` | `if (ret == ESP_ERR_TIMEOUT) { ... }` |
| 将错误码传播给调用者 | 直接 `return func()` | `return modem->ops->send_cmd(...)` |

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
esp_err_t lwlte_xxx_create(const lwlte_xxx_config_t *config,
                            lwlte_platform_t *platform,
                            lwlte_xxx_t **out)
{
    ESP_RETURN_ON_FALSE(config && platform && out, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    esp_err_t ret = ESP_OK;

    /* 1. 分配自身 */
    lwlte_xxx_t *me = calloc(1, sizeof(lwlte_xxx_t));
    ESP_GOTO_ON_FALSE(me, ESP_ERR_NO_MEM, err, TAG, "calloc xxx failed");

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

## 6. ops 表调用中的错误处理

ops 表方法全部返回 `esp_err_t`。调用时遵循统一接口守卫模式（详见 `oop-design.md` 第 4 章）：

### 6.1 必填方法 — 直接调用并传播

```c
/* 必填方法：上层直接通过 ops 调用，错误向上传播 */
esp_err_t lwlte_core_set_apn(lwlte_core_t *me, const char *apn)
{
    ESP_RETURN_ON_FALSE(me && apn, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    return me->modem->ops->set_apn(me->modem, apn);
}
```

### 6.2 选填方法 — NULL 检查后提供默认行为

```c
esp_err_t lwlte_modem_sleep(modem_t *me)
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
| 自定义 `LWLTE_ERR_*` 错误码体系 | 使用 ESP-IDF 内置 `esp_err_t` 和标准错误码 |
| 自定义错误检查宏（`LWLTE_RETURN_ON_*` 等） | 使用 `esp_check.h` 的 `ESP_RETURN_ON_*` / `ESP_GOTO_ON_*` |
| `ESP_GOTO_ON_*` 用 `ret` 之外的变量名 | 宏内部固定写 `ret`，否则编译不过 |
| goto 标签不用 `err` | 统一用 `err`，全项目一致 |
| Core 层及以上调用 `ESP_ERROR_CHECK` | 致命 abort 只允许在 Port / Board Init |
| 返回 `int` 表示错误 | 统一返回 `esp_err_t` |
