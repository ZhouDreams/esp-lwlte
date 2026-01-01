/*
    File: lwlte_sys_thread.c
    Author: JovisDreams
    Date: 2025-12-29
    Description: System Thread encapsulation source file
    Platform: ESP-IDF
*/
#include "lwlte_sys_thread.h"
#include "lwlte_sys_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 用一个 trampoline 包一层，避免直接暴露 FreeRTOS 语义 */
typedef struct {
    lwlte_sys_thread_fn_t fn;
    void* arg;
} lwlte_thread_wrap_t;

static void lwlte_thread_trampoline(void* p)
{
    lwlte_thread_wrap_t w = *(lwlte_thread_wrap_t*)p;
    vPortFree(p);

    /* run user function */
    w.fn(w.arg);

    /* if user function ever returns, delete task */
    vTaskDelete(NULL);
}

lwlte_sys_thread_t lwlte_sys_thread_create(lwlte_sys_thread_fn_t fn, const lwlte_sys_thread_cfg_t* cfg)
{
    if (!fn || !cfg) {
        return NULL;
    }

    lwlte_thread_wrap_t* w =
        (lwlte_thread_wrap_t*)pvPortMalloc(sizeof(lwlte_thread_wrap_t));
    if (!w) {
        return NULL;
    }

    w->fn  = fn;
    w->arg = cfg->arg;

    TaskHandle_t th = NULL;
    BaseType_t ok = xTaskCreate(
        lwlte_thread_trampoline,
        cfg->name ? cfg->name : "lwlte",
        cfg->stack_size / sizeof(StackType_t),
        (void*)w,
        (UBaseType_t)cfg->priority,
        &th
    );

    if (ok != pdPASS) {
        vPortFree(w);
        return NULL;
    }

    return (lwlte_sys_thread_t)th;
}

void lwlte_sys_thread_delete(lwlte_sys_thread_t t)
{
    vTaskDelete((TaskHandle_t)t);
}

void lwlte_sys_thread_sleep(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

lwlte_tick_t lwlte_sys_time_get_ticks(void)
{
    return xTaskGetTickCount();
}

lwlte_tick_t lwlte_sys_time_get_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}
