idf_component_register(SRCS "sd_card.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES sdmmc fatfs esp_driver_sdmmc esp_driver_gpio freertos)

# ── SD Bus Speed Test Levels ──
# Change LYRA_SD_SPEED to test GPIO Matrix limits:
#   0 = SDR 40 MHz  (HS 3.3V, PLL160/4)        ← safe baseline
#   1 = SDR 50 MHz  (HS 3.3V, PLL200/4)        ← push HS limit
#   2 = SDR 50 MHz  (UHS-I 1.8V, no DDR)       ← 1.8V signaling test
#   3 = SDR 100 MHz (UHS-I SDR50, 1.8V)        ← aggressive
#   4 = DDR 40 MHz  (UHS-I DDR50, 1.8V)        ← known broken on GPIO Matrix
set(LYRA_SD_SPEED "3" CACHE STRING "SD speed test level (0-4)")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LYRA_SD_SPEED=${LYRA_SD_SPEED})
