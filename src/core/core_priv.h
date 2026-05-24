/**
 * @file core_priv.h
 * @brief LTE 核心服务内部接口
 * @details LTE core service internal interface
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

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "lwlte_core.h"
#include "modem.h"

/*********************
 *      DEFINES
 *********************/
#define CORE_MAX_PDP_CONTEXTS                 4
#define CORE_DEFAULT_PRIMARY_CID              1
#define CORE_DEFAULT_NET_ACTIVATE_TIMEOUT_MS  120000
#define CORE_DEFAULT_RECONNECT_DELAY_MS       5000
#define CORE_DEFAULT_FSM_QUEUE_SIZE           16
#define CORE_DEFAULT_FSM_TASK_STACK           4096
#define CORE_DEFAULT_FSM_TASK_PRIORITY        8
#define CORE_EVENT_QUEUE_SIZE                 16
#define CORE_EVENT_TASK_STACK                 4096
#define CORE_EVENT_TASK_PRIORITY              8
#define CORE_FSM_WAIT_MS                      100
#define CORE_NET_MAX_RETRY                    3

/**********************
 *      TYPEDEFS
 **********************/
typedef enum {
    CORE_SIG_MODEM_EVENT = 0,
    CORE_SIG_START,
    CORE_SIG_STOP,
    CORE_SIG_NET_ACTIVATE,
    CORE_SIG_NET_DEACTIVATE,
    CORE_SIG_NET_STEP_DONE,
    CORE_SIG_NET_STEP_TIMEOUT,
    CORE_SIG_RECONNECT,
} core_fsm_sig_type_t;

typedef struct {
    core_fsm_sig_type_t type;
    modem_event_t modem_event;
    int error_code;
} core_fsm_sig_t;

typedef struct {
    TaskHandle_t task;
    QueueHandle_t queue;
    SemaphoreHandle_t task_done_sema;
    bool running;
    bool stop_requested;
} core_fsm_t;

typedef enum {
    NET_STEP_IDLE = 0,
    NET_STEP_CHECK_SIM,
    NET_STEP_CHECK_SIGNAL,
    NET_STEP_CHECK_REGISTRATION,
    NET_STEP_SET_APN,
    NET_STEP_ACTIVATE_PDP,
    NET_STEP_DONE,
    NET_STEP_ERROR,
} net_mgr_step_t;

typedef struct {
    net_mgr_step_t current_step;
    uint32_t step_start_time_ms;
    uint32_t step_timeout_ms;
    int retry_count;
    int max_retry;
    TimerHandle_t reconnect_timer;
    SemaphoreHandle_t reconnect_cb_done_sema;
    int reconnect_cb_active;
    lwlte_net_state_t state;
    bool reconnect_enabled;
} net_mgr_t;

typedef struct {
    modem_pdp_context_t contexts[CORE_MAX_PDP_CONTEXTS];
    uint8_t primary_cid;
} pdp_mgr_t;

struct lwlte_core {
    lwlte_core_config_t config;
    modem_t *modem;
    esp_event_loop_handle_t event_loop;
    core_fsm_t fsm;
    net_mgr_t net_mgr;
    pdp_mgr_t pdp_mgr;
    lwlte_core_state_t state;
    bool destroying;
    bool destroy_in_progress;
    SemaphoreHandle_t lock;
    TaskHandle_t event_loop_task;
    SemaphoreHandle_t event_callback_done_sema;
    TaskHandle_t event_callback_task;
    int event_callback_active;
    bool event_callback_waiting;
    lwlte_core_event_callback_t event_callback;
    void *event_user_ctx;
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/
esp_err_t core_fsm_init(lwlte_core_t *me);

/**
 * @brief 停止 FSM 任务
 * @details Stop FSM task without deleting queue/semaphore resources
 * @param[in] me LTE 核心服务句柄
 */
void core_fsm_stop(lwlte_core_t *me);

void core_fsm_deinit(lwlte_core_t *me);
esp_err_t core_fsm_send(lwlte_core_t *me, const core_fsm_sig_t *sig);
bool core_fsm_is_task(lwlte_core_t *me);

esp_err_t net_mgr_init(lwlte_core_t *me);
esp_err_t net_mgr_deinit(lwlte_core_t *me);
void net_mgr_cancel_reconnect(lwlte_core_t *me);
esp_err_t net_mgr_start_activation(lwlte_core_t *me);
esp_err_t net_mgr_deactivate(lwlte_core_t *me);
esp_err_t net_mgr_handle_pdp_activated(lwlte_core_t *me,
                                       const modem_pdp_context_t *pdp);
esp_err_t net_mgr_handle_pdp_deactivated(lwlte_core_t *me,
                                         const modem_pdp_context_t *pdp);
esp_err_t net_mgr_get_state(lwlte_core_t *me, lwlte_net_state_t *state);
esp_err_t net_mgr_set_state(lwlte_core_t *me, lwlte_net_state_t state);
void net_mgr_set_reconnect_enabled(lwlte_core_t *me, bool enabled);

esp_err_t pdp_mgr_init(pdp_mgr_t *me, uint8_t primary_cid);
esp_err_t pdp_mgr_get(const pdp_mgr_t *me, uint8_t cid,
                      modem_pdp_context_t *pdp);
esp_err_t pdp_mgr_update(pdp_mgr_t *me, const modem_pdp_context_t *pdp);
esp_err_t pdp_mgr_set_active(pdp_mgr_t *me, uint8_t cid, bool active);

esp_err_t core_set_state(lwlte_core_t *me, lwlte_core_state_t state);
lwlte_core_state_t core_get_state_value(lwlte_core_t *me);
bool core_is_destroying(lwlte_core_t *me);
esp_err_t core_post_event(lwlte_core_t *me, lwlte_core_event_id_t event_id,
                          const lwlte_core_event_data_t *data);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif
