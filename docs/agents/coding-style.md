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
 *         - ESP_OK: 成功； Success
 *         - ESP_FAIL: 失败； Failure
 */
static esp_err_t init_network(const config_t *config);

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

static esp_err_t init_network(const config_t *config)
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
 *         - ESP_OK:
 *         - ESP_ERR_INVALID_STATE:
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
    LWLTE_XXX_STATE_IDLE = 0,      /**< 空闲状态； Idle state */
    LWLTE_XXX_STATE_CONNECTING,    /**< 连接中； Connecting */
    LWLTE_XXX_STATE_CONNECTED,     /**< 已连接； Connected */
} lwlte_xxx_state_t;
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
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
```

### 宏定义注释格式

简单的宏使用行尾注释，复杂的宏使用块注释：

```c
/** 标志位定义； Flags definition */
#define LWLTE_XXX_FLAG_ENABLED    BIT(0)   /**< 已启用； Enabled */
#define LWLTE_XXX_FLAG_CONNECTED  BIT(1)   /**< 已连接； Connected */

/**
 * @brief 检查是否已初始化
 * @details Check if initialized
 */
#define LWLTE_XXX_CHECK_INIT(me) \
    do { \
        if (!(me)->initialized) { \
            return ESP_ERR_INVALID_STATE; \
        } \
    } while(0)
```

### 注释语言规则

- **@brief**: 中文
- **@details**: 英文（@brief 的翻译）
- **@note**: 中文
- **@param/@return**: 中文
- **行尾注释**: `/**< 中文； English */` 格式
