# AGENTS_ZH.md

本文件为 AI 编码助手在此仓库中工作时的索引。

> **保持同步：** `AGENTS.md`（英文）与 `AGENTS_ZH.md`（中文）必须时刻保持同步。修改其中一个时，必须在同一次提交中对另一个做出等价修改。

## 项目概述

esp-lwlte 是一个专为 ESP-IDF 平台自主研发的组件库，封装与 LTE 模块的 AT 指令通信逻辑，当前以 ESP32-C3 为主控、上海合宙 Air780EP 为 LTE 模块进行开发测试，未来计划适配多种 LTE 模块但保持 ESP-IDF 平台专属性。

## 文档索引

| 主题 | 文档 |
|------|------|
| 目录说明 | [docs/agents/directory-structure.md](docs/agents/directory-structure.md) |
| 功能 Roadmap | [docs/agents/feature-roadmap.md](docs/agents/feature-roadmap.md) |
| 架构概览 | [docs/agents/architecture.md](docs/agents/architecture.md) |
| 类设计 | [docs/agents/classes.md](docs/agents/classes.md) |
| 构建与调试 | [docs/agents/build-and-debug.md](docs/agents/build-and-debug.md) |
| 代码规范与模板 | [docs/agents/coding-style.md](docs/agents/coding-style.md) |
| C 语言 OOP 设计规范 | [docs/agents/oop-design.md](docs/agents/oop-design.md) |
| 错误处理机制 | [docs/agents/err.md](docs/agents/err.md) |
| Air780EP AT 指令与 URC | [docs/agents/at_cmd_air780ep.md](docs/agents/at_cmd_air780ep.md) |
| ML307R AT 指令与 URC | [docs/agents/at_cmd_ml307r.md](docs/agents/at_cmd_ml307r.md) |
| Air780EP CME ERROR 码 | [docs/agents/cme_error_air780ep.md](docs/agents/cme_error_air780ep.md) |
| 代码 Review 检查清单与流程 | [docs/agents/review-checklist.md](docs/agents/review-checklist.md) |

## 文件使用指南

- 编写代码时**必须**遵循 [代码规范与模板](docs/agents/coding-style.md) 和 [C 语言 OOP 设计规范](docs/agents/oop-design.md)
- 涉及编译项目、烧录、串口监视时**必须**遵循 [构建与调试](docs/agents/build-and-debug.md)
- [reference/esp-lwlte-old/](reference/esp-lwlte-old/) 为旧项目归档，仅供思路参考和信息查阅，主项目的构建与编码**必须**以本文件及上述规范为准

## 文档修改指南

- 不要直接修改本文件（AGENTS_ZH.md），除非需要调整索引结构
- 内容变更请修改 `docs/agents/` 下的对应文件
- 如需新增主题，在 `docs/agents/` 下创建新文件并在此添加链接
