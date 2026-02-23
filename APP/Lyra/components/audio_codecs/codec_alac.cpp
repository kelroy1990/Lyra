/*
 * codec_alac.cpp — ALAC (Apple Lossless Audio Codec) decoder
 *
 * Wraps the Apple ALACDecoder C++ class for use with the audio_codecs
 * vtable framework.  Requires the M4A sample table (from m4a_demuxer.c)
 * to locate each compressed frame in the file.
 *
 * Also implements codec_m4a_open() — the single dispatcher called for
 * .m4a / .m4b / .mp4 files.  It runs m4a_parse() once and then either
 * initialises the ALAC decoder (here) or calls codec_aac_open_m4a()
 * (defined in codec_aac.c) for M4A-AAC files.
 *
 * Output: int32_t stereo interleaved, left-justified.
 *   16-bit samples: value << 16
 *   24-bit samples: value << 8   (3-byte LE packed output from ALACDecoder)
 *   32-bit samples: value as-is
 *
 * Note: ALACDecoder writes packed little-endian output:
 *   16-bit → 2 bytes/sample,  24-bit → 3 bytes/sample,  32-bit → 4 bytes/sample
 */

#include "audio_codecs_internal.h"
#include "m4a_demuxer.h"

extern "C" {
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
}

/* Apple ALAC decoder C++ class */
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

static const char *TAG = "codec_alac";

/* ------------------------------------------------------------------ */
/* Per-instance decoder state                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    m4a_info_t   m4a;            /* sample table (owns sample_sizes/offsets) */
    uint32_t     frame_idx;      /* next compressed frame to decode           */
    ALACDecoder *dec;            /* C++ decoder instance                      */
    uint8_t     *frame_buf;      /* compressed-frame read buffer              */
    uint32_t     frame_buf_max;  /* allocated size of frame_buf               */
    uint8_t     *pcm_buf;        /* raw PCM output from ALACDecoder           */
    uint32_t     pcm_buf_size;   /* allocated size of pcm_buf                 */
} alac_state_t;

/* ------------------------------------------------------------------ */
/* vtable — decode                                                     */
/* ------------------------------------------------------------------ */

static int32_t alac_decode_fn(codec_handle_t *h, int32_t *buffer,
                               uint32_t max_frames)
{
    alac_state_t *st = (alac_state_t *)h->alac.state;
    if (!st) return -1;

    m4a_info_t *m4a = &st->m4a;
    if (st->frame_idx >= m4a->sample_count) return 0;   /* EOF */

    uint64_t off  = m4a->sample_offsets[st->frame_idx];
    uint32_t size = m4a->sample_sizes[st->frame_idx];

    if (size == 0 || size > st->frame_buf_max) {
        st->frame_idx++;
        return 0;   /* skip invalid frame */
    }

    if (fseek(h->file, (long)off, SEEK_SET) != 0) return -1;
    if (fread(st->frame_buf, 1, size, h->file) != size) return -1;

    BitBuffer bb;
    BitBufferInit(&bb, st->frame_buf, size);

    uint32_t out_samples = 0;
    int32_t  err = st->dec->Decode(&bb, st->pcm_buf,
                                    st->dec->mConfig.frameLength,
                                    m4a->channels, &out_samples);
    st->frame_idx++;
    if (err != ALAC_noErr || out_samples == 0) return 0;

    if (out_samples > max_frames) out_samples = max_frames;

    uint8_t  depth = m4a->bits_per_sample;
    uint32_t ch    = m4a->channels;

    if (depth <= 16) {
        /* 16-bit LE packed, 2 bytes/sample → left-justify (<<16) */
        const int16_t *src = (const int16_t *)st->pcm_buf;
        if (ch == 1) {
            for (uint32_t i = 0; i < out_samples; i++) {
                int32_t s = (int32_t)src[i] << 16;
                buffer[i * 2]     = s;
                buffer[i * 2 + 1] = s;
            }
        } else {
            uint32_t n = out_samples * 2;
            for (uint32_t i = 0; i < n; i++)
                buffer[i] = (int32_t)src[i] << 16;
        }
    } else if (depth <= 24) {
        /* 24-bit LE packed, 3 bytes/sample → left-justify (<<8) */
        const uint8_t *src = st->pcm_buf;
        if (ch == 1) {
            for (uint32_t i = 0; i < out_samples; i++) {
                uint32_t raw = (uint32_t)src[0]
                             | ((uint32_t)src[1] << 8)
                             | ((uint32_t)src[2] << 16);
                buffer[i * 2]     = (int32_t)(raw << 8);
                buffer[i * 2 + 1] = (int32_t)(raw << 8);
                src += 3;
            }
        } else {
            uint32_t n = out_samples * 2;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t raw = (uint32_t)src[0]
                             | ((uint32_t)src[1] << 8)
                             | ((uint32_t)src[2] << 16);
                buffer[i] = (int32_t)(raw << 8);
                src += 3;
            }
        }
    } else {
        /* 32-bit LE, 4 bytes/sample → use directly */
        const int32_t *src = (const int32_t *)st->pcm_buf;
        if (ch == 1) {
            for (uint32_t i = 0; i < out_samples; i++) {
                buffer[i * 2]     = src[i];
                buffer[i * 2 + 1] = src[i];
            }
        } else {
            memcpy(buffer, src, (size_t)out_samples * 2 * sizeof(int32_t));
        }
    }

    return (int32_t)out_samples;
}

/* ------------------------------------------------------------------ */
/* vtable — seek (exact, via sample table)                             */
/* ------------------------------------------------------------------ */

static bool alac_seek_fn(codec_handle_t *h, uint64_t frame_pos)
{
    alac_state_t *st = (alac_state_t *)h->alac.state;
    if (!st) return false;

    uint32_t fl = st->dec->mConfig.frameLength;
    if (fl == 0) return false;

    /*
     * frame_pos is a PCM frame index.  Each compressed ALAC frame decodes
     * to exactly mConfig.frameLength PCM frames (except the last which may
     * be shorter).  Divide to get the compressed-frame index.
     */
    uint32_t new_idx = (uint32_t)(frame_pos / fl);
    if (new_idx >= st->m4a.sample_count)
        new_idx = st->m4a.sample_count;
    st->frame_idx = new_idx;
    return true;
}

/* ------------------------------------------------------------------ */
/* vtable — close                                                      */
/* ------------------------------------------------------------------ */

static void alac_close_fn(codec_handle_t *h)
{
    alac_state_t *st = (alac_state_t *)h->alac.state;
    if (st) {
        delete st->dec;
        free(st->frame_buf);
        free(st->pcm_buf);
        m4a_free(&st->m4a);
        free(st);
        h->alac.state = NULL;
    }
}

static const codec_vtable_t alac_vtable = {
    alac_decode_fn,
    alac_seek_fn,
    alac_close_fn,
};

/* ------------------------------------------------------------------ */
/* Internal: initialise ALAC state from a pre-parsed m4a_info_t       */
/* (called from codec_m4a_open; takes ownership of info's heap arrays) */
/* ------------------------------------------------------------------ */

static bool alac_init_state(codec_handle_t *h, m4a_info_t *info)
{
    alac_state_t *st = (alac_state_t *)calloc(1, sizeof(alac_state_t));
    if (!st) { m4a_free(info); return false; }

    /* Transfer heap ownership */
    st->m4a = *info;
    info->sample_sizes   = NULL;
    info->sample_offsets = NULL;

    /* Create and initialise ALACDecoder */
    st->dec = new ALACDecoder();
    int32_t err = st->dec->Init(st->m4a.config, st->m4a.config_size);
    if (err != ALAC_noErr) {
        ESP_LOGE(TAG, "ALACDecoder::Init failed: %ld", (long)err);
        delete st->dec;
        m4a_free(&st->m4a);
        free(st);
        return false;
    }

    /* Compressed-frame read buffer: scan for worst-case frame size */
    uint32_t max_frame = 0;
    for (uint32_t i = 0; i < st->m4a.sample_count; i++)
        if (st->m4a.sample_sizes[i] > max_frame)
            max_frame = st->m4a.sample_sizes[i];
    if (max_frame == 0) max_frame = 65536;

    st->frame_buf_max = max_frame;
    st->frame_buf = (uint8_t *)malloc(max_frame);
    if (!st->frame_buf) {
        delete st->dec;
        m4a_free(&st->m4a);
        free(st);
        return false;
    }

    /* PCM output buffer: frameLength × channels × 4 bytes covers all depths */
    uint32_t fl = st->dec->mConfig.frameLength;
    if (fl == 0) fl = 4096;
    st->pcm_buf_size = fl * st->m4a.channels * 4;
    st->pcm_buf = (uint8_t *)malloc(st->pcm_buf_size);
    if (!st->pcm_buf) {
        free(st->frame_buf);
        delete st->dec;
        m4a_free(&st->m4a);
        free(st);
        return false;
    }

    h->alac.state          = st;
    h->info.sample_rate    = st->m4a.sample_rate;
    h->info.bits_per_sample = st->m4a.bits_per_sample;
    h->info.channels       = st->m4a.channels;
    h->info.total_frames   = st->m4a.total_samples;
    h->info.duration_ms    = st->m4a.duration_ms;
    h->info.format         = CODEC_FORMAT_ALAC;
    h->vt                  = &alac_vtable;

    ESP_LOGI(TAG, "ALAC: %luHz %d-ch %d-bit | %lu frames | %lu ms",
             (unsigned long)st->m4a.sample_rate, st->m4a.channels,
             st->m4a.bits_per_sample,
             (unsigned long)st->m4a.sample_count,
             (unsigned long)st->m4a.duration_ms);
    return true;
}

/* ------------------------------------------------------------------ */
/* codec_m4a_open — public dispatcher for .m4a/.m4b/.mp4 files         */
/* ------------------------------------------------------------------ */

/* Forward declaration of the M4A-AAC path (defined in codec_aac.c)  */
extern "C" bool codec_aac_open_m4a(codec_handle_t *h, m4a_info_t *info);

extern "C" bool codec_m4a_open(codec_handle_t *h)
{
    m4a_info_t info;
    if (!m4a_parse(h->file, &info)) {
        ESP_LOGE(TAG, "m4a_parse failed");
        return false;
    }

    if (info.codec == M4A_CODEC_ALAC) {
        return alac_init_state(h, &info);
    } else {
        /* M4A_CODEC_AAC — delegate to codec_aac.c; passes ownership of info */
        return codec_aac_open_m4a(h, &info);
    }
}
