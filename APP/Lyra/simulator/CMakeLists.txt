cmake_minimum_required(VERSION 3.16)
project(lyra_sim C)

# -----------------------------------------------------------------------
# SDL2 (find installed via vcpkg or system — must be found before LVGL)
# -----------------------------------------------------------------------

find_package(SDL2 REQUIRED)

# -----------------------------------------------------------------------
# LVGL (fetched from GitHub — matches espressif/lvgl version)
# -----------------------------------------------------------------------

include(FetchContent)

# Tell LVGL where our lv_conf.h is BEFORE it gets configured.
# LV_CONF_PATH must be set before FetchContent_MakeAvailable so that
# LVGL's custom.cmake picks it up and adds the parent dir to includes.
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS    ON CACHE BOOL "" FORCE)

FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lvgl)

# LVGL's SDL driver needs SDL2 headers during compilation
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

# Fix LVGL's MSVC dllimport/dllexport for static linking.
# LVGL's root CMakeLists.txt sets LV_ATTRIBUTE_EXTERN_DATA=__declspec(dllimport)
# on INTERFACE, which causes the linker to look for __imp_ prefixed symbols.
# Since we build LVGL as a static library, only symbols that LVGL references
# internally get pulled from the archive — fonts like montserrat_20/28 that
# aren't used by LVGL itself become unresolvable. Fix: remove dllimport.
if(MSVC)
    get_target_property(_lvgl_iface_defs lvgl INTERFACE_COMPILE_DEFINITIONS)
    list(FILTER _lvgl_iface_defs EXCLUDE REGEX "LV_ATTRIBUTE_EXTERN_DATA")
    set_target_properties(lvgl PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_lvgl_iface_defs}")
endif()

# -----------------------------------------------------------------------
# UI sources (shared with ESP-IDF build)
# -----------------------------------------------------------------------

set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/ui")

set(UI_SRCS
    "${UI_DIR}/ui_init.c"
    "${UI_DIR}/ui_theme.c"
    "${UI_DIR}/ui_now_playing.c"
    "${UI_DIR}/ui_browser.c"
    "${UI_DIR}/ui_settings.c"
)

# -----------------------------------------------------------------------
# Simulator executable
# -----------------------------------------------------------------------

add_executable(lyra_sim
    sim_main.c
    ui_data_mock.c
    ${UI_SRCS}
)

target_include_directories(lyra_sim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"    # hal_stubs.h, lv_conf.h
    "${UI_DIR}/include"              # ui.h, ui_data.h
    "${UI_DIR}"                      # ui_internal.h
)

# Link LVGL + SDL2
target_link_libraries(lyra_sim PRIVATE
    lvgl
    SDL2::SDL2
    SDL2::SDL2main
)

# C standard
set_target_properties(lyra_sim PROPERTIES C_STANDARD 11)

# On Windows, copy SDL2.dll next to the executable
if(WIN32)
    add_custom_command(TARGET lyra_sim POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2>
            $<TARGET_FILE_DIR:lyra_sim>
        COMMENT "Copying SDL2.dll"
    )
endif()
