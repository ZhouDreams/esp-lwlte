idf_component_register(
    SRCS "at_engine/at_engine.c"
         "modem/modem.c"
         "modem/modem_air780ep.c"
         "core/core.c"
         "core/core_fsm.c"
         "core/net_mgr.c"
         "core/pdp_mgr.c"
         "lwlte/lwlte.c"
         "lwlte/lwlte_air780ep.c"
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS lwlte core modem at_engine
    REQUIRES esp_driver_uart esp_driver_gpio esp_event
)
