#include "audio_codecs.h"
#include "audio_codecs_internal.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <esp_log.h>

static const char *TAG = "codec";

//--------------------------------------------------------------------+
// Format detection
//--------------------------------------------------------------------+

codec_format_t codec_detect_format(const char *filepath)
{
    if (!filepath) return CODEC_FORMAT_UNKNOWN;

    const char *dot = strrchr(filepath, '.');
    if (!dot) return CODEC_FORMAT_UNKNOWN;

    // Case-insensitive extension check
    if (strcasecmp(dot, ".wav")  == 0) return CODEC_FORMAT_WAV;
    if (strcasecmp(dot, ".aif")  == 0) return CODEC_FORMAT_WAV;   /* AIFF via dr_wav */
    if (strcasecmp(dot, ".aiff") == 0) return CODEC_FORMAT_WAV;
    if (strcasecmp(dot, ".flac") == 0) return CODEC_FORMAT_FLAC;
    if (strcasecmp(dot, ".mp3")  == 0) return CODEC_FORMAT_MP3;
    if (strcasecmp(dot, ".dsf")  == 0) return CODEC_FORMAT_DSD;
    if (strcasecmp(dot, ".dff")  == 0) return CODEC_FORMAT_DSD;   /* DSDIFF container */
    if (strcasecmp(dot, ".aac")  == 0) return CODEC_FORMAT_AAC;   /* AAC-LC/HE-AAC ADTS */
    if (strcasecmp(dot, ".opus") == 0) return CODEC_FORMAT_OPUS;  /* Opus in Ogg */
    if (strcasecmp(dot, ".m4a")  == 0) return CODEC_FORMAT_M4A;   /* M4A (AAC or ALAC) */
    if (strcasecmp(dot, ".m4b")  == 0) return CODEC_FORMAT_M4A;   /* M4B audiobook */
    if (strcasecmp(dot, ".mp4")  == 0) return CODEC_FORMAT_M4A;   /* MP4 audio-only */

    return CODEC_FORMAT_UNKNOWN;
}

//--------------------------------------------------------------------+
// Open: allocate handle, detect format, dispatch to decoder
//--------------------------------------------------------------------+

codec_handle_t *codec_open(const char *filepath)
{
    if (!filepath) return NULL;

    codec_format_t fmt = codec_detect_format(filepath);
    if (fmt == CODEC_FORMAT_UNKNOWN) {
        ESP_LOGE(TAG, "Unsupported format: %s", filepath);
        return NULL;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", filepath);
        return NULL;
    }
    setvbuf(f, NULL, _IOFBF, 32768);  // 32KB read-ahead buffer for SD throughput

    codec_handle_t *h = calloc(1, sizeof(codec_handle_t));
    if (!h) {
        fclose(f);
        ESP_LOGE(TAG, "Out of memory for codec handle");
        return NULL;
    }

    h->file = f;
    h->info.format = fmt;

    bool ok = false;
    switch (fmt) {
        case CODEC_FORMAT_WAV:
            ok = codec_wav_open(h);
            break;
        case CODEC_FORMAT_FLAC:
            ok = codec_flac_open(h);
            break;
        case CODEC_FORMAT_MP3:
            ok = codec_mp3_open(h);
            break;
        case CODEC_FORMAT_DSD:
            ok = codec_dsd_open(h);
            break;
        case CODEC_FORMAT_AAC:
            ok = codec_aac_open(h);
            break;
        case CODEC_FORMAT_OPUS:
            ok = codec_opus_open(h);
            break;
        case CODEC_FORMAT_M4A:
            ok = codec_m4a_open(h);
            break;
        default:
            break;
    }

    if (!ok) {
        ESP_LOGE(TAG, "Decoder init failed: %s", filepath);
        fclose(f);
        free(h);
        return NULL;
    }

    // Calculate duration if total_frames is known
    if (h->info.total_frames > 0 && h->info.sample_rate > 0) {
        h->info.duration_ms = (uint32_t)((h->info.total_frames * 1000ULL) / h->info.sample_rate);
    }

    static const char *fmt_names[] = {
        "???", "WAV", "FLAC", "MP3", "DSD", "AAC", "Opus", "M4A", "ALAC"
    };
    /* Use the format stored in h->info — sub-openers (ALAC/AAC) may have updated it */
    uint32_t actual_fmt = (uint32_t)h->info.format;
    const char *fmt_name = (actual_fmt < sizeof(fmt_names)/sizeof(fmt_names[0]))
                         ? fmt_names[actual_fmt] : "???";
    ESP_LOGI(TAG, "Opened: %s [%s %luHz %d-bit %dch %llu frames %.1fs gain=%.2fdB]",
             filepath, fmt_name,
             h->info.sample_rate, h->info.bits_per_sample,
             h->info.channels, h->info.total_frames,
             h->info.duration_ms / 1000.0f,
             h->info.gain_db);

    return h;
}

//--------------------------------------------------------------------+
// Decode / Seek / Info / Close — dispatch to vtable
//--------------------------------------------------------------------+

int32_t codec_decode(codec_handle_t *h, int32_t *buffer, uint32_t max_frames)
{
    if (!h || !h->vt || !h->vt->decode) return -1;
    return h->vt->decode(h, buffer, max_frames);
}

bool codec_seek(codec_handle_t *h, uint64_t frame_pos)
{
    if (!h || !h->vt || !h->vt->seek) return false;
    return h->vt->seek(h, frame_pos);
}

const codec_info_t *codec_get_info(const codec_handle_t *h)
{
    if (!h) return NULL;
    return &h->info;
}

void codec_close(codec_handle_t *h)
{
    if (!h) return;
    if (h->vt && h->vt->close) {
        h->vt->close(h);
    }
    if (h->file) {
        fclose(h->file);
    }
    free(h);
}
