idf_component_register(
    SRCS "main.c"
         "air780ep_basic_connect.c"
         "air780ep_mqtt_client.c"
         "ml307r_probe.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
