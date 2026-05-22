idf_component_register(
    SRCS "at_engine/at_engine.c"
    INCLUDE_DIRS include
    REQUIRES esp_driver_uart
)
