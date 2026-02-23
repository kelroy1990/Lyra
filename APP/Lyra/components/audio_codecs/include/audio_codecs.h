#pragma once

#include <stdint.h>
#include <stdbool.h>

//--------------------------------------------------------------------+
// Codec format types
//--------------------------------------------------------------------+

typedef enum {
    CODEC_FORMAT_UNKNOWN = 0,
    CODEC_FORMAT_WAV,
    CODEC_FORMAT_FLAC,
    CODEC_FORMAT_MP3,
    CODEC_FORMAT_DSD,   /* DSF/DFF container — codec_decode() outputs DoP int32_t frames,
                           DSP pipeline must be bypassed for this format */
    CODEC_FORMAT_AAC,   /* AAC-LC / HE-AAC in ADTS container (.aac) */
    CODEC_FORMAT_OPUS,  /* Opus audio in Ogg container (.opus) — always 48 kHz output */
    CODEC_FORMAT_M4A,   /* M4A/M4B/MP4 container — dispatcher; sub-opens as AAC or ALAC */
    CODEC_FORMAT_ALAC,  /* Apple Lossless in M4A container (.m4a) */
} codec_format_t;

//--------------------------------------------------------------------+
// Audio file info (populated after opening)
//--------------------------------------------------------------------+

typedef struct {
    uint32_t sample_rate;      /* PCM: audio sample rate.
                                  DSD:  DoP PCM rate = DSD_rate / 16
                                        (DSD64→176400, DSD128→352800, DSD256→705600) */
    uint8_t  bits_per_sample;  /* PCM: original bit depth (16/24/32).  DSD: always 32 (DoP) */
    uint8_t  channels;         /* 1 (mono) or 2 (stereo) */
    uint64_t total_frames;     /* PCM: stereo frames.  DSD: DoP frames (DSD samples / 16) */
    uint32_t duration_ms;      /* Duration in milliseconds (0 if unknown) */
    codec_format_t format;
    bool     is_dsd;           /* true → output is DoP, DSP chain must be bypassed */
    float    gain_db;          /* ReplayGain track gain in dB (0.0 = no tag / no adjustment) */
} codec_info_t;

//--------------------------------------------------------------------+
// Opaque decoder handle
//--------------------------------------------------------------------+

typedef struct codec_handle_s codec_handle_t;

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

/**
 * @brief Open an audio file and initialize the appropriate decoder
 *
 * Auto-detects format from file extension (.wav, .flac, .mp3).
 * All decoders output int32_t stereo interleaved, left-justified.
 *
 * @param filepath Full path to the audio file (e.g. "/sdcard/music/song.flac")
 * @return Handle on success, NULL on failure
 */
codec_handle_t *codec_open(const char *filepath);

/**
 * @brief Decode next block of PCM frames
 *
 * Output: int32_t stereo interleaved, left-justified.
 * Mono files are expanded to stereo (duplicated to both channels).
 *
 * @param handle   Decoder handle from codec_open()
 * @param buffer   Output buffer for decoded PCM (must hold max_frames * 2 int32_t)
 * @param max_frames Maximum number of stereo frames to decode
 * @return Number of frames decoded (0 = EOF, negative = error)
 */
int32_t codec_decode(codec_handle_t *handle, int32_t *buffer, uint32_t max_frames);

/**
 * @brief Seek to a specific PCM frame position
 *
 * @param handle    Decoder handle
 * @param frame_pos Target frame position (0 = beginning)
 * @return true on success
 */
bool codec_seek(codec_handle_t *handle, uint64_t frame_pos);

/**
 * @brief Get audio file information
 *
 * @param handle Decoder handle
 * @return Pointer to info struct (valid while handle is open)
 */
const codec_info_t *codec_get_info(const codec_handle_t *handle);

/**
 * @brief Close decoder and release all resources
 */
void codec_close(codec_handle_t *handle);

/**
 * @brief Detect codec format from file extension
 *
 * @param filepath File path to check
 * @return Detected format or CODEC_FORMAT_UNKNOWN
 */
codec_format_t codec_detect_format(const char *filepath);
