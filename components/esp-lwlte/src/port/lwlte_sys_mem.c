/*
    File: lwlte_sys_mem.c
    Author: JovisDreams
    Date: 2026-01-06
    Description: System Memory encapsulation source file
    Platform: ESP-IDF
*/
#include "esp_heap_caps.h"
#include "lwlte_sys_mem.h"

void *lwlte_sys_mem_malloc(lwlte_base_type_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void lwlte_sys_mem_free(void *ptr)
{
    heap_caps_free(ptr);
}
