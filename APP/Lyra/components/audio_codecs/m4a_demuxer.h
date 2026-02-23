#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Codec type detected inside an M4A/MP4 container */
typedef enum {
    M4A_CODEC_AAC  = 0,   /* mp4a + esds → AudioSpecificConfig          */
    M4A_CODEC_ALAC = 1,   /* alac + nested alac FullBox                  */
} m4a_codec_t;

/*
 * Parsed M4A audio track information + flat sample table.
 *
 * sample_sizes[]   — bytes of each compressed frame (heap-allocated)
 * sample_offsets[] — absolute file offset of each compressed frame (heap-allocated)
 *
 * Memory estimate: ~12 bytes/frame.
 * Example: 1h ALAC 48kHz (frameLength=4096) ≈ 42 000 frames ≈ 500 KB → PSRAM OK.
 *
 * Call m4a_free() to release sample table arrays.
 */
typedef struct {
    uint32_t    sample_rate;
    uint32_t    duration_ms;
    uint8_t     channels;
    uint8_t     bits_per_sample;
    uint64_t    total_samples;    /* PCM samples per channel from mdhd duration */
    uint32_t    timescale;
    m4a_codec_t codec;

    /*
     * Codec-specific configuration blob (big-endian as read from the file):
     *   AAC  : AudioSpecificConfig (ASC) from esds descriptor, 2–5 bytes
     *   ALAC : magic cookie = 4-byte FullBox header + 24-byte ALACSpecificConfig, 28 bytes
     */
    uint8_t  config[64];
    uint32_t config_size;

    /* Flat sample table */
    uint32_t  sample_count;
    uint32_t *sample_sizes;    /* [sample_count] compressed bytes per frame */
    uint64_t *sample_offsets;  /* [sample_count] absolute file byte offsets  */
} m4a_info_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse an M4A/MP4/M4B file and extract the audio track sample table.
 *
 * Scans the top-level box structure for 'moov', then recursively parses
 * moov → trak (audio) → mdia → mdhd/hdlr/minf → stbl → stsd/stsc/stsz/stco.
 *
 * On success, out->sample_sizes and out->sample_offsets are heap-allocated.
 * The caller must call m4a_free(out) when done.
 *
 * @param f    Open FILE* (may be positioned anywhere; function uses fseek internally)
 * @param out  Output struct (all fields overwritten)
 * @return     true on success, false on parse error or unsupported format
 */
bool m4a_parse(FILE *f, m4a_info_t *out);

/** @brief Release heap arrays allocated by m4a_parse(). Safe to call on zeroed struct. */
void m4a_free(m4a_info_t *info);

#ifdef __cplusplus
}
#endif
