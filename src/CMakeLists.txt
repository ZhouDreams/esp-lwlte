idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
    INCLUDE_DIRS include
    REQUIRES esp_driver_uart esp_driver_gpio
)
