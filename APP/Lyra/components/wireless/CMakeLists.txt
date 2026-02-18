idf_component_register(SRCS "wireless.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event
                                     esp_driver_gpio esp_timer lwip log freertos)
