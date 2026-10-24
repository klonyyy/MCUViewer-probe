# Probe application: Wi-Fi and mDNS, the SWD owner task with BMD's GDB server and the
# NETWORK PROBE protocol for MCUViewer: the net task and the request and streaming service
idf_component_register(
    SRCS
        AppInterface.cpp
        Application.cpp
        GdbServer.cpp
        NetServer.cpp
        ProbeService.cpp
        TargetLink.cpp
        Wifi.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES Platform esp_app_format esp_event esp_hw_support esp_netif esp_rom esp_timer esp_wifi heap lwip nvs_flash vfs
)
