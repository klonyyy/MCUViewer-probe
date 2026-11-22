# ESP-NOW link between a probe and a dongle: the packet header, the session rules, the rate ladder and pairing,
# header-only (MIT)
idf_component_register(
    INCLUDE_DIRS .
    REQUIRES network_probe_proto esp_wifi mbedtls
)
