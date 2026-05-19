# 代码规范与模板

**本项目为纯 C 语言项目，不使用 C++。**

## 头文件模板 (.h)

```c
/**
 * @file
 * @brief  // brief写中文
 * @details // details写brief的英文翻译
 * @author
 * @date
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
*********************/

/*********************
 *      DEFINES
*********************/

/**********************
 *      TYPEDEFS
**********************/

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

## 源文件模板 (.c)

```c
/**
 * @file
 * @brief  // brief写中文
 * @details // details写brief的英文翻译
 * @author
 * @date
 */

/*********************
 *      INCLUDES
*********************/

/*********************
 *      DEFINES
*********************/

/**********************
 *      TYPEDEFS
**********************/

/**********************
 *  STATIC PROTOTYPES
**********************/

/**********************
 *  STATIC VARIABLES
**********************/

/**********************
 *      MACROS
**********************/

/**********************
 *   GLOBAL FUNCTIONS
**********************/

/**********************
 *   STATIC FUNCTIONS
**********************/
```

## Static 函数注释规范

**重要：static 函数的 Doxygen 注释应放在 `STATIC PROTOTYPES` 区域，而不是函数定义处。**

这样做的好处：
- 函数声明和文档放在一起，便于查阅
- 函数定义处保持简洁，专注于实现
- 与头文件公共 API 的注释风格一致

```c
/**********************
 *  STATIC PROTOTYPES
 **********************/

/**
 * @brief 初始化网络连接
 * @details Initialize network connection
 * @param[in] config 配置参数； Configuration parameters
 * @return
 *         - LWLTE_OK: 成功； Success
 *         - LWLTE_ERROR: 失败； Failure
 */
static lwlte_err_t init_network(const config_t *config);

/**
 * @brief 处理数据接收
 * @details Handle data reception
 * @param[in] data 数据缓冲区； Data buffer
 * @param[in] len 数据长度； Data length
 */
static void handle_rx_data(const uint8_t *data, size_t len);

/**********************
 *  STATIC VARIABLES
 **********************/
...

/**********************
 *   STATIC FUNCTIONS
 **********************/

static lwlte_err_t init_network(const config_t *config)
{
    /* 实现代码 - 无需重复注释 */
    ...
}

static void handle_rx_data(const uint8_t *data, size_t len)
{
    /* 实现代码 - 无需重复注释 */
    ...
}
```

## Doxygen 注释格式

头文件中的公共成员，以及源文件中的 typedef、static 成员使用以下格式：

```c
/**
 * @brief // 写中文
 * @details // 写brief的英文翻译
 * @note  // 中文写解释
 * @param[in]
 * @param[out]
 * @return
 *         - LWLTE_OK:
 *         - LWLTE_NOT_INITIALIZED:
 *         - // and so on
 */
```

### 文件头格式

```c
/**
 * @file lwlte_xxx.h
 * @brief 模块的简短描述（中文）
 * @details Module brief description in English
 * @author Author Name
 * @date YYYY-MM-DD
 */
```

### 枚举注释格式

每个枚举值使用行尾注释，格式为 `/**< 中文描述； English description */`：

```c
/**
 * @brief 枚举的简短描述（中文）
 * @details Enum brief description in English
 */
typedef enum {
    LWLTE_STATE_IDLE = 0,      /**< 空闲状态； Idle state */
    LWLTE_STATE_CONNECTING,    /**< 连接中； Connecting */
    LWLTE_STATE_CONNECTED,     /**< 已连接； Connected */
} lwlte_state_t;
```

### 结构体注释格式

每个成员使用行尾注释：

```c
/**
 * @brief 结构体的简短描述（中文）
 * @details Struct brief description in English
 */
typedef struct {
    const char* client_id;    /**< 客户端 ID； Client ID */
    uint32_t port;            /**< 端口号； Port number */
    uint32_t timeout_ms;      /**< 超时时间（毫秒）； Timeout in milliseconds */
} lwlte_config_t;
```

### 函数注释格式

```c
/**
 * @brief 函数的简短描述（中文）
 * @details Function brief description in English
 * @note 可选的注意事项
 * @param[in] param1 参数说明
 * @param[out] param2 输出参数说明
 * @return
 *         - LWLTE_OK: 成功
 *         - LWLTE_INVALID_ARG: 参数无效
 */
```

### 宏定义注释格式

简单的宏使用行尾注释，复杂的宏使用块注释：

```c
/** 标志位定义； Flags definition */
#define LWLTE_FLAG_ENABLED    LWLTE_BIT0   /**< 已启用； Enabled */
#define LWLTE_FLAG_CONNECTED  LWLTE_BIT1   /**< 已连接； Connected */

/**
 * @brief 检查是否已初始化
 * @details Check if initialized
 */
#define LWLTE_CHECK_INIT() \
    do { \
        if (!s_ctx.initialized) { \
            return LWLTE_NOT_INITIALIZED; \
        } \
    } while(0)
```

### 注释语言规则

- **@brief**: 中文
- **@details**: 英文（@brief 的翻译）
- **@note**: 中文
- **@param/@return**: 中文
- **行尾注释**: `/**< 中文； English */` 格式

## 错误处理规范

本项目采用类似 ESP-IDF 的错误处理机制，所有 API 返回 `lwlte_err_t` 枚举类型。

### 错误检查宏使用规则

| 场景 | 宏 | 说明 |
|------|-----|------|
| 有资源需清理 | `LWLTE_GOTO_ON_FALSE` / `LWLTE_GOTO_ON_ERROR` | 跳转到 cleanup 标签 |
| 无资源需清理 | `LWLTE_RETURN_ON_FALSE` / `LWLTE_RETURN_ON_ERROR` | 直接返回 |
| 失败可忽略 | `LWLTE_LOG_ON_ERROR` | 仅记录日志，不中断流程 |
| 必须成功（致命错误） | `LWLTE_ERROR_CHECK` | **abort() 终止程序** |

### goto cleanup 模式

当函数中需要创建多个资源时，使用 goto cleanup 模式：

```c
lwlte_err_t some_init(void)
{
    lwlte_err_t err = LWLTE_OK;

    s_ctx.queue = lwlte_sys_queue_create(...);
    LWLTE_GOTO_ON_FALSE(s_ctx.queue, LWLTE_NO_MEM, cleanup, TAG, "Failed to create queue");

    s_ctx.sem = lwlte_sys_semaphore_create();
    LWLTE_GOTO_ON_FALSE(s_ctx.sem, LWLTE_NO_MEM, cleanup, TAG, "Failed to create semaphore");

    LWLTE_ERROR_CHECK(lwlte_ll_uart_init(...));

    return LWLTE_OK;

cleanup:
    if (s_ctx.sem) lwlte_sys_semaphore_delete(s_ctx.sem);
    if (s_ctx.queue) lwlte_sys_queue_delete(s_ctx.queue);
    memset(&s_ctx, 0, sizeof(s_ctx));
    return err;
}
```

### 不需要宏的情况

| 情况 | 原因 | 示例 |
|------|------|------|
| 直接 `return func_call();` | 交给上层检查 | `return lwlte_core_fsm_post_event(event);` |
| 条件判断后处理 | 已有特定逻辑 | `if (lwlte_sys_queue_recv(...) == LWLTE_OK) { ... }` |

### 详细文档

参见 [docs/err.md](../err.md) 获取完整的错误处理机制文档。
