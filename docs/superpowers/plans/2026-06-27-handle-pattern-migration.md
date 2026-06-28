# 句柄定义模式迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把项目自有全部不透明句柄从 Pattern B(结构体即句柄)迁移到 Pattern A(指针即句柄),同时把 struct tag 改为生态主流的 `_t` 形态。

**Architecture:** 单一协调的机械改写,按依赖层自底向上编辑(at_engine → modem → core → tcp/mqtt/ping → lwlte → example → tests)。typedef 改动全局原子,**中间态不可编译**,仅最终验证。

**Tech Stack:** C(ESP-IDF v6.0 + FreeRTOS)、pytest 主机契约测试。

**依据:** `docs/superpowers/specs/2026-06-27-handle-pattern-migration-design.md`。

---

## ⚠️ 必读约束

1. **原子性**:任何一个 `typedef struct X X_t;` 改成 `typedef struct X_t *X_t;` 后,所有 `X_t *` 声明处立即多一层指针。因此各 Task 是**编辑顺序**(便于审阅),不是「可编译增量」。**禁止在任何中间 Task 跑 build / pytest 期待通过**——验证只在 Task 10。
2. **提交**:本计划含 `git commit` 步骤(标准流程)。但项目规范要求**用户显式授权方可 commit**;执行时未获授权则跳过 commit 步、保留改动。
3. **TDD 说明**:这是机械重构,无新行为。以「`idf.py build` 编译通过 + 已更新的 `tests/host` 契约测试全绿」为唯一验证门,非经典红绿。

## 全程适用的机械改写规则

**声明处 N 个 `*` 减 1;表达式(访问/传参)一行不改。**

| 改前 | 改后 |
|---|---|
| `typedef struct X X_t;` | `typedef struct X_t *X_t;` |
| `struct X { ... }`(定义) | `struct X_t { ... }` |
| `X_t *me`(参数/局部/返回/成员) | `X_t me` |
| `X_t **out`(出参) | `X_t *out` |
| `X_t *p = (X_t *)arg;`(强转) | `X_t p = (X_t)arg;` |
| `*out = me;` / `me->x` / `sizeof(*me)` / 调用 `fn(me->at)` | **不变** |
| 子类按值嵌入 `X_t base;` | **`struct X_t base;`**(特例,见 Task 2) |

**X = 9 个类型的旧 tag**:`lwlte_handle / core_handle / at_engine_handle / modem_handle / tcp_client_handle / mqtt_client_handle / ping_client_handle / tcp_client_conn / lwlte_tcp_conn`

## 残留检测 grep(Task 10 用)

```bash
# 1) 旧 struct tag 残留(须为 0)
rg "struct (lwlte_handle|core_handle|at_engine_handle|modem_handle|tcp_client_handle|mqtt_client_handle|ping_client_handle|tcp_client_conn|lwlte_tcp_conn)\b" src example
# 2) 双指针句柄残留(须为 0)
rg "(_handle_t|_conn_t)\s*\*\s*\*" src example
# 3) Pattern B typedef 残留(须为 0,全部应带 *)
rg "typedef struct \w+ \w*(_handle_t|_conn_t)\s*;" src
```

> 单个 `X_t *out`(出参)与 `X_t *out_conn` 是迁移后的**合法**形态,不算残留。漏改的单 `*` 参数/返回值由 **build 的类型不匹配错误**兜底定位。

## File Structure(改动面)

**头文件(public/层间):** `src/include/lwlte.h`、`src/core/core.h`、`src/at_engine/at_engine.h`、`src/modem/modem.h`、`src/tcp_client/tcp_client.h`、`src/mqtt_client/mqtt_client.h`、`src/ping_client/ping_client.h`
**私有头(struct 定义 + fn 指针表 + 私有 API):** `src/lwlte/lwlte_priv.h`、`src/core/core_priv.h`、`src/modem/modem_priv.h`、`src/tcp_client/tcp_client_priv.h`、`src/mqtt_client/mqtt_client_priv.h`、`src/ping_client/ping_client_priv.h`
**实现 .c:** 各模块 + `src/modem/modem_air780ep.c`、`src/modem/modem_ml307r.c`、`src/lwlte/lwlte_air780ep.c`、`src/lwlte/lwlte_ml307r.c`、`src/at_engine/at_engine.c`(含 struct 定义)
**示例:** `example/*.c`(7 个示例 + main.c)
**测试:** `tests/host/*.py`(11 个契约测试)

---

## Task 0: 基线验证(确认起点为绿)

**Files:** 无改动

- [ ] **Step 1: 编译基线**

优先 MCP:`esp-idf-eim_build_project`;否则:
```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```
Expected: 编译成功(后续所有改动以此为准)。

- [ ] **Step 2: 契约测试基线**

```bash
python3 -m pytest tests/host -q
```
Expected: 全绿(记录通过数,迁移后须保持)。

> 若基线非绿,先修基线,勿开始迁移。

---

## Task 1: at_engine 模块(叶子,无项目内依赖)

**Files:**
- Modify: `src/at_engine/at_engine.h`(typedef + 签名)
- Modify: `src/at_engine/at_engine.c`(struct 定义 + 实现签名 + 局部)

- [ ] **Step 1: typedef**

`src/at_engine/at_engine.h:81`:
```c
// 改前
typedef struct at_engine_handle at_engine_handle_t;
// 改后
typedef struct at_engine_t *at_engine_handle_t;
```

- [ ] **Step 2: struct 定义改名**

`src/at_engine/at_engine.c:89`:`struct at_engine_handle {` → `struct at_engine_t {`

- [ ] **Step 3: 头文件签名(枚举所有 `at_engine_handle_t *`)**
```bash
rg -n "at_engine_handle_t\s*\*" src/at_engine/at_engine.h
```
对每处声明砍一个 `*`。代表样例:
```c
// 改前
at_engine_handle_t *at_engine_create(const at_engine_config_t *config);
esp_err_t at_engine_destroy(at_engine_handle_t *me);
esp_err_t at_engine_send_cmd(at_engine_handle_t *me, const char *cmd, ...);
void at_engine_end_exclusive(at_engine_handle_t *me);
// 改后
at_engine_handle_t at_engine_create(const at_engine_config_t *config);
esp_err_t at_engine_destroy(at_engine_handle_t me);
esp_err_t at_engine_send_cmd(at_engine_handle_t me, const char *cmd, ...);
void at_engine_end_exclusive(at_engine_handle_t me);
```

- [ ] **Step 4: .c 实现签名 + 局部**
```bash
rg -n "at_engine_handle_t\s*\*" src/at_engine/at_engine.c
```
对每处砍 `*`(函数定义签名 + `at_engine_handle_t me = calloc(...)` 等局部)。**`me->xxx`、`sizeof(*me)`、`calloc` 右值不动。**

---

## Task 2: modem 模块(含最高风险 §4.1 嵌入修复)

**Files:**
- Modify: `src/modem/modem.h`(typedef + 签名 + 事件回调 typedef)
- Modify: `src/modem/modem_priv.h`(struct 定义 + ~23 个 ops fn typedef + 私有 API + 基类成员)
- Modify: `src/modem/modem.c`(实现签名 + 局部)
- Modify: `src/modem/modem_air780ep.c`(子类:`base` 嵌入修复 + static op 签名 + 局部)
- Modify: `src/modem/modem_ml307r.c`(同上)

- [ ] **Step 1: typedef**

`src/modem/modem.h:49`:
```c
typedef struct modem_handle modem_handle_t;   // →
typedef struct modem_t *modem_handle_t;
```

- [ ] **Step 2: struct 定义改名 + 借用成员**

`src/modem/modem_priv.h:238`:`struct modem_handle {` → `struct modem_t {`

struct 内的 AT 引擎借用成员**保持 `*` 不变**(因为 `at_engine_handle_t` 经 Task 1 已是指针,`at_engine_handle_t *at` 本就是「指向句柄的指针」→ 改后 `at_engine_handle_t at`):
```c
// 改前(modem_priv.h:240)
at_engine_handle_t *at;
// 改后
at_engine_handle_t at;
```

- [ ] **Step 3: ops 函数指针 typedef(约 23 个)**

`src/modem/modem_priv.h` 内所有 `modem_*_fn` typedef 形参 `modem_handle_t *me` → `modem_handle_t me`:
```bash
rg -n "typedef.*\(\*modem_\w+_fn\).*modem_handle_t\s*\*" src/modem/modem_priv.h
```
代表样例:
```c
// 改前
typedef esp_err_t (*modem_no_arg_fn)(modem_handle_t *me);
typedef esp_err_t (*modem_get_info_fn)(modem_handle_t *me, modem_info_t *info);
// 改后
typedef esp_err_t (*modem_no_arg_fn)(modem_handle_t me);
typedef esp_err_t (*modem_get_info_fn)(modem_handle_t me, modem_info_t *info);
```

- [ ] **Step 4: 事件回调 typedef**

`src/modem/modem.h:453`:
```c
typedef void (*modem_event_callback_t)(modem_handle_t *modem, ...);   // →
typedef void (*modem_event_callback_t)(modem_handle_t modem, ...);
```

- [ ] **Step 5: 私有 API 签名(modem_priv.h)**

`modem_base_init` / `modem_base_deinit` / `modem_base_stop_event_task` / `modem_post_event` / `modem_set_state` 形参 `modem_handle_t *me` → `modem_handle_t me`;`at_engine_handle_t *at` → `at_engine_handle_t at`:
```bash
rg -n "modem_handle_t\s*\*|at_engine_handle_t\s*\*" src/modem/modem_priv.h
```

- [ ] **Step 6: modem.h 公共签名**

```bash
rg -n "modem_handle_t\s*\*" src/modem/modem.h
```
所有 `modem_xxx(modem_handle_t *me, ...)` → `(modem_handle_t me, ...)`。

- [ ] **Step 7: modem.c 实现 + 局部**

```bash
rg -n "modem_handle_t\s*\*" src/modem/modem.c
```
砍 `*`。注意 `modem.c` 内若有借用的 `at_engine_handle_t *` 也一并砍。

- [ ] **Step 8: 🔴 子类按值嵌入基类修复(最高风险)**

`src/modem/modem_air780ep.c:100` 与 `src/modem/modem_ml307r.c:86`:
```c
// 改前(两处)
typedef struct {
    modem_handle_t base;          // Pattern A 下这是「指针成员」,会破坏 container_of
    ...
} modem_air780ep_t;
// 改后(必须用原始 struct tag,按值嵌入)
typedef struct {
    struct modem_t base;
    ...
} modem_air780ep_t;
```
> 调用处 `modem_base_init(&impl->base, ...)` **不变**(`&impl->base` 即 `struct modem_t *` = `modem_handle_t`)。

- [ ] **Step 9: 子类 static op 函数签名 + 局部**

```bash
rg -n "modem_handle_t\s*\*" src/modem/modem_air780ep.c src/modem/modem_ml307r.c
```
所有 `static esp_err_t air780ep_xxx(modem_handle_t *me)` → `(modem_handle_t me)`;局部 `modem_handle_t *` → `modem_handle_t`。**`MODEM_CONTAINER_OF(me, modem_air780ep_t, base)` 一行不动。**

---

## Task 3: core 模块

**Files:**
- Modify: `src/core/core.h`(typedef + 公共签名 + 3 个 callback typedef)
- Modify: `src/core/core_priv.h`(struct 定义 + 私有 API + 借用成员)
- Modify: `src/core/core.c`、`src/core/core_fsm.c`(实现 + 局部)

- [ ] **Step 1: typedef + struct 定义**

`src/core/core.h:39`:`typedef struct core_handle core_handle_t;` → `typedef struct core_t *core_handle_t;`
`src/core/core_priv.h:102`:`struct core_handle {` → `struct core_t {`

- [ ] **Step 2: 3 个 callback typedef(core.h)**

```bash
rg -n "typedef.*\(\*core_\w+_callback_t\).*core_handle_t\s*\*" src/core/core.h
```
`core_protocol_callback_t` / `core_protocol_closed_callback_t` / `core_cmd_done_callback_t` 形参 `core_handle_t *me`(或 `*core`)→ `core_handle_t me`。

- [ ] **Step 3: struct 借用成员(core_priv.h)**

`struct core_t` 内 `modem_handle_t *modem;` → `modem_handle_t modem;`(modem 经 Task 2 已迁移)。ESP-IDF 句柄成员(如 `esp_event_loop_handle_t loop;`)**不动**。
```bash
rg -n "modem_handle_t\s*\*" src/core/core_priv.h
```

- [ ] **Step 4: 公共签名 + 私有 API**

```bash
rg -n "core_handle_t\s*\*|modem_handle_t\s*\*" src/core/core.h src/core/core_priv.h
```
`core_create(const core_config_t *config, modem_handle_t *modem)` → `(const core_config_t *config, modem_handle_t modem)`;返回 `core_handle_t *core_create(...)` → `core_handle_t core_create(...)`;其余 `core_xxx(core_handle_t *me, ...)` → `(core_handle_t me, ...)`。

- [ ] **Step 5: 实现 + 局部**

```bash
rg -n "core_handle_t\s*\*|modem_handle_t\s*\*" src/core/core.c src/core/core_fsm.c
```
砍 `*`。`core_handle_t *core = NULL;` → `core_handle_t core = NULL;`(Pattern A 下指针可赋 NULL)。**`me->xxx`、container_of 不动。**

---

## Task 4: tcp_client 模块(含 conn 类型)

**Files:**
- Modify: `src/tcp_client/tcp_client.h`(2 个 typedef + 签名)
- Modify: `src/tcp_client/tcp_client_priv.h`(2 个 struct 定义 + 借用成员)
- Modify: `src/tcp_client/tcp_client.c`(实现 + 局部 + 别名强转)

- [ ] **Step 1: 2 个 typedef**

`src/tcp_client/tcp_client.h:31-32`:
```c
typedef struct tcp_client_handle tcp_client_handle_t;   // → typedef struct tcp_client_t *tcp_client_handle_t;
typedef struct tcp_client_conn tcp_client_conn_t;       // → typedef struct tcp_client_conn_t *tcp_client_conn_t;
```

- [ ] **Step 2: 2 个 struct 定义改名**

`tcp_client_priv.h:94`:`struct tcp_client_handle {` → `struct tcp_client_t {`
`tcp_client_priv.h:107`:`struct tcp_client_conn {` → `struct tcp_client_conn_t {`

- [ ] **Step 3: struct 成员**

```bash
rg -n "(tcp_client_handle_t|tcp_client_conn_t|core_handle_t)\s*\*" src/tcp_client/tcp_client_priv.h
```
- `struct tcp_client_t` 内:`core_handle_t *core;` → `core_handle_t core;`;`tcp_client_conn_t *conn;` / `*deferred_destroy_conn;` → 各砍 `*`。
- `struct tcp_client_conn_t` 内:`tcp_client_handle_t *client;` → `tcp_client_handle_t client;`。
- ESP-IDF / FreeRTOS 成员不动。

- [ ] **Step 4: 头文件签名(含出参)**

```bash
rg -n "(tcp_client_handle_t|tcp_client_conn_t)\s*\*\*" src/tcp_client/tcp_client.h
```
```c
// 改前
tcp_client_handle_t *tcp_client_create(const tcp_client_config_t *config, core_handle_t *core);
esp_err_t tcp_client_open(tcp_client_handle_t *me, const tcp_client_open_config_t *config,
                          tcp_client_conn_t **out_conn);
esp_err_t tcp_client_send(tcp_client_conn_t *conn, ...);
// 改后
tcp_client_handle_t tcp_client_create(const tcp_client_config_t *config, core_handle_t core);
esp_err_t tcp_client_open(tcp_client_handle_t me, const tcp_client_open_config_t *config,
                          tcp_client_conn_t *out_conn);
esp_err_t tcp_client_send(tcp_client_conn_t conn, ...);
```

- [ ] **Step 5: .c 实现 + 局部 + 别名强转**

```bash
rg -n "(tcp_client_handle_t|tcp_client_conn_t|core_handle_t|lwlte_tcp_conn_t)\s*\*" src/tcp_client/tcp_client.c
```
- 所有声明砍 `*`;`*out_conn = ...;` 不动。
- **别名强转**(`tcp_client.c:1282` 等):`(lwlte_tcp_conn_t *)conn`(单 `*`)**不变**——`lwlte_tcp_conn_t` 经 Task 7 已是指针,单 `*` 强转仍正确。

---

## Task 5: mqtt_client 模块

**Files:**
- Modify: `src/mqtt_client/mqtt_client.h`、`src/mqtt_client/mqtt_client_priv.h`、`src/mqtt_client/mqtt_client.c`

- [ ] **Step 1: typedef + struct 定义 + 借用成员**

`mqtt_client.h:33`:`typedef struct mqtt_client_handle mqtt_client_handle_t;` → `typedef struct mqtt_client_t *mqtt_client_handle_t;`
`mqtt_client_priv.h:90`:`struct mqtt_client_handle {` → `struct mqtt_client_t {`
struct 内 `core_handle_t *core;` → `core_handle_t core;`(ESP-IDF `esp_event_loop_handle_t loop;` 不动)。

- [ ] **Step 2: 签名(全模块)**

```bash
rg -n "(mqtt_client_handle_t|core_handle_t)\s*\*" src/mqtt_client/mqtt_client.h src/mqtt_client/mqtt_client_priv.h src/mqtt_client/mqtt_client.c
```
```c
// 改前
mqtt_client_handle_t *mqtt_client_create(const mqtt_client_config_t *config, core_handle_t *core);
esp_err_t mqtt_client_destroy(mqtt_client_handle_t *me);
// 改后
mqtt_client_handle_t mqtt_client_create(const mqtt_client_config_t *config, core_handle_t core);
esp_err_t mqtt_client_destroy(mqtt_client_handle_t me);
```
实现签名 + 局部砍 `*`;`me->xxx` 不动。

---

## Task 6: ping_client 模块

**Files:**
- Modify: `src/ping_client/ping_client.h`、`src/ping_client/ping_client_priv.h`、`src/ping_client/ping_client.c`

- [ ] **Step 1: typedef + struct 定义 + 借用成员**

`ping_client.h:30`:`typedef struct ping_client_handle ping_client_handle_t;` → `typedef struct ping_client_t *ping_client_handle_t;`
`ping_client_priv.h:42`:`struct ping_client_handle {` → `struct ping_client_t {`
struct 内 `core_handle_t *core;` → `core_handle_t core;`。

- [ ] **Step 2: 签名(全模块)**

```bash
rg -n "(ping_client_handle_t|core_handle_t)\s*\*" src/ping_client/ping_client.h src/ping_client/ping_client_priv.h src/ping_client/ping_client.c
```
```c
// 改前
ping_client_handle_t *ping_client_create(core_handle_t *core);
static esp_err_t begin_ping_call(ping_client_handle_t *me, core_handle_t **out_core);
// 改后
ping_client_handle_t ping_client_create(core_handle_t core);
static esp_err_t begin_ping_call(ping_client_handle_t me, core_handle_t *out_core);
```
> `begin_ping_call` 的 `core_handle_t **out_core` 是借用出参 → 砍成 `*out_core`;`*out_core = ...;` 不动。

> Task 4/5/6 互不依赖,可并行执行。

---

## Task 7: lwlte 门面(public + priv + .c + 两个 init)

**Files:**
- Modify: `src/include/lwlte.h`(typedef + 公共签名 + 事件数据成员)
- Modify: `src/lwlte/lwlte_priv.h`(struct 定义 + 借用成员 + 私有 API)
- Modify: `src/lwlte/lwlte.c`、`src/lwlte/lwlte_air780ep.c`、`src/lwlte/lwlte_ml307r.c`(实现 + 局部 + 别名强转)

- [ ] **Step 1: 2 个 typedef(lwlte.h)**

`src/include/lwlte.h:38`:`typedef struct lwlte_handle lwlte_handle_t;` → `typedef struct lwlte_t *lwlte_handle_t;`
`src/include/lwlte.h:44`:`typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;` → `typedef struct lwlte_tcp_conn_t *lwlte_tcp_conn_t;`

- [ ] **Step 2: struct 定义 + 借用成员(lwlte_priv.h)**

`lwlte_priv.h:40`:`struct lwlte_handle {` → `struct lwlte_t {`
struct 内全部句柄成员砍 `*`:
```bash
rg -n "_handle_t\s*\*" src/lwlte/lwlte_priv.h
```
```c
// 改前
struct lwlte_handle { at_engine_handle_t *at; modem_handle_t *modem; core_handle_t *core;
                      mqtt_client_handle_t *mqtt; tcp_client_handle_t *tcp; ping_client_handle_t *ping; ... };
// 改后
struct lwlte_t { at_engine_handle_t at; modem_handle_t modem; core_handle_t core;
                 mqtt_client_handle_t mqtt; tcp_client_handle_t tcp; ping_client_handle_t ping; ... };
```
> `esp_event_loop_handle_t event_loop;`(IDF 句柄)不动;`SemaphoreHandle_t` 等 FreeRTOS 类型不动。

- [ ] **Step 3: 私有 API(lwlte_priv.h)**

```c
// 改前
esp_err_t lwlte_create_empty(lwlte_handle_t **out_lte);
esp_err_t lwlte_wait_ready(lwlte_handle_t *me, uint32_t timeout_ms);
// 改后
esp_err_t lwlte_create_empty(lwlte_handle_t *out_lte);
esp_err_t lwlte_wait_ready(lwlte_handle_t me, uint32_t timeout_ms);
```

- [ ] **Step 4: 公共签名(lwlte.h)**

```bash
rg -n "(lwlte_handle_t|lwlte_tcp_conn_t)\s*\*\*" src/include/lwlte.h
rg -n "(lwlte_handle_t|lwlte_tcp_conn_t)\s*\*\s*\w" src/include/lwlte.h
```
```c
// 改前(所有权出参 ** → *)
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config, lwlte_handle_t **out_lte);
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config, lwlte_handle_t **out_lte);
esp_err_t lwlte_tcp_open(lwlte_handle_t *me, ..., lwlte_tcp_conn_t **out_conn);
// 普通签名
esp_err_t lwlte_destroy(lwlte_handle_t *me);
esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn, ...);
// 改后
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config, lwlte_handle_t *out_lte);
esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config, lwlte_handle_t *out_lte);
esp_err_t lwlte_tcp_open(lwlte_handle_t me, ..., lwlte_tcp_conn_t *out_conn);
esp_err_t lwlte_destroy(lwlte_handle_t me);
esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t conn, ...);
```

- [ ] **Step 5: 事件数据成员(lwlte.h)**

`lwlte_tcp_event_data_t` 内 `lwlte_tcp_conn_t *conn;` → `lwlte_tcp_conn_t conn;`(lwlte.h:277)。

- [ ] **Step 6: lwlte.c 实现 + 局部 + helper + 别名强转**

```bash
rg -n "(_handle_t|_conn_t)\s*\*" src/lwlte/lwlte.c
```
- 内部 helper:
```c
// 改前
static esp_err_t begin_api_call(lwlte_handle_t *me, bool require_core, core_handle_t **out_core);
static esp_err_t begin_mqtt_api_call(lwlte_handle_t *me, mqtt_client_handle_t **out_mqtt);
// 改后
static esp_err_t begin_api_call(lwlte_handle_t me, bool require_core, core_handle_t *out_core);
static esp_err_t begin_mqtt_api_call(lwlte_handle_t me, mqtt_client_handle_t *out_mqtt);
```
- 局部:`core_handle_t *core = NULL;` → `core_handle_t core = NULL;`;`mqtt_client_handle_t *mqtt = mqtt_client_create(...);` → `mqtt_client_handle_t mqtt = mqtt_client_create(...);`;`mqtt_client_handle_t *mqtt = me->mqtt;` → `mqtt_client_handle_t mqtt = me->mqtt;`;`void facade_core_cmd_done_cb(core_handle_t *core, ...)` → `(core_handle_t core, ...)`。
- **别名强转**(`lwlte.c:767`):`tcp_client_open(tcp, &open_config, (tcp_client_conn_t **)out_conn)` → `(tcp_client_conn_t *)out_conn`(砍一 `*`)。
- `lwlte_create_empty(lwlte_handle_t *out_lte)` 实现签名同步;`*out_lte = me;` **不动**。

- [ ] **Step 7: 两个 init 文件**

```bash
rg -n "lwlte_handle_t\s*\*" src/lwlte/lwlte_air780ep.c src/lwlte/lwlte_ml307r.c
```
`lwlte_air780ep_init(const lwlte_air780ep_config_t *config, lwlte_handle_t **out_lte)` → `(... lwlte_handle_t *out_lte)`(ml307r 同理);内部 `lwlte_handle_t *` 局部砍 `*`。

---

## Task 8: example/(应用层用法)

**Files:** `example/air780ep_basic_connect.c`、`air780ep_mqtt_client.c`、`air780ep_tcp_client.c`、`ml307r_basic_connect.c`、`ml307r_mqtt_client.c`、`ml307r_tcp_client.c`、`main.c`

- [ ] **Step 1: 枚举所有声明**
```bash
rg -n "(lwlte_handle_t|lwlte_tcp_conn_t)\s*\*" example
```

- [ ] **Step 2: 逐处砍 `*`(声明处;调用处不动)**

代表样例(每个示例文件结构相同):
```c
// 改前
static void do_ping(lwlte_handle_t *lte);
lwlte_handle_t *lte = NULL;
lwlte_handle_t *lte = (lwlte_handle_t *)arg;
static lwlte_tcp_conn_t *s_conn;
lwlte_tcp_conn_t *conn = data->conn;
// 改后
static void do_ping(lwlte_handle_t lte);
lwlte_handle_t lte = NULL;
lwlte_handle_t lte = (lwlte_handle_t)arg;      // 强转砍 *
static lwlte_tcp_conn_t s_conn;
lwlte_tcp_conn_t conn = data->conn;
```
> 强转 `(lwlte_handle_t *)arg` → `(lwlte_handle_t)arg`(Pattern A 下句柄即指针,单层强转)。调用 `lwlte_air780ep_init(&cfg, &lte)`、`lwlte_tcp_send(conn, ...)` 等**不变**。

---

## Task 9: tests/host/(契约测试断言同步)

**Files:** `tests/host/` 下断言命中 `_handle_t *` / `struct xxx_handle {` 的 11 个测试:
`test_mqtt_end_to_end_contract.py`、`test_lwlte_start_stop_lifecycle.py`、`test_lwlte_start_lifecycle.py`、`test_tcp_client_end_to_end_contract.py`、`test_ping_end_to_end_contract.py`、`test_ping_classes_doc_contract.py`、`test_ml307r_contract.py`、`test_air780ep_command_gated_init.py`、`test_air780ep_cpin_policy.py`、`test_mqtt_tls_ssl_contract.py`、`test_net_mgr_activation_flow.py`

- [ ] **Step 1: 枚举断言**
```bash
rg -n "_handle_t\s*\*|_conn_t\s*\*|struct\s+(lwlte_handle|core_handle|at_engine_handle|modem_handle|tcp_client_handle|mqtt_client_handle|ping_client_handle|tcp_client_conn|lwlte_tcp_conn)" tests/host
```

- [ ] **Step 2: 同步为 Pattern A + `_t` tag**

对每处断言应用同规则:签名串 `xxx_handle_t *me` → `xxx_handle_t me`;`struct xxx_handle\s*\{` → `struct xxx_t\s*\{`。代表样例:
```python
# 改前
self.assertIn("esp_err_t lwlte_mqtt_start(lwlte_handle_t *me);", ...)
self.assertIn("esp_err_t modem_mqtt_connect(modem_handle_t *me);", ...)
r"struct at_engine_handle\s*\{(?P<body>.*?)\n\};"      # test_lwlte_start_lifecycle.py:174
# 改后
self.assertIn("esp_err_t lwlte_mqtt_start(lwlte_handle_t me);", ...)
self.assertIn("esp_err_t modem_mqtt_connect(modem_handle_t me);", ...)
r"struct at_engine_t\s*\{(?P<body>.*?)\n\};"
```
> 仅改「期望字符串/正则」,不改测试逻辑。双指针出参断言(如 `lwlte_handle_t **out_lte`)→ 单 `*`(`lwlte_handle_t *out_lte`)。

---

## Task 10: 全局验证 + 提交

**Files:** 无新改动

- [ ] **Step 1: 残留检测(三项 grep 须全为空)**
```bash
rg "struct (lwlte_handle|core_handle|at_engine_handle|modem_handle|tcp_client_handle|mqtt_client_handle|ping_client_handle|tcp_client_conn|lwlte_tcp_conn)\b" src example
rg "(_handle_t|_conn_t)\s*\*\s*\*" src example
rg "typedef struct \w+ \w*(_handle_t|_conn_t)\s*;" src
```
Expected: 全部无输出。若有输出,回到对应 Task 修补。

- [ ] **Step 2: 编译验证**

优先 MCP:`esp-idf-eim_build_project`;否则 `source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build`。
Expected: 成功,零 `incomplete type` / 类型不匹配。若有类型不匹配错误,其行号即漏改的声明处,回去砍 `*`。

- [ ] **Step 3: 契约测试验证**
```bash
python3 -m pytest tests/host -q
```
Expected: 全绿(通过数与 Task 0 基线一致)。若有失败,失败用例指出的断言串即测试与源码不一致处,核对 Task 9 / 对应源码 Task。

- [ ] **Step 4: 语义抽样核对**
人工/检索确认以下「不变」项确实未改:
```bash
# 这些应仍有大量命中(说明未误改)
rg "sizeof\(\*me\)" src | wc -l
rg "\*out_lte =|\*out_conn =|\*out_core =" src | wc -l
rg "MODEM_CONTAINER_OF" src
```

- [ ] **Step 5: 提交(需用户授权)**

```bash
git add -A
git commit -m "refactor: migrate handle typedefs to Pattern A and align struct tags to _t

- 9 opaque handle types: typedef struct X *X_t (Pattern A)
- rename struct tags to _t form (align with esp_lcd / ESP-IDF mainstream)
- modem subclasses embed base via raw 'struct modem_t base' (container_of safe)
- update host contract tests to Pattern A signatures"
```
> 若未获授权,跳过本步,保留工作区改动。

---

## 回滚

若需回退:`git restore .`(未提交时)或 `git revert <commit>`。本迁移为纯机械、零行为改动,回滚无副作用。
