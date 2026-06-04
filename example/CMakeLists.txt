idf_component_register(
    SRCS "main.c"
         "basic_connect.c"
         "mqtt_client.c"
         "ml307r_probe.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
