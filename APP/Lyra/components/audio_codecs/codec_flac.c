// dr_flac configuration: no stdio (we use custom I/O callbacks)
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DRFLAC_BUFFER_SIZE 8192  // 8KB internal read buffer (reduces SD I/O calls)

#include "audio_codecs_internal.h"
#include "dr_flac.h"
#include <esp_log.h>

static const char *TAG = "codec_flac";

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
// FLAC open: init dr_flac with custom I/O
//--------------------------------------------------------------------+

bool codec_flac_open(codec_handle_t *h)
{
    drflac *flac = drflac_open(flac_read_cb, flac_seek_cb, flac_tell_cb, h->file, NULL);
    if (!flac) {
        ESP_LOGE(TAG, "drflac_open failed");
        return false;
    }

    h->flac.drflac = flac;
    h->info.sample_rate = flac->sampleRate;
    h->info.bits_per_sample = flac->bitsPerSample;
    h->info.channels = flac->channels;
    h->info.total_frames = flac->totalPCMFrameCount;
    h->vt = &flac_vtable;

    ESP_LOGI(TAG, "FLAC: %luHz %d-bit %dch, %llu frames",
             h->info.sample_rate, h->info.bits_per_sample,
             h->info.channels, h->info.total_frames);

    return true;
}
