idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_driver_gpio
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        nvs_flash
        json
        mbedtls
)
