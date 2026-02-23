idf_component_register(SRCS "usb_msc.c" "usb_mode.c" "audio_source.c" "app_main.c" "usb_descriptors.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb tinyusb esp_timer esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_codec_dev audio_pipeline storage audio_codecs sd_player power wireless net_audio dlna spotify)

# Inject our tusb_config.h into TinyUSB's compilation so it finds our config
idf_component_get_property(tusb_lib tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# TinyUSB expects descriptor callbacks implemented by the application.
# Force the linker to pull them from this component's archive.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u tud_descriptor_device_cb"
    "-u tud_descriptor_configuration_cb"
    "-u tud_descriptor_string_cb"
    "-u tud_msc_inquiry_cb"
    "-u tud_msc_read10_cb"
    "-u tud_msc_write10_cb")
