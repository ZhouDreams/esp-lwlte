/**
 * @file lwlte.h
 * @brief LTE 用户门面公共接口
 * @details LTE user facade public interface
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
#include "esp_err.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief LTE 用户门面句柄
 * @details LTE user facade handle
 */
typedef struct lwlte lwlte_t;

/**
 * @brief LTE 门面状态
 * @details LTE facade state
 */
typedef enum {
    LWLTE_STATE_STOPPED = 0,        /**< 已停止； Stopped */
    LWLTE_STATE_STARTING,           /**< 启动中； Starting */
    LWLTE_STATE_READY,              /**< 已就绪； Ready */
    LWLTE_STATE_NET_ACTIVATING,     /**< 网络激活中； Network activating */
    LWLTE_STATE_ONLINE,             /**< 网络在线； Online */
    LWLTE_STATE_ERROR,              /**< 错误； Error */
    LWLTE_STATE_DESTROYING,         /**< 销毁中； Destroying */
} lwlte_state_t;

/**
 * @brief LTE 网络状态
 * @details LTE network state
 */
typedef enum {
    LWLTE_NET_STATE_OFFLINE = 0,    /**< 离线； Offline */
    LWLTE_NET_STATE_ACTIVATING,     /**< 激活中； Activating */
    LWLTE_NET_STATE_ONLINE,         /**< 在线； Online */
    LWLTE_NET_STATE_ERROR,          /**< 错误； Error */
} lwlte_net_state_t;

/**
 * @brief LTE 用户事件 ID
 * @details LTE user event ID
 */
typedef enum {
    LWLTE_EVENT_STARTED = 0,        /**< 已启动； Started */
    LWLTE_EVENT_READY,              /**< 已就绪； Ready */
    LWLTE_EVENT_NET_CONNECTING,     /**< 网络连接中； Network connecting */
    LWLTE_EVENT_NET_ONLINE,         /**< 网络在线； Network online */
    LWLTE_EVENT_NET_OFFLINE,        /**< 网络离线； Network offline */
    LWLTE_EVENT_NET_ERROR,          /**< 网络错误； Network error */
    LWLTE_EVENT_STOPPED,            /**< 已停止； Stopped */
    LWLTE_EVENT_ERROR,              /**< 错误； Error */
} lwlte_event_id_t;

/**
 * @brief LTE 用户事件数据
 * @details LTE user event data
 */
typedef struct {
    lwlte_net_state_t net_state;    /**< 网络状态； Network state */
    int error_code;                 /**< 错误码； Error code */
} lwlte_event_data_t;

/**
 * @brief LTE 用户事件回调
 * @details LTE user event callback
 * @note data 指针仅在回调执行期间有效；如需异步使用，调用方必须复制其中内容。
 * @param[in] lte LTE 用户门面句柄
 * @param[in] event_id 事件 ID
 * @param[in] data 事件数据，可能为 NULL
 * @param[in] user_ctx 用户上下文
 */
typedef void (*lwlte_event_callback_t)(lwlte_t *lte,
                                       lwlte_event_id_t event_id,
                                       const lwlte_event_data_t *data,
                                       void *user_ctx);

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 销毁 LTE 用户门面
 * @details Destroy LTE user facade
 * @note me 必须是 lwlte_air780ep_init() 成功返回的句柄。ESP_OK 返回后句柄失效，调用方不得继续使用。
 * @note 销毁按门面持有资源的反向顺序执行。若清理失败，返回第一个下层错误，所有权保持保守状态，调用方不得假定资源已完全释放。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许销毁或销毁已在进行
 *         - ESP_FAIL: 下层清理失败
 *         - 其他 esp_err_t: 下层销毁或清理错误
 */
esp_err_t lwlte_destroy(lwlte_t *me);

/**
 * @brief 注册 LTE 用户事件回调
 * @details Register LTE user event callback
 * @note 门面仅保存一个用户回调槽位，重复调用会覆盖之前的回调和用户上下文。
 * @note callback 为 NULL 时注销当前用户回调；user_ctx 不被门面拥有，注册期间必须由调用方保持有效。
 * @param[in] me LTE 用户门面句柄
 * @param[in] callback 事件回调函数，NULL 表示注销
 * @param[in] user_ctx 用户上下文，原样传回回调
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_register_event_callback(lwlte_t *me,
                                        lwlte_event_callback_t callback,
                                        void *user_ctx);

/**
 * @brief 连接 LTE 网络
 * @details Connect LTE network
 * @note 该函数异步提交网络连接请求，ESP_OK 仅表示请求已提交，不表示网络已上线。
 * @note 最终联网结果通过用户事件回调或 lwlte_get_net_state() 查询获得。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许连接或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层连接错误
 */
esp_err_t lwlte_connect(lwlte_t *me);

/**
 * @brief 断开 LTE 网络
 * @details Disconnect LTE network
 * @note 该函数异步提交网络断开请求，ESP_OK 仅表示请求已提交，最终结果通过用户事件回调或状态查询获得。
 * @param[in] me LTE 用户门面句柄
 * @return
 *         - ESP_OK: 请求已提交
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 当前状态不允许断开或门面正在销毁
 *         - ESP_FAIL: 请求提交失败
 *         - 其他 esp_err_t: 下层断开错误
 */
esp_err_t lwlte_disconnect(lwlte_t *me);

/**
 * @brief 获取 LTE 门面状态
 * @details Get LTE facade state
 * @param[in] me LTE 用户门面句柄
 * @param[out] state LTE 门面状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_get_state(lwlte_t *me, lwlte_state_t *state);

/**
 * @brief 获取 LTE 网络状态
 * @details Get LTE network state
 * @param[in] me LTE 用户门面句柄
 * @param[out] state LTE 网络状态输出指针
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 门面正在销毁
 */
esp_err_t lwlte_get_net_state(lwlte_t *me, lwlte_net_state_t *state);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
