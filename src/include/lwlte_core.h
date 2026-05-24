/**
 * @file lwlte_core.h
 * @brief LTE 核心服务公共接口
 * @details LTE core service public interface
 * @author JovisDreams
 * @date 2026-05-24
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

#include "esp_err.h"
#include "esp_event.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief 调制解调器句柄前置声明
 * @details Modem handle forward declaration
 */
typedef struct modem modem_t;

/**
 * @brief LTE 核心服务句柄
 * @details LTE core service handle
 */
typedef struct lwlte_core lwlte_core_t;

/**
 * @brief LTE 核心服务配置
 * @details LTE core service configuration
 */
typedef struct {
    const char *apn;                     /**< APN； APN */
    uint8_t primary_cid;                 /**< 主 PDP 上下文 ID； Primary PDP context ID */
    uint32_t net_activate_timeout_ms;    /**< 网络激活总超时； Network activation timeout */
    uint32_t reconnect_delay_ms;         /**< 重连延迟； Reconnect delay */
    bool auto_connect;                   /**< 是否自动联网； Whether to connect automatically */
    int fsm_queue_size;                  /**< FSM 队列长度； FSM queue size */
    int fsm_task_stack;                  /**< FSM 任务栈大小； FSM task stack size */
    int fsm_task_priority;               /**< FSM 任务优先级； FSM task priority */
} lwlte_core_config_t;

/**
 * @brief LTE 核心服务状态
 * @details LTE core service state
 */
typedef enum {
    LWLTE_CORE_STATE_STOPPED = 0,        /**< 已停止； Stopped */
    LWLTE_CORE_STATE_STARTING,           /**< 启动中； Starting */
    LWLTE_CORE_STATE_READY,              /**< 已就绪； Ready */
    LWLTE_CORE_STATE_NET_ACTIVATING,     /**< 网络激活中； Network activating */
    LWLTE_CORE_STATE_ONLINE,             /**< 网络在线； Online */
    LWLTE_CORE_STATE_ERROR,              /**< 错误； Error */
    LWLTE_CORE_STATE_DESTROYING,         /**< 销毁中； Destroying */
} lwlte_core_state_t;

/**
 * @brief LTE 网络状态
 * @details LTE network state
 */
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,         /**< 离线； Offline */
    LWLTE_NET_STATE_ACTIVATING,          /**< 激活中； Activating */
    LWLTE_NET_STATE_ONLINE,              /**< 在线； Online */
    LWLTE_NET_STATE_ERROR,               /**< 错误； Error */
} lwlte_net_state_t;

/**
 * @brief LTE 核心服务事件基
 * @details LTE core service event base
 */
ESP_EVENT_DECLARE_BASE(LWLTE_CORE_EVENT);

/**
 * @brief LTE 核心服务事件 ID
 * @details LTE core service event ID
 */
typedef enum {
    LWLTE_CORE_EVENT_STARTED = 0,        /**< Core 已启动； Core started */
    LWLTE_CORE_EVENT_READY,              /**< Core 已就绪； Core ready */
    LWLTE_CORE_EVENT_NET_CONNECTING,     /**< 网络连接中； Network connecting */
    LWLTE_CORE_EVENT_NET_ONLINE,         /**< 网络在线； Network online */
    LWLTE_CORE_EVENT_NET_OFFLINE,        /**< 网络离线； Network offline */
    LWLTE_CORE_EVENT_NET_ERROR,          /**< 网络错误； Network error */
    LWLTE_CORE_EVENT_STOPPED,            /**< Core 已停止； Core stopped */
    LWLTE_CORE_EVENT_ERROR,              /**< Core 错误； Core error */
} lwlte_core_event_id_t;

/**
 * @brief LTE 核心服务事件数据
 * @details LTE core service event data
 */
typedef struct {
    lwlte_net_state_t net_state;         /**< 网络状态； Network state */
    int error_code;                      /**< 错误码； Error code */
} lwlte_core_event_data_t;

/**
 * @brief LTE 核心服务事件回调
 * @details LTE core service event callback
 * @param[in] core LTE 核心服务句柄
 * @param[in] event_id LTE 核心服务事件 ID
 * @param[in] data LTE 核心服务事件数据，可能为 NULL
 * @param[in] user_ctx 用户上下文
 */
typedef void (*lwlte_core_event_callback_t)(lwlte_core_t *core,
                                             lwlte_core_event_id_t event_id,
                                             const lwlte_core_event_data_t *data,
                                             void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 创建 LTE 核心服务
 * @details Create LTE core service
 * @note modem 由调用方持有，Core 仅借用，不会销毁该调制解调器。
 * @param[in] config LTE 核心服务配置
 * @param[in] modem 调制解调器句柄
 * @return
 *         - 非 NULL: 创建成功，返回 LTE 核心服务句柄
 *         - NULL: 参数无效或内存不足
 */
lwlte_core_t *lwlte_core_create(const lwlte_core_config_t *config,
                                 modem_t *modem);

/**
 * @brief 销毁 LTE 核心服务
 * @details Destroy LTE core service
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t lwlte_core_destroy(lwlte_core_t *me);

/**
 * @brief 启动 LTE 核心服务
 * @details Start LTE core service
 * @note 该函数异步提交启动请求，返回 ESP_OK 表示请求已提交；最终状态通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_core_start(lwlte_core_t *me);

/**
 * @brief 停止 LTE 核心服务
 * @details Stop LTE core service
 * @note 该函数异步提交停止请求，返回 ESP_OK 表示请求已提交；最终状态通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_core_stop(lwlte_core_t *me);

/**
 * @brief 注册 LTE 核心服务事件回调
 * @details Register LTE core service event callback
 * @note Core 仅保存一个回调槽位，重复调用会覆盖之前的回调和用户上下文。
 * @param[in] me LTE 核心服务句柄
 * @param[in] callback 事件回调函数
 * @param[in] user_ctx 用户上下文
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t lwlte_core_register_event_callback(lwlte_core_t *me,
                                              lwlte_core_event_callback_t callback,
                                              void *user_ctx);

/**
 * @brief 获取 LTE 核心服务事件循环
 * @details Get LTE core service event loop
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - 非 NULL: LTE 核心服务事件循环句柄
 *         - NULL: 参数无效或事件循环未创建
 */
esp_event_loop_handle_t lwlte_core_get_event_loop(lwlte_core_t *me);

/**
 * @brief 获取 LTE 核心服务状态
 * @details Get LTE core service state
 * @param[in] me LTE 核心服务句柄
 * @param[out] state LTE 核心服务状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t lwlte_core_get_state(lwlte_core_t *me, lwlte_core_state_t *state);

/**
 * @brief 获取 LTE 网络状态
 * @details Get LTE network state
 * @param[in] me LTE 核心服务句柄
 * @param[out] state LTE 网络状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 */
esp_err_t lwlte_core_get_net_state(lwlte_core_t *me, lwlte_net_state_t *state);

/**
 * @brief 连接 LTE 网络
 * @details Connect LTE network
 * @note 该函数异步提交网络连接请求，返回 ESP_OK 表示请求已提交；最终结果通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_core_connect(lwlte_core_t *me);

/**
 * @brief 断开 LTE 网络
 * @details Disconnect LTE network
 * @note 该函数异步提交网络断开请求，返回 ESP_OK 表示请求已提交；最终结果通过事件或回调通知。
 * @param[in] me LTE 核心服务句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_FAIL: 请求提交失败
 */
esp_err_t lwlte_core_disconnect(lwlte_core_t *me);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
