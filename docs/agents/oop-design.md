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

**公共头文件（`lwlte_xxx.h`）**：

```c
typedef struct lwlte_xxx lwlte_xxx_t;  /* 前置声明，不暴露内部 */

lwlte_xxx_t *lwlte_xxx_init(const lwlte_xxx_config_t *config);
void         lwlte_xxx_deinit(lwlte_xxx_t *me);
```

**私有头文件或 `.c` 文件**：

```c
struct lwlte_xxx {
    lwlte_xxx_config_t config;
    lwlte_sys_flags_t  flags;
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
/* 基类 */
struct lwlte_module_base {
    const struct lwlte_module_ops *ops;  /* vptr，见第3章 */
    const char                    *name;
    lwlte_module_state_t           state;
};

/* 子类：AT 命令管理器 */
struct lwlte_at_manager {
    struct lwlte_module_base base;   /* 第一个字段——继承 */
    lwlte_at_state_t         at_state;
    lwlte_at_request_t      *current;
    /* ... AT 特有字段 */
};

/* 子类：网络管理器 */
struct lwlte_net_manager {
    struct lwlte_module_base base;   /* 第一个字段——继承 */
    uint32_t                 activation_id;
    uint8_t                  retry_count;
    /* ... 网络特有字段 */
};
```

**规则**：基类必须放在子类结构体的第 0 偏移位置。C99 §6.7.2.1 保证第一个成员的地址等于结构体地址，这是向上转型的基础。

### 2.2 子类初始化链

子类 init 函数第一行必须调用父类 init（教程第6章）：

```c
int lwlte_at_manager_init(struct lwlte_at_manager *me,
                           const char *name,
                           lwlte_core_t *core)
{
    int rc = lwlte_module_base_init(&me->base, name, &at_ops);
    if (rc != 0)
        return rc;

    me->core     = core;
    me->at_state = LWLTE_AT_STATE_IDLE;
    /* ... 子类自己的初始化 */
    return 0;
}
```

### 2.3 向上转型（Upcasting）

将子类指针转为基类指针——永远写 `&obj.base`，**禁止强转**（教程第12章）：

```c
struct lwlte_at_manager at_mgr;
lwlte_at_manager_init(&at_mgr, "at", core);

/* 正确：取 base 成员地址 */
struct lwlte_module_base *base = &at_mgr.base;

/* 错误：强转——base 不在偏移 0 时会出错 */
struct lwlte_module_base *base = (struct lwlte_module_base *)&at_mgr;  /* 禁止 */
```

教程原文："让编译器自己算偏移，你别去碰。"

### 2.4 向下转型（Downcasting）：container_of

当函数收到基类指针，需要反推出子类对象时，使用 `container_of` 宏（教程第13章）：

```c
/* container_of 宏定义（放入公共头文件） */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```

**三步分解**（教程原文）：
1. `(char *)(ptr)` — 转为字节指针，让减法按字节算
2. `- offsetof(type, member)` — 减去成员在外层结构体中的偏移
3. `(type *)` — 将结果按外层结构体类型解读

**使用示例**：

```c
/* 基类上下文 */
struct lwlte_module_base {
    const struct lwlte_module_ops *ops;
    const char                    *name;
    lwlte_module_state_t           state;
};

/* AT 管理器——子类 */
struct lwlte_at_manager {
    struct lwlte_module_base base;
    lwlte_at_state_t         at_state;
    lwlte_at_request_t      *current;
};

/* 子类方法中从 base 反推自身 */
static int at_on_event(struct lwlte_module_base *me, void *event)
{
    struct lwlte_at_manager *self =
        container_of(me, struct lwlte_at_manager, base);

    /* 现在可以访问 self->current, self->at_state 等子类字段 */
    if (self->at_state != LWLTE_AT_STATE_IDLE)
        return -1;
    return 0;
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
| 向下转型 | `container_of(me, 子类类型, base)` |
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
/* lwlte_platform.h — 平台抽象接口 */
struct lwlte_platform_ops {
    /* 线程 */
    lwlte_sys_thread_t (*thread_create)(lwlte_sys_thread_fn_t fn,
                                        const lwlte_sys_thread_cfg_t *cfg);
    void               (*thread_delete)(lwlte_sys_thread_t t);
    void               (*thread_sleep)(int ms);

    /* 内存 */
    void *(*mem_malloc)(size_t size);
    void  (*mem_free)(void *ptr);

    /* 队列 */
    lwlte_sys_queue_t (*queue_create)(size_t item_size, uint32_t max_items);
    int               (*queue_send)(lwlte_sys_queue_t q, const void *item,
                                    uint32_t timeout_ms);
    int               (*queue_recv)(lwlte_sys_queue_t q, void *item,
                                    uint32_t timeout_ms);
    void              (*queue_delete)(lwlte_sys_queue_t q);

    /* 互斥锁 */
    lwlte_sys_mutex_t (*mutex_create)(void);
    int               (*mutex_lock)(lwlte_sys_mutex_t m, uint32_t timeout_ms);
    int               (*mutex_unlock)(lwlte_sys_mutex_t m);
    void              (*mutex_delete)(lwlte_sys_mutex_t m);
};
```

**设计原则**：ops 表中区分必填和选填字段（教程第14章）：
- **必填**：没有合理默认行为的操作（如 `thread_create`）
- **选填**：有合理默认行为的操作（如 `thread_sleep` 可默认用 `vTaskDelay`）

### 3.3 vptr 落地

将 ops 指针作为对象结构体的**第一个字段**（教程第10章）：

```c
/* 平台上下文——持有 vptr */
typedef struct {
    const struct lwlte_platform_ops *ops;  /* vptr：指向平台操作表 */
    /* ... 平台内部状态 */
} lwlte_platform_t;
```

父类 init 时注入 ops（教程第10章）：

```c
int lwlte_platform_init(lwlte_platform_t *me,
                        const struct lwlte_platform_ops *ops)
{
    if (!me || !ops)
        return -1;

    me->ops = ops;   /* vptr 注入——多态的根 */
    return 0;
}
```

### 3.4 具体平台实现

ESP-IDF 平台实现 ops 表为 `static const`（教程第10章、第14章 ops 表必须用 `static const`）：

```c
/* lwlte_port_espidf.c */

static const struct lwlte_platform_ops espidf_ops = {
    .thread_create = espidf_thread_create,
    .thread_delete = espidf_thread_delete,
    .thread_sleep  = espidf_thread_sleep,
    .mem_malloc    = espidf_mem_malloc,
    .mem_free      = espidf_mem_free,
    .queue_create  = espidf_queue_create,
    .queue_send    = espidf_queue_send,
    .queue_recv    = espidf_queue_recv,
    .queue_delete  = espidf_queue_delete,
    .mutex_create  = espidf_mutex_create,
    .mutex_lock    = espidf_mutex_lock,
    .mutex_unlock  = espidf_mutex_unlock,
    .mutex_delete  = espidf_mutex_delete,
};

lwlte_platform_t *lwlte_platform_espidf_create(void)
{
    lwlte_platform_t *me = malloc(sizeof(lwlte_platform_t));
    if (!me) return NULL;

    int rc = lwlte_platform_init(me, &espidf_ops);
    if (rc != 0) {
        free(me);
        return NULL;
    }
    return me;
}
```

**规则**：ops 表必须 `static const`——链接到 `.rodata` 只读段防篡改，所有同型对象共享一份表。`const` 锁内容不锁指针，允许 init 时赋值（教程第10章）。

### 3.5 多态调用

所有调用统一为 `me->ops->method(me, ...)` 模式（教程第11章）：

```c
/* 业务逻辑代码——零平台依赖 */
lwlte_err_t lwlte_core_init(lwlte_platform_t *platform,
                             const lwlte_config_t *config)
{
    /* 所有平台操作走 ops 表 */
    lwlte_sys_thread_t thread =
        platform->ops->thread_create(fsm_task, &thread_cfg);

    lwlte_sys_queue_t queue =
        platform->ops->queue_create(sizeof(sig_item_t), 16);

    void *buf = platform->ops->mem_malloc(1024);

    /* ... */
}
```

**关键效果**（教程第12章）：业务逻辑代码中"一个硬件字样都没有"。更换平台时只需换一张 ops 表的实现，业务代码零改动。

### 3.6 多态规则总结

| 规则 | 做法 |
|------|------|
| ops 表定义 | `struct xxx_ops { 函数指针字段; }` |
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
int lwlte_module_start(struct lwlte_module_base *me)
{
    if (!me)
        return -1;

    assert(me->ops && me->ops->start &&
           "module.start is required — subclass must implement");
    return me->ops->start(me);
}
```

带 Release 兜底的稳健写法（教程第14章）：

```c
int lwlte_module_start(struct lwlte_module_base *me)
{
    if (!me)
        return -1;

    assert(me->ops && me->ops->start);
    if (!me->ops || !me->ops->start)
        return -LWLTE_ERR_NOT_IMPLEMENTED;  /* release 构建的最后一道闸 */

    return me->ops->start(me);
}
```

### 4.3 策略二：选填（optional）

```c
int lwlte_module_set_param(struct lwlte_module_base *me, uint32_t param)
{
    if (!me || !me->ops)
        return -1;

    if (!me->ops->set_param) {
        /* 默认行为：该模块不支持参数设置，安静跳过 */
        LWLTE_LOGD(me->name, "set_param not supported, skip");
        return 0;
    }
    return me->ops->set_param(me, param);
}
```

子类 ops 表有意不填选填字段（C 的零初始化自动置 NULL）：

```c
static const struct lwlte_module_ops at_ops = {
    .start = at_start,
    .stop  = at_stop,
    /* .set_param 故意不填——AT 模块无此需求，自动为 NULL */
};
```

### 4.4 策略三：全必填（严格接口）

```c
/* 接口契约：所有函数必须实现 */
struct lwlte_transport_ops {
    int (*connect)(struct lwlte_transport *me, const char *host, uint16_t port);
    int (*send)(struct lwlte_transport *me, const uint8_t *data, size_t len);
    int (*recv)(struct lwlte_transport *me, uint8_t *buf, size_t len,
                uint32_t timeout_ms);
    int (*close)(struct lwlte_transport *me);
};

/* 每个统一接口都 assert */
int lwlte_transport_connect(struct lwlte_transport *me,
                             const char *host, uint16_t port)
{
    if (!me || !host)
        return -1;
    assert(me->ops && me->ops->connect &&
           "transport.connect is part of the interface contract");
    return me->ops->connect(me, host, port);
}

int lwlte_transport_send(struct lwlte_transport *me,
                          const uint8_t *data, size_t len)
{
    if (!me || !data)
        return -1;
    assert(me->ops && me->ops->send &&
           "transport.send is part of the interface contract");
    return me->ops->send(me, data, len);
}
```

### 4.5 MCU 上的 assert 替换

教程第14章建议用工业宏替代标准 `assert`，避免直接 `abort()`：

```c
#define LWLTE_ASSERT(cond)                         \
    do {                                           \
        if (!(cond)) {                             \
            LWLTE_LOGE("ASSERT", "%s:%d: %s",     \
                       __FILE__, __LINE__, #cond);  \
            lwlte_sys_reset();                     \
        }                                          \
    } while (0)
```

---

## 5. 工程整合：平台抽象与模块初始化

> 参考教程：第15章「Platform 抽象」

### 5.1 三层架构

教程第15章确立了经典三层结构：

```
应用层 (main.c / app.c)
    │  #include "lwlte.h"
    │  零硬件字样
    ▼
板级层 (lwlte_board_init.c)
    │  唯一认识硬件的文件
    │  负责装配具体实现类 + 绑定句柄
    ▼
驱动层 (lwlte_xxx.c + lwlte_port_xxx.c)
    │  父类 + 子类 + platform ops
    ▼
硬件层 (ESP-IDF FreeRTOS / UART / GPIO)
```

### 5.2 平台注入模式

模块不直接依赖平台函数，而是通过平台 ops 注入（教程第15章）：

```c
/* lwlte_core.h — 公共头文件 */
typedef struct lwlte_core lwlte_core_t;

lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 lwlte_platform_t *platform);  /* 平台注入 */
void           lwlte_core_destroy(lwlte_core_t *me);

/* lwlte_core.c — 内部实现 */
struct lwlte_core {
    const lwlte_core_config_t config;
    lwlte_platform_t          *platform;  /* 持有平台句柄 */
    struct lwlte_at_manager    at;
    struct lwlte_net_manager   net;
    /* ... */
};

lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 lwlte_platform_t *platform)
{
    lwlte_core_t *me = platform->ops->mem_malloc(sizeof(lwlte_core_t));
    if (!me) return NULL;

    me->platform = platform;

    /* 所有系统调用走 platform->ops */
    me->at.fsm_queue = platform->ops->queue_create(
        sizeof(sig_item_t), CONFIG_FSM_QUEUE_SIZE);
    /* ... */
    return me;
}
```

### 5.3 板级装配

板级文件是唯一认识具体硬件的地方（教程第15章 `led_board_init.c` 模式）：

```c
/* lwlte_board_init.c — 板级层唯一认识硬件 */

/* 具体子类实例——static，文件私有 */
static struct lwlte_at_espidf  s_at_mgr;
static struct lwlte_net_espidf s_net_mgr;

/* 对外暴露的基类句柄 */
lwlte_core_t *g_lwlte_core;

int lwlte_board_init(void)
{
    /* 1. 创建平台实现 */
    lwlte_platform_t *platform = lwlte_platform_espidf_create();
    if (!platform) return -1;

    /* 2. 装配 core */
    lwlte_core_config_t config = {
        .uart_port   = CONFIG_LWLTE_UART_PORT,
        .tx_gpio     = CONFIG_LWLTE_TX_GPIO,
        .rx_gpio     = CONFIG_LWLTE_RX_GPIO,
        .baud_rate   = 115200,
        .apn         = CONFIG_LWLTE_APN,
    };
    g_lwlte_core = lwlte_core_create(&config, platform);
    if (!g_lwlte_core) return -1;

    return 0;
}
```

**规则**：换硬件时只改板级文件。教程原文："三行改动全部在 `led_board_init.c` 里面。应用层、子类 `.c` 文件、父类 `.c` 文件、`leds.h` 全部零改动。"

### 5.4 应用层零硬件依赖

应用层只 include 公共头文件，不暴露任何子类类型（教程第15章）：

```c
/* app.c — 零硬件字样 */

#include "lwlte.h"

void app_main(void)
{
    /* 只通过公共句柄操作 */
    lwlte_core_start(g_lwlte_core);

    lwlte_err_t err = lwlte_mqtt_publish(g_mqtt, &req);

    /* 完全不知道底下是 ESP32 + Air780EP 还是 PC 模拟 */
}
```

教程原文："应用层一个硬件字样都没有。它不认识 GPIO、不认识 PWM、不认识 I2C。"

### 5.5 模块生命周期模板

每个模块遵循统一的生命周期模式（教程第5章 HAL `Init/DeInit` 模式）：

```c
/* 公共 API */
lwlte_xxx_t *lwlte_xxx_create(const lwlte_xxx_config_t *config,
                               lwlte_platform_t *platform);
void         lwlte_xxx_destroy(lwlte_xxx_t *me);
lwlte_err_t  lwlte_xxx_start(lwlte_xxx_t *me);
lwlte_err_t  lwlte_xxx_stop(lwlte_xxx_t *me);

/* create 模式（替代旧的 init 模式） */
lwlte_xxx_t *lwlte_xxx_create(const lwlte_xxx_config_t *config,
                               lwlte_platform_t *platform)
{
    if (!config || !platform) return NULL;

    lwlte_xxx_t *me = platform->ops->mem_malloc(sizeof(lwlte_xxx_t));
    if (!me) return NULL;

    /* 初始化基类 */
    int rc = lwlte_module_base_init(&me->base, config->name, &xxx_ops);
    if (rc != 0) {
        platform->ops->mem_free(me);
        return NULL;
    }

    /* 保存平台引用 */
    me->platform = platform;

    /* 创建内部资源 */
    me->fsm_queue = platform->ops->queue_create(sizeof(sig_t), 16);
    if (!me->fsm_queue) {
        platform->ops->mem_free(me);
        return NULL;
    }

    return me;
}
```

---

## 6. 禁止事项

以下模式在旧项目中出现过，新项目中**严格禁止**：

| 禁止项 | 原因 | 替代方案 |
|--------|------|----------|
| `extern` 全局上下文变量 | 破坏封装，无法多实例 | 句柄模式 `lwlte_xxx_t *me` |
| 模块间直接访问 `s_ctx.xxx` | 紧耦合 | 通过基类指针 + container_of |
| 直接调用平台函数 `lwlte_sys_xxx()` | 硬编码平台依赖 | `platform->ops->xxx()` |
| 强制类型转换做向上转型 | base 不在偏移 0 时崩溃 | `&obj.base` |
| ops 表不加 `const` | 运行时可被篡改 | `static const struct xxx_ops` |
| 模块内 FSM 基础设施重复 | 代码膨胀 | 提取公共 FSM 基类 |
| 单例全局变量 `s_ctx` | 无法测试、无法多实例 | `create` 返回句柄 |
