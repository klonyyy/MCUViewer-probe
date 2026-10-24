# Probe application: Wi-Fi and mDNS, the SWD owner task with BMD's GDB server
idf_component_register(
    SRCS
        AppInterface.cpp
        Application.cpp
        GdbServer.cpp
        Wifi.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES Platform esp_event esp_netif esp_wifi nvs_flash
)
