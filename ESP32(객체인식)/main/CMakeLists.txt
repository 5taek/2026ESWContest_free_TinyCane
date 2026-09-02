idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera nvs_flash esp_wifi esp_http_server esp_netif
                             esp_driver_gpio esp_driver_i2c esp_driver_i2s
                             esp_http_client esp-tls esp_partition esp_timer
                             esp_driver_usb_serial_jtag lwip mdns)