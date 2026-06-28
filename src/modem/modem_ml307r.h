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
    modem_base_config_t base;           /**< 公共基础配置； Common base configuration */
    int at_rx_line_buf_size;            /**< AT 单行缓冲大小； AT line buffer size */
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
modem_handle_t modem_ml307r_create(at_engine_handle_t at,
                                   const modem_ml307r_config_t *config);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
