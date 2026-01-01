/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "lwlte.h"
#include "driver/uart.h"
#include "esp_log.h"

#define PRIORITY_NORMAL 1
#define PRIORITY_HIGHEST 10
#define AIR780EP_MODULE_INIT_PRIORITY PRIORITY_NORMAL

/*configure the UART for the Air780EP module*/
#define AIR780EP_UART_NUM UART_NUM_1
#define AIR780EP_UART_TX 0
#define AIR780EP_UART_RX 1
#define AIR780EP_GPIO_EN 3
#define AIR780EP_UART_BAUDRATE 115200
#define AIR780EP_UART_BUF_SIZE 1024
#define AIR780EP_AT_WAIT_TICKS pdMS_TO_TICKS(1000)
#define AIR780EP_INIT_MAX_TIME_MS 120000

static const char* TAG = "appmain";
static lwlte_config_t lwlte_config = {
    .gpio_en_num = AIR780EP_GPIO_EN,
    .uart_num = AIR780EP_UART_NUM,
    .uart_tx_io_num = AIR780EP_UART_TX,
    .uart_rx_io_num = AIR780EP_UART_RX,
    .uart_buf_size = AIR780EP_UART_BUF_SIZE,
    .uart_baudrate = AIR780EP_UART_BAUDRATE,
    .at_wait_ticks = AIR780EP_AT_WAIT_TICKS,
    .init_max_time_ms = AIR780EP_INIT_MAX_TIME_MS,
};

void app_main(void)
{
    // air780ep_module_init(&air780ep_config, &air780ep_event_group, AIR780EP_MODULE_INIT_PRIORITY);
    // xEventGroupWaitBits(air780ep_event_group, Air780EP_INITIALIZED, pdFALSE, pdFALSE, portMAX_DELAY);
    // while (1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(5000));
    //     BaseType_t csq = air780ep_get_signal_strength();
    //     if (csq != -1) {
    //         ESP_LOGI(TAG, "Signal strength is %d", csq);    
    //     }
    //     else {
    //         ESP_LOGE(TAG, "Failed to get signal strength");
    //     }
    // }
    lwlte_core_init(&lwlte_config);
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    
}
