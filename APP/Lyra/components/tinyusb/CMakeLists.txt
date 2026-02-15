# Local TinyUSB 0.20.0 component for ESP-IDF
# Replaces the managed espressif/tinyusb component

set(TUSB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/tinyusb-0.20.0")

idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "esp32s3")
    set(tusb_mcu "OPT_MCU_ESP32S3")
elseif(${target} STREQUAL "esp32s2")
    set(tusb_mcu "OPT_MCU_ESP32S2")
elseif(${target} STREQUAL "esp32p4")
    set(tusb_mcu "OPT_MCU_ESP32P4")
elseif(${target} STREQUAL "esp32h4")
    set(tusb_mcu "OPT_MCU_ESP32H4")
endif()

idf_component_get_property(freertos_include freertos ORIG_INCLUDE_PATH)

set(includes_public
    "${TUSB_SRC}/src"
    "${freertos_include}"
)

set(includes_private
    "${TUSB_SRC}/src/device"
)

# Class drivers we use: Audio + CDC + MSC
set(srcs
    "${TUSB_SRC}/src/class/audio/audio_device.c"
    "${TUSB_SRC}/src/class/cdc/cdc_device.c"
    "${TUSB_SRC}/src/class/msc/msc_device.c"
    # DWC2 portable driver (Synopsys, used by all ESP32 variants)
    "${TUSB_SRC}/src/portable/synopsys/dwc2/dcd_dwc2.c"
    "${TUSB_SRC}/src/portable/synopsys/dwc2/dwc2_common.c"
    # Core device stack
    "${TUSB_SRC}/src/common/tusb_fifo.c"
    "${TUSB_SRC}/src/device/usbd.c"
    "${TUSB_SRC}/src/device/usbd_control.c"
    "${TUSB_SRC}/src/tusb.c"
)

set(requirements_private "")

if(${target} STREQUAL "esp32p4")
    list(APPEND requirements_private
        esp_mm  # required by dcd_dwc2.c: #include "esp_cache.h"
    )
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${includes_public}
    PRIV_INCLUDE_DIRS ${includes_private}
    PRIV_REQUIRES ${requirements_private}
)

target_compile_options(${COMPONENT_LIB} PUBLIC "-DCFG_TUSB_MCU=${tusb_mcu}")

# Suppress warning in usbd.c where uint8_t is compared with BUILTIN_DRIVER_COUNT
set_source_files_properties("${TUSB_SRC}/src/device/usbd.c"
    PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
