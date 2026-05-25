# 目录说明

## 顶级目录

```
esp-lwlte/
├── src/           # 组件源码
├── examples/      # 示例代码
├── docs/          # 项目文档
└── reference/     # 只读参考文档（git ignore）
```

## 各目录说明

### src/ — 组件源码

esp-lwlte 组件的全部源代码。公共 API 与内部实现按以下目录组织：

```
src/
├── include/       # 用户公共头文件，仅导出 lwlte.h、lwlte_air780ep.h
├── lwlte/         # 用户门面层（lwlte_t、模块 factory、资源组合根）
├── core/          # Core Service 层（网络状态机、PDP 管理、连接/重连）
├── modem/         # 模块适配层（modem_t 抽象 + 具体模块实现）
└── at_engine/     # AT 引擎层（通用 AT 协议引擎 + UART 硬件操作）
```

`src/include/` 只放真正给 App include 的用户 API。
`core.h`、`modem.h`、`modem_air780ep.h`、`at_engine.h` 放在各自模块目录，通过组件 `PRIV_INCLUDE_DIRS` 给内部源码使用，不导出给用户 App。

### examples/ — 示例代码

演示如何使用 esp-lwlte 组件的示例程序。每个示例是一个独立的 ESP-IDF 项目或 main 函数入口。

### docs/ — 项目文档

所有项目文档，包括架构设计、编码规范、构建调试指南等。AI 编码助手通过 `AGENTS.md` → `docs/agents/` 索引这些文档。

### reference/ — 只读参考文档

**git ignore 目录**，存放只读的参考资料（如归档的旧项目）。**规则**：

- **禁止修改**：除非当前任务明确说明需要修改 reference 内的文件，否则一律视为只读
- **仅供参考**：用于查阅旧版实现思路、AT 命令格式等历史信息
- **不参与构建**：reference 内的代码不会编译、不会测试、不会影响主项目
