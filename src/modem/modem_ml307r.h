/**
 * @file modem_ml307r.h
 * @brief ML307R 调制解调器公共接口
 * @details ML307R modem public interface
 * @author JovisDreams
 * @date 2026-06-06
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
 * @brief ML307R 调制解调器配置
 * @details ML307R modem configuration
 */
typedef struct {
    gpio_num_t en_pin;                  /**< EN GPIO，GPIO_NUM_NC 表示不控制； EN GPIO, GPIO_NUM_NC disables control */
    uint32_t reset_pulse_ms;            /**< 复位脉冲时间(EN 拉低保持时长)； Reset pulse time (EN low hold duration) */
    uint32_t ready_timeout_ms;          /**< 启动 AT OK 等待总超时； Startup AT OK wait total timeout */
    uint32_t default_cmd_timeout_ms;    /**< 默认命令超时； Default command timeout */
    int event_queue_size;               /**< 事件队列长度； Event queue size */
    int event_task_stack;               /**< 事件任务栈大小； Event task stack size */
    int event_task_priority;            /**< 事件任务优先级； Event task priority */
} modem_ml307r_config_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 ML307R 调制解调器
 * @details Create ML307R modem
 * @param[in] at AT 引擎句柄
 * @param[in] config ML307R 调制解调器配置
 * @return
 *         - 调制解调器句柄: 成功
 *         - NULL: 失败
 */
modem_handle_t *modem_ml307r_create(at_engine_handle_t *at,
                                    const modem_ml307r_config_t *config);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
