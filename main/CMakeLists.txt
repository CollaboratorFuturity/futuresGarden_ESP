idf_component_register(
                        SRCS  "main.c"
                              "EXIO/TCA9554PWR.c"
                              "LCD_Driver/ST7701S.c"
                              "LVGL_Driver/LVGL_Driver.c"
                              "I2C_Driver/I2C_Driver.c"
                              "SD_Card/SD_MMC.c"
                              "LVGL_UI/orb_ui.c"
                              "Wireless/Wireless.c"
                              "Wireless/config_fetch.c"
                              "Wireless/log_sink.c"
                              "NFC/nfc.c"
                              "Audio/i2s_audio.c"
                              "BAT_Driver/BAT_Driver.c"
                              "Button/button.c"
                              "Convai/convai.c"
                              "OTA/ota.c"

                        INCLUDE_DIRS "./EXIO"
                                     "./Audio"
                                     "./NFC"
                                     "./LCD_Driver"
                                     "./LVGL_Driver"
                                     "./I2C_Driver"
                                     "./SD_Card"
                                     "./LVGL_UI"
                                     "./Wireless"
                                     "./BAT_Driver"
                                     "./Button"
                                     "./Convai"
                                     "./OTA"
                                     "."

                        EMBED_TXTFILES "certs/gts_root_r4.pem"
                                       "certs/gts_root_r1.pem"

                        PRIV_REQUIRES driver
                                      esp_driver_gpio esp_driver_i2c esp_driver_ledc
                                      esp_driver_i2s esp_driver_sdmmc esp_driver_spi
                                      esp_adc esp_lcd esp_timer esp_psram
                                      esp_hw_support spi_flash
                                      fatfs sdmmc vfs
                                      esp_wifi esp_netif esp_event lwip nvs_flash bt
                                      esp_http_client cjson
                                      lvgl
                                      esp_websocket_client esp-tls mbedtls
                                      app_update esp_https_ota
                        )
