# Code Review: LWLTE Facade Factories

**日期**: 2026-07-04
**文件**: `src/lwlte/lwlte_air780ep.c`, `src/lwlte/lwlte_ml307r.c`

## 🔴 高严重度

（无）

## 🟡 中严重度

（无）

## 🟢 低严重度

（无显著问题）

## 无问题维度

- **维度 A（资源账本）**：工厂函数不直接分配资源——调用各层 create 函数。预算由各层管理。无乘法型分配。
- **维度 B（内存安全）**：每步 create 失败调用 `cleanup_after_failure` → `lwlte_destroy` → `destroy_owned_resources` 按反序销毁已创建的子系统（tcp → mqtt → ping → core → modem → at）。未创建的子系统为 NULL，跳过。反序清理正确。
- **维度 C（并发）**：工厂在初始化期间调用，facade 尚未返回给调用方，无并发访问。`cleanup_after_failure` 中的 `lwlte_destroy` 使用 facade lock 但无竞争（单线程）。
- **维度 D（失败路径）**：所有 create 失败路径调用 `cleanup_after_failure`。若 `lwlte_destroy` 本身失败（如某子系统 destroy 返回错误），`restore_after_destroy_failure` 回滚 `destroying` 标志但 `me` 未释放——此时 facade 泄漏。这是边界场景（init 清理期间 destroy 失败），可接受的 best-effort 行为。
- **维度 E（AT/Modem）**：工厂正确组装 AT Engine → Modem → Core 链路。无直接 AT 交互。
- **维度 F（跨模块契约）**：event loop 从 config 传递到 core 和 facade handler。APN NULL 时 fallback 到空字符串。AT line buf size 默认 2048，同时传给 AT Engine 和 ML307R modem adapter（Air780EP 不需要）。
- **维度 G（类型与边界）**：`validate_config` 校验 UART num 范围、GPIO（required + optional）、baud rate > 0、primary_cid == 1、所有可配置整数 >= 0。校验完整。
- **维度 H（代码质量）**：Air780EP 有清晰的步骤分隔注释。两个工厂的 `validate_config`/`gpio_*`/`non_negative_int`/`cleanup_after_failure` 代码重复，但这是设计选择——提取公共 helper 需要额外内部头文件，当前自包含方式更清晰。

## 备注

两个工厂文件是纯组合根（composition root），逻辑简单正确。`cleanup_after_failure` → `lwlte_destroy` 的反序清理链依赖 Module #10 中修复的 `end_api_call` 竞态（已在本轮修复）。工厂创建过程中 `active_api_calls == 0`，`wait_api_calls_idle` 立即返回，无阻塞。
