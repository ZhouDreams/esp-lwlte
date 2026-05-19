# 构建与调试环境

## 工具优先级

本项目已接入 **Espressif MCP server**（`espressif-docs`），能通过 MCP 完成的操作**必须优先使用 MCP**：

| 场景 | 优先级 | 说明 |
|------|--------|------|
| 查阅 ESP-IDF API / 配置 / 文档 | **MCP 优先** | 使用 `search_espressif_sources` 检索官方文档 |
| 编译 / 烧录 / menuconfig | **MCP 优先** | 如有对应 MCP 工具则优先使用 |
| 串口监视 | **脚本** | 无对应 MCP，使用 `serial_monitor.py` |

核心原则：**能用 MCP 的优先用 MCP，MCP 覆盖不到的用脚本或命令行兜底。**

## ESP-IDF 路径

当前机器上 ESP-IDF 安装路径为：

```text
~/.espressif/v6.0/esp-idf
```

## 环境初始化

在终端中执行以下命令初始化 ESP-IDF 环境：

```bash
source ~/.espressif/v6.0/esp-idf/export.sh
```

## 串口设备查找

串口设备名不固定，每次调试前需要动态查找：

```bash
ls /dev/tty.usb* /dev/cu.usb* 2>/dev/null
```

也可通过以下命令确认设备是否已连接：

```bash
idf.py -p /dev/<PORT> monitor
```

## 常用命令

```bash
idf.py build                              # 编译项目
idf.py -p /dev/<PORT> flash               # 烧录
idf.py -p /dev/<PORT> monitor             # 串口监视
idf.py -p /dev/<PORT> flash monitor       # 烧录并监视
idf.py menuconfig                         # 菜单配置
idf.py fullclean build                    # 清理后重新构建
```

## AI 对构建与验证的默认态度

**自动构建、自动烧录、自动读取串口日志是可行的。**
因此，当用户要求验证时，AI 不应默认假设"只能进行静态分析"。

建议的默认优先级如下：

1. 先进行静态分析
2. 若用户要求验证，则优先尝试构建
3. 若问题涉及板级行为，则进一步尝试烧录与串口日志验证
4. 若执行失败，需明确说明失败发生在哪一步，以及可能原因

AI 输出中应明确区分：

- 仅完成静态分析
- 已完成编译验证
- 已完成烧录验证
- 已完成串口日志验证
- 尚未完成实机功能验证

## 串口调试经验

### 串口被占用

如果 `idf.py flash` 或 `idf.py monitor` 提示串口占用，应优先检查：

- 是否有上一次遗留的 monitor 进程未退出
- 是否有其他串口工具仍在占用串口

处理方式通常为：

- 关闭相关进程
- 重新插拔设备（必要时）
- 再次尝试 `flash` 或 `monitor`

### AI 串口监控脚本（推荐）

`idf.py monitor` 需要 TTY，在 AI agent 的非交互环境中无法运行。
使用项目内置脚本替代：

```bash
# 先初始化 ESP-IDF 环境（提供 pyserial）
source ~/.espressif/v6.0/esp-idf/export.sh

# 复位板子并读取 20 秒串口日志
python3 docs/agents/serial_monitor.py --timeout 20

# 指定端口
python3 docs/agents/serial_monitor.py --timeout 15 --port /dev/cu.usbserial-130

# 不复位，只读取当前输出
python3 docs/agents/serial_monitor.py --timeout 10 --no-reset
```

参数说明：

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--timeout` | 读取时长（秒） | 15 |
| `--port` | 串口设备路径 | 自动检测第一个 `/dev/cu.usb*` |
| `--baud` | 波特率 | 115200 |
| `--no-reset` | 不执行硬件复位，仅读取 | 默认会复位 |

### macOS 串口读取兜底方案

当脚本不可用时，可以直接通过命令行读取串口日志：

```bash
stty -f /dev/<PORT> 115200
cat /dev/<PORT>
```

或者使用 `screen`：

```bash
screen /dev/<PORT> 115200
```

该方案适用于：

- 只查看启动日志
- 只抓取一段异常信息
- 快速确认板子是否发生重启、WDT、abort、panic

### 串口日志分析要求

若 AI 协助分析串口日志，不应只停留在"有报错"这一层面，还应尽量归纳：

- 报错发生在哪个阶段（上电启动、初始化、运行中）
- 是 panic、abort、watchdog、assert 还是普通 warning
- 最可能关联哪个模块
- 下一步优先应该检查哪些代码或配置
