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
    modem_base_config_t base;           /**< 公共基础配置； Common base configuration */
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
