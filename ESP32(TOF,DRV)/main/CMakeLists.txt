idf_component_register(SRCS "smartcane.cc" "TOF_task.cc" "I2C_SET.cc" "DRV.cc" "usb_cdc_comm.cc"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES
                        vl53l5cx
                        esp_driver_gpio
                        esp_driver_i2c
                        esp_driver_ledc
                        esp_driver_usb_serial_jtag
                        esp_timer
                        hal
                        soc
                    )