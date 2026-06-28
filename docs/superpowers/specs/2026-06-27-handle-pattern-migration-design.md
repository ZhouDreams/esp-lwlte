# 句柄定义模式迁移设计:Pattern B → Pattern A(+ struct tag 生态对齐)

> 状态:已与用户确认设计(2026-06-27)。本文件仅产出**设计方案**,不含源码改动。
> 依据:`reference/handle-pattern-migration.md`(交给执行 agent 的改写说明)。
> 后续:由 writing-plans 据此生成分步实施计划。

## 1. 背景与目标

项目当前用 **Pattern B**(结构体即句柄),与所在 ESP-IDF + FreeRTOS 生态惯用的 **Pattern A**(指针即句柄)不一致。

```c
// Pattern B(当前):typedef struct X handle_t;   // 句柄是结构体,使用处要显式写 *
// Pattern A(目标):typedef struct struct_x *handle_t;  // 句柄已是指针,使用处少写一个 *
```

**核心结论(运行等价)**:两种模式间接层数、函数体、二进制行为完全相同。Pattern A 只是把一个 `*` 从「使用处」挪进了 typedef。因此凡是「用已有句柄表达式去访问成员或传参」的地方(`me->xxx`、`some_fn(me->at)`)**一行都不用改**;只有**类型声明处**的 `*` 个数要调整。

**本次目标**:把项目自有的全部不透明句柄类型统一改成 Pattern A,并**同时**把 struct tag 改为生态主流的 `_t` 形态,做到「全生态对齐」。

## 2. struct tag 命名规约调研(ESP-IDF / FreeRTOS 真实写法)

在本地 ESP-IDF(`~/.espressif/v6.0/esp-idf`)源码中查证:

- **Pattern A 是绝对主流**:全 `components` 树仅 2 处 Pattern B 残留(均在边角)。
- **struct tag 命名分布**:

| tag 形态 | 频次 | 典型例 |
|---|---|---|
| `xxx_t` 后缀 | **108 处(主流)** | `typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;`(esp_lcd) |
| 纯 snake_case 词干 | ~10 处 | `typedef struct esp_timer* esp_timer_handle_t;`、`typedef struct esp_http_client *esp_http_client_handle_t;` |
| `xxx_s` 后缀 | 6 处 | `struct dac_cosine_s`、`struct esp_task_wdt_user_handle_s` |
| `xxx_handle`(本项目当前写法) | **仅 1 处** | 几乎无先例 |
| CamelCase(`XxxDefinition`) | — | **仅 FreeRTOS 用**(`struct QueueDefinition`、`tskTaskControlBlock`),注释明示「为兼容 kernel-aware debugger 的旧命名」 |

**结论**:
- 本项目当前 `typedef struct lwlte_handle lwlte_handle_t;` 的 tag 写法**不符合 ESP-IDF 生态**(全树仅 1 先例)。
- ESP-IDF 组件**不用** CamelCase;那只是 FreeRTOS 专属。
- 生态主流是 snake_case + **`_t` 后缀**(esp_lcd 即是此例)。
- esp_lcd 标准模板:`typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;`(tag 与公共名同词干、带 `_t`)。

**决策**:Pattern B→A 迁移**同时**把 struct tag 改为 `_t` 形态(对齐 esp_lcd 及 108 处主流)。tag 对消费者完全不可见,改名零行为/ABI 影响。

## 3. 范围

### 3.1 纳入迁移的类型(9 个)

| 公共 typedef(不变) | 当前 tag → 新 tag | struct 定义处 |
|---|---|---|
| `lwlte_handle_t` | `lwlte_handle` → `lwlte_t` | `src/lwlte/lwlte_priv.h:40` |
| `core_handle_t` | `core_handle` → `core_t` | `src/core/core_priv.h:102` |
| `at_engine_handle_t` | `at_engine_handle` → `at_engine_t` | `src/at_engine/at_engine.c:89` |
| `modem_handle_t` | `modem_handle` → `modem_t` | `src/modem/modem_priv.h:238` |
| `tcp_client_handle_t` | `tcp_client_handle` → `tcp_client_t` | `src/tcp_client/tcp_client_priv.h:94` |
| `mqtt_client_handle_t` | `mqtt_client_handle` → `mqtt_client_t` | `src/mqtt_client/mqtt_client_priv.h:90` |
| `ping_client_handle_t` | `ping_client_handle` → `ping_client_t` | `src/ping_client/ping_client_priv.h:42` |
| `tcp_client_conn_t` | `tcp_client_conn` → `tcp_client_conn_t` | `src/tcp_client/tcp_client_priv.h:107` |
| `lwlte_tcp_conn_t` | `lwlte_tcp_conn` → `lwlte_tcp_conn_t` | (无定义,纯别名,见 §4.2) |

> 执行前应全仓 grep 复核无新增/遗漏:`rg "typedef struct \w+ \*?\w*(_handle_t|_conn_t)\s*;" src`。

**连接类型判定**:`tcp_client_conn_t` / `lwlte_tcp_conn_t` 经判定**是句柄语义**(不透明、堆分配、create/destroy 生命周期、按指针传递/存储),纳入迁移。二者实为同一堆对象的别名(`tcp_client.c` 内有 `(lwlte_tcp_conn_t *)conn` 强转)。

### 3.2 不动的类型

- 所有 `*_config_t` / `*_event_data_t` / `*_request_t` 等**普通数据结构**(非句柄语义)。
- ESP-IDF 句柄:`esp_event_loop_handle_t`、`esp_timer_handle_t`、`esp_http_client_handle_t` 等——它们**本来就是 Pattern A**。结构体里这类成员(如 `esp_event_loop_handle_t event_loop;`)**保持原样**。
- FreeRTOS 类型:`TaskHandle_t`、`SemaphoreHandle_t`、`QueueHandle_t` 等**一律不动**。

### 3.3 交付边界

**本次只产出设计方案 + 后续实施计划,不改源码。** 源码改动在用户审阅计划后另行决定是否执行。

## 4. 边界与坑(按风险排序)

### 4.1 Modem 子类按值嵌入基类(最高风险)

`modem_air780ep_t` / `modem_ml307r_t` 把基类**按值嵌入**为首个成员,并用 `MODEM_CONTAINER_OF` 下转:

```c
// modem_air780ep.c:100 / modem_ml307r.c:86
typedef struct {
    modem_handle_t base;          // ← 按值嵌入基类
    modem_air780ep_config_t config;
    ...
} modem_air780ep_t;

// 下转(me 指向嵌入的 base 子对象)
return MODEM_CONTAINER_OF(me, modem_air780ep_t, base);   // modem_air780ep.c:1466
```

迁移后 Pattern A 使 `modem_handle_t` 成为**指针类型**。若保留 `modem_handle_t base;`,它会变成「指针成员」,`MODEM_CONTAINER_OF` 的 `offsetof(..., base)` 会取到该指针成员的偏移而非嵌入结构体的偏移,**downcast 崩坏**。

**规则**:子类按值嵌入基类处**必须**改写为原始 struct tag:

```c
typedef struct {
    struct modem_t base;          // ← 按值嵌入原始 struct(Pattern A 下唯一正确写法)
    ...
} modem_air780ep_t;
```

调用处 `modem_base_init(&impl->base, ...)` **不变**:`&impl->base` 的类型是 `struct modem_t *`,即 Pattern A 下的 `modem_handle_t`,签名 `modem_base_init(modem_handle_t me, ...)` 直接匹配。函数体内 `MODEM_CONTAINER_OF(me, modem_air780ep_t, base)` **不动**(me 仍是指向 base 子对象的指针)。

### 4.2 不透明类型别名(`lwlte_tcp_conn_t` ↔ `tcp_client_conn_t`)

二者是分别前向声明的两个 tag,却指向**同一堆对象**,层间靠强转互通。`lwlte_tcp_conn_t` **无 struct 定义**(纯别名,仅在 `lwlte.h` 前向声明,在 `lwlte.c` 委托给 `tcp_client_*` 时强转)。

- 两类型必须**同步**迁移(Pattern A)+ 改名(`_t` tag)。
- 层间强转处 `**` → `*`:
  - `lwlte.c:767`:`tcp_client_open(tcp, &open_config, (tcp_client_conn_t **)out_conn)` → `(tcp_client_conn_t *)out_conn`
  - `tcp_client.c` 内 `(lwlte_tcp_conn_t *)conn`(单 `*`,不变)

### 4.3 函数指针 typedef 表(ops / callbacks)

- `src/modem/modem_priv.h`:约 23 个 modem ops 函数 typedef(`modem_no_arg_fn`、`modem_get_info_fn` …)——签名 `modem_handle_t *me` → `modem_handle_t me`。
- `src/modem/modem.h:453`:`modem_event_callback_t` —— 同上。
- `src/core/core.h`:`core_protocol_callback_t`、`core_protocol_closed_callback_t`、`core_cmd_done_callback_t` —— `core_handle_t *me` → `core_handle_t me`。
- modem 子类(`modem_air780ep.c` / `modem_ml307r.c`)里的 **static op 函数签名**(如 `static esp_err_t air780ep_destroy(modem_handle_t *me)`)同步改;函数体内 `MODEM_CONTAINER_OF(me, ...)` **不动**。

### 4.4 出参 `**` → `*`

- **所有权出参**:`lwlte_handle_t **out_lte` → `*out_lte`;`tcp_client_conn_t **out_conn` / `lwlte_tcp_conn_t **out_conn` → `*out_conn`。
- **借用出参**(返回内部句柄的指针,非所有权转移):`core_handle_t **out_core` → `*out_core`(`lwlte.c`、`ping_client.c`);`mqtt_client_handle_t **out_mqtt` → `*out_mqtt`(`lwlte.c`)。
- `*out = me;`、`calloc(1, sizeof(*me))`、`sizeof(handle_t)` **一律不变**。

### 4.5 Python 契约测试(必须同步,11 个文件)

`tests/host/` 下契约测试以**精确字符串**断言源码签名与 struct 定义:

```python
# 当前(Pattern B)
self.assertIn("esp_err_t lwlte_mqtt_start(lwlte_handle_t *me);", ...)
r"struct at_engine_handle\s*\{(?P<body>.*?)\n\};"   # test_lwlte_start_lifecycle.py:174
```

迁移后须逐字改为 Pattern A 签名与 `_t` tag:

```python
self.assertIn("esp_err_t lwlte_mqtt_start(lwlte_handle_t me);", ...)
r"struct at_engine_t\s*\{(?P<body>.*?)\n\};"
```

涉及文件(断言命中 `_handle_t *` / struct tag 的):`test_mqtt_end_to_end_contract.py`、`test_lwlte_start_stop_lifecycle.py`、`test_lwlte_start_lifecycle.py`、`test_tcp_client_end_to_end_contract.py`、`test_ping_end_to_end_contract.py`、`test_ping_classes_doc_contract.py`、`test_ml307r_contract.py`、`test_air780ep_command_gated_init.py`、`test_air780ep_cpin_policy.py`、`test_mqtt_tls_ssl_contract.py`、`test_net_mgr_activation_flow.py`。这是迁移的**验证门**,也是改动面。

### 4.6 struct 成员:「指向句柄的指针」→「直接存句柄」

```c
// 当前
struct lwlte_handle { at_engine_handle_t *at; modem_handle_t *modem; core_handle_t *core; ... };
// 迁移后(存指针语义不变,只是少写一个 *)
struct lwlte_t { at_engine_handle_t at; modem_handle_t modem; core_handle_t core; ... };
```

ESP-IDF 句柄成员(如 `esp_event_loop_handle_t event_loop;`)保持原样。改完后项目自有句柄成员与 IDF 句柄成员**长得一样**(都是「按值存指针」)。

### 4.7 const 正确性

`const lwlte_handle_t me;` 在 Pattern A 下展开为 `struct lwlte_t * const me`(常量指针,非指向常量)。跟随 ESP-IDF 惯例:**句柄按值传,不加 const**。一般无需引入 `const_xxx_handle_t`。

## 5. 机械改写规则(对照表)

函数体内部表达式基本不动,只有「类型书写」要改:

| 出现形式(改前) | 改成 | 说明 |
|---|---|---|
| `typedef struct X handle_t;` | `typedef struct X_t *handle_t;` | typedef 加 `*`,tag 改 `_t` |
| `struct X { ... }`(定义处) | `struct X_t { ... }` | tag 改名 |
| `handle_t *me`(参数/局部/返回值/成员) | `handle_t me` | 砍一个 `*` |
| `handle_t **out`(出参) | `handle_t *out` | 砍一个 `*` |
| `handle_t *me = calloc(1, sizeof(*me));` | `handle_t me = calloc(1, sizeof(*me));` | 只改左值类型,右边不动 |
| `*out_lte = me;` | `*out_lte = me;` | **完全不变** |
| `me->lock`、`me->at->...` | 不变 | 解引用层级一致,`->` 照旧 |
| `sizeof(*me)` / `sizeof(handle_t)` | 不变 | |
| 调用处 `some_fn(me->at, ...)` | 不变 | 实参表达式不变 |
| 子类按值嵌入 `handle_t base;` | `struct X_t base;` | **特例**:必须用原始 struct tag(见 §4.1) |
| 层间强转 `(other_handle_t **)p` | `(other_handle_t *)p` | 砍一个 `*`(见 §4.2) |

> 直觉:**凡「类型名 + N 个 `*`」的声明处,N 减 1;凡用已有句柄表达式访问/传参处,一行不改。** 唯二例外:子类按值嵌入基类(改用原始 tag)、不透明别名层间强转(随出参一起砍 `*`)。

## 6. 验证标准

1. **编译通过**:`idf.py build`(MCP 优先),零 `incomplete type` / 类型不匹配告警。
2. **契约测试全绿**:`pytest tests/host`,签名/struct 定义断言已同步为 Pattern A。
3. **语义抽样核对**:`me->xxx` 访问、`*out = calloc(...)`、`sizeof(*me)`、`MODEM_CONTAINER_OF` 行为一致;modem 子类 downcast 仍正确。
4. **ABI 不变**:句柄始终是指针,结构体大小不变。

## 7. 改动面概览(供实施计划参考)

- **typedef 处**:9(各公共/层间头文件)。
- **struct 定义处**:8(`lwlte_tcp_conn` 无定义)。
- **modem 子类按值嵌入**:`modem_air780ep.c`、`modem_ml307r.c`(各 1)。
- **头文件签名**:公共 API(`lwlte.h`)、层间 API(`core.h`/`modem.h`/`at_engine.h`/`tcp_client.h`/`mqtt_client.h`/`ping_client.h`)、私有 API(`*_priv.h`)。
- **函数指针 typedef**:`modem_priv.h`(~23)、`modem.h`(1)、`core.h`(3)。
- **.c 实现签名 + 局部类型声明**:全部模块实现文件 + 子类 static op 函数。
- **example/**:句柄变量声明与使用(声明处砍 `*`,使用处不动)。
- **tests/host/**:11 个契约测试断言同步。
- **struct 成员**:各 priv 头里的句柄成员(砍 `*`)。

## 8. 决策记录

| 决策点 | 选择 | 理由 |
|---|---|---|
| 连接类型是否纳入 | 纳入(`tcp_client_conn_t` / `lwlte_tcp_conn_t`) | 判定为句柄语义;纳入后风格彻底统一 |
| 交付边界 | 仅方案,不改源码 | 用户要求先出方案审阅 |
| 执行策略 | 单一协调改动集,自底向上层序 | typedef 改动全局原子,部分态不可编译 |
| struct tag 形态 | 改 `_t` 词干(对齐 esp_lcd) | 生态主流(108 处),tag 不可见、零行为影响 |
