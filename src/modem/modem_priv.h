/**
 * @file modem_priv.h
 * @brief 调制解调器内部接口
 * @details Modem internal interface
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
#include <stdbool.h>
#include <stddef.h>

#include "at_engine.h"
#include "modem.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*********************
 *      DEFINES
 *********************/

/**
 * @brief 从成员指针获取包含它的结构体指针
 * @details Get container structure pointer from member pointer
 */
#define MODEM_CONTAINER_OF(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/**********************
 *      TYPEDEFS
 **********************/

/**
 * @brief 无额外参数的调制解调器操作函数
 * @details Modem operation function without extra arguments
 */
typedef esp_err_t (*modem_no_arg_fn)(modem_handle_t me);

/**
 * @brief 获取模块信息操作函数
 * @details Get modem information operation function
 */
typedef esp_err_t (*modem_get_info_fn)(modem_handle_t me, modem_info_t *info);

/**
 * @brief 获取 SIM 状态操作函数
 * @details Get SIM status operation function
 */
typedef esp_err_t (*modem_get_sim_status_fn)(modem_handle_t me,
                                             modem_sim_status_t *status);

/**
 * @brief 获取信号质量操作函数
 * @details Get signal quality operation function
 */
typedef esp_err_t (*modem_get_signal_fn)(modem_handle_t me,
                                         modem_signal_t *signal);

/**
 * @brief 获取注册状态操作函数
 * @details Get registration status operation function
 */
typedef esp_err_t (*modem_get_registration_fn)(modem_handle_t me,
                                               modem_reg_status_t *status);

/**
 * @brief 获取分组域附着状态操作函数
 * @details Get packet attach status operation function
 */
typedef esp_err_t (*modem_get_packet_attach_status_fn)(modem_handle_t me,
                                                       bool *attached);

/**
 * @brief 设置 APN 操作函数
 * @details Set APN operation function
 */
typedef esp_err_t (*modem_set_apn_fn)(modem_handle_t me, uint8_t cid,
                                      const char *apn);

/**
 * @brief PDP 上下文 ID 操作函数
 * @details PDP context ID operation function
 */
typedef esp_err_t (*modem_pdp_cid_fn)(modem_handle_t me, uint8_t cid);

/**
 * @brief 获取 PDP 上下文操作函数
 * @details Get PDP context operation function
 */
typedef esp_err_t (*modem_get_pdp_context_fn)(modem_handle_t me, uint8_t cid,
                                              modem_pdp_context_t *pdp);

/**
 * @brief 写入并配置 SSL context 操作函数
 * @details Provision SSL context operation function
 */
typedef esp_err_t (*modem_ssl_provision_fn)(modem_handle_t me,
                                            const modem_ssl_context_config_t *config,
                                            const modem_ssl_credentials_t *credentials);

/**
 * @brief 查询 SSL context 状态操作函数
 * @details Query SSL context status operation function
 */
typedef esp_err_t (*modem_ssl_get_context_status_fn)(modem_handle_t me,
                                                     uint8_t context_id,
                                                     modem_ssl_context_status_t *status);

/**
 * @brief 配置 MQTT 操作函数
 * @details Configure MQTT operation function
 */
typedef esp_err_t (*modem_mqtt_configure_fn)(modem_handle_t me,
                                             const modem_mqtt_config_t *config);

/**
 * @brief MQTT 主题操作函数
 * @details MQTT topic operation function
 */
typedef esp_err_t (*modem_mqtt_topic_fn)(modem_handle_t me,
                                         const modem_mqtt_topic_t *topic);

/**
 * @brief MQTT 发布操作函数
 * @details MQTT publish operation function
 */
typedef esp_err_t (*modem_mqtt_publish_fn)(modem_handle_t me,
                                           const modem_mqtt_publish_t *publish);

/**
 * @brief 查询 MQTT 状态操作函数
 * @details Query MQTT status operation function
 */
typedef esp_err_t (*modem_mqtt_get_status_fn)(modem_handle_t me,
                                              modem_mqtt_status_t *status);

/**
 * @brief Ping 诊断操作函数
 * @details Ping diagnostic operation function
 */
typedef esp_err_t (*modem_ping_fn)(modem_handle_t me,
                                   const modem_ping_request_t *request,
                                   modem_ping_reply_t *replies,
                                   size_t max_replies,
                                   modem_ping_summary_t *summary);

/**
 * @brief 打开 Socket 操作函数
 * @details Open socket operation function
 */
typedef esp_err_t (*modem_socket_open_fn)(modem_handle_t me,
                                          const modem_socket_open_t *open);

/**
 * @brief 发送 Socket 数据操作函数
 * @details Send socket data operation function
 */
typedef esp_err_t (*modem_socket_send_fn)(modem_handle_t me,
                                          const modem_socket_send_t *send);

/**
 * @brief 接收 Socket 数据操作函数
 * @details Receive socket data operation function
 */
typedef esp_err_t (*modem_socket_recv_fn)(modem_handle_t me,
                                          const modem_socket_recv_t *recv,
                                          modem_socket_recv_result_t *result);

/**
 * @brief 关闭 Socket 操作函数
 * @details Close socket operation function
 */
typedef esp_err_t (*modem_socket_close_fn)(modem_handle_t me,
                                           const modem_socket_close_t *close);

/**
 * @brief 调制解调器虚函数表
 * @details Modem virtual function table
 */
typedef struct modem_ops {
    /* ── 生命周期管理； Lifecycle ─────────────────────────── */
    modem_no_arg_fn destroy;                         /**< 销毁子类资源； Destroy subclass resources */
    modem_no_arg_fn start;                           /**< 启动模块； Start modem */
    modem_no_arg_fn stop;                            /**< 停止并断电； Stop and power off */
    modem_no_arg_fn reset;                           /**< 复位模块； Reset modem */

    /* ── 状态与信息查询； Status & information query ──────── */
    modem_get_info_fn get_info;                      /**< 获取模块信息； Get modem information */
    modem_get_sim_status_fn get_sim_status;          /**< 获取 SIM 状态； Get SIM status */
    modem_get_signal_fn get_signal;                  /**< 获取信号质量； Get signal quality */
    modem_get_registration_fn get_registration;      /**< 获取注册状态； Get registration status */
    modem_get_packet_attach_status_fn get_packet_attach_status; /**< 获取分组域附着状态； Get packet attach status */

    /* ── 网络与 PDP 上下文； Network & PDP context ────────── */
    modem_set_apn_fn set_apn;                        /**< 设置 APN； Set APN */
    modem_pdp_cid_fn activate_pdp;                   /**< 激活 PDP； Activate PDP */
    modem_pdp_cid_fn deactivate_pdp;                 /**< 去激活 PDP； Deactivate PDP */
    modem_get_pdp_context_fn get_pdp_context;        /**< 获取 PDP 上下文； Get PDP context */

    /* ── SSL/TLS context； SSL/TLS context ─────────────────── */
    modem_ssl_provision_fn ssl_provision;            /**< 写入并配置 SSL context； Provision SSL context */
    modem_ssl_get_context_status_fn ssl_get_context_status; /**< 查询 SSL context 状态； Query SSL context status */

    /* ── MQTT 客户端； MQTT client ───────────────────────── */
    modem_mqtt_configure_fn mqtt_configure;          /**< 配置 MQTT； Configure MQTT */
    modem_no_arg_fn mqtt_tcp_connect;                /**< 建立 MQTT TCP 通道； Connect MQTT TCP channel */
    modem_no_arg_fn mqtt_connect;                    /**< 连接 MQTT； Connect MQTT */
    modem_no_arg_fn mqtt_disconnect;                 /**< 断开 MQTT； Disconnect MQTT */
    modem_no_arg_fn mqtt_tcp_disconnect;             /**< 断开 MQTT TCP 通道； Disconnect MQTT TCP channel */
    modem_mqtt_topic_fn mqtt_subscribe;              /**< 订阅 MQTT 主题； Subscribe MQTT topic */
    modem_mqtt_topic_fn mqtt_unsubscribe;            /**< 取消订阅 MQTT 主题； Unsubscribe MQTT topic */
    modem_mqtt_publish_fn mqtt_publish;              /**< 发布 MQTT 消息； Publish MQTT message */
    modem_mqtt_get_status_fn mqtt_get_status;        /**< 查询 MQTT 状态； Query MQTT status */

    /* ── Socket 客户端； Socket client ───────────────────── */
    modem_socket_open_fn socket_open;                /**< 打开 socket； Open socket */
    modem_socket_send_fn socket_send;                /**< 发送 socket 数据； Send socket data */
    modem_socket_recv_fn socket_recv;                /**< 接收 socket 数据； Receive socket data */
    modem_socket_close_fn socket_close;              /**< 关闭 socket； Close socket */

    /* ── 诊断； Diagnostics ──────────────────────────────── */
    modem_ping_fn ping;                              /**< 执行 Ping 诊断； Execute ping diagnostic */
} modem_ops_t;

/**
 * @brief 调制解调器基类
 * @details Modem base class
 */
struct modem_t {
    const modem_ops_t *ops;                       /**< 虚函数表； Virtual function table */
    at_engine_handle_t at;                              /**< AT 引擎句柄，借用； Borrowed AT engine handle */
    SemaphoreHandle_t lock;                       /**< 内部状态锁； Internal state lock */
    QueueHandle_t event_queue;                    /**< 事件队列； Event queue */
    TaskHandle_t event_task;                      /**< 事件任务； Event task */
    SemaphoreHandle_t event_task_done_sema;       /**< 事件任务退出信号量； Event task done semaphore */
    SemaphoreHandle_t event_cb_done_sema;         /**< 事件回调完成信号量； Event callback done semaphore */
    modem_event_callback_t event_cb;              /**< 上层事件回调； Upper-layer event callback */
    void *event_user_ctx;                         /**< 上层事件回调上下文； Upper-layer event callback context */
    int event_cb_active;                          /**< 正在执行的事件回调数量； Active event callback count */
    modem_state_t state;                          /**< 当前状态； Current state */
    bool destroying;                              /**< 是否正在销毁； Whether destroying */
    bool event_task_stop_requested;               /**< 事件任务停止请求； Event task stop request */
    const char *name;                             /**< 模块名称； Modem name */
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief 初始化调制解调器基类
 * @details Initialize modem base class
 * @param[in,out] me 调制解调器基类对象
 * @param[in] name 模块名称
 * @param[in] at AT 引擎句柄
 * @param[in] ops 虚函数表
 * @param[in] event_queue_size 事件队列长度，<=0 使用默认值
 * @param[in] event_task_stack 事件任务栈大小，<=0 使用默认值
 * @param[in] event_task_priority 事件任务优先级，<=0 使用默认值
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_NO_MEM: 内存不足
 */
esp_err_t modem_base_init(modem_handle_t me, const char *name, at_engine_handle_t at,
                          const modem_ops_t *ops, int event_queue_size,
                          int event_task_stack, int event_task_priority);

/**
 * @brief 反初始化调制解调器基类
 * @details Deinitialize modem base class
 * @param[in,out] me 调制解调器基类对象
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 */
esp_err_t modem_base_deinit(modem_handle_t me);

/**
 * @brief 停止调制解调器事件任务
 * @details Stop modem event task
 * @param[in,out] me 调制解调器句柄
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 */
esp_err_t modem_base_stop_event_task(modem_handle_t me);

/**
 * @brief 投递调制解调器事件
 * @details Post modem event
 * @param[in] me 调制解调器句柄
 * @param[in] event 调制解调器事件
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 *         - ESP_ERR_TIMEOUT: 事件队列已满
 */
esp_err_t modem_post_event(modem_handle_t me, const modem_event_t *event);

/**
 * @brief 设置调制解调器状态
 * @details Set modem state
 * @param[in,out] me 调制解调器句柄
 * @param[in] state 调制解调器状态
 * @return
 *         - ESP_OK: 成功
 *         - ESP_ERR_INVALID_ARG: 参数无效
 *         - ESP_ERR_INVALID_STATE: 状态错误
 */
esp_err_t modem_set_state(modem_handle_t me, modem_state_t state);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
