# ---------------------------------------------------------------------------
# audio_codecs component
#
# Codec libraries used:
#   - dr_wav / dr_flac / dr_mp3  : header-only, under third_party/
#   - opencore-aacdec             : C sources globbed from bell/external
#   - libopus                     : add_subdirectory from bell/external
#   - DSD / codec_dsd             : DSF + DFF (DSDIFF) container parser
#   - ALAC decoder                : Apple Lossless, from bell/external/alac
#   - M4A demuxer                 : ISO BMFF parser for .m4a/.m4b/.mp4
# ---------------------------------------------------------------------------

set(BELL_EXT
    "${CMAKE_CURRENT_SOURCE_DIR}/../spotify/vendor/cspot/cspot/bell/external")

# ------------------------------------------------------------------
# ALAC decoder: Apple Lossless (decoder sources only, no encoder)
# ------------------------------------------------------------------
set(ALAC_DIR "${BELL_EXT}/alac/codec")
set(ALAC_SRCS
    "${ALAC_DIR}/ALACDecoder.cpp"
    "${ALAC_DIR}/ALACBitUtilities.c"
    "${ALAC_DIR}/ag_dec.c"
    "${ALAC_DIR}/dp_dec.c"
    "${ALAC_DIR}/matrix_dec.c"
    "${ALAC_DIR}/EndianPortable.c"
)

# ------------------------------------------------------------------
# opencore-aacdec: glob all C sources from src/
# (pure C, no platform-specific assembly, compiles clean on RISC-V)
# ------------------------------------------------------------------
file(GLOB OPENCORE_SRCS "${BELL_EXT}/opencore-aacdec/src/*.c")

set(OPENCORE_INC
    "${BELL_EXT}/opencore-aacdec/include"
    "${BELL_EXT}/opencore-aacdec/src"
    "${BELL_EXT}/opencore-aacdec/oscl"
)

# ------------------------------------------------------------------
# libopus: use the library's own CMakeLists for correct RISC-V build
# (disables programs / tests / install so only the decoder is built)
#
# NOTE: add_subdirectory is NOT scriptable — ESP-IDF runs CMakeLists
# in script mode during dependency extraction (component_get_requirements).
# Guard it (and all target_* calls below) so the script phase works.
# ------------------------------------------------------------------
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS              OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING               OFF CACHE BOOL "" FORCE)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_subdirectory(
        "${BELL_EXT}/opus"
        "${CMAKE_CURRENT_BINARY_DIR}/opus_build"
    )
endif()

# ------------------------------------------------------------------
# Component registration
# ------------------------------------------------------------------
idf_component_register(
    SRCS
        "audio_codecs.c"
        "codec_wav.c"
        "codec_flac.c"
        "codec_mp3.c"
        "codec_dsd.c"
        "codec_aac.c"
        "codec_opus.c"
        "m4a_demuxer.c"
        "codec_alac.cpp"
        ${OPENCORE_SRCS}
        ${ALAC_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "third_party"
        ${OPENCORE_INC}
        ${ALAC_DIR}
    REQUIRES
        log
)

# ------------------------------------------------------------------
# Post-registration target setup (not scriptable — same guard)
# ------------------------------------------------------------------
if(NOT CMAKE_SCRIPT_MODE_FILE)

# Compile-time defines for opencore-aacdec
# (mirrors the original opencore-aacdec/CMakeLists.txt flags)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    AAC_PLUS
    HQ_SBR
    PARAMETRICSTEREO
    C_EQUIVALENT
)

# Silence warnings that are harmless in the opencore-aacdec C sources
set_source_files_properties(${OPENCORE_SRCS} PROPERTIES
    COMPILE_FLAGS "-Wno-array-parameter -Wno-misleading-indentation \
                   -Wno-maybe-uninitialized -Wno-unused-variable"
)

# Link and include libopus
target_link_libraries(${COMPONENT_LIB} PRIVATE Opus::opus)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${BELL_EXT}/opus/include"
)

endif() # NOT CMAKE_SCRIPT_MODE_FILE
