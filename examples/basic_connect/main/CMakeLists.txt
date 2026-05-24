idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart esp_event
)
