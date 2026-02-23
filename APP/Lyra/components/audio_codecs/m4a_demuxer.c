/*
 * m4a_demuxer.c — ISO BMFF / M4A container parser
 *
 * Supports .m4a, .m4b, .mp4 files with audio tracks containing:
 *   AAC-LC / HE-AAC  (mp4a + esds → AudioSpecificConfig)
 *   ALAC             (alac + nested alac FullBox → ALACSpecificConfig magic cookie)
 *
 * Box traversal: moov → trak (audio) → mdia → mdhd/hdlr/minf → stbl →
 *                stsd / stts / stsc / stsz / stco|co64
 *
 * Multiple traks (e.g. video + audio in .mp4) are handled by resetting
 * per-trak state at each 'trak' box and only committing tables for
 * the track whose hdlr handler_type is 'soun'.
 *
 * moov-at-end ("streaming" optimisation) is supported: the top-level scan
 * finds moov regardless of position.
 */

#include "m4a_demuxer.h"
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>

static const char *TAG = "m4a";

/* ------------------------------------------------------------------ */
/* Byte-swap helpers (big-endian, no alignment requirement)            */
/* ------------------------------------------------------------------ */

static inline uint16_t rd16(const uint8_t *b)
{ return ((uint16_t)b[0] << 8) | b[1]; }

static inline uint32_t rd32(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
         | ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

static inline uint64_t rd64(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
    return v;
}

static inline bool feq4(const uint8_t *a, const char *s)
{ return a[0]==s[0] && a[1]==s[1] && a[2]==s[2] && a[3]==s[3]; }

/* BER variable-length integer (used in esds descriptors)             */
static uint32_t ber_len(const uint8_t **pp, const uint8_t *end)
{
    uint32_t v = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Box header reader                                                   */
/* ------------------------------------------------------------------ */

/*
 * Read one ISO BMFF box header.
 * On entry: f positioned at the start of the box.
 * On return:
 *   type[4]    = FourCC
 *   *body_end  = absolute file offset of first byte past the box body
 *   f          = positioned at first byte of the body
 * Returns false on EOF / read error.
 */
static bool box_hdr(FILE *f, uint8_t type[4], int64_t *body_end)
{
    long h_start = ftell(f);
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return false;

    uint32_t sz32 = rd32(hdr);
    memcpy(type, hdr + 4, 4);

    if (sz32 == 1) {           /* extended 64-bit size */
        uint8_t e[8];
        if (fread(e, 1, 8, f) != 8) return false;
        *body_end = h_start + (int64_t)rd64(e);
    } else if (sz32 == 0) {   /* extends to EOF */
        *body_end = (int64_t)0x7FFFFFFFFFFFFFFFLL;
    } else {
        *body_end = h_start + (int64_t)sz32;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Parser context                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Track-level state (reset per trak) */
    bool     is_audio;           /* set when hdlr handler_type == 'soun' */
    bool     mdhd_ok;
    uint32_t timescale;
    uint64_t mdhd_duration;      /* in timescale units                   */

    /* stsd results */
    m4a_codec_t codec;
    uint8_t  config[64];
    uint32_t config_size;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint32_t sample_rate;
    bool     stsd_ok;

    /* stsz results */
    uint32_t  sample_count;
    uint32_t *sample_sizes;      /* heap                                 */
    bool      stsz_ok;

    /* stsc results (run-length encoded chunk → samples table) */
    uint32_t  stsc_count;
    uint32_t *stsc_fc;           /* first_chunk (1-indexed), heap        */
    uint32_t *stsc_spc;          /* samples_per_chunk, heap              */
    bool      stsc_ok;

    /* stco / co64 results */
    uint32_t  chunk_count;
    uint64_t *chunk_offsets;     /* heap                                 */
    bool      stco_ok;
} pctx_t;

/* Forward declaration */
static void parse_children(FILE *f, int64_t end, pctx_t *ctx, int depth);

/* ------------------------------------------------------------------ */
/* esds → AudioSpecificConfig                                          */
/* ------------------------------------------------------------------ */

static void parse_esds(pctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if (len < 4) return;
    const uint8_t *p   = data + 4;  /* skip FullBox version+flags */
    const uint8_t *end = data + len;

    /* ES_Descriptor tag 0x03 */
    if (p >= end || *p++ != 0x03) return;
    ber_len(&p, end);                /* skip ES_Descriptor length */
    if (p + 3 > end) return;
    p += 2;                          /* ES_ID */
    uint8_t flags = *p++;
    if (flags & 0x80) { if (p + 2 > end) return; p += 2; }       /* streamDependenceFlag */
    if (flags & 0x40) { if (p >= end) return; p += *p + 1; }     /* URL_flag */
    if (flags & 0x20) { if (p + 2 > end) return; p += 2; }       /* OCRstreamFlag */

    /* DecoderConfigDescriptor tag 0x04 */
    if (p >= end || *p++ != 0x04) return;
    ber_len(&p, end);
    /* objectTypeIndication(1) + streamType/bufferSizeDB(4) + maxBR(4) + avgBR(4) = 13 */
    if (p + 13 > end) return;
    p += 13;

    /* DecoderSpecificInfo (AudioSpecificConfig) tag 0x05 */
    if (p >= end || *p++ != 0x05) return;
    uint32_t asc_len = ber_len(&p, end);
    if (asc_len == 0 || asc_len > 60 || p + asc_len > end) return;

    memcpy(ctx->config, p, asc_len);
    ctx->config_size = asc_len;
    ctx->codec       = M4A_CODEC_AAC;
}

/* ------------------------------------------------------------------ */
/* stsd — sample description box                                       */
/* ------------------------------------------------------------------ */

static void parse_stsd(FILE *f, pctx_t *ctx, int64_t body_end)
{
    /* FullBox(4) + entry_count(4) = 8 bytes */
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return;
    if (rd32(hdr + 4) == 0) return;   /* no entries */

    /* Read first sample-entry box header */
    uint8_t  se_type[4];
    int64_t  se_end;
    if (!box_hdr(f, se_type, &se_end)) return;

    /*
     * Common AudioSampleEntry header (28 bytes):
     *   [0..5]   reserved (6)
     *   [6..7]   data-reference-index (2)
     *   [8..15]  reserved (8)
     *   [16..17] channel_count
     *   [18..19] sample_size (bit depth; 16 for compressed codecs)
     *   [20..21] pre_defined / compression_id
     *   [22..23] reserved
     *   [24..27] samplerate (16.16 fixed-point; high 16 bits = Hz)
     */
    uint8_t ase[28];
    if (fread(ase, 1, 28, f) != 28) return;

    uint16_t channels   = rd16(ase + 16);
    uint16_t samplesize = rd16(ase + 18);
    uint32_t sr_hz      = rd32(ase + 24) >> 16;

    if (feq4(se_type, "mp4a")) {
        /* Scan extension boxes for esds */
        while (ftell(f) <= se_end - 8) {
            uint8_t inner[4];
            int64_t inner_end;
            if (!box_hdr(f, inner, &inner_end)) break;
            if (feq4(inner, "esds")) {
                long pos  = ftell(f);
                uint32_t elen = (uint32_t)(inner_end - pos);
                if (elen > 0 && elen <= 512) {
                    uint8_t *eb = malloc(elen);
                    if (eb) {
                        if (fread(eb, 1, elen, f) == elen)
                            parse_esds(ctx, eb, elen);
                        free(eb);
                    }
                }
                break;
            }
            if (inner_end <= ftell(f)) break;
            fseek(f, (long)inner_end, SEEK_SET);
        }
        if (ctx->config_size > 0) {
            ctx->channels        = (uint8_t)channels;
            ctx->bits_per_sample = (uint8_t)samplesize;
            ctx->sample_rate     = sr_hz;
            ctx->stsd_ok         = true;
        }

    } else if (feq4(se_type, "alac")) {
        /* Scan extension boxes for nested 'alac' FullBox */
        while (ftell(f) <= se_end - 8) {
            uint8_t inner[4];
            int64_t inner_end;
            if (!box_hdr(f, inner, &inner_end)) break;
            if (feq4(inner, "alac")) {
                /*
                 * Body = FullBox header (version 1B + flags 3B = 4B)
                 *      + ALACSpecificConfig (24B, big-endian) = 28 bytes total.
                 * This 28-byte blob is the "magic cookie" passed to ALACDecoder::Init().
                 *
                 * ALACSpecificConfig layout at config[4..27] (big-endian):
                 *   config[4..7]  frameLength      (uint32)
                 *   config[8]     compatibleVersion(uint8)
                 *   config[9]     bitDepth         (uint8)  ← bits_per_sample
                 *   config[10]    pb               (uint8)
                 *   config[11]    mb               (uint8)
                 *   config[12]    kb               (uint8)
                 *   config[13]    numChannels      (uint8)  ← channels
                 *   config[14..15] maxRun          (uint16 BE)
                 *   config[16..19] maxFrameBytes   (uint32 BE)
                 *   config[20..23] avgBitRate      (uint32 BE)
                 *   config[24..27] sampleRate      (uint32 BE) ← sample_rate
                 */
                uint8_t magic[28];
                if (fread(magic, 1, 28, f) == 28) {
                    memcpy(ctx->config, magic, 28);
                    ctx->config_size     = 28;
                    ctx->codec           = M4A_CODEC_ALAC;
                    ctx->channels        = magic[13];          /* numChannels  */
                    ctx->bits_per_sample = magic[9];           /* bitDepth     */
                    ctx->sample_rate     = rd32(magic + 24);   /* sampleRate   */
                    ctx->stsd_ok         = true;
                }
                break;
            }
            if (inner_end <= ftell(f)) break;
            fseek(f, (long)inner_end, SEEK_SET);
        }
    }
}

/* ------------------------------------------------------------------ */
/* mdhd — media header                                                 */
/* ------------------------------------------------------------------ */

static void parse_mdhd(FILE *f, pctx_t *ctx)
{
    uint8_t ver[4];
    if (fread(ver, 1, 4, f) != 4) return;   /* version + flags */

    if (ver[0] == 1) {
        /* 64-bit timestamps: creation_time(8)+modification_time(8)+timescale(4)+duration(8) */
        uint8_t b[28];
        if (fread(b, 1, 28, f) != 28) return;
        ctx->timescale     = rd32(b + 16);
        ctx->mdhd_duration = rd64(b + 20);
    } else {
        /* 32-bit timestamps: creation_time(4)+modification_time(4)+timescale(4)+duration(4) */
        uint8_t b[16];
        if (fread(b, 1, 16, f) != 16) return;
        ctx->timescale     = rd32(b + 8);
        ctx->mdhd_duration = rd32(b + 12);
    }
    ctx->mdhd_ok = true;
}

/* ------------------------------------------------------------------ */
/* stsz — sample size table                                            */
/* ------------------------------------------------------------------ */

static void parse_stsz(FILE *f, pctx_t *ctx)
{
    /* FullBox(4) + sample_size(4) + sample_count(4) = 12 bytes */
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return;

    uint32_t uniform_size = rd32(hdr + 4);
    uint32_t count        = rd32(hdr + 8);
    if (count == 0) return;

    ctx->sample_sizes = malloc(count * sizeof(uint32_t));
    if (!ctx->sample_sizes) return;

    if (uniform_size != 0) {
        for (uint32_t i = 0; i < count; i++)
            ctx->sample_sizes[i] = uniform_size;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            uint8_t b[4];
            if (fread(b, 1, 4, f) != 4) {
                free(ctx->sample_sizes);
                ctx->sample_sizes = NULL;
                return;
            }
            ctx->sample_sizes[i] = rd32(b);
        }
    }
    ctx->sample_count = count;
    ctx->stsz_ok      = true;
}

/* ------------------------------------------------------------------ */
/* stsc — sample-to-chunk table (run-length encoded)                   */
/* ------------------------------------------------------------------ */

static void parse_stsc(FILE *f, pctx_t *ctx)
{
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return;
    uint32_t count = rd32(hdr + 4);
    if (count == 0) return;

    ctx->stsc_fc  = malloc(count * sizeof(uint32_t));
    ctx->stsc_spc = malloc(count * sizeof(uint32_t));
    if (!ctx->stsc_fc || !ctx->stsc_spc) {
        free(ctx->stsc_fc);  ctx->stsc_fc  = NULL;
        free(ctx->stsc_spc); ctx->stsc_spc = NULL;
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[12];
        if (fread(e, 1, 12, f) != 12) {
            free(ctx->stsc_fc);  ctx->stsc_fc  = NULL;
            free(ctx->stsc_spc); ctx->stsc_spc = NULL;
            return;
        }
        ctx->stsc_fc[i]  = rd32(e);      /* first_chunk (1-indexed) */
        ctx->stsc_spc[i] = rd32(e + 4);  /* samples_per_chunk       */
        /* ignore sample_description_index at e+8 */
    }
    ctx->stsc_count = count;
    ctx->stsc_ok    = true;
}

/* ------------------------------------------------------------------ */
/* stco / co64 — chunk offset tables                                   */
/* ------------------------------------------------------------------ */

static void parse_stco(FILE *f, pctx_t *ctx, bool is64)
{
    uint8_t hdr[8];
    if (fread(hdr, 1, 8, f) != 8) return;
    uint32_t count = rd32(hdr + 4);
    if (count == 0) return;

    ctx->chunk_offsets = malloc(count * sizeof(uint64_t));
    if (!ctx->chunk_offsets) return;

    for (uint32_t i = 0; i < count; i++) {
        if (is64) {
            uint8_t b[8];
            if (fread(b, 1, 8, f) != 8) { free(ctx->chunk_offsets); ctx->chunk_offsets = NULL; return; }
            ctx->chunk_offsets[i] = rd64(b);
        } else {
            uint8_t b[4];
            if (fread(b, 1, 4, f) != 4) { free(ctx->chunk_offsets); ctx->chunk_offsets = NULL; return; }
            ctx->chunk_offsets[i] = rd32(b);
        }
    }
    ctx->chunk_count = count;
    ctx->stco_ok     = true;
}

/* ------------------------------------------------------------------ */
/* Box dispatcher                                                      */
/* ------------------------------------------------------------------ */

static void dispatch_box(FILE *f, const uint8_t *type, int64_t body_end,
                          pctx_t *ctx, int depth)
{
    if (feq4(type, "moov") || feq4(type, "mdia") ||
        feq4(type, "minf") || feq4(type, "stbl")) {
        parse_children(f, body_end, ctx, depth + 1);

    } else if (feq4(type, "trak")) {
        /*
         * Save and reset per-trak mdhd state so that the last audio trak's
         * mdhd wins.  Sample tables are gated on is_audio so video traks
         * never pollute them.
         */
        bool     save_is_audio = ctx->is_audio;
        bool     save_mdhd_ok  = ctx->mdhd_ok;
        uint32_t save_ts       = ctx->timescale;
        uint64_t save_dur      = ctx->mdhd_duration;

        ctx->is_audio = false;
        ctx->mdhd_ok  = false;

        parse_children(f, body_end, ctx, depth + 1);

        if (!ctx->is_audio) {
            /* Non-audio trak (video, chapters, …) — restore audio mdhd */
            ctx->is_audio      = save_is_audio;
            ctx->mdhd_ok       = save_mdhd_ok;
            ctx->timescale     = save_ts;
            ctx->mdhd_duration = save_dur;
        }

    } else if (feq4(type, "mdhd")) {
        if (!ctx->mdhd_ok) parse_mdhd(f, ctx);

    } else if (feq4(type, "hdlr")) {
        /* FullBox(4) + pre_defined(4) + handler_type(4) = 12 bytes */
        uint8_t b[12];
        if (fread(b, 1, 12, f) == 12)
            ctx->is_audio = feq4(b + 8, "soun");

    } else if (feq4(type, "stsd") && ctx->is_audio && !ctx->stsd_ok) {
        parse_stsd(f, ctx, body_end);

    } else if (feq4(type, "stsz") && ctx->is_audio && !ctx->stsz_ok) {
        parse_stsz(f, ctx);

    } else if (feq4(type, "stsc") && ctx->is_audio && !ctx->stsc_ok) {
        parse_stsc(f, ctx);

    } else if (feq4(type, "stco") && ctx->is_audio && !ctx->stco_ok) {
        parse_stco(f, ctx, false);

    } else if (feq4(type, "co64") && ctx->is_audio && !ctx->stco_ok) {
        parse_stco(f, ctx, true);
    }
    /* All other boxes: fall through — body_end used by parse_children to skip */
}

static void parse_children(FILE *f, int64_t end, pctx_t *ctx, int depth)
{
    if (depth > 8) return;   /* guard against malformed files */
    while (ftell(f) <= (long)(end - 8)) {
        uint8_t  type[4];
        int64_t  body_end;
        long     pos_before = ftell(f);

        if (!box_hdr(f, type, &body_end)) break;
        if (body_end <= pos_before + 8) break;   /* degenerate box */

        dispatch_box(f, type, body_end, ctx, depth);

        if (body_end < (int64_t)0x7FFFFFFFFFFFFFFFLL)
            fseek(f, (long)body_end, SEEK_SET);
        else
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Sample offset reconstruction (stsc + stco + stsz → flat table)     */
/* ------------------------------------------------------------------ */

static bool build_offsets(pctx_t *ctx, m4a_info_t *out)
{
    uint32_t N = ctx->sample_count;
    out->sample_offsets = malloc(N * sizeof(uint64_t));
    if (!out->sample_offsets) return false;

    uint32_t sample_idx = 0;
    uint32_t run        = 0;   /* current stsc entry index */

    for (uint32_t chunk = 0; chunk < ctx->chunk_count && sample_idx < N; chunk++) {
        /*
         * stsc is run-length encoded.  Each entry says "starting from chunk
         * first_chunk (1-based), every chunk has samples_per_chunk samples,
         * until the next entry changes it."
         * Advance run when the next entry's first_chunk <= current chunk+1.
         */
        uint32_t chunk1 = chunk + 1;
        while (run + 1 < ctx->stsc_count && ctx->stsc_fc[run + 1] <= chunk1)
            run++;

        uint32_t spc      = ctx->stsc_spc[run];
        uint64_t byte_pos = ctx->chunk_offsets[chunk];

        for (uint32_t s = 0; s < spc && sample_idx < N; s++) {
            out->sample_offsets[sample_idx] = byte_pos;
            byte_pos += ctx->sample_sizes[sample_idx];
            sample_idx++;
        }
    }

    if (sample_idx < N) {
        ESP_LOGW(TAG, "Offset reconstruction: expected %lu frames, got %lu",
                 (unsigned long)N, (unsigned long)sample_idx);
        out->sample_count = sample_idx;   /* truncate to what we have */
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool m4a_parse(FILE *f, m4a_info_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Scan top-level boxes for 'moov' */
    fseek(f, 0, SEEK_SET);
    int64_t moov_end        = -1;
    long    moov_body_start = -1;

    for (;;) {
        uint8_t type[4];
        int64_t body_end;
        if (!box_hdr(f, type, &body_end)) break;

        if (feq4(type, "moov")) {
            moov_body_start = ftell(f);
            moov_end        = body_end;
            break;
        }
        if (body_end >= (int64_t)0x7FFFFFFFFFFFFFFFLL) break;
        if (fseek(f, (long)body_end, SEEK_SET) != 0) break;
    }

    if (moov_end < 0) {
        ESP_LOGE(TAG, "moov box not found");
        return false;
    }

    pctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    fseek(f, moov_body_start, SEEK_SET);
    parse_children(f, moov_end, &ctx, 0);

    /* Validate all required tables were found */
    if (!ctx.stsd_ok) { ESP_LOGE(TAG, "stsd parse failed");      goto fail; }
    if (!ctx.stsz_ok) { ESP_LOGE(TAG, "stsz parse failed");      goto fail; }
    if (!ctx.stsc_ok) { ESP_LOGE(TAG, "stsc parse failed");      goto fail; }
    if (!ctx.stco_ok) { ESP_LOGE(TAG, "stco/co64 parse failed"); goto fail; }

    /* Populate output */
    out->codec           = ctx.codec;
    out->sample_rate     = ctx.sample_rate;
    out->channels        = ctx.channels;
    out->bits_per_sample = ctx.bits_per_sample;
    memcpy(out->config, ctx.config, ctx.config_size);
    out->config_size     = ctx.config_size;
    out->sample_count    = ctx.sample_count;
    out->timescale       = ctx.timescale;
    out->total_samples   = ctx.mdhd_ok ? ctx.mdhd_duration : (uint64_t)ctx.sample_count;
    out->duration_ms     = (ctx.mdhd_ok && ctx.timescale > 0)
                         ? (uint32_t)((ctx.mdhd_duration * 1000ULL) / ctx.timescale)
                         : 0;

    /* Transfer sample_sizes ownership (avoid free in fail path) */
    out->sample_sizes  = ctx.sample_sizes;
    ctx.sample_sizes   = NULL;

    /* Reconstruct flat sample_offsets from stsc + stco + stsz */
    if (!build_offsets(&ctx, out)) {
        ESP_LOGE(TAG, "OOM for sample_offsets");
        goto fail;
    }

    ESP_LOGI(TAG, "M4A: %s %luHz %d-ch %d-bit | %lu frames | %lu ms",
             out->codec == M4A_CODEC_AAC ? "AAC" : "ALAC",
             (unsigned long)out->sample_rate,
             out->channels, out->bits_per_sample,
             (unsigned long)out->sample_count,
             (unsigned long)out->duration_ms);

    /* Release temporary tables */
    free(ctx.stsc_fc);
    free(ctx.stsc_spc);
    free(ctx.chunk_offsets);
    return true;

fail:
    free(ctx.sample_sizes);
    free(ctx.stsc_fc);
    free(ctx.stsc_spc);
    free(ctx.chunk_offsets);
    free(out->sample_sizes);
    free(out->sample_offsets);
    out->sample_sizes   = NULL;
    out->sample_offsets = NULL;
    return false;
}

void m4a_free(m4a_info_t *info)
{
    if (info) {
        free(info->sample_sizes);
        free(info->sample_offsets);
        info->sample_sizes   = NULL;
        info->sample_offsets = NULL;
    }
}
