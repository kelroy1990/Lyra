// dr_mp3 configuration: no stdio (we use custom I/O callbacks)
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO

#include "audio_codecs_internal.h"
#include "dr_mp3.h"
#include <esp_log.h>
#include <stdlib.h>

static const char *TAG = "codec_mp3";

//--------------------------------------------------------------------+
// dr_mp3 I/O callbacks (read/seek/tell via FILE*)
//--------------------------------------------------------------------+

static size_t mp3_read_cb(void *userdata, void *buffer, size_t bytes_to_read)
{
    return fread(buffer, 1, bytes_to_read, (FILE *)userdata);
}

static drmp3_bool32 mp3_seek_cb(void *userdata, int offset, drmp3_seek_origin origin)
{
    int whence;
    switch (origin) {
        case DRMP3_SEEK_SET: whence = SEEK_SET; break;
        case DRMP3_SEEK_CUR: whence = SEEK_CUR; break;
        default: return DRMP3_FALSE;
    }
    return fseek((FILE *)userdata, offset, whence) == 0;
}

static drmp3_bool32 mp3_tell_cb(void *userdata, drmp3_int64 *pCursor)
{
    long pos = ftell((FILE *)userdata);
    if (pos < 0) return DRMP3_FALSE;
    *pCursor = (drmp3_int64)pos;
    return DRMP3_TRUE;
}

//--------------------------------------------------------------------+
// MP3 decode: dr_mp3 outputs float, we convert to int32 left-justified
//--------------------------------------------------------------------+

// Static buffer for float→int32 conversion (480 stereo frames)
static float s_mp3_float_buf[480 * 2];

static int32_t mp3_decode(codec_handle_t *h, int32_t *buffer, uint32_t max_frames)
{
    drmp3 *mp3 = (drmp3 *)h->mp3.drmp3;
    if (!mp3) return -1;

    if (max_frames > 480) max_frames = 480;  // clamp to static buffer size

    drmp3_uint64 frames = drmp3_read_pcm_frames_f32(mp3, max_frames, s_mp3_float_buf);
    if (frames == 0) return 0;

    uint32_t channels = h->info.channels;

    if (channels == 1) {
        // Mono → stereo expansion + float→int32
        for (uint32_t i = 0; i < (uint32_t)frames; i++) {
            int32_t sample = (int32_t)(s_mp3_float_buf[i] * 2147483648.0f);
            buffer[i * 2]     = sample;
            buffer[i * 2 + 1] = sample;
        }
    } else {
        // Stereo: float→int32 conversion
        uint32_t total_samples = (uint32_t)frames * 2;
        for (uint32_t i = 0; i < total_samples; i++) {
            buffer[i] = (int32_t)(s_mp3_float_buf[i] * 2147483648.0f);
        }
    }

    return (int32_t)frames;
}

//--------------------------------------------------------------------+
// MP3 seek
//--------------------------------------------------------------------+

static bool mp3_seek(codec_handle_t *h, uint64_t frame_pos)
{
    drmp3 *mp3 = (drmp3 *)h->mp3.drmp3;
    if (!mp3) return false;
    return drmp3_seek_to_pcm_frame(mp3, frame_pos) == DRMP3_TRUE;
}

//--------------------------------------------------------------------+
// MP3 close
//--------------------------------------------------------------------+

static void mp3_close(codec_handle_t *h)
{
    drmp3 *mp3 = (drmp3 *)h->mp3.drmp3;
    if (mp3) {
        drmp3_uninit(mp3);
        free(mp3);
        h->mp3.drmp3 = NULL;
    }
}

//--------------------------------------------------------------------+
// vtable
//--------------------------------------------------------------------+

static const codec_vtable_t mp3_vtable = {
    .decode = mp3_decode,
    .seek   = mp3_seek,
    .close  = mp3_close,
};

//--------------------------------------------------------------------+
// MP3 open: init dr_mp3 with custom I/O
//--------------------------------------------------------------------+

bool codec_mp3_open(codec_handle_t *h)
{
    // Get file size before dr_mp3 init (file is at position 0 from codec_open)
    fseek((FILE *)h->file, 0, SEEK_END);
    long file_size = ftell((FILE *)h->file);
    fseek((FILE *)h->file, 0, SEEK_SET);

    drmp3 *mp3 = calloc(1, sizeof(drmp3));
    if (!mp3) {
        ESP_LOGE(TAG, "Out of memory for drmp3 (%u bytes)", (unsigned)sizeof(drmp3));
        return false;
    }

    if (!drmp3_init(mp3, mp3_read_cb, mp3_seek_cb, mp3_tell_cb, NULL, h->file, NULL)) {
        ESP_LOGE(TAG, "drmp3_init failed");
        free(mp3);
        return false;
    }

    h->mp3.drmp3 = mp3;
    h->info.sample_rate = mp3->sampleRate;
    h->info.bits_per_sample = 16;   // MP3 effective dynamic range ~16-bit
    h->info.channels = mp3->channels;

    // Get total frame count — try Xing/Info header first (instant), fall back to bitrate estimation
    // Avoids drmp3_get_pcm_frame_count() which scans the entire file (~8s on SD)
    drmp3_uint64 uint64_max = (drmp3_uint64)0xFFFFFFFF << 32 | (drmp3_uint64)0xFFFFFFFF;

    if (mp3->totalPCMFrameCount != uint64_max) {
        // Xing/Info header present — exact count
        uint64_t total = mp3->totalPCMFrameCount;
        if (total >= mp3->delayInPCMFrames)
            total -= mp3->delayInPCMFrames;
        if (total >= mp3->paddingInPCMFrames)
            total -= mp3->paddingInPCMFrames;
        h->info.total_frames = total;
    } else if (file_size > 0 && mp3->sampleRate > 0) {
        // No Xing header — estimate from file size + first frame bitrate
        // Bitrate lookup table (same as drmp3_hdr_bitrate_kbps internal function)
        static const uint8_t halfrate[2][3][15] = {
            { { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 },
              { 0,4,8,12,16,20,24,28,32,40,48,56,64,72,80 },
              { 0,16,24,28,32,40,48,56,64,72,80,88,96,112,128 } },
            { { 0,16,20,24,28,32,40,48,56,64,80,96,112,128,160 },
              { 0,16,24,28,32,40,48,56,64,80,96,112,128,160,192 },
              { 0,16,32,48,64,80,96,112,128,144,160,176,192,208,224 } },
        };
        const uint8_t *hdr = mp3->decoder.header;
        int mpeg1 = (hdr[1] & 0x08) ? 1 : 0;
        int layer = (hdr[1] >> 1) & 3;    // 1=III, 2=II, 3=I
        int br_idx = hdr[2] >> 4;
        uint32_t kbps = (layer >= 1 && layer <= 3 && br_idx < 15)
                        ? 2 * halfrate[mpeg1][layer - 1][br_idx] : 0;

        if (kbps > 0) {
            // duration_s = file_bytes * 8 / bitrate_bps
            h->info.total_frames = (uint64_t)((double)file_size * 8.0
                                   / (double)(kbps * 1000) * mp3->sampleRate);
            ESP_LOGI(TAG, "MP3: estimated %lu kbps, %ld bytes (CBR=exact, VBR~approx)",
                     (unsigned long)kbps, file_size);
        } else {
            h->info.total_frames = 0;
        }
    } else {
        h->info.total_frames = 0;
    }

    h->vt = &mp3_vtable;

    ESP_LOGI(TAG, "MP3: %luHz %dch, %llu frames",
             h->info.sample_rate, h->info.channels, h->info.total_frames);

    return true;
}
