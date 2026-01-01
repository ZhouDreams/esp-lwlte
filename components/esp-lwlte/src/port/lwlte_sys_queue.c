/*
    File: lwlte_sys_queue.c
    Author: JovisDreams
    Date: 2025-12-29
    Description: System Queue encapsulation source file
    Platform: ESP-IDF
*/
#include "lwlte_sys_queue.h"
#include "lwlte_sys_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


static TickType_t ms_to_ticks(uint32_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    if (timeout_ms == LWLTE_SYS_WAIT_FOREVER) {
        return portMAX_DELAY;
    }
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    return (t == 0) ? 1 : t;
}

lwlte_sys_queue_t lwlte_sys_queue_create(BaseType_t item_size, BaseType_t depth) {
    QueueHandle_t q = xQueueCreate(depth, item_size);
    return (lwlte_sys_queue_t)q;
}

void lwlte_sys_queue_delete(lwlte_sys_queue_t q) {
    if (!q) return;
    vQueueDelete((QueueHandle_t)q);
}

bool lwlte_sys_queue_send(
    lwlte_sys_queue_t q,
    const void* item,
    uint32_t timeout_ms
) {
    if (!q || !item) return false;

    BaseType_t ok = xQueueSend(
        (QueueHandle_t)q,
        item,
        ms_to_ticks(timeout_ms)
    );
    return (ok == pdPASS);
}

bool lwlte_sys_queue_recv(lwlte_sys_queue_t q, void* item, uint32_t timeout_ms) {
    if (!q || !item) return false;

    BaseType_t ok = xQueueReceive((QueueHandle_t)q, item, ms_to_ticks(timeout_ms));
    return (ok == pdPASS);
}

bool lwlte_sys_queue_send_from_isr(
    lwlte_sys_queue_t q,
    const void* item,
    bool* need_yield
) {
    if (!q || !item) return false;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t ok = xQueueSendFromISR(
        (QueueHandle_t)q,
        item,
        &xHigherPriorityTaskWoken
    );

    if (need_yield) {
        *need_yield = (xHigherPriorityTaskWoken == pdTRUE);
    }
    return (ok == pdPASS);
}
