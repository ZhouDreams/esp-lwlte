/*
    File: lwlte_sys_thread.h
    Author: JovisDreams
    Date: 2025-12-29
    Description: System Thread encapsulation header file
    Platform: ESP-IDF
*/
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "lwlte_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* opaque handle */
typedef void* lwlte_sys_thread_t;

/* thread entry function */
typedef void (*lwlte_sys_thread_fn_t)(void* arg);

/* thread config */
typedef struct {
    const char* name;        /* thread name (optional) */
    uint32_t    stack_size;  /* bytes */
    uint32_t    priority;    /* abstract priority */
    void*       arg;         /* argument passed to entry */
} lwlte_sys_thread_cfg_t;

/**
 * Create and start a thread.
 *
 * @return thread handle or NULL on failure
 */
lwlte_sys_thread_t lwlte_sys_thread_create(lwlte_sys_thread_fn_t fn, const lwlte_sys_thread_cfg_t* cfg);

/**
 * Delete a thread.
 * (Optional; many cores never delete worker threads)
 */
void lwlte_sys_thread_delete(lwlte_sys_thread_t t);

/**
 * Sleep current thread for milliseconds.
 */
void lwlte_sys_thread_sleep(uint32_t ms);

/**
 * Get the current time in ticks.
 */
lwlte_tick_t lwlte_sys_time_get_ticks(void);

/**
 * Get the current time in milliseconds.
 */
lwlte_tick_t lwlte_sys_time_get_ms(void);

#ifdef __cplusplus
}
#endif
