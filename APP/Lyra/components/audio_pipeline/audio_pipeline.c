/**
 * @file    audio_pipeline.c
 * @date    2026-02-11
 * @brief   Audio pipeline: source management, DSP/EQ processing, I2S output
 *
 * This component will contain:
 * - I2S driver configuration for ES9039Q2M (32-bit stereo, MCLK generation)
 * - Audio source manager (USB UAC2 input / microSD decoder input)
 * - DSP processing chain (EQ, volume, sample rate conversion if needed)
 * - Ring buffer between source and I2S output (PSRAM-backed)
 * - Dynamic sample rate switching (reconfigure I2S + MCLK on rate change)
 * - DAC EN and Audio LDO EN GPIO control
 * - ES9039Q2M SPI register interface
 *
 * Phase: F1 (I2S output to ES9039Q2M)
 */
