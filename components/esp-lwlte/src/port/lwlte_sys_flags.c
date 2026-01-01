/*
    File: lwlte_sys_flags.c
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer System Flags encapsulation source file
    Platform: ESP-IDF
*/
#include "lwlte_sys_flags.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifndef LWLTE_SYS_WAIT_FOREVER
#define LWLTE_SYS_WAIT_FOREVER  (UINT32_MAX)
#endif

static TickType_t ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return 0;
    }
    if (timeout_ms == LWLTE_SYS_WAIT_FOREVER) {
        return portMAX_DELAY;
    }
    /* pdMS_TO_TICKS handles rounding; beware of 0 ticks for small ms if tick rate low */
    TickType_t t = pdMS_TO_TICKS(timeout_ms);
    return (t == 0) ? 1 : t;
}

lwlte_sys_flags_t lwlte_sys_flags_create(void)
{
    EventGroupHandle_t eg = xEventGroupCreate();
    return (lwlte_sys_flags_t)eg;
}

void lwlte_sys_flags_delete(lwlte_sys_flags_t f)
{
    if (!f) return;
    vEventGroupDelete((EventGroupHandle_t)f);
}

void lwlte_sys_flags_set(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bits)
{
    if (!f) return;
    xEventGroupSetBits((EventGroupHandle_t)f, (EventBits_t)bits);
}

void lwlte_sys_flags_clear(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bits)
{
    if (!f) return;
    xEventGroupClearBits((EventGroupHandle_t)f, (EventBits_t)bits);
}

lwlte_sys_flagbits_t lwlte_sys_flags_get(lwlte_sys_flags_t f)
{
    if (!f) return 0;
    return (lwlte_sys_flagbits_t)xEventGroupGetBits((EventGroupHandle_t)f);
}

bool lwlte_sys_flags_get_bit(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bit)
{
    if (!f) return false;
    return (bool)(xEventGroupGetBits((EventGroupHandle_t)f) & bit);
}

lwlte_sys_flagbits_t lwlte_sys_flags_wait(
    lwlte_sys_flags_t f,
    lwlte_sys_flagbits_t wait_bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms
){
    if (!f) return 0;

    EventBits_t ret = xEventGroupWaitBits(
        (EventGroupHandle_t)f,
        (EventBits_t)wait_bits,
        clear_on_exit ? pdTRUE : pdFALSE,
        wait_all ? pdTRUE : pdFALSE,
        ms_to_ticks(timeout_ms)
    );

    /* If timeout, FreeRTOS returns current bits (may be 0 or partial).
       We return ret & wait_bits to express "satisfied bits". */
    return (lwlte_sys_flagbits_t)(ret & (EventBits_t)wait_bits);
}