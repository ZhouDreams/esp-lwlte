/*
    File: lwlte_sys_mutex.c
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer System Mutex encapsulation source file
    Platform: ESP-IDF
*/

#include "lwlte_sys_mutex.h"
#include "freertos/FreeRTOS.h"

lwlte_sys_mutex_t lwlte_sys_mutex_create(void)
{
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return (lwlte_sys_mutex_t)m;
}

void lwlte_sys_mutex_lock(lwlte_sys_mutex_t m) {
    if (m == NULL) {
        return;
    }
    xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
}

void lwlte_sys_mutex_unlock(lwlte_sys_mutex_t m) {
    if (m == NULL) {
        return;
    }
    xSemaphoreGive((SemaphoreHandle_t)m);
}

void lwlte_sys_mutex_delete(lwlte_sys_mutex_t m) {
    if (m == NULL) {
        return;
    }
    vSemaphoreDelete((SemaphoreHandle_t)m);
}

lwlte_sys_mutex_t lwlte_sys_semaphore_create(void)
{
    SemaphoreHandle_t s = xSemaphoreCreateBinary();
    return (lwlte_sys_semaphore_t)s;
}

void lwlte_sys_semaphore_signal(lwlte_sys_semaphore_t s) {
    if (s == NULL) {
        return;
    }
    xSemaphoreGive((SemaphoreHandle_t)s);
}

void lwlte_sys_semaphore_wait(lwlte_sys_semaphore_t s, BaseType_t timeout_ms) {
    if (s == NULL) {
        return;
    }
    xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(timeout_ms));
}

void lwlte_sys_semaphore_delete(lwlte_sys_semaphore_t s) {
    if (s == NULL) {
        return;
    }
    vSemaphoreDelete((SemaphoreHandle_t)s);
}