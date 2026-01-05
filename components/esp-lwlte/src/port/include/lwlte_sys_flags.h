/*
    File: lwlte_sys_flags.h
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer System Flags encapsulation header file
    - Encapsulate the EventGroup APIs of FreeRTOS.
    Platform: ESP-IDF
*/
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define LWLTE_FLAGS_ALL_BITS 0xFFFFFF

#ifdef __cplusplus
extern "C" {
#endif

typedef void*    lwlte_sys_flags_t;   // opaque handle
typedef uint32_t lwlte_sys_flagbits_t;

/**
 * Create a flags object. Returns NULL on failure.
 */
lwlte_sys_flags_t lwlte_sys_flags_create(void);

/**
 * Delete a flags object.
 */
void lwlte_sys_flags_delete(lwlte_sys_flags_t f);

/**
 * Set bits (OR).
 */
void lwlte_sys_flags_set(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bits);

/**
 * Clear bits.
 */
void lwlte_sys_flags_clear(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bits);

/**
 * Get current bits (non-blocking snapshot).
 */
lwlte_sys_flagbits_t lwlte_sys_flags_get(lwlte_sys_flags_t f);

/**
 * Get a specific bit.
 * @param f Flags object
 * @param bit The bit to get
 * @return True if the bit is set, false otherwise
 */
bool lwlte_sys_flags_get_bit(lwlte_sys_flags_t f, lwlte_sys_flagbits_t bit);

/**
 * Wait until condition satisfied.
 *
 * @param wait_bits     Bits to wait for
 * @param wait_all      true: wait all bits; false: wait any bit
 * @param clear_on_exit true: clear satisfied bits before return
 * @param timeout_ms    0: no wait; UINT32_MAX: wait forever; else milliseconds
 *
 * @return Bits that were set when the wait condition satisfied, or 0 on timeout.
 */
lwlte_sys_flagbits_t lwlte_sys_flags_wait(
    lwlte_sys_flags_t f,
    lwlte_sys_flagbits_t wait_bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms
);

 

#ifdef __cplusplus
}
#endif