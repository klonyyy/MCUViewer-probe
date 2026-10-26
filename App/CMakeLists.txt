# Probe application: Wi-Fi with setup mode and mDNS, the SWD owner task with BMD's GDB server
# and the NETWORK PROBE protocol for MCUViewer: the net task, the request and streaming service
# and the SWO capture
idf_component_register(
    SRCS
        AppInterface.cpp
        Application.cpp
        GdbServer.cpp
        NetServer.cpp
        NetworkSettings.cpp
        ProbeService.cpp
        SetupPortal.cpp
        SwoCapture.cpp
        TargetLink.cpp
        Wifi.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES Platform esp_app_format esp_driver_gpio esp_driver_uart esp_event esp_hal_uart esp_http_server esp_hw_support esp_netif esp_rom esp_timer esp_wifi heap lwip nvs_flash vfs
)
