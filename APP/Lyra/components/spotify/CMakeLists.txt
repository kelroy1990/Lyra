# F8-E: Spotify Connect via cspot
#
# Current state: stub only.
# To activate: add cspot submodule, implement spotify_glue.cpp, enable C++17.
#
# When cspot is added:
#   set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
#   set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD_REQUIRED ON)
#   target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

idf_component_register(
    SRCS
        "spotify.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        nvs_flash
        log
        freertos
)
