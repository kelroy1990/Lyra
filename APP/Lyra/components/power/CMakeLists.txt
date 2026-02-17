# Set to 1 when the target board has the MAX77972 charger/fuel gauge IC.
# Dev board (ESP32-P4-Function-EV) does NOT have this chip — leave at 0.
set(LYRA_HAS_MAX77972 "0" CACHE STRING "Board has MAX77972 fuel gauge (0=no, 1=yes)")

if(LYRA_HAS_MAX77972)
    set(POWER_SRCS "power.c" "max77972.c")
    set(POWER_PRIV_REQUIRES driver esp_driver_i2c esp_driver_gpio esp_timer freertos log)
else()
    set(POWER_SRCS "power.c")
    set(POWER_PRIV_REQUIRES log)
endif()

idf_component_register(SRCS ${POWER_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES ${POWER_PRIV_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PUBLIC LYRA_HAS_MAX77972=${LYRA_HAS_MAX77972})
