idf_component_register(SRCS "hello_world_main.c" "usb_descriptors.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb)

# Inject our tusb_config.h into TinyUSB's compilation so it finds our config
# instead of looking for a missing one. This replicates what esp_tinyusb did.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# TinyUSB expects descriptor callbacks implemented by the application.
# Force the linker to pull them from this component's archive.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u tud_descriptor_device_cb"
    "-u tud_descriptor_configuration_cb"
    "-u tud_descriptor_string_cb")
