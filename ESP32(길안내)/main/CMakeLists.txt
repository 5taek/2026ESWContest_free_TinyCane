idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cjson esp_http_client esp_wifi esp_event esp_netif nvs_flash mbedtls esp_http_server esp_driver_usb_serial_jtag esp_driver_gpio)