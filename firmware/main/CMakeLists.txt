idf_component_register(
    SRCS app_main.c gdb_server.c wifi.c
    INCLUDE_DIRS .
    PRIV_REQUIRES bmd esp_event esp_netif esp_wifi nvs_flash
)
