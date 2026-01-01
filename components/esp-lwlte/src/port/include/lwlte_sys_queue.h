/*
    File: lwlte_sys_queue.h
    Author: JovisDreams
    Date: 2025-12-29
    Description: System Queue encapsulation header file
    Platform: ESP-IDF
*/
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* opaque handle */
typedef void* lwlte_sys_queue_t;

/**
 * Create a queue.
 *
 * @param item_size   size of each item (bytes)
 * @param depth       number of items
 *
 * @return queue handle or NULL on failure
 */
lwlte_sys_queue_t lwlte_sys_queue_create(BaseType_t item_size, BaseType_t depth);

/**
 * Delete a queue.
 */
void lwlte_sys_queue_delete(lwlte_sys_queue_t q);

/**
 * Send an item to the queue (copy by value).
 *
 * @param q           queue handle
 * @param item        pointer to item data
 * @param timeout_ms  0: no wait
 *                     UINT32_MAX: wait forever
 *                     else: timeout in ms
 *
 * @return true on success, false on timeout/failure
 */
bool lwlte_sys_queue_send(
    lwlte_sys_queue_t q,
    const void* item,
    uint32_t timeout_ms
);

/**
 * Receive an item from the queue.
 *
 * @param q           queue handle
 * @param item        output buffer
 * @param timeout_ms  same rule as send
 *
 * @return true on success, false on timeout/failure
 */
bool lwlte_sys_queue_recv(
    lwlte_sys_queue_t q,
    void* item,
    uint32_t timeout_ms
);

/**
 * Send from ISR context.
 *
 * @param need_yield  set to true if a context switch is required
 */
bool lwlte_sys_queue_send_from_isr(
    lwlte_sys_queue_t q,
    const void* item,
    bool* need_yield
);

#ifdef __cplusplus
}
#endif