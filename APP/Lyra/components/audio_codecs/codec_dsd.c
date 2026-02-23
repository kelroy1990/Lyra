/*
 * codec_dsd.c — DSD container decoder with DoP output
 *
 * Supported containers:
 *   DSF  (Sony DSD Stream File, .dsf)   ← implemented
 *   DFF  (Philips DSDIFF, .dff)         ← implemented
 *
 * Output format (DoP — DSD over PCM, v1.1):
 *   int32_t stereo interleaved frames, one L word + one R word per DoP frame.
 *
 *   Each 32-bit word:
 *     bits [31:24] = 0x00  (padding, ignored by DAC)
 *     bits [23:16] = DoP marker, alternates 0x05 / 0xFA per frame
 *     bits [15: 0] = 16 DSD bits for this channel (2 consecutive DSF bytes,
 *                    stored little-endian: earlier byte at [7:0])
 *
 * DoP PCM rates reported in codec_info_t.sample_rate:
 *   DSD64  (2.8224  MHz DSD) → 176 400 Hz DoP  (BCLK 11.3 MHz)
 *   DSD128 (5.6448  MHz DSD) → 352 800 Hz DoP  (BCLK 22.6 MHz)
 *   DSD256 (11.2896 MHz DSD) → 705 600 Hz DoP  (BCLK 45.2 MHz ← APLL limit)
 *
 * DSF data layout (Sony spec):
 *   Blocks of `block_size` bytes per channel, interleaved: [L block][R block][L block]…
 *   Within each byte, DSD bit 0 (LSB) is the earliest sample in time.
 *   Two consecutive bytes per channel → one DoP frame (16 DSD bits/channel).
 *
 * The I2S driver sees this as normal 32-bit PCM at the DoP rate.
 * The ES9039Q2M detects the 0x05/0xFA markers and switches to DSD mode internally.
 * The DSP chain (EQ/biquad) must be bypassed — check codec_info_t.is_dsd.
 */

#include "audio_codecs_internal.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "codec_dsd";

/* ------------------------------------------------------------------ */
/* DoP marker bytes (alternate every frame)                            */
/* ------------------------------------------------------------------ */
#define DOP_MARKER_A  0x05u
#define DOP_MARKER_B  0xFAu

/* ------------------------------------------------------------------ */
/* Internal decoder state                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Common fields */
    uint64_t data_offset;         /* File offset to first byte of DSD data        */
    uint64_t total_dsd_samples;   /* Total DSD samples per channel                */
    uint8_t  dop_marker;          /* Current alternating marker (A or B)          */
    uint64_t dop_frames_out;      /* Total DoP frames emitted (for seek tracking) */

    /* DSF-only: block I/O buffers */
    uint32_t block_size;          /* bytes per channel per interleaved block       */
    uint8_t *blk_l;               /* Current L channel block (heap, DSF only)     */
    uint8_t *blk_r;               /* Current R channel block (heap, DSF only)     */
    uint32_t blk_frame_pos;       /* Next DoP frame index within current blocks   */
    uint32_t blk_frames;          /* DoP frames available in current block pair   */
    bool     blk_loaded;          /* false = must load first block on first call  */

    /* DFF-only: is_dff flag + audio data size */
    bool     is_dff;
    uint64_t dff_data_size;       /* DSD audio bytes in the DSD chunk             */
} dsd_state_t;

/* ------------------------------------------------------------------ */
/* Little-endian read helpers                                          */
/* ------------------------------------------------------------------ */

static bool read_u32_le(FILE *f, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

static bool read_u64_le(FILE *f, uint64_t *out)
{
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return false;
    *out = 0;
    for (int i = 0; i < 8; i++) *out |= ((uint64_t)b[i] << (8 * i));
    return true;
}

/* ------------------------------------------------------------------ */
/* Big-endian read helpers (DSDIFF / DFF)                              */
/* ------------------------------------------------------------------ */

static bool read_u16_be(FILE *f, uint16_t *out)
{
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return false;
    *out = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

static bool read_u32_be(FILE *f, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
    return true;
}

static bool read_u64_be(FILE *f, uint64_t *out)
{
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return false;
    *out = 0;
    for (int i = 0; i < 8; i++) *out = (*out << 8) | b[i];
    return true;
}

/* ------------------------------------------------------------------ */
/* DFF (DSDIFF) container parser                                       */
/*                                                                     */
/* DSDIFF is a big-endian IFF variant.  All chunk sizes are 8 bytes.  */
/*                                                                     */
/* File layout:                                                        */
/*   FRM8 (ID:4, size:8 BE) {                                          */
/*       "DSD " (form type, 4 bytes)                                   */
/*       FVER  (version)                                               */
/*       PROP  (properties, contains FS, CHNL, CMPR sub-chunks)       */
/*       DSD   (raw interleaved DSD audio bytes)                       */
/*   }                                                                 */
/*                                                                     */
/* DSD audio bytes: L0 R0 L1 R1 L2 R2 …  (byte-interleaved, stereo)  */
/* Within each byte: MSB = earliest sample.                            */
/* DoP packing (same byte order as DSF for DAC compatibility):         */
/*   dop_l = (marker << 16) | (L1 << 8) | L0                          */
/*   dop_r = (marker << 16) | (R1 << 8) | R0                          */
/* ------------------------------------------------------------------ */

static bool dff_parse(FILE *f, dsd_state_t *st, codec_info_t *info)
{
    /* FRM8 root chunk header is already consumed by the magic check in
     * codec_dsd_open(); re-read from the start. */
    fseek(f, 0, SEEK_SET);

    char     id[4];
    uint64_t frm8_size;

    if (fread(id, 1, 4, f) != 4 || memcmp(id, "FRM8", 4) != 0) return false;
    if (!read_u64_be(f, &frm8_size)) return false;

    char form_type[4];
    if (fread(form_type, 1, 4, f) != 4 || memcmp(form_type, "DSD ", 4) != 0) {
        ESP_LOGE(TAG, "DFF: FRM8 form type is not 'DSD '");
        return false;
    }

    /* Body end (absolute offset) = 4(ID) + 8(size) + frm8_size */
    int64_t body_end = 12LL + (int64_t)frm8_size;

    uint32_t sample_rate   = 0;
    uint16_t channel_count = 0;
    uint64_t dsd_data_offset = 0;
    uint64_t dsd_data_size   = 0;

    /* Scan FRM8 sub-chunks */
    while (ftell(f) + 12 <= body_end) {
        char     cid[4];
        uint64_t csz;
        if (fread(cid, 1, 4, f) != 4) break;
        if (!read_u64_be(f, &csz)) break;

        long    cbody  = ftell(f);
        int64_t c_end  = cbody + (int64_t)csz;

        if (memcmp(cid, "PROP", 4) == 0) {
            /* PROP: 4-byte prop type "SND " + PROP sub-chunks */
            char prop_type[4];
            if (fread(prop_type, 1, 4, f) != 4 ||
                memcmp(prop_type, "SND ", 4) != 0) {
                fseek(f, (long)c_end, SEEK_SET);
                continue;
            }
            while (ftell(f) + 12 <= c_end) {
                char     pid[4];
                uint64_t psz;
                if (fread(pid, 1, 4, f) != 4) break;
                if (!read_u64_be(f, &psz)) break;
                long pbody = ftell(f);

                if (memcmp(pid, "FS  ", 4) == 0 && psz >= 4) {
                    read_u32_be(f, &sample_rate);
                } else if (memcmp(pid, "CHNL", 4) == 0 && psz >= 2) {
                    read_u16_be(f, &channel_count);
                }
                /* CMPR, ABSS, LSCO etc. are skipped */
                fseek(f, (long)(pbody + (int64_t)psz), SEEK_SET);
            }

        } else if (memcmp(cid, "DSD ", 4) == 0) {
            /* Raw DSD audio data */
            dsd_data_offset = (uint64_t)cbody;
            dsd_data_size   = csz;
        }

        fseek(f, (long)c_end, SEEK_SET);
    }

    if (sample_rate == 0 || channel_count == 0 || dsd_data_size == 0) {
        ESP_LOGE(TAG, "DFF: incomplete PROP or missing DSD chunk");
        return false;
    }
    if (channel_count != 2) {
        ESP_LOGE(TAG, "DFF: only stereo supported (channels=%u)", channel_count);
        return false;
    }
    if (sample_rate != 2822400 && sample_rate != 5644800 && sample_rate != 11289600) {
        ESP_LOGE(TAG, "DFF: unsupported DSD rate %lu Hz", (unsigned long)sample_rate);
        return false;
    }

    fseek(f, (long)dsd_data_offset, SEEK_SET);

    st->data_offset       = dsd_data_offset;
    st->dff_data_size     = dsd_data_size;
    st->is_dff            = true;
    /* DSD samples per channel = data_size_bytes / num_channels */
    st->total_dsd_samples = (dsd_data_size / 2) * 8;
    st->dop_marker        = DOP_MARKER_A;
    st->dop_frames_out    = 0;
    /* block_size / blk_l / blk_r not used for DFF */

    /* info */
    info->sample_rate     = sample_rate / 16;  /* DoP rate */
    info->bits_per_sample = 32;
    info->channels        = 2;
    info->total_frames    = dsd_data_size / 4; /* 4 bytes per DoP frame (stereo) */
    info->is_dsd          = true;
    info->format          = CODEC_FORMAT_DSD;

    const char *lvl = (sample_rate == 2822400)  ? "DSD64"
                    : (sample_rate == 5644800)   ? "DSD128" : "DSD256";
    ESP_LOGI(TAG, "DFF: %s — DSD %lu Hz → DoP %lu Hz | data=%llu B",
             lvl,
             (unsigned long)sample_rate,
             (unsigned long)info->sample_rate,
             (unsigned long long)dsd_data_size);
    return true;
}

/* ------------------------------------------------------------------ */
/* DSF container parser                                                */
/* ------------------------------------------------------------------ */

static bool dsf_parse(FILE *f, dsd_state_t *st, codec_info_t *info)
{
    char magic[4];

    /* ── DSD chunk ────────────────────────────────────────────────── */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "DSD ", 4) != 0) {
        ESP_LOGE(TAG, "Not a DSF file (bad DSD magic)");
        return false;
    }
    uint64_t dsd_chunk_size, total_file_size, metadata_offset;
    if (!read_u64_le(f, &dsd_chunk_size))  return false;  /* = 28 */
    if (!read_u64_le(f, &total_file_size)) return false;
    if (!read_u64_le(f, &metadata_offset)) return false;

    /* ── fmt chunk ────────────────────────────────────────────────── */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "DSF: missing fmt chunk");
        return false;
    }
    uint64_t fmt_size;
    if (!read_u64_le(f, &fmt_size)) return false;  /* = 52 */

    uint32_t format_version, format_id, channel_type, channel_count;
    uint32_t sample_rate, bits_per_sample, block_size, reserved;
    uint64_t sample_count;

    if (!read_u32_le(f, &format_version))  return false;
    if (!read_u32_le(f, &format_id))       return false;
    if (!read_u32_le(f, &channel_type))    return false;
    if (!read_u32_le(f, &channel_count))   return false;
    if (!read_u32_le(f, &sample_rate))     return false;
    if (!read_u32_le(f, &bits_per_sample)) return false;
    if (!read_u64_le(f, &sample_count))    return false;
    if (!read_u32_le(f, &block_size))      return false;
    if (!read_u32_le(f, &reserved))        return false;

    /* Validate */
    if (channel_count != 2) {
        ESP_LOGE(TAG, "DSF: only stereo supported (channels=%lu)", (unsigned long)channel_count);
        return false;
    }
    if (sample_rate != 2822400 && sample_rate != 5644800 && sample_rate != 11289600) {
        ESP_LOGE(TAG, "DSF: unsupported DSD rate %lu Hz (expected 2822400/5644800/11289600)",
                 (unsigned long)sample_rate);
        return false;
    }
    if (block_size == 0 || (block_size & 1)) {
        ESP_LOGE(TAG, "DSF: block_size %lu must be non-zero and even", (unsigned long)block_size);
        return false;
    }

    /* ── data chunk ───────────────────────────────────────────────── */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "data", 4) != 0) {
        ESP_LOGE(TAG, "DSF: missing data chunk");
        return false;
    }
    uint64_t data_chunk_size;
    if (!read_u64_le(f, &data_chunk_size)) return false;

    /* Record data start position */
    long pos = ftell(f);
    if (pos < 0) return false;

    /* Populate decoder state */
    st->data_offset       = (uint64_t)pos;
    st->total_dsd_samples = sample_count;
    st->block_size        = block_size;
    st->blk_frames        = block_size / 2;  /* 2 DSD bytes → 1 DoP frame (16 bits/ch) */
    st->blk_frame_pos     = block_size;      /* forces load of first block on first decode */
    st->blk_loaded        = false;
    st->dop_marker        = DOP_MARKER_A;
    st->dop_frames_out    = 0;

    /* Populate codec_info_t */
    info->sample_rate     = sample_rate / 16; /* DoP PCM rate: 176400 / 352800 / 705600 */
    info->bits_per_sample = 32;               /* DoP words sent as 32-bit I2S frames     */
    info->channels        = 2;
    info->total_frames    = sample_count / 16;/* DoP frames = DSD samples / 16           */
    info->is_dsd          = true;
    info->format          = CODEC_FORMAT_DSD;

    const char *level = (sample_rate == 2822400)  ? "DSD64"  :
                        (sample_rate == 5644800)   ? "DSD128" : "DSD256";

    ESP_LOGI(TAG, "DSF: %s — DSD %lu Hz → DoP %lu Hz | "
                  "block=%lu bytes | samples/ch=%llu | duration=%.1fs",
             level,
             (unsigned long)sample_rate,
             (unsigned long)info->sample_rate,
             (unsigned long)block_size,
             (unsigned long long)sample_count,
             info->duration_ms / 1000.0f);   /* duration_ms set by codec_open() */

    return true;
}

/* ------------------------------------------------------------------ */
/* Block loader: reads one [L block][R block] pair from file           */
/* ------------------------------------------------------------------ */

static bool dsd_load_next_block(codec_handle_t *h)
{
    dsd_state_t *st = (dsd_state_t *)h->dsd.state;

    /* DSF interleaved layout: block_size bytes L, block_size bytes R */
    size_t n_l = fread(st->blk_l, 1, st->block_size, h->file);
    if (n_l == 0) return false;  /* EOF */

    size_t n_r = fread(st->blk_r, 1, st->block_size, h->file);

    /* Pad partial reads with DSD silence (0x69 = alternating bits, mid-code) */
    if (n_l < st->block_size) memset(st->blk_l + n_l, 0x69, st->block_size - n_l);
    if (n_r < st->block_size) memset(st->blk_r + n_r, 0x69, st->block_size - n_r);

    /* DoP frames from this block = bytes / 2 (capped to what L delivered) */
    st->blk_frames    = (uint32_t)(n_l / 2);
    st->blk_frame_pos = 0;
    st->blk_loaded    = true;

    return (st->blk_frames > 0);
}

/* ------------------------------------------------------------------ */
/* vtable — decode                                                     */
/* ------------------------------------------------------------------ */

static int32_t dsd_decode(codec_handle_t *h, int32_t *buf, uint32_t max_frames)
{
    dsd_state_t *st = (dsd_state_t *)h->dsd.state;
    uint32_t out = 0;

    /* ── DFF path: read directly from interleaved file stream ─────── */
    if (st->is_dff) {
        uint8_t raw[512];   /* 128 DoP frames × 4 bytes */
        while (out < max_frames) {
            uint32_t batch = max_frames - out;
            if (batch > 128) batch = 128;
            size_t n = fread(raw, 4, batch, h->file);
            if (n == 0) break;
            uint8_t mk = st->dop_marker;
            for (size_t i = 0; i < n; i++) {
                uint8_t *p = raw + i * 4;
                /* DFF interleaved: L0=p[0], R0=p[1], L1=p[2], R1=p[3]
                 * DoP: earlier byte at [7:0], later byte at [15:8]    */
                buf[out * 2]     = (int32_t)(((uint32_t)mk << 16)
                                 | ((uint32_t)p[2] << 8) | p[0]);
                buf[out * 2 + 1] = (int32_t)(((uint32_t)mk << 16)
                                 | ((uint32_t)p[3] << 8) | p[1]);
                mk = (mk == DOP_MARKER_A) ? DOP_MARKER_B : DOP_MARKER_A;
                out++;
            }
            st->dop_marker = mk;
        }
        st->dop_frames_out += out;
        return (int32_t)out;
    }

    /* ── DSF path: block-based decode ─────────────────────────────── */
    while (out < max_frames) {

        /* Load a fresh block when current one is exhausted */
        if (st->blk_frame_pos >= st->blk_frames) {
            if (!dsd_load_next_block(h)) break;   /* EOF */
        }

        uint32_t avail = st->blk_frames - st->blk_frame_pos;
        uint32_t emit  = (avail < (max_frames - out)) ? avail : (max_frames - out);

        uint32_t pos = st->blk_frame_pos;
        uint8_t  mk  = st->dop_marker;

        for (uint32_t i = 0; i < emit; i++, pos++) {
            uint32_t bi = pos * 2;  /* byte index into block (2 bytes per DoP frame) */

            /*
             * Pack two consecutive DSF bytes (16 DSD bits) per channel into a DoP word.
             * DSF stores bits LSB-first within each byte (bit 0 = earliest DSD sample).
             * DoP carries them as-is, little-endian in bits [15:0]:
             *   bits [15:8] = second DSF byte  (DSD samples 8-15)
             *   bits  [7:0] = first  DSF byte  (DSD samples 0-7)
             * Bits [23:16] = DoP marker (0x05 or 0xFA).
             * Bits [31:24] = 0x00 (padding).
             */
            uint32_t dop_l = ((uint32_t)mk            << 16)
                           | ((uint32_t)st->blk_l[bi + 1] <<  8)
                           |  (uint32_t)st->blk_l[bi];

            uint32_t dop_r = ((uint32_t)mk            << 16)
                           | ((uint32_t)st->blk_r[bi + 1] <<  8)
                           |  (uint32_t)st->blk_r[bi];

            buf[out * 2]     = (int32_t)dop_l;
            buf[out * 2 + 1] = (int32_t)dop_r;

            /* Alternate marker every DoP frame */
            mk = (mk == DOP_MARKER_A) ? DOP_MARKER_B : DOP_MARKER_A;
            out++;
        }

        st->blk_frame_pos  = pos;
        st->dop_marker     = mk;
        st->dop_frames_out += emit;
    }

    return (int32_t)out;  /* 0 = EOF */
}

/* ------------------------------------------------------------------ */
/* vtable — seek                                                       */
/* ------------------------------------------------------------------ */

static bool dsd_seek(codec_handle_t *h, uint64_t frame_pos)
{
    dsd_state_t *st = (dsd_state_t *)h->dsd.state;

    /* ── DFF path: linear seek ─────────────────────────────────────── */
    if (st->is_dff) {
        /* 4 bytes per DoP frame (L0,R0,L1,R1) */
        uint64_t byte_off = st->data_offset + frame_pos * 4;
        if (fseek(h->file, (long)byte_off, SEEK_SET) != 0) {
            ESP_LOGE(TAG, "DFF seek failed (offset=%llu)",
                     (unsigned long long)byte_off);
            return false;
        }
        st->dop_marker     = DOP_MARKER_A;
        st->dop_frames_out = frame_pos;
        return true;
    }

    /* ── DSF path: block-aligned seek ─────────────────────────────── */
    /*
     * frame_pos is in DoP frames (= DSD samples / 16).
     * DSF data is laid out as sequential [L block][R block] pairs.
     * Each pair covers blk_frames DoP frames.
     *
     * Block index  = frame_pos / blk_frames
     * Frame within = frame_pos % blk_frames
     * File offset  = data_offset + block_idx * block_size * 2  (L+R per pair)
     */
    uint32_t blk_frames_full = st->block_size / 2;  /* frames in a full block */
    uint64_t block_idx       = frame_pos / blk_frames_full;
    uint32_t frame_in_blk    = (uint32_t)(frame_pos % blk_frames_full);

    uint64_t byte_off = st->data_offset + block_idx * (uint64_t)st->block_size * 2;

    if (fseek(h->file, (long)byte_off, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "DSD seek failed (offset=%llu)", (unsigned long long)byte_off);
        return false;
    }

    /* Reset block state; reload at the target block */
    st->blk_loaded     = false;
    st->blk_frames     = 0;
    st->blk_frame_pos  = blk_frames_full;  /* triggers reload */
    st->dop_marker     = DOP_MARKER_A;     /* reset marker on seek */
    st->dop_frames_out = frame_pos;

    if (!dsd_load_next_block(h)) return false;
    st->blk_frame_pos = frame_in_blk;

    return true;
}

/* ------------------------------------------------------------------ */
/* vtable — close                                                      */
/* ------------------------------------------------------------------ */

static void dsd_close(codec_handle_t *h)
{
    dsd_state_t *st = (dsd_state_t *)h->dsd.state;
    if (st) {
        free(st->blk_l);
        free(st->blk_r);
        free(st);
        h->dsd.state = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Vtable                                                              */
/* ------------------------------------------------------------------ */

static const codec_vtable_t s_dsd_vtable = {
    .decode = dsd_decode,
    .seek   = dsd_seek,
    .close  = dsd_close,
};

/* ------------------------------------------------------------------ */
/* codec_dsd_open — entry point called by codec_open()                 */
/* ------------------------------------------------------------------ */

bool codec_dsd_open(codec_handle_t *h)
{
    dsd_state_t *st = calloc(1, sizeof(dsd_state_t));
    if (!st) {
        ESP_LOGE(TAG, "OOM for dsd_state_t");
        return false;
    }

    /* Detect container by magic (file is at offset 0) */
    char magic[4];
    if (fread(magic, 1, 4, h->file) != 4) {
        ESP_LOGE(TAG, "Cannot read DSD container magic");
        free(st);
        return false;
    }
    fseek(h->file, 0, SEEK_SET);  /* rewind — parser reads from the beginning */

    bool ok = false;

    if (memcmp(magic, "DSD ", 4) == 0) {
        ok = dsf_parse(h->file, st, &h->info);
    } else if (memcmp(magic, "FRM8", 4) == 0) {
        ok = dff_parse(h->file, st, &h->info);
    } else {
        ESP_LOGE(TAG, "Unknown DSD magic: %02X %02X %02X %02X",
                 (uint8_t)magic[0], (uint8_t)magic[1],
                 (uint8_t)magic[2], (uint8_t)magic[3]);
        free(st);
        return false;
    }

    if (!ok) {
        free(st);
        return false;
    }

    /* Allocate block I/O buffers for DSF (not needed for DFF) */
    if (!st->is_dff) {
        st->blk_l = malloc(st->block_size);
        st->blk_r = malloc(st->block_size);
        if (!st->blk_l || !st->blk_r) {
            ESP_LOGE(TAG, "OOM for DSD block buffers (%lu bytes each)",
                     (unsigned long)st->block_size);
            free(st->blk_l);
            free(st->blk_r);
            free(st);
            return false;
        }
    }

    h->dsd.state = st;
    h->vt        = &s_dsd_vtable;
    return true;
}
