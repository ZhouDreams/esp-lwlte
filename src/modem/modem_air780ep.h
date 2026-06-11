/**
 * @file modem_air780ep.h
 * @brief Air780EP 调制解调器公共接口
 * @details Air780EP modem public interface
 * @author JovisDreams
 * @date 2026-05-23
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stdint.h>

#include "driver/gpio.h"

#include "at_engine.h"
#include "modem.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief Air780EP 调制解调器配置
 * @details Air780EP modem configuration
 */
typedef struct {
    gpio_num_t en_pin;                  /**< EN GPIO，GPIO_NUM_NC 表示不控制； EN GPIO, GPIO_NUM_NC disables control */
    uint32_t reset_pulse_ms;            /**< 复位脉冲时间(EN 拉低保持时长)； Reset pulse time (EN low hold duration) */
    uint32_t ready_timeout_ms;          /**< 启动 AT OK 等待总超时； Startup AT OK wait total timeout */
    uint32_t default_cmd_timeout_ms;    /**< 默认命令超时； Default command timeout */
    int event_queue_size;               /**< 事件队列长度； Event queue size */
    int event_task_stack;               /**< 事件任务栈大小； Event task stack size */
    int event_task_priority;            /**< 事件任务优先级； Event task priority */
} modem_air780ep_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 Air780EP 调制解调器
 * @details Create Air780EP modem
 * @param[in] at AT 引擎句柄
 * @param[in] config Air780EP 调制解调器配置
 * @return
 *         - 调制解调器句柄: 成功
 *         - NULL: 失败
 */
modem_handle_t *modem_air780ep_create(at_engine_handle_t *at,
                                      const modem_air780ep_config_t *config);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
