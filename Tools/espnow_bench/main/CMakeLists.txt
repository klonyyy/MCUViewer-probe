# ESP-NOW bench: round trip, throughput and losses between two boards
idf_component_register(
    SRCS
        EspNowBench.cpp
    PRIV_REQUIRES esp_driver_gpio esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
)
