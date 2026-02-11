/**
 * @file    audio_pipeline.h
 * @date    2026-02-11
 * @brief   Public API for audio pipeline (source -> DSP -> I2S -> DAC)
 *
 * Will expose:
 * - audio_pipeline_init()          : Configure I2S, DAC, GPIOs
 * - audio_pipeline_start()         : Launch audio processing task
 * - audio_pipeline_set_source()    : Switch between USB / SD sources
 * - audio_pipeline_set_sample_rate(): Change rate (reconfigures I2S + MCLK)
 * - audio_pipeline_set_eq()        : Apply EQ preset
 * - audio_pipeline_get_jack_state(): 4.4mm balanced jack detection
 */
