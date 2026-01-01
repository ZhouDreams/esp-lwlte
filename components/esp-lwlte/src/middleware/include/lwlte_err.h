/*
    File: lwlte_err.h
    Author: JovisDreams
    Date: 2025-12-30
    Description: Error code definition
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LWLTE_OK = 0,
    LWLTE_ERROR ,
    LWLTE_TIMEOUT,
    LWLTE_INVALID_ARG,
    LWLTE_NOT_SUPPORTED,
    LWLTE_NOT_INITIALIZED,
    LWLTE_ALREADY_INITIALIZED,
} lwlte_err_t;

#ifdef __cplusplus
}
#endif