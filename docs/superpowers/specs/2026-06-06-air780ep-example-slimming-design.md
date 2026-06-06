# Air780EP 示例命名与瘦身设计

## 背景

当前 `example/basic_connect.c` 和 `example/mqtt_client.c` 都通过 `lwlte_air780ep_init()` 创建 Air780EP 门面，但示例名称仍是通用的 basic connect / mqtt client。仓库已新增 ML307R 门面后，这两个示例需要明确标识 Air780EP，避免和后续 ML307R 示例混淆。

两个示例当前还包含较多辅助代码，例如状态字符串转换、周期状态打印、重复清理函数、ThingsBoard RPC 回复和多 topic 订阅确认。作为入门 example，这些内容会掩盖最重要的调用顺序。

## 目标

- 将 basic connect 和 mqtt client 明确改为 Air780EP 示例。
- 将示例代码瘦身到能展示主流程的最小形态。
- 保留必要中文注释，让读者能理解关键步骤。
- MQTT 示例必须同时演示上行发布和下行接收。

## 非目标

- 不新增 ML307R connect 或 MQTT 示例。
- 不改动 `lwlte.h` 公共 API 或底层实现。
- 不把示例拆成公共 helper 文件，避免为简单 example 增加新的抽象层。
- 不保留 ThingsBoard RPC 回复流程；下行数据只打印。

## 方案选择

采用“保留最小完整主线”的方案。

`basic_connect` 保留 Air780EP 配置、事件回调、`lwlte_start()`、等待 `LWLTE_EVENT_NET_ONLINE`、执行一次 ping、常驻空闲。删除状态名大 switch、周期状态打印和独立 cleanup helper。

`mqtt_client` 保留 Air780EP + MQTT 配置、事件回调、联网、`lwlte_mqtt_start()`、订阅一个下行 topic、定时发布 telemetry、收到 MQTT 数据时打印 topic/payload。删除 RPC response、attributes/RPC 双订阅确认和状态名大 switch。

## 文件与命名

- `example/basic_connect.c` 重命名为 `example/air780ep_basic_connect.c`。
- `example/mqtt_client.c` 重命名为 `example/air780ep_mqtt_client.c`。
- `example/example.h` 的选择宏和 run 函数改为 `EXAMPLE_AIR780EP_BASIC_CONNECT`、`EXAMPLE_AIR780EP_MQTT_CLIENT`、`example_air780ep_basic_connect_run()`、`example_air780ep_mqtt_client_run()`。
- `example/main.c` 的默认选择和 switch 分支同步更新。
- `example/CMakeLists.txt` 使用新文件名。
- `example/README.md` 同步更新示例列表、选择宏、说明文字和日志示例。
- `docs/agents/directory-structure.md` 同步更新 example 文件名示例，避免活动 agent 指南继续引用旧文件名。

## Basic Connect 流程

1. 定义 Air780EP 默认硬件连接：UART1、GPIO0 TX、GPIO1 RX、GPIO2 EN、115200、CID 1。
2. 创建 `lwlte_air780ep_config_t`，只填写必须字段和 Air780EP 启动相关超时。
3. 调用 `lwlte_air780ep_init()` 创建门面。
4. 注册事件回调，事件回调只维护 online/error 标志并打印关键事件。
5. 调用 `lwlte_start()` 异步启动联网。
6. 等待网络 online 或超时。
7. online 后执行一次 `lwlte_ping()`，打印 ping 汇总。
8. 进入常驻循环，保持示例运行。

## MQTT Client 流程

1. 使用 Air780EP 默认硬件连接和 MQTT menuconfig 配置。
2. 创建启用 `mqtt_client` 的 `lwlte_air780ep_config_t`。
3. 注册事件回调并启动 LTE 联网。
4. 网络 online 后调用 `lwlte_mqtt_start()`。
5. MQTT connected 后订阅 `v1/devices/me/attributes`，用于验证下行接收。
6. 收到 `LWLTE_EVENT_MQTT_DATA` 时打印 topic 和 payload。
7. 主循环定时向 `v1/devices/me/telemetry` 发布简单 JSON telemetry。

## 注释策略

示例保留项目现有 C 文件模板区域和 static 函数 Doxygen 注释。主流程使用短中文注释说明关键步骤，例如“创建 Air780EP 门面”“启动异步联网”“订阅下行属性 topic”。不为每行代码增加注释，避免示例再次变长。

## 错误处理

示例遇到初始化、注册回调、启动、订阅等关键错误时打印错误并进入常驻等待。错误路径不再封装独立 cleanup helper；示例是长期运行入口，优先保证主流程清晰。

网络或 MQTT 等待超时打印 warning，并进入常驻等待，避免任务返回造成示例行为不明确。

## 验证

- 静态检查：确认没有旧宏、旧 run 函数或旧源文件名残留在 example 构建入口中。
- 编译验证：使用 ESP-IDF MCP 构建当前项目。
- 实机行为不在本次设计强制范围内；如后续需要，可烧录并观察 Air780EP 网络、ping、MQTT publish/subscribe 日志。
