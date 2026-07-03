# Code Review List

> 审查清单。模块按"自下而上、基础先行"排序：AT Engine 是资源账本（维度 A）的典型案例、也是所有上层的基础，故排首位。
> 单次审查范围 = 一个模块；一个模块一次审完。已审模块若被改动，重新标记为 `⬜ Pending`（回归 review）。

| # | Module | Path | Status | Report |
|---|--------|------|--------|--------|
| 1 | AT Engine | src/at_engine/at_engine.c, at_engine.h | ✅ Done | report-01-at_engine.md |
| 2 | Modem Base / Wrapper | src/modem/modem.c, modem.h, modem_priv.h | ✅ Done | report-02-modem_base.md |
| 3 | Modem Air780EP Impl | src/modem/modem_air780ep.c, modem_air780ep.h | ✅ Done | report-03-modem_air780ep.md |
| 4 | Modem ML307R Impl | src/modem/modem_ml307r.c, modem_ml307r.h | ✅ Done | report-04-modem_ml307r.md |
| 5 | Core main + FSM | src/core/core.c, core_fsm.c | ✅ Done | report-05-core-fsm.md |
| 6 | Core Net/PDP mgr | src/core/net_mgr.c, pdp_mgr.c | ✅ Done | report-06-net-pdp-mgr.md |
| 7 | MQTT Client Service | src/mqtt_client/mqtt_client.c | ✅ Done | report-07-mqtt-client.md |
| 8 | TCP Client Service | src/tcp_client/tcp_client.c | ⬜ Pending | — |
| 9 | Ping Service | src/ping_client/ping_client.c | ⬜ Pending | — |
| 10 | LWLTE Facade (general) | src/lwlte/lwlte.c | ⬜ Pending | — |
| 11 | LWLTE Facade factories | src/lwlte/lwlte_air780ep.c, lwlte_ml307r.c | ⬜ Pending | — |

## 排序依据

- **#1 AT Engine 优先**：本项目踩过最深的坑是 `response_pool = max_response_lines * rx_line_buf_size` 撑爆 ESP32-C3 heap；维度 A（资源账本与乘法型分配）是最高优先级，AT Engine 是这条线的基础。
- **#2–#4 Modem 层**：紧邻 AT Engine 之上，含 URC 解析、事件队列/task、大量 AT 响应解析与堆分配（topic/payload 复制）。Air780EP/ML307R 单文件 6000+ 行，各自独立成模块。
- **#5–#6 Core 层**：`core_handle_t` 的组合成员（`core_fsm_t`/`net_mgr_t`/`pdp_mgr_t`）紧耦合，按 FSM 主控与 net/pdp 管理拆成两个模块。
- **#7–#9 Service 层**：MQTT / TCP / Ping 三个上层 service，均通过 `core_submit_cmd()` 串行化，各有独立 FSM。
- **#10–#11 Facade 层**：门面通用 API 与 composition root 工厂，最后审查。

## 硬件上下文（Phase 0 记录）

- **MCU**：ESP32-C3（RISC-V 单核），heap 受限（典型可用 ~300KB），无 PSRAM。
- **任务栈**：所有 FreeRTOS task 栈均在片内 RAM，栈溢出风险高；`rx_task`/`event_task`/`fsm_task` 栈大小由配置项控制。
- **UART**：AT Engine 直接操作 UART，非 DMA（cache 一致性风险低，但仍需确认 rx_buf_size 落在兼容区域）。
- **层间契约**：Facade → Service(MQTT/TCP/Ping) → Core → Modem → AT Engine，逐层单向；数据 ownership：modem_event_t payload 在回调期间有效（borrowed），上层入队前必须深拷贝（owned）。
