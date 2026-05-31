# C 语言 OOP 设计规范

本规范基于[《兆铭嵌入式 C 语言面向对象教程》](https://zhaochengbo.github.io/zhaoming-embedded/)的核心思想编写，规定了 esp-lwlte 项目中用 C 实现面向对象设计的编码规则与代码模板。核心三要素：**封装、继承、多态**。

---

## 1. 封装：句柄模式与信息隐藏

> 参考教程：第3章「手搓 class」、第4章「数据归位」、第5章「HAL 映射」

### 1.1 数据三级分类

所有变量必须归入以下三级之一，禁止出现裸的全局变量（教程第4章）：

| 级别 | 归属 | C 实现 | 示例 |
|------|------|--------|------|
| 实例数据 | 每个对象独有 | struct 字段 | `pin`, `brightness` |
| 模块共享数据 | 整个模块一份 | `static` 文件作用域 | `s_init_count` |
| 只读常量 | 编译期确定 | `static const` | `MAX_PIN` |

**规则**：`.c` 文件顶部的变量声明区域必须按以下顺序排列（教程第4章工业文件结构约定）：

```c
/* 1. 只读常量 */
static const uint32_t MAX_TIMEOUT_MS = 60000;
static const uint8_t  MAX_RETRY     = 3;

/* 2. 模块共享数据 */
static unsigned int s_init_count;
static int          s_debug_flag;

/* 3. 文件私有函数前置声明 */
static void update_internal_state(struct lwlte_module *me);
static bool param_valid(uint32_t param);
```

教程原文："50 个 driver 文件全按这个结构走"——任何工程师看一眼就知道每个变量的归属。

### 1.2 句柄模式（opaque pointer）

对外暴露的模块实例必须使用不完整类型（opaque struct）指针作为句柄，禁止暴露结构体定义（教程第3章）：

**用户公共头文件（`src/include/lwlte.h`）**：

```c
typedef struct lwlte lwlte_t;  /* 前置声明，不暴露内部 */

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);
esp_err_t lwlte_destroy(lwlte_t *me);
```

内部层 factory 可沿用该层既定的指针返回 create 模式，例如 `core_create()`、`modem_air780ep_create()`；不要把内部 factory 形态投射成用户公共 API。

### 1.2.1 生命周期命名规则

本项目统一使用以下生命周期语义：

- 用户门面 API 使用 `esp_err_t xxx_init(const xxx_config_t *config, xxx_t **out)`，例如 `lwlte_air780ep_init()`。
- 独立 opaque 堆对象使用 `xxx_create()` / `xxx_destroy()`；`create` 返回对象指针，失败返回 `NULL`，`destroy` 返回 `esp_err_t`。
- 内嵌对象、基类对象、组合成员使用 `xxx_init(me, ...)` / `xxx_deinit(me)`；二者都返回 `esp_err_t`，不负责分配或释放 `me` 自身。
- `create` 内部可以调用私有或层内 `init`；初始化失败时必须按已成功初始化的反序调用对应 `deinit`，再释放堆内存。
- `destroy` 内部必须调用对应 `deinit`，再释放堆内存；如果清理阶段出现错误，返回第一个错误码。

**私有头文件或 `.c` 文件**：

```c
struct lwlte_xxx {
    lwlte_xxx_config_t config;
    uint32_t           flags;
    /* ... 内部字段 */
};
```

**规则**：所有操作函数第一个参数必须是 `me` 指针（教程第5章 HAL 模式）：

```c
int lwlte_xxx_do_something(lwlte_xxx_t *me, int param);  /* me 指针第一位 */
```

教程原文："同一个函数，传不同的 `me` 指针，操作不同的实例。"

### 1.3 static 信息隐藏

模块内部函数和变量必须用 `static` 限定文件作用域（教程第2章、第4章）：

```c
/* 文件私有函数——外部不可见 */
static int validate_config(const lwlte_config_t *config);
static void handle_internal_event(void *arg);

/* 文件私有变量——外部不可直接访问 */
static lwlte_xxx_t *s_active_instance;
```

如果需要对外暴露模块级共享数据，通过 getter 函数而非 `extern` 变量：

```c
/* 正确：通过函数访问 */
unsigned int lwlte_xxx_get_init_count(void)
{
    return s_init_count;
}

/* 错误：对外暴露全局变量 */
extern unsigned int g_init_count;  /* 禁止 */
```

### 1.4 模块文件结构模板

```c
/**
 * @file lwlte_xxx.c
 * @brief 模块简短描述（中文）
 * @details Module brief description in English
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
/* 1. 只读常量 */
static const uint32_t MAX_XXX = 100;

/* 2. 模块共享数据 */
static unsigned int s_init_count;

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

---

## 2. 继承：结构体嵌套与 container_of

> 参考教程：第6章「代码一半重复」、第12章「向上转型」、第13章「container_of」

### 2.1 结构体嵌套实现单继承

公共字段提取为基类结构体，子类将基类作为**第一个成员**（教程第6章）：

```c
/* 基类：Modem Adapter 通用对象 */
struct modem {
    const modem_ops_t      *ops;          /* vptr，见第3章 */
    at_engine_t            *at;
    SemaphoreHandle_t       lock;
    QueueHandle_t           event_queue;
    modem_event_callback_t  event_cb;
    void                   *event_user_ctx;
    modem_state_t           state;
    const char             *name;
};

/* 子类：Air780EP Modem 实现 */
typedef struct {
    modem_t                  base;        /* 第一个字段——继承 */
    modem_air780ep_config_t  config;
    modem_signal_t           last_signal;
    modem_sim_status_t       last_sim_status;
    modem_reg_status_t       last_reg_status;
    bool                     initialized;
} modem_air780ep_t;
```

**规则**：基类必须放在子类结构体的第 0 偏移位置。C99 §6.7.2.1 保证第一个成员的地址等于结构体地址，这是向上转型的基础。

### 2.2 子类初始化链

子类 init 函数第一行必须调用父类 init（教程第6章）：

```c
static esp_err_t air780ep_object_init(modem_air780ep_t *me,
                                      at_engine_t *at,
                                      const modem_air780ep_config_t *config)
{
    ESP_RETURN_ON_FALSE(me && at && config, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    me->config = *config;
    me->last_signal.rssi = 99;
    me->last_sim_status = MODEM_SIM_UNKNOWN;
    me->last_reg_status = MODEM_REG_UNKNOWN;

    esp_err_t ret = modem_base_init(&me->base, "air780ep", at, &air780ep_ops,
                                    config->event_queue_size,
                                    config->event_task_stack,
                                    config->event_task_priority);
    if (ret != ESP_OK) {
        return ret;
    }

    me->initialized = false;
    return ESP_OK;
}
```

### 2.3 向上转型（Upcasting）

将子类指针转为基类指针——永远写 `&obj.base`，**禁止强转**（教程第12章）：

```c
modem_air780ep_t air780ep;
air780ep_object_init(&air780ep, at, &config);

/* 正确：取 base 成员地址 */
modem_t *base = &air780ep.base;

/* 错误：强转——base 不在偏移 0 时会出错 */
modem_t *bad_base = (modem_t *)&air780ep;  /* 禁止 */
```

教程原文："让编译器自己算偏移，你别去碰。"

### 2.4 向下转型（Downcasting）：MODEM_CONTAINER_OF

当函数收到 `modem_t *` 基类指针，需要反推出具体子类对象时，使用 `MODEM_CONTAINER_OF` 宏（教程第13章）：

```c
/* 放入 Modem 模块私有头文件 */
#define MODEM_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

**三步分解**（教程原文）：
1. `(char *)(ptr)` — 转为字节指针，让减法按字节算
2. `- offsetof(type, member)` — 减去成员在外层结构体中的偏移
3. `(type *)` — 将结果按外层结构体类型解读

**使用示例**：

```c
/* Modem 专用 container_of 宏 */
#define MODEM_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Air780EP 子类方法中从 base 反推自身 */
static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);

    /* 现在可以访问 self->config、self->last_signal 等子类字段。 */
    esp_err_t ret = query_air780ep_signal(self, signal);
    if (ret == ESP_OK) {
        self->last_signal = *signal;
    }
    return ret;
}
```

**关键性质**（教程第13章）：
- `offsetof` 在编译期求值，`container_of` 运行时就是一条减法指令
- 零运行时开销，对比 C++ `dynamic_cast` 的几十 cycle RTTI 查表
- 跨平台位置无关：32 位和 64 位下一字不改照样工作

**Linux 内核增强版**（`include/linux/container_of.h`，供参考）：

```c
#define container_of(ptr, type, member) ({                    \
    void *__mptr = (void *)(ptr);                             \
    static_assert(__same_type(*(ptr), ((type *)0)->member) || \
                  __same_type(*(ptr), void),                  \
                  "pointer type mismatch in container_of()"); \
    ((type *)(__mptr - offsetof(type, member))); })
```

内核版比基础版多了三点：避免重求值（`__mptr`）、编译期类型检查（`static_assert`）、统一指针类型（`void *`）。

### 2.5 继承规则总结

| 规则 | 做法 |
|------|------|
| 提取公共字段 | 新建基类 struct，放共有字段 + ops 指针 |
| 子类嵌套 | 子类第一个字段放基类 |
| 父类 init | 处理基类字段，接收 ops 注入 |
| 子类 init 链 | 第一行调父类 init |
| 向上转型 | `&obj.base`，禁止强转 |
| 向下转型 | `MODEM_CONTAINER_OF(me, modem_air780ep_t, base)` |
| 父类行为函数 | 接收基类指针，所有子类共用 |

这也是 C++ `class 子类 : public 基类` 的底层内存布局——教程原文："把编译器的隐藏动作写到了明面上。"

---

## 3. 多态：ops 操作表与虚函数表

> 参考教程：第9章「ops 操作表」、第10章「ops 放进对象」、第11章「多态完整图景」

### 3.1 演进路径

教程展示了一个从原始到成熟的五步演进：

```
函数指针 → 函数指针传参 → ops 操作表 → ops 放入对象(vptr) → 完整多态
```

本项目直接采用最终形态：**ops 表 + vptr 落地**。

### 3.2 ops 操作表定义

需要可替换行为的模块必须定义 ops 结构体（函数指针表），作为该模块的"虚函数表"（教程第9章）：

```c
/* src/modem/modem_priv.h — Modem 内部多态接口 */
typedef esp_err_t (*modem_no_arg_fn)(modem_t *me);
typedef esp_err_t (*modem_get_signal_fn)(modem_t *me,
                                         modem_signal_t *signal);
typedef esp_err_t (*modem_set_apn_fn)(modem_t *me, uint8_t cid,
                                      const char *apn);
typedef esp_err_t (*modem_pdp_cid_fn)(modem_t *me, uint8_t cid);

typedef struct modem_ops {
    modem_no_arg_fn destroy;
    modem_no_arg_fn init;
    modem_no_arg_fn reset;
    modem_get_signal_fn get_signal;
    modem_set_apn_fn set_apn;
    modem_pdp_cid_fn activate_pdp;
    modem_pdp_cid_fn deactivate_pdp;
} modem_ops_t;
```

**设计原则**：ops 表中区分必填和选填字段（教程第14章）：
- **必填**：没有合理默认行为的操作（如 `init`、`get_signal`、`activate_pdp`）
- **选填**：有合理默认行为或模块可能不支持的操作（如 `reset` 可返回 `ESP_ERR_NOT_SUPPORTED`）

### 3.3 vptr 落地

将 ops 指针作为对象结构体的**第一个字段**（教程第10章）：

```c
/* src/modem/modem_priv.h — 基类持有 vptr */
struct modem {
    const modem_ops_t *ops;  /* vptr：指向具体模块操作表 */
    at_engine_t       *at;
    modem_state_t      state;
    QueueHandle_t      event_queue;
};
```

具体子类把基类放在第一个字段，并在创建时注入 ops（教程第10章）：

```c
typedef struct {
    modem_t                 base;    /* 第一个字段，实现向上转型 */
    modem_air780ep_config_t config;
    modem_signal_t          last_signal;
} modem_air780ep_t;

static esp_err_t modem_base_init(modem_t *me,
                                 const modem_ops_t *ops,
                                 at_engine_t *at)
{
    if (!me || !ops || !at) {
        return ESP_ERR_INVALID_ARG;
    }

    me->ops = ops;  /* vptr 注入——多态的根 */
    me->at = at;
    me->state = MODEM_STATE_CREATED;
    me->event_queue = xQueueCreate(8, sizeof(modem_event_t));
    if (!me->event_queue) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
```

### 3.4 具体 Modem 实现

Air780EP 子类实现 ops 表为 `static const`（教程第10章、第14章 ops 表必须用 `static const`）：

```c
/* src/modem/modem_air780ep.c */

static const modem_ops_t air780ep_ops = {
    .destroy        = air780ep_destroy,
    .start          = air780ep_start,
    .reset          = air780ep_reset,
    .get_signal     = air780ep_get_signal,
    .set_apn        = air780ep_set_apn,
    .activate_pdp   = air780ep_activate_pdp,
    .deactivate_pdp = air780ep_deactivate_pdp,
};

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config)
{
    modem_air780ep_t *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    esp_err_t ret = modem_base_init(&self->base, &air780ep_ops, at);
    if (ret != ESP_OK) {
        free(self);
        return NULL;
    }

    self->config = *config;
    return &self->base;
}
```

**规则**：ops 表必须 `static const`——链接到 `.rodata` 只读段防篡改，所有同型对象共享一份表。`const` 锁内容不锁指针，允许 init 时赋值（教程第10章）。

### 3.5 多态调用

所有调用统一为 `me->ops->method(me, ...)` 模式（教程第11章）：

```c
/* Core 只调用 modem_* 层间包装 API，不知道具体模块型号。 */
esp_err_t modem_get_signal(modem_t *me, modem_signal_t *signal)
{
    ESP_RETURN_ON_FALSE(me && signal, ESP_ERR_INVALID_ARG, TAG, "NULL argument");
    ESP_RETURN_ON_FALSE(me->ops && me->ops->get_signal,
                        ESP_ERR_NOT_SUPPORTED, TAG, "get_signal not supported");

    return me->ops->get_signal(me, signal);
}

/* Air780EP 子类方法中从 base 反推自身，并直接调用下层 AT Engine。 */
static esp_err_t air780ep_get_signal(modem_t *me, modem_signal_t *signal)
{
    modem_air780ep_t *self = MODEM_CONTAINER_OF(me, modem_air780ep_t, base);

    char *lines[4];
    at_response_t resp = {
        .max_lines = 4,
        .lines     = lines,
    };
    esp_err_t ret = at_engine_send_cmd(self->base.at, "AT+CSQ", &resp, 3000);
    ESP_RETURN_ON_ERROR(ret, TAG, "send AT+CSQ failed");

    return parse_csq(&resp, signal);
}
```

**关键效果**（教程第12章）：Core 只认识 `modem_t` 和 `modem_*` 包装 API，不认识 Air780EP、SIM800 等具体模块。换模块时替换具体 Modem 子类和 Facade factory 装配，Core 代码零改动。

### 3.6 多态规则总结

| 规则 | 做法 |
|------|------|
| ops 表定义 | 复杂签名先定义 `xxx_fn` 函数指针类型，`struct xxx_ops` 中只放字段名和类型名 |
| vptr 位置 | 对象结构体第一个字段：`const struct xxx_ops *ops` |
| ops 实例化 | `static const struct xxx_ops xxx_ops = { .method = impl }` |
| ops 注入 | init 时传入：`me->ops = ops` |
| ops 调用 | `me->ops->method(me, ...)` |
| ops 存储 | `static const`，放 `.rodata` |

---

## 4. 抽象接口：纯虚函数模式

> 参考教程：第14章「纯虚与抽象类」

### 4.1 三种策略

教程第14章给出了虚函数不实现时的三种处理策略：

| 策略 | C 写法 | 子类不填后果 | 适用场景 |
|------|--------|-------------|----------|
| 必填 | 统一接口 `assert` 检查 | 调试期 assert 失败 | 没有合理默认行为 |
| 选填 | 统一接口检查 NULL，提供默认 | 继承父类默认行为 | 有合理默认行为 |
| 全必填（接口） | ops 表所有字段都 assert | 子类必须实现每一个 | 严格的接口契约 |

判据（教程原文）："没有合理的默认行为 → 必填 → 接口风格。有合理默认 → 选填 → 混合。"

### 4.2 策略一：必填（require）

```c
/* 统一接口——assert 守卫 */
esp_err_t modem_require_start(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    assert(me->ops && me->ops->start &&
           "modem.start is required - subclass must implement");
    return me->ops->start(me);
}
```

带 Release 兜底的稳健写法（教程第14章）：

```c
esp_err_t modem_require_start(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    assert(me->ops && me->ops->start);
    if (!me->ops || !me->ops->start)
        return ESP_ERR_NOT_SUPPORTED;  /* release 构建的最后一道闸 */

    return me->ops->start(me);
}
```

### 4.3 策略二：选填（optional）

```c
esp_err_t modem_reset_optional(modem_t *me)
{
    ESP_RETURN_ON_FALSE(me && me->ops, ESP_ERR_INVALID_ARG, TAG, "NULL argument");

    if (!me->ops->reset) {
        /* 默认行为：该模块不支持软件复位。 */
        ESP_LOGD(TAG, "reset not supported by %s", me->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return me->ops->reset(me);
}
```

子类 ops 表有意不填选填字段（C 的零初始化自动置 NULL）：

```c
static const modem_ops_t diagnostic_modem_ops = {
    .start = diagnostic_modem_start,
    .get_signal = diagnostic_modem_get_signal,
    /* .reset 故意不填——该诊断实现无复位能力，自动为 NULL */
};
```

### 4.4 策略三：全必填（严格接口）

```c
/* 接口契约：所有函数必须实现 */
struct lwlte_channel_ops {
    int (*connect)(struct lwlte_channel *me, const char *host, uint16_t port);
    int (*send)(struct lwlte_channel *me, const uint8_t *data, size_t len);
    int (*recv)(struct lwlte_channel *me, uint8_t *buf, size_t len,
                uint32_t timeout_ms);
    int (*close)(struct lwlte_channel *me);
};

/* 每个统一接口都 assert */
int lwlte_channel_connect(struct lwlte_channel *me,
                          const char *host, uint16_t port)
{
    if (!me || !host)
        return -1;
    assert(me->ops && me->ops->connect &&
           "channel.connect is part of the interface contract");
    return me->ops->connect(me, host, port);
}

int lwlte_channel_send(struct lwlte_channel *me,
                       const uint8_t *data, size_t len)
{
    if (!me || !data)
        return -1;
    assert(me->ops && me->ops->send &&
           "channel.send is part of the interface contract");
    return me->ops->send(me, data, len);
}
```

### 4.5 MCU 上的 assert 替换

教程第14章建议用工业宏替代标准 `assert`，便于统一记录日志并按 ESP-IDF 方式处理致命错误：

```c
#define LWLTE_ASSERT(cond)                         \
    do {                                           \
        if (!(cond)) {                             \
            LWLTE_LOGE("ASSERT", "%s:%d: %s",     \
                        __FILE__, __LINE__, #cond);  \
            esp_restart();                         \
        }                                          \
    } while (0)
```

---

## 5. 工程整合：门面装配与直接 ESP-IDF 集成

> 参考教程：第15章的装配思想；本项目不做跨 RTOS 抽象，组件内部直接使用 ESP-IDF / FreeRTOS / C 标准库 API。

### 5.1 App / Facade / 内部分层

本项目保留"业务逻辑与装配分离"的思想：业务代码只操作 `lwlte_t`，板级初始化或 App 自有配置代码可通过公开 config 填写 UART/GPIO，真正的依赖树装配集中在 LWLTE Facade 模块 factory 内部。

```
应用层 (main.c / app.c)
    │  #include "lwlte.h"
    │  业务代码只调用用户操作 API；初始化代码可填写公开 UART/GPIO 配置
    ▼
门面 factory (src/lwlte/lwlte_air780ep.c)
    │  composition root：创建 AT Engine、Modem、Core 并持有依赖树
    ▼
内部层 (Core / Modem / AT Engine)
    │  分层调用；各层可直接使用 ESP-IDF / FreeRTOS API
    ▼
硬件层 (ESP-IDF FreeRTOS / UART / GPIO)
```

### 5.2 内部 service 装配模式

内部 service 不作为 App 可 include 的公共接口暴露。Core 通过 `src/core/core.h` 提供层间 API，由 Facade 模块 factory 创建并持有：

```c
/* src/core/core.h — 层间头文件 */
typedef struct core core_t;

core_t *core_create(const core_config_t *config, modem_t *modem);
esp_err_t core_destroy(core_t *me);
esp_err_t core_start(core_t *me);
esp_err_t core_connect(core_t *me);

/* src/core/core_priv.h — Core 模块私有结构 */
struct core {
    core_config_t config;
    modem_t      *modem;
    core_fsm_t    fsm;
    net_mgr_t     net_mgr;
    pdp_mgr_t     pdp_mgr;
};

/* src/core/core.c — 内部实现 */
core_t *core_create(const core_config_t *config, modem_t *modem)
{
    core_t *me = calloc(1, sizeof(core_t));
    if (!me) return NULL;

    me->config = *config;
    me->modem = modem;
    me->fsm.queue = xQueueCreate(config->fsm_queue_size, sizeof(core_fsm_sig_t));
    if (!me->fsm.queue) {
        free(me);
        return NULL;
    }

    /* Core 只通过 modem_* 层间 API 使用 Modem。 */
    modem_register_event_callback(modem, core_modem_event_cb, me);
    /* ... */
    return me;
}
```

### 5.3 用户门面 factory / 板级初始化

App 或板级文件只调用用户门面 factory；真正认识 AT Engine、Modem、具体模块 factory 和 Core 的 composition root 在 Facade 模块 factory 内部：

```c
/* app_lte.c — App 只看到用户 API */

lwlte_t *g_lte;

int lwlte_board_init(void)
{
    lwlte_air780ep_config_t config = {
        .uart_num       = CONFIG_LWLTE_UART_NUM,
        .uart_tx_pin    = CONFIG_LWLTE_TX_GPIO,
        .uart_rx_pin    = CONFIG_LWLTE_RX_GPIO,
        .uart_baud_rate = 115200,
        .apn            = CONFIG_LWLTE_APN,
        .primary_cid    = 1,
        .auto_connect   = true,
    };
    if (lwlte_air780ep_init(&config, &g_lte) != ESP_OK) return -1;

    return 0;
}
```

**规则**：板级初始化只填 `lwlte_air780ep_config_t` 并调用用户门面 factory。换模块时新增或替换对应的门面 factory 与具体 Modem 子类；App 和 Core 不直接认识模块型号。

### 5.4 业务 App 代码边界

业务 App 代码只 include `lwlte.h` 并操作 `lwlte_t`，不暴露任何子类类型。板级初始化或 App 自有配置代码也 include `lwlte.h`，并填写其中声明的模块配置如 `lwlte_air780ep_config_t`。

```c
/* app.c — 业务操作代码 */

#include "lwlte.h"

void app_main(void)
{
    /* 只通过公共句柄操作 */
    esp_err_t err = lwlte_connect(g_lte);
    if (err != ESP_OK) {
        /* 业务侧决定如何降级或重试。 */
    }

    /* 完全不知道底下的 AT Engine / Modem / Core 装配细节 */
}
```

业务代码不直接操作 GPIO、UART 或 AT 指令；这些配置和装配细节留在初始化代码与 Facade factory 中。

### 5.5 生命周期模板

用户门面 factory 返回 `esp_err_t`，通过 out 参数交付 `lwlte_t *`，便于区分参数错误、内存不足、ready 超时和下层初始化失败：

```c
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);
esp_err_t lwlte_destroy(lwlte_t *me);

esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte)
{
    ESP_RETURN_ON_FALSE(config && out_lte, ESP_ERR_INVALID_ARG, TAG,
                        "NULL argument");

    esp_err_t ret = ESP_OK;
    lwlte_t *me = calloc(1, sizeof(*me));
    ESP_RETURN_ON_FALSE(me, ESP_ERR_NO_MEM, TAG, "calloc facade failed");

    ret = create_at_modem_core_tree(me, config);
    if (ret != ESP_OK) {
        cleanup_after_failure(me);
        return ret;
    }

    if (config->auto_connect) {
        ret = lwlte_connect(me);
        if (ret != ESP_OK) {
            cleanup_after_failure(me);
            return ret;
        }
    }

    *out_lte = me;
    return ESP_OK;
}
```

内部 service 和模块 factory 可以沿用本层既定的指针返回模式；失败原因由日志和清理路径记录，调用方按依赖树反向释放：

```c
core_t *core_create(const core_config_t *config, modem_t *modem);
modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config);

modem_t *modem_air780ep_create(at_engine_t *at,
                               const modem_air780ep_config_t *config)
{
    modem_air780ep_t *self = calloc(1, sizeof(*self));
    if (!self) return NULL;

    esp_err_t ret = modem_base_init(&self->base, "air780ep", at, &air780ep_ops,
                                    config->event_queue_size,
                                    config->event_task_stack,
                                    config->event_task_priority);
    if (ret != ESP_OK) {
        free(self);
        return NULL;
    }

    self->config = *config;
    return &self->base;
}
```

---

## 6. 禁止事项

以下模式在旧项目中出现过，新项目中**严格禁止**：

| 禁止项 | 原因 | 替代方案 |
|--------|------|----------|
| `extern` 全局上下文变量 | 破坏封装，无法多实例 | 句柄模式 `lwlte_xxx_t *me` |
| 模块间直接访问 `s_ctx.xxx` | 紧耦合 | 通过公开/层间 API 传递句柄；只有真实子类 downcast 才用 `MODEM_CONTAINER_OF` |
| 自造系统 API 包装层或 port ops 表 | 本项目 ESP-IDF-only，增加抽象会制造无效复杂度 | 组件内部直接使用 ESP-IDF / FreeRTOS / C 标准库 API |
| 强制类型转换做向上转型 | base 不在偏移 0 时崩溃 | `&obj.base` |
| ops 表不加 `const` | 运行时可被篡改 | `static const struct xxx_ops` |
| 尚未形成稳定重复抽象就提公共基类 | 过早抽象会误导组合关系 | 保持本模块实现；只有出现具体重复且接口稳定时再提公共 helper |
| 单例全局变量 `s_ctx` | 无法测试、无法多实例 | `create` 返回句柄 |
