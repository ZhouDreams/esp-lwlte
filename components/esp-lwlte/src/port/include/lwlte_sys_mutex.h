/*
    File: lwlte_sys_mutex.h
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer System Mutex encapsulation header file
    Platform: ESP-IDF
*/
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* lwlte_sys_mutex_t;
typedef void* lwlte_sys_semaphore_t;

lwlte_sys_mutex_t lwlte_sys_mutex_create(void);

void lwlte_sys_mutex_lock(lwlte_sys_mutex_t m);

void lwlte_sys_mutex_unlock(lwlte_sys_mutex_t m);

void lwlte_sys_mutex_delete(lwlte_sys_mutex_t m);

lwlte_sys_semaphore_t lwlte_sys_semaphore_create(void);

void lwlte_sys_semaphore_signal(lwlte_sys_semaphore_t s);

void lwlte_sys_semaphore_wait(lwlte_sys_semaphore_t s, BaseType_t timeout_ms);

void lwlte_sys_semaphore_delete(lwlte_sys_semaphore_t s);

#ifdef __cplusplus
}
#endif