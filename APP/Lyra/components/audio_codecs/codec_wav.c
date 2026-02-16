// dr_wav configuration: no stdio (we use custom I/O callbacks)
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO

#include "audio_codecs_internal.h"
#include "dr_wav.h"
#include <esp_log.h>
#include <stdlib.h>

static const char *TAG = "codec_wav";

//--------------------------------------------------------------------+
// dr_wav I/O callbacks (read/seek/tell via FILE*)
//--------------------------------------------------------------------+

static size_t wav_read_cb(void *userdata, void *buffer, size_t bytes_to_read)
{
    return fread(buffer, 1, bytes_to_read, (FILE *)userdata);
}

static drwav_bool32 wav_seek_cb(void *userdata, int offset, drwav_seek_origin origin)
{
    int whence;
    switch (origin) {
        case DRWAV_SEEK_SET: whence = SEEK_SET; break;
        case DRWAV_SEEK_CUR: whence = SEEK_CUR; break;
        default: return DRWAV_FALSE;
    }
    return fseek((FILE *)userdata, offset, whence) == 0;
}

static drwav_bool32 wav_tell_cb(void *userdata, drwav_int64 *pCursor)
{
    long pos = ftell((FILE *)userdata);
    if (pos < 0) return DRWAV_FALSE;
    *pCursor = (drwav_int64)pos;
    return DRWAV_TRUE;
}

//--------------------------------------------------------------------+
// WAV decode: dr_wav outputs int32 left-justified for all formats
//--------------------------------------------------------------------+

static int32_t wav_decode(codec_handle_t *h, int32_t *buffer, uint32_t max_frames)
{
    drwav *wav = (drwav *)h->wav.drwav;
    if (!wav) return -1;

    if (h->info.channels == 1) {
        // Mono: decode into temp, then expand to stereo
        static drwav_int32 mono_buf[480];
        drwav_uint64 frames = drwav_read_pcm_frames_s32(wav, max_frames, mono_buf);
        for (uint32_t i = 0; i < (uint32_t)frames; i++) {
            buffer[i * 2]     = (int32_t)mono_buf[i];
            buffer[i * 2 + 1] = (int32_t)mono_buf[i];
        }
        return (int32_t)frames;
    }

    // Stereo (or more): decode directly
    // drwav_int32 is 'int' on RISC-V, int32_t is 'long int' — cast is safe (both 32-bit)
    drwav_int32 *decode_buf = (drwav_int32 *)buffer;
    drwav_uint64 frames = drwav_read_pcm_frames_s32(wav, max_frames, decode_buf);
    return (int32_t)frames;
}

//--------------------------------------------------------------------+
// WAV seek
//--------------------------------------------------------------------+

static bool wav_seek(codec_handle_t *h, uint64_t frame_pos)
{
    drwav *wav = (drwav *)h->wav.drwav;
    if (!wav) return false;
    return drwav_seek_to_pcm_frame(wav, frame_pos) == DRWAV_TRUE;
}

//--------------------------------------------------------------------+
// WAV close
//--------------------------------------------------------------------+

static void wav_close(codec_handle_t *h)
{
    drwav *wav = (drwav *)h->wav.drwav;
    if (wav) {
        drwav_uninit(wav);
        free(wav);
        h->wav.drwav = NULL;
    }
}

//--------------------------------------------------------------------+
// vtable
//--------------------------------------------------------------------+

static const codec_vtable_t wav_vtable = {
    .decode = wav_decode,
    .seek   = wav_seek,
    .close  = wav_close,
};

//--------------------------------------------------------------------+
// WAV open: init dr_wav with custom I/O
//--------------------------------------------------------------------+

bool codec_wav_open(codec_handle_t *h)
{
    drwav *wav = calloc(1, sizeof(drwav));
    if (!wav) {
        ESP_LOGE(TAG, "Out of memory for drwav");
        return false;
    }

    if (!drwav_init(wav, wav_read_cb, wav_seek_cb, wav_tell_cb, h->file, NULL)) {
        ESP_LOGE(TAG, "drwav_init failed");
        free(wav);
        return false;
    }

    h->wav.drwav = wav;
    h->info.sample_rate = wav->sampleRate;
    h->info.bits_per_sample = wav->bitsPerSample;
    h->info.channels = wav->channels;
    h->info.total_frames = wav->totalPCMFrameCount;
    h->vt = &wav_vtable;

    const char *fmt_str = "PCM";
    if (wav->translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT) fmt_str = "Float";
    else if (wav->translatedFormatTag == DR_WAVE_FORMAT_ADPCM) fmt_str = "ADPCM";
    else if (wav->translatedFormatTag == DR_WAVE_FORMAT_DVI_ADPCM) fmt_str = "IMA-ADPCM";

    ESP_LOGI(TAG, "WAV: %luHz %d-bit(%s) %dch, %llu frames",
             h->info.sample_rate, h->info.bits_per_sample, fmt_str,
             h->info.channels, h->info.total_frames);

    return true;
}
