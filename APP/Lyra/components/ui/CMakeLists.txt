# ESP-IDF component registration for Lyra UI
#
# Note: On ESP-IDF, the data provider is ui_data_esp.c (reads from real subsystems).
# On the PC simulator, ui_data_mock.c is used instead (linked in simulator/CMakeLists.txt).

idf_component_register(
    SRCS
        "ui_init.c"
        "ui_theme.c"
        "ui_now_playing.c"
        "ui_browser.c"
        "ui_settings.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
)

# Note: REQUIRES lvgl will be added once lvgl/lvgl is declared as a
# managed component dependency in components/display/idf_component.yml.
# For now this component is not wired into the ESP-IDF build
# (no other component depends on it yet).
