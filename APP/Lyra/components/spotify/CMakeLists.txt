# F8-E: Spotify Connect via cspot + bell libraries
#
# cspot: vendor/cspot/cspot  (core Spotify protocol)
# bell:  vendor/cspot/cspot/bell  (platform abstraction, nanopb, tremor vorbis)
#
# Zeroconf HTTP server: esp_http_server (no CivetWeb/BellHTTPServer needed)
# mDNS registration:    espressif/mdns (managed component — see idf_component.yml)
# Audio output:         Lyra's process_audio() callback (no bell AudioSink)

# --- Bell / cspot build options (set BEFORE add_subdirectory) ---
set(BELL_ONLY_CJSON         ON  CACHE BOOL "" FORCE)  # avoid nlohmann/json (C++20)
set(BELL_DISABLE_WEBSERVER  ON  CACHE BOOL "" FORCE)  # we use esp_http_server
set(BELL_DISABLE_MQTT       ON  CACHE BOOL "" FORCE)  # not needed
set(BELL_DISABLE_SINKS      ON  CACHE BOOL "" FORCE)  # we use Lyra's audio system
# cspot uses tremor (ivorbis) directly — not via bell's VorbisDecoder wrapper.
# Tremor sources (external/tremor/*.c) are always compiled regardless of this flag.
# Disabling the codec layer avoids pulling in AAC/MP3 headers that don't exist.
set(BELL_DISABLE_CODECS     ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_VORBIS       ON  CACHE BOOL "" FORCE)  # tremor still compiled (see above)
set(BELL_VORBIS_FLOAT       OFF CACHE BOOL "" FORCE)  # use tremor (fixed-point)
set(BELL_CODEC_AAC          OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3          OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS         OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC         OFF CACHE BOOL "" FORCE)
set(BELL_DISABLE_FMT        OFF CACHE BOOL "" FORCE)  # needed by cspot logging
set(BELL_DISABLE_REGEX      ON  CACHE BOOL "" FORCE)  # saves ~30KB flash

# Disable JSON tests (nlohmann ships its own test suite)
set(JSON_BuildTests OFF CACHE INTERNAL "")

# --- Component registration ---
idf_component_register(
    SRCS
        "spotify.c"
        "spotify_glue.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        nvs_flash
        log
        freertos
        esp_http_server
        esp_netif
        espressif__mdns      # managed component (espressif/mdns), not IDF built-in
        mbedtls
        lwip
)

# --- Add cspot as subdirectory (which pulls in bell and nanopb) ---
# cspot/CMakeLists.txt: builds cspot static library and its dependencies.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cspot/cspot"
    "${CMAKE_CURRENT_BINARY_DIR}/cspot_build"
)

# --- Link cspot (and transitively bell) to our component ---
target_link_libraries(${COMPONENT_LIB} PRIVATE cspot)

# --- Enable C++ exceptions for bell/cspot ---
# ESP-IDF globally compiles with -fno-exceptions -fno-rtti.
# bell/cspot use 'throw'/'catch' so we need -fexceptions. They do NOT use
# dynamic_cast/typeid, so -frtti is intentionally omitted: with -frtti,
# typeinfo for bell::SocketStream (which inherits std::iostream) would
# reference typeinfo for std::iostream — absent from ESP-IDF's embedded
# libstdc++ — causing linker errors.
target_compile_options(bell  PRIVATE -fexceptions)
target_compile_options(cspot PRIVATE -fexceptions)
# Our glue code calls into bell/cspot; needs exceptions for correct unwinding.
target_compile_options(${COMPONENT_LIB} PRIVATE -fexceptions)

# --- C++17 for spotify_glue.cpp (cspot core compiles at C++20 per its CMake) ---
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)

# --- Silence known-harmless warnings from cspot/bell includes ---
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-format
)
