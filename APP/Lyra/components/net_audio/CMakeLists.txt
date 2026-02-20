# dr_libs headers path (PRIV_INCLUDE_DIRS — no re-compilation, just declarations)
set(DRLIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../audio_codecs/third_party")

idf_component_register(
    SRCS
        "net_audio.c"
        "http_stream.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        ${DRLIBS_DIR}
    PRIV_REQUIRES
        audio_codecs
        esp_http_client
        mbedtls
        esp_netif
        lwip
        log
        esp_timer
        freertos
)
