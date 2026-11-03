# Wi-Fi dongle demo: a USB network adapter that joins a probe's access point
idf_component_register(
    SRCS
        WifiDongle.cpp
    PRIV_REQUIRES esp_event esp_hw_support esp_wifi nvs_flash
)
