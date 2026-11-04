# ESP-NOW dongle: a USB serial port to MCUViewer on one side, ESP-NOW to the probes on the other
idf_component_register(
    SRCS
        Dongle.cpp
        DongleMain.cpp
    INCLUDE_DIRS .
    REQUIRES espnow_link network_probe_proto
    PRIV_REQUIRES esp_event esp_hw_support esp_rom esp_timer esp_wifi nvs_flash
)
