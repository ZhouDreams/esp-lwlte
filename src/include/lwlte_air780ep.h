/**
 * @file lwlte_air780ep.h
 * @brief Air780EP LTE 用户门面公共接口
 * @details Air780EP LTE user facade public interface
 * @author JovisDreams
 * @date 2026-05-25
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#include "lwlte.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief Air780EP LTE 初始化配置
 * @details Air780EP LTE initialization configuration
 * @note uart_num、uart_tx_pin、uart_rx_pin、uart_baud_rate 和 primary_cid 为必填字段。
 * @note en_pin 可设为 GPIO_NUM_NC，以禁用门面对 EN GPIO 的控制。
 * @note 超时、任务和缓冲区字段为 0 时使用下层默认值；init_ready_timeout_ms 为 0 时使用门面默认值。
 * @note init_ready_timeout_ms 覆盖 Air780EP RDY 等待和 Core ready 等待的初始化总超时。
 * @note apn 为 NULL 或空字符串表示门面不配置 APN 字符串。
 * @note UART 端口必须满足 UART_NUM_0 <= uart_num < UART_NUM_MAX；UART TX/RX 必须是有效 GPIO 且不能为 GPIO_NUM_NC。
 * @note uart_baud_rate 必须大于 0；Air780EP 门面当前仅支持 primary_cid 为 1。
 * @note 有符号的队列、任务和缓冲区字段允许 0 表示默认值，非 0 值必须大于 0。
 */
typedef struct {
    uart_port_t uart_num;                 /**< 必填 UART 端口号； Required UART port number */
    gpio_num_t uart_tx_pin;               /**< 必填 UART TX GPIO，不能为 GPIO_NUM_NC； Required UART TX GPIO, not GPIO_NUM_NC */
    gpio_num_t uart_rx_pin;               /**< 必填 UART RX GPIO，不能为 GPIO_NUM_NC； Required UART RX GPIO, not GPIO_NUM_NC */
    int uart_baud_rate;                   /**< 必填 UART 波特率，必须大于 0； Required UART baud rate, must be > 0 */
    gpio_num_t en_pin;                    /**< 可选模块 EN GPIO，GPIO_NUM_NC 表示不控制； Optional module EN GPIO, GPIO_NUM_NC disables control */
    const char *apn;                      /**< 可选 APN，NULL/空表示门面不配置； Optional APN, NULL/empty means facade does not configure it */
    uint8_t primary_cid;                  /**< 必填主 PDP 上下文 ID，Air780EP 门面当前仅支持 1； Required primary PDP context ID, Air780EP facade currently supports 1 only */
    bool auto_connect;                    /**< ready 后是否自动提交联网请求，不等待网络上线； Whether to submit connect after ready, without waiting online */
    uint32_t init_ready_timeout_ms;        /**< 初始化 RDY+Core ready 总超时，0 使用门面默认值； Total init RDY+Core ready timeout, 0 uses facade default */
    uint32_t net_activate_timeout_ms;      /**< 网络激活总超时，0 使用 Core 默认值； Network activation timeout, 0 uses Core default */
    uint32_t reconnect_delay_ms;           /**< 重连延迟，0 使用 Core 默认值； Reconnect delay, 0 uses Core default */
    int at_rx_buf_size;                   /**< AT RX 缓冲大小，0 使用默认值； AT RX buffer size, 0 uses default */
    int at_rx_task_stack;                 /**< AT RX 任务栈大小，0 使用默认值； AT RX task stack, 0 uses default */
    int at_rx_task_priority;              /**< AT RX 任务优先级，0 使用默认值； AT RX task priority, 0 uses default */
    int at_rx_line_buf_size;              /**< AT 单行缓冲大小，0 使用默认值； AT line buffer size, 0 uses default */
    int at_cmd_default_timeout_ms;         /**< AT 默认命令超时，0 使用默认值； AT default command timeout, 0 uses default */
    int at_max_response_lines;             /**< AT 最大响应行数，0 使用默认值； AT maximum response lines, 0 uses default */
    uint32_t modem_reset_pulse_ms;         /**< Modem 复位脉冲(EN 拉低保持)时长，0 表示不额外等待； Modem reset pulse (EN low hold) length, 0 skips extra wait */
    uint32_t modem_default_cmd_timeout_ms; /**< Modem 默认命令超时，0 使用默认值； Modem default command timeout, 0 uses default */
    int modem_event_queue_size;            /**< Modem 事件队列长度，0 使用默认值； Modem event queue size, 0 uses default */
    int modem_event_task_stack;            /**< Modem 事件任务栈大小，0 使用默认值； Modem event task stack, 0 uses default */
    int modem_event_task_priority;         /**< Modem 事件任务优先级，0 使用默认值； Modem event task priority, 0 uses default */
    int core_fsm_queue_size;               /**< Core FSM 队列长度，0 使用默认值； Core FSM queue size, 0 uses default */
    int core_fsm_task_stack;               /**< Core FSM 任务栈大小，0 使用默认值； Core FSM task stack, 0 uses default */
    int core_fsm_task_priority;            /**< Core FSM 任务优先级，0 使用默认值； Core FSM task priority, 0 uses default */
} lwlte_air780ep_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 初始化 Air780EP LTE 用户门面
 * @details Initialize Air780EP LTE user facade
 * @note 该函数阻塞直到 AT Engine、Modem 和 Core 创建并启动到 ready，或发生错误/超时。
 * @note ESP_OK 返回时 *out_lte 为可用句柄，所有权转移给调用方，必须通过 lwlte_destroy() 释放。
 * @note 非 ESP_OK 返回时不会转移句柄所有权，门面会尽力释放已创建的内部资源。
 * @note ESP_OK 不保证网络在线；auto_connect 为 true 时仅在 ready 后提交连接请求，不等待网络上线。
 * @note config 及其 apn 指针由调用方拥有，在函数返回前必须保持有效。
 * @param[in] config Air780EP LTE 初始化配置
 * @param[out] out_lte LTE 用户门面句柄输出指针
 * @return
 *         - ESP_OK: 初始化成功，门面已 ready
 *         - ESP_ERR_INVALID_ARG: 参数无效、必填字段缺失或字段超出有效范围
 *         - ESP_ERR_NO_MEM: 内存不足
 *         - ESP_ERR_TIMEOUT: 等待 ready 超时或下层命令超时
 *         - ESP_ERR_INVALID_STATE: 下层状态错误或 auto_connect 请求无法提交
 *         - ESP_FAIL: GPIO、UART、Modem、Core 或连接请求提交失败
 *         - 其他 esp_err_t: 下层初始化、启动或清理错误
 */
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_t **out_lte);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
