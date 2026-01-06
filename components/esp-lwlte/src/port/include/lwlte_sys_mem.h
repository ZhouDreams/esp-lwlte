/*
    File: lwlte_sys_mem.h
    Author: JovisDreams
    Date: 2026-01-06
    Description: System Memory encapsulation header file
    Platform: ESP-IDF
*/
#pragma once

#include "lwlte_sys_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void *lwlte_sys_mem_malloc(lwlte_base_type_t size);

void  lwlte_sys_mem_free(void *ptr);

#ifdef __cplusplus
}
#endif