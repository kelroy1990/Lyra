// dr_flac configuration: no stdio (we use custom I/O callbacks)
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DRFLAC_BUFFER_SIZE 8192  // 8KB internal read buffer (reduces SD I/O calls)

#include "audio_codecs_internal.h"
#include "dr_flac.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "codec_flac";

//--------------------------------------------------------------------+
// ReplayGain pre-scan: find REPLAYGAIN_TRACK_GAIN in VORBIS_COMMENT
//
// Called before drflac_open().  Reads FLAC metadata blocks looking for
// block type 4 (VORBIS_COMMENT).  Rewinds to offset 0 before returning.
// Returns gain in dB (0.0 if no tag found).
//--------------------------------------------------------------------+

static float flac_read_replaygain(FILE *f)
{
    float gain = 0.0f;

    fseek(f, 0, SEEK_SET);

    /* Verify fLaC marker */
    uint8_t marker[4];
    if (fread(marker, 1, 4, f) != 4 || memcmp(marker, "fLaC", 4) != 0)
        goto done;

    for (;;) {
        /* 4-byte metadata block header:
         *   [0]     bit7 = last_metadata_block flag, [6:0] = block_type
         *   [1..3]  block_length (24-bit BE, does NOT include the 4-byte header)
         */
        uint8_t hdr[4];
        if (fread(hdr, 1, 4, f) != 4) break;

        bool     last_block = (hdr[0] & 0x80) != 0;
        uint8_t  btype      =  hdr[0] & 0x7F;
        uint32_t blen       = ((uint32_t)hdr[1] << 16)
                            | ((uint32_t)hdr[2] <<  8)
                            |  (uint32_t)hdr[3];

        if (btype == 4) { /* VORBIS_COMMENT */
            /*
             * Vorbis comment content (all lengths are little-endian):
             *   4 bytes LE: vendor string length (n)
             *   n bytes:    vendor string
             *   4 bytes LE: comment count
             *   for each:   4 bytes LE length + "KEY=VALUE" UTF-8 string
             *
             * REPLAYGAIN_TRACK_GAIN=+x.xx dB  (or negative, no space before dB)
             */
            uint8_t *blk = malloc(blen);
            if (!blk) break;
            if (fread(blk, 1, blen, f) != blen) { free(blk); break; }

            const uint8_t *p   = blk;
            const uint8_t *end = blk + blen;

            /* Skip vendor string */
            if (p + 4 > end) { free(blk); break; }
            uint32_t vlen = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                          | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p += 4;
            if (p + vlen <= end) p += vlen; else { free(blk); break; }

            /* Comment count */
            if (p + 4 > end) { free(blk); break; }
            uint32_t nc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p += 4;

            for (uint32_t i = 0; i < nc && p + 4 <= end; i++) {
                uint32_t clen = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                              | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                p += 4;
                if (clen > (uint32_t)(end - p)) break;

                /* "REPLAYGAIN_TRACK_GAIN=" is 22 characters */
                if (clen >= 22 &&
                    strncasecmp((const char *)p, "REPLAYGAIN_TRACK_GAIN=", 22) == 0) {
                    gain = strtof((const char *)p + 22, NULL);
                }
                p += clen;
            }
            free(blk);
            /* VORBIS_COMMENT is unique per file — stop scanning */
            break;
        } else {
            /* Skip this block */
            fseek(f, (long)blen, SEEK_CUR);
        }

        if (last_block) break;
    }

done:
    fseek(f, 0, SEEK_SET);
    return gain;
}

//--------------------------------------------------------------------+
// dr_flac I/O callbacks (read/seek/tell via FILE*)
//--------------------------------------------------------------------+

static size_t flac_read_cb(void *userdata, void *buffer, size_t bytes_to_read)
{
    return fread(buffer, 1, bytes_to_read, (FILE *)userdata);
}

static drflac_bool32 flac_seek_cb(void *userdata, int offset, drflac_seek_origin origin)
{
    int whence;
    switch (origin) {
        case DRFLAC_SEEK_SET: whence = SEEK_SET; break;
        case DRFLAC_SEEK_CUR: whence = SEEK_CUR; break;
        case DRFLAC_SEEK_END: whence = SEEK_END; break;
        default: return DRFLAC_FALSE;
    }
    return fseek((FILE *)userdata, offset, whence) == 0;
}

static drflac_bool32 flac_tell_cb(void *userdata, drflac_int64 *pCursor)
{
    long pos = ftell((FILE *)userdata);
    if (pos < 0) return DRFLAC_FALSE;
    *pCursor = (drflac_int64)pos;
    return DRFLAC_TRUE;
}

//--------------------------------------------------------------------+
// FLAC decode: dr_flac outputs int32 left-justified natively
//--------------------------------------------------------------------+

static int32_t flac_decode(codec_handle_t *h, int32_t *buffer, uint32_t max_frames)
{
    drflac *flac = (drflac *)h->flac.drflac;
    if (!flac) return -1;

    if (h->info.channels == 1) {
        // Mono: decode into temp, then expand to stereo
        static drflac_int32 mono_buf[1024];  // must match max decode block size
        drflac_uint64 frames = drflac_read_pcm_frames_s32(flac, max_frames, mono_buf);
        for (uint32_t i = 0; i < (uint32_t)frames; i++) {
            buffer[i * 2]     = (int32_t)mono_buf[i];
            buffer[i * 2 + 1] = (int32_t)mono_buf[i];
        }
        return (int32_t)frames;
    }

    // Stereo (or more): decode into drflac_int32 buffer, then copy
    // drflac_int32 is 'int' on RISC-V, int32_t is 'long int' — must use correct type
    drflac_int32 *decode_buf = (drflac_int32 *)buffer;  // safe: both are 32-bit
    drflac_uint64 frames = drflac_read_pcm_frames_s32(flac, max_frames, decode_buf);
    return (int32_t)frames;
}

//--------------------------------------------------------------------+
// FLAC seek
//--------------------------------------------------------------------+

static bool flac_seek(codec_handle_t *h, uint64_t frame_pos)
{
    drflac *flac = (drflac *)h->flac.drflac;
    if (!flac) return false;
    return drflac_seek_to_pcm_frame(flac, frame_pos) == DRFLAC_TRUE;
}

//--------------------------------------------------------------------+
// FLAC close
//--------------------------------------------------------------------+

static void flac_close(codec_handle_t *h)
{
    drflac *flac = (drflac *)h->flac.drflac;
    if (flac) {
        drflac_close(flac);
        h->flac.drflac = NULL;
    }
}

//--------------------------------------------------------------------+
// vtable
//--------------------------------------------------------------------+

static const codec_vtable_t flac_vtable = {
    .decode = flac_decode,
    .seek   = flac_seek,
    .close  = flac_close,
};

//--------------------------------------------------------------------+
// FLAC open: pre-scan for ReplayGain, then init dr_flac with custom I/O
//--------------------------------------------------------------------+

bool codec_flac_open(codec_handle_t *h)
{
    /* Pre-scan metadata blocks for REPLAYGAIN_TRACK_GAIN before dr_flac
     * takes ownership of the read position.  The scan rewinds to 0. */
    float gain_db = flac_read_replaygain(h->file);

    drflac *flac = drflac_open(flac_read_cb, flac_seek_cb, flac_tell_cb, h->file, NULL);
    if (!flac) {
        ESP_LOGE(TAG, "drflac_open failed");
        return false;
    }

    h->flac.drflac          = flac;
    h->info.sample_rate     = flac->sampleRate;
    h->info.bits_per_sample = flac->bitsPerSample;
    h->info.channels        = flac->channels;
    h->info.total_frames    = flac->totalPCMFrameCount;
    h->info.gain_db         = gain_db;
    h->vt = &flac_vtable;

    ESP_LOGI(TAG, "FLAC: %luHz %d-bit %dch, %llu frames, gain=%.2f dB",
             h->info.sample_rate, h->info.bits_per_sample,
             h->info.channels, h->info.total_frames, gain_db);

    return true;
}
