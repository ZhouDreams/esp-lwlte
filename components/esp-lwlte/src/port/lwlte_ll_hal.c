/*
    File: lwlte_ll_uart.c
    Author: JovisDreams
    Date: 2025-12-28
    Description: Low-level Layer UART driver source file
    Platform: ESP-IDF
*/

#include "lwlte_ll_hal.h"
#include "lwlte_sys_types.h"
#include "lwlte_core.h"
#include "lwlte_err.h"
#include "lwlte_sys_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char* TAG = "lwlte_ll_hal";

static struct {
    lwlte_ll_uart_config_t config;
    QueueHandle_t uart_rx_queue;
    TaskHandle_t uart_rx_task_handle;
} s_lwlte_ll_uart_context;

static void lwlte_ll_uart_rx_task(void *pvParameters)
{
    LWLTE_LOGI(TAG, "lwlte_ll_uart_rx_task starts.");
    uart_event_t event;
    char buf[s_lwlte_ll_uart_context.config.uart_buf_size];
    while (1) {
        if(xQueueReceive(s_lwlte_ll_uart_context.uart_rx_queue, 
            &event, portMAX_DELAY) == pdPASS) {
            if (event.type == UART_DATA){
                /* Read the data from the UART */
                int len = uart_read_bytes(s_lwlte_ll_uart_context.config.uart_num, 
                    &buf, event.size, pdMS_TO_TICKS(100));
                if (len > 0) {
                    buf[len] = '\0';
                    lwlte_core_input(buf, len);
                }
            }
        }
    }
}

lwlte_err_t lwlte_ll_uart_write(const char* data, size_t size)
{
    if (uart_write_bytes(s_lwlte_ll_uart_context.config.uart_num, data, size) < 0) {
        return LWLTE_ERROR;
    }
    return LWLTE_OK;
}

lwlte_err_t lwlte_ll_uart_init(lwlte_ll_uart_config_t *config)
{
    /* Initialize the context */
    s_lwlte_ll_uart_context.config = *config;
    /* Install the UART driver */
    ESP_ERROR_CHECK(uart_driver_install(s_lwlte_ll_uart_context.config.uart_num,
                                  s_lwlte_ll_uart_context.config.uart_buf_size,
                                  s_lwlte_ll_uart_context.config.uart_buf_size,
                                      10, 
                                      &s_lwlte_ll_uart_context.uart_rx_queue,
                                0));
    /* Configure the UART parameters */
    ESP_ERROR_CHECK(uart_param_config(s_lwlte_ll_uart_context.config.uart_num, 
        &s_lwlte_ll_uart_context.config.uart_config));
    /* Set the UART pins */
    ESP_ERROR_CHECK(uart_set_pin(s_lwlte_ll_uart_context.config.uart_num, 
        s_lwlte_ll_uart_context.config.uart_tx_io_num, 
        s_lwlte_ll_uart_context.config.uart_rx_io_num, 
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Create the UART RX task */
    xTaskCreate(lwlte_ll_uart_rx_task, "lwlte_ll_uart_rx_task", 4096,
        NULL, tskIDLE_PRIORITY + 10, &s_lwlte_ll_uart_context.uart_rx_task_handle);
    LWLTE_LOGI(TAG, "lwlte_ll_uart_init completed.");
    return ESP_OK;
}

lwlte_err_t lwlte_ll_uart_deinit(lwlte_base_type_t uart_num)
{
    ESP_ERROR_CHECK(uart_driver_delete(uart_num));
    LWLTE_LOGI(TAG, "lwlte_ll_uart_deinit completed.");
    return ESP_OK;
}

lwlte_err_t lwlte_ll_gpio_init(lwlte_base_type_t gpio_num)
{
    gpio_reset_pin(gpio_num);
    gpio_set_direction(gpio_num, GPIO_MODE_OUTPUT);
    gpio_set_level(gpio_num, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(gpio_num, 1);
    LWLTE_LOGI(TAG, "lwlte_ll_gpio_init completed.");
    return LWLTE_OK;
}

