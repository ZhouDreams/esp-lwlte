/**
 * @file main.c
 * @brief Unified example entry
 * @details Select one example by editing EXAMPLE_SELECTED.
 */

/*********************
 *      INCLUDES
 *********************/
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                 "example"

#define EXAMPLE_SELECTED    EXAMPLE_BASIC_CONNECT

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void idle_forever(void);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    switch (EXAMPLE_SELECTED) {
    case EXAMPLE_BASIC_CONNECT:
        example_basic_connect_run();
        break;
    case EXAMPLE_MQTT_CLIENT:
        example_mqtt_client_run();
        break;
    case EXAMPLE_ML307R_PROBE:
        example_ml307r_probe_run();
        break;
    default:
        ESP_LOGE(TAG, "invalid EXAMPLE_SELECTED=%d", EXAMPLE_SELECTED);
        idle_forever();
        break;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
