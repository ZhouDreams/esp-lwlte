idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "modem/modem_ml307r.c"
         "core/core.c"
         "core/core_fsm.c"
         "core/net_mgr.c"
         "core/pdp_mgr.c"
         "mqtt_client/mqtt_client.c"
         "ping_client/ping_client.c"
         "lwlte/lwlte.c"
         "lwlte/lwlte_air780ep.c"
         "lwlte/lwlte_ml307r.c"
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS lwlte core mqtt_client ping_client modem at_engine
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
