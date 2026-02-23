/*
 * codec_aac.c — AAC-LC / HE-AAC decoder via opencore-aacdec
 *
 * Two input modes:
 *   ADTS (.aac)  — raw ADTS bitstream; approximate seek via avg frame size
 *   M4A  (.m4a)  — ISO BMFF container; exact seek via sample table
 *
 * Output: int32_t stereo interleaved, left-justified (16-bit PCM << 16).
 */

#include "audio_codecs_internal.h"
#include "m4a_demuxer.h"
#include "pvmp4audiodecoder_api.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "codec_aac";

/* ADTS sampling frequency index table (ISO 14496-3 Table 1.16) */
static const uint32_t k_aac_sample_rates[13] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025,  8000, 7350
};

typedef struct {
    tPVMP4AudioDecoderExternal ext;
    void    *mem;

    /* M4A-AAC mode (is_m4a == true) */
    bool     is_m4a;
    m4a_info_t m4a;
    uint32_t   m4a_frame_idx;
    uint8_t   *m4a_frame_buf;      /* heap: max compressed frame size */
    uint32_t   m4a_frame_buf_sz;

    /* ADTS mode (is_m4a == false) */
    long     adts_sync_offset;     /* file offset of first valid ADTS sync */
    uint32_t avg_frame_bytes;      /* average frame size (for seek estimate) */

    /* Decoder I/O buffers */
    uint8_t  in_buf[PVMP4AUDIODECODER_INBUFSIZE]; /* ADTS input only */
    /* Output: worst-case HE-AAC stereo = 2048 samples/ch × 2 ch = 4096 */
    int16_t  out_buf[4096 + 2048]; /* +2048 spare for pOutputBuffer_plus */
} aac_state_t;

/* -----------------------------------------------------------------------
 * ADTS header helpers
 * ----------------------------------------------------------------------- */

static uint32_t adts_parse(const uint8_t *h, uint32_t *sr_hz,
                            uint8_t *channels, uint32_t *hdr_size)
{
    if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) return 0;

    uint8_t  prot_absent  = h[1] & 0x01;
    uint8_t  sf_idx       = (h[2] >> 2) & 0x0F;
    uint8_t  ch_cfg       = ((h[2] & 0x01) << 2) | (h[3] >> 6);
    uint32_t frame_length = ((uint32_t)(h[3] & 0x03) << 11)
                          | ((uint32_t)h[4] << 3)
                          |  (h[5] >> 5);

    if (sr_hz    && sf_idx < 13) *sr_hz    = k_aac_sample_rates[sf_idx];
    if (channels)                *channels = ch_cfg ? ch_cfg : 2;
    if (hdr_size)                *hdr_size = prot_absent ? 7u : 9u;

    return frame_length;
}

/* -----------------------------------------------------------------------
 * Convert int16_t interleaved → int32_t left-justified stereo
 * ----------------------------------------------------------------------- */

static void to_int32_stereo(const int16_t *src, int32_t *dst,
                             uint32_t frames, uint32_t ch)
{
    if (ch == 1) {
        for (uint32_t i = 0; i < frames; i++) {
            int32_t s = (int32_t)src[i] << 16;
            dst[i * 2]     = s;
            dst[i * 2 + 1] = s;
        }
    } else {
        uint32_t n = frames * 2;
        for (uint32_t i = 0; i < n; i++)
            dst[i] = (int32_t)src[i] << 16;
    }
}

/* -----------------------------------------------------------------------
 * vtable — decode (dispatches to ADTS or M4A path)
 * ----------------------------------------------------------------------- */

static int32_t aac_decode(codec_handle_t *h, int32_t *buffer, uint32_t max_frames)
{
    aac_state_t *st = (aac_state_t *)h->aac.state;
    if (!st) return -1;

    if (st->is_m4a) {
        /* ----- M4A-AAC: raw frames from sample table ----- */
        if (st->m4a_frame_idx >= st->m4a.sample_count) return 0;  /* EOF */

        uint64_t off  = st->m4a.sample_offsets[st->m4a_frame_idx];
        uint32_t size = st->m4a.sample_sizes[st->m4a_frame_idx];

        if (size == 0 || size > st->m4a_frame_buf_sz) {
            st->m4a_frame_idx++;
            return 0;
        }
        if (fseek(h->file, (long)off, SEEK_SET) != 0) return -1;
        if (fread(st->m4a_frame_buf, 1, size, h->file) != size) return -1;

        st->ext.pInputBuffer             = st->m4a_frame_buf;
        st->ext.inputBufferCurrentLength = (int32_t)size;
        st->ext.inputBufferUsedLength    = 0;
        st->ext.remainderBits            = 0;
        st->ext.pOutputBuffer            = st->out_buf;
        st->ext.pOutputBuffer_plus       = st->out_buf + 2048;
        st->ext.desiredChannels          = 2;

        int32_t err = PVMP4AudioDecodeFrame(&st->ext, st->mem);
        st->m4a_frame_idx++;
        if (err != MP4AUDEC_SUCCESS && err != MP4AUDEC_LOST_FRAME_SYNC) {
            ESP_LOGW(TAG, "M4A decode error: %ld", (long)err);
            return 0;
        }

        uint32_t frames = (uint32_t)st->ext.frameLength
                        * (uint32_t)st->ext.aacPlusUpsamplingFactor;
        if (frames > max_frames) frames = max_frames;
        to_int32_stereo(st->out_buf, buffer, frames, (uint32_t)h->info.channels);
        return (int32_t)frames;
    }

    /* ----- ADTS mode ----- */
    uint8_t hdr[9];
    if (fread(hdr, 1, 7, h->file) != 7) return 0;

    /* Sync recovery */
    if (hdr[0] != 0xFF || (hdr[1] & 0xF0) != 0xF0) {
        uint8_t prev = hdr[0], cur = hdr[1];
        bool found = false;
        for (int tries = 0; tries < 4096; tries++) {
            prev = cur;
            if (fread(&cur, 1, 1, h->file) != 1) return 0;
            if (prev == 0xFF && (cur & 0xF0) == 0xF0) {
                hdr[0] = prev; hdr[1] = cur;
                if (fread(hdr + 2, 1, 5, h->file) != 5) return 0;
                found = true;
                break;
            }
        }
        if (!found) { ESP_LOGW(TAG, "ADTS sync lost"); return -1; }
    }

    uint32_t hdr_size, frame_length;
    frame_length = adts_parse(hdr, NULL, NULL, &hdr_size);
    if (frame_length < hdr_size || frame_length > PVMP4AUDIODECODER_INBUFSIZE) {
        ESP_LOGW(TAG, "Bad ADTS frame length: %lu", (unsigned long)frame_length);
        return 0;
    }
    if (hdr_size == 9) {
        if (fread(hdr + 7, 1, 2, h->file) != 2) return 0;
    }

    memcpy(st->in_buf, hdr, hdr_size);
    uint32_t payload = frame_length - hdr_size;
    if (fread(st->in_buf + hdr_size, 1, payload, h->file) != payload) return 0;

    st->ext.pInputBuffer             = st->in_buf;
    st->ext.inputBufferCurrentLength = (int32_t)frame_length;
    st->ext.inputBufferUsedLength    = 0;
    st->ext.remainderBits            = 0;
    st->ext.pOutputBuffer            = st->out_buf;
    st->ext.pOutputBuffer_plus       = st->out_buf + 2048;
    st->ext.desiredChannels          = 2;

    int32_t err = PVMP4AudioDecodeFrame(&st->ext, st->mem);
    if (err != MP4AUDEC_SUCCESS) {
        if (err != MP4AUDEC_LOST_FRAME_SYNC)
            ESP_LOGW(TAG, "Decode error: %ld", (long)err);
        return 0;
    }

    uint32_t frames = (uint32_t)st->ext.frameLength
                    * (uint32_t)st->ext.aacPlusUpsamplingFactor;
    if (frames > max_frames) frames = max_frames;
    to_int32_stereo(st->out_buf, buffer, frames, (uint32_t)h->info.channels);
    return (int32_t)frames;
}

/* -----------------------------------------------------------------------
 * vtable — seek
 * ----------------------------------------------------------------------- */

static bool aac_seek(codec_handle_t *h, uint64_t frame_pos)
{
    aac_state_t *st = (aac_state_t *)h->aac.state;
    if (!st) return false;

    if (st->is_m4a) {
        /* M4A-AAC: exact seek by sample-table index */
        uint32_t frames_per_packet = (uint32_t)st->ext.frameLength;
        if (frames_per_packet == 0) frames_per_packet = 1024;  /* AAC-LC default */
        uint32_t new_idx = (uint32_t)(frame_pos / frames_per_packet);
        if (new_idx >= st->m4a.sample_count) new_idx = st->m4a.sample_count;
        st->m4a_frame_idx = new_idx;
        return true;
    }

    /* ADTS: rewind */
    if (frame_pos == 0) {
        fseek(h->file, st->adts_sync_offset, SEEK_SET);
        return true;
    }

    /* ADTS: approximate seek for non-zero positions */
    if (st->avg_frame_bytes == 0) return false;

    /* Each ADTS frame ≈ 1024 PCM frames (AAC-LC).  HE-AAC doubles via SBR
     * upsampling but the bitstream frame count stays the same. */
    uint64_t est_frame  = frame_pos / 1024;
    int64_t  est_offset = (int64_t)st->adts_sync_offset
                        + (int64_t)(est_frame * (uint64_t)st->avg_frame_bytes);
    if (est_offset < (int64_t)st->adts_sync_offset)
        est_offset = (int64_t)st->adts_sync_offset;

    fseek(h->file, (long)est_offset, SEEK_SET);

    /* Scan forward up to 512 bytes for a valid sync word */
    uint8_t scan[512];
    size_t  n = fread(scan, 1, sizeof(scan), h->file);
    for (size_t i = 0; i + 1 < n; i++) {
        if (scan[i] == 0xFF && (scan[i + 1] & 0xF0) == 0xF0) {
            fseek(h->file, (long)(est_offset + (int64_t)i), SEEK_SET);
            return true;
        }
    }
    /* Fallback to stream start */
    fseek(h->file, st->adts_sync_offset, SEEK_SET);
    return true;
}

/* -----------------------------------------------------------------------
 * vtable — close
 * ----------------------------------------------------------------------- */

static void aac_close(codec_handle_t *h)
{
    aac_state_t *st = (aac_state_t *)h->aac.state;
    if (st) {
        if (st->is_m4a) {
            m4a_free(&st->m4a);
            free(st->m4a_frame_buf);
        }
        free(st->mem);
        free(st);
        h->aac.state = NULL;
    }
}

static const codec_vtable_t aac_vtable = {
    .decode = aac_decode,
    .seek   = aac_seek,
    .close  = aac_close,
};

/* -----------------------------------------------------------------------
 * Shared: allocate and initialise the opencore-aacdec working memory
 * ----------------------------------------------------------------------- */

static bool aac_alloc_decoder(aac_state_t *st)
{
    uint32_t mem_req = PVMP4AudioDecoderGetMemRequirements();
    st->mem = malloc(mem_req);
    if (!st->mem) {
        ESP_LOGE(TAG, "OOM for decoder memory (%lu B)", (unsigned long)mem_req);
        return false;
    }

    st->ext.outputFormat         = OUTPUTFORMAT_16PCM_INTERLEAVED;
    st->ext.desiredChannels      = 2;
    st->ext.inputBufferMaxLength = PVMP4AUDIODECODER_INBUFSIZE;

    if (PVMP4AudioDecoderInitLibrary(&st->ext, st->mem) != MP4AUDEC_SUCCESS) {
        ESP_LOGE(TAG, "PVMP4AudioDecoderInitLibrary failed");
        free(st->mem);
        st->mem = NULL;
        return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * codec_aac_open — ADTS (.aac) path
 * ----------------------------------------------------------------------- */

bool codec_aac_open(codec_handle_t *h)
{
    /* Scan up to 64 KB for the first valid ADTS sync word (skips ID3 tags) */
    uint8_t  scan[512];
    long     sync_offset = -1;
    uint32_t sr_hz       = 0;
    uint8_t  channels    = 0;

    for (long base = 0; base < 65536; base += (long)sizeof(scan)) {
        size_t got = fread(scan, 1, sizeof(scan), h->file);
        if (got < 7) break;
        for (size_t i = 0; i + 7 <= got; i++) {
            uint32_t fl = adts_parse(&scan[i], &sr_hz, &channels, NULL);
            if (fl >= 7 && fl <= PVMP4AUDIODECODER_INBUFSIZE && sr_hz > 0) {
                sync_offset = base + (long)i;
                break;
            }
        }
        if (sync_offset >= 0) break;
        if (got < sizeof(scan)) break;
        fseek(h->file, -8, SEEK_CUR);  /* overlap to avoid missing cross-boundary syncs */
    }

    if (sync_offset < 0 || sr_hz == 0) {
        ESP_LOGE(TAG, "No valid ADTS sync found");
        return false;
    }

    aac_state_t *st = calloc(1, sizeof(aac_state_t));
    if (!st) return false;

    if (!aac_alloc_decoder(st)) { free(st); return false; }

    st->adts_sync_offset = sync_offset;
    fseek(h->file, sync_offset, SEEK_SET);

    /* Sample ~50 ADTS frames to compute avg_frame_bytes for seek estimation */
    {
        uint32_t total = 0, count = 0;
        for (int i = 0; i < 50; i++) {
            uint8_t hdr[7];
            if (fread(hdr, 1, 7, h->file) != 7) break;
            uint32_t fl = adts_parse(hdr, NULL, NULL, NULL);
            if (fl < 7 || fl > PVMP4AUDIODECODER_INBUFSIZE) break;
            total += fl;
            count++;
            fseek(h->file, (long)fl - 7, SEEK_CUR);
        }
        st->avg_frame_bytes = count ? (total / count) : 0;
        fseek(h->file, sync_offset, SEEK_SET);
    }

    h->aac.state            = st;
    h->info.sample_rate     = sr_hz;
    h->info.bits_per_sample = 16;
    h->info.channels        = channels;
    h->info.total_frames    = 0;
    h->info.duration_ms     = 0;
    h->vt                   = &aac_vtable;

    ESP_LOGI(TAG, "AAC(ADTS): %lu Hz %d-ch  avg_frame=%lu B",
             (unsigned long)sr_hz, channels,
             (unsigned long)st->avg_frame_bytes);
    return true;
}

/* -----------------------------------------------------------------------
 * codec_aac_open_m4a — M4A-AAC path
 *
 * Called from codec_m4a_open (codec_alac.cpp) with a pre-parsed m4a_info_t.
 * Takes ownership of info->sample_sizes and info->sample_offsets on success
 * (frees them on failure too).
 * ----------------------------------------------------------------------- */

bool codec_aac_open_m4a(codec_handle_t *h, m4a_info_t *info)
{
    if (info->codec != M4A_CODEC_AAC) {
        ESP_LOGE(TAG, "codec_aac_open_m4a: not AAC");
        m4a_free(info);
        return false;
    }

    aac_state_t *st = calloc(1, sizeof(aac_state_t));
    if (!st) { m4a_free(info); return false; }

    if (!aac_alloc_decoder(st)) { free(st); m4a_free(info); return false; }

    /* Configure decoder from AudioSpecificConfig (no ADTS header) */
    st->ext.pInputBuffer             = info->config;
    st->ext.inputBufferCurrentLength = (int32_t)info->config_size;
    st->ext.inputBufferUsedLength    = 0;
    st->ext.remainderBits            = 0;
    if (PVMP4AudioDecoderConfig(&st->ext, st->mem) != MP4AUDEC_SUCCESS) {
        ESP_LOGW(TAG, "PVMP4AudioDecoderConfig failed — using stsd metadata");
        /* Non-fatal: decoder still initialised; metadata from stsd is used */
    }

    /* Find worst-case compressed frame size for the read buffer */
    uint32_t max_frame = 0;
    for (uint32_t i = 0; i < info->sample_count; i++)
        if (info->sample_sizes[i] > max_frame)
            max_frame = info->sample_sizes[i];
    if (max_frame == 0) max_frame = PVMP4AUDIODECODER_INBUFSIZE;

    st->m4a_frame_buf = malloc(max_frame);
    if (!st->m4a_frame_buf) {
        free(st->mem);
        free(st);
        m4a_free(info);
        return false;
    }
    st->m4a_frame_buf_sz = max_frame;

    /* Transfer sample table ownership */
    st->m4a = *info;
    info->sample_sizes   = NULL;
    info->sample_offsets = NULL;

    st->is_m4a = true;

    h->aac.state            = st;
    h->info.sample_rate     = st->m4a.sample_rate;
    h->info.bits_per_sample = 16;
    h->info.channels        = st->m4a.channels;
    h->info.total_frames    = st->m4a.total_samples;
    h->info.duration_ms     = st->m4a.duration_ms;
    h->info.format          = CODEC_FORMAT_AAC;
    h->vt                   = &aac_vtable;

    ESP_LOGI(TAG, "AAC(M4A): %lu Hz %d-ch | %lu frames | %lu ms",
             (unsigned long)st->m4a.sample_rate, st->m4a.channels,
             (unsigned long)st->m4a.sample_count,
             (unsigned long)st->m4a.duration_ms);
    return true;
}
