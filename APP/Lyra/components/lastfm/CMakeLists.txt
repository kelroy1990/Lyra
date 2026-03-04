idf_component_register(SRCS "lastfm.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES settings esp_http_client mbedtls json esp_timer nvs_flash)
