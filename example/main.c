/**
 * @file main.c
 * @brief 统一示例入口
 * @details Unified example entry.
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

#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT

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
    case EXAMPLE_AIR780EP_BASIC_CONNECT:
        example_air780ep_basic_connect_run();
        break;
    case EXAMPLE_AIR780EP_MQTT_CLIENT:
        example_air780ep_mqtt_client_run();
        break;
    case EXAMPLE_ML307R_BASIC_CONNECT:
        example_ml307r_basic_connect_run();
        break;
    case EXAMPLE_ML307R_MQTT_CLIENT:
        example_ml307r_mqtt_client_run();
        break;
    case EXAMPLE_AIR780EP_TCP_CLIENT:
        example_air780ep_tcp_client_run();
        break;
    case EXAMPLE_ML307R_TCP_CLIENT:
        example_ml307r_tcp_client_run();
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
