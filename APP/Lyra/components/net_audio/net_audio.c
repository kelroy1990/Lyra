#include "net_audio.h"
#include "http_stream.h"

// NOTE: DO NOT define DR_*_IMPLEMENTATION — already compiled in audio_codecs component.
// Including headers here for declarations only (callback function signatures).
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "net_audio";

//--------------------------------------------------------------------+
// Config
//--------------------------------------------------------------------+

#define NET_AUDIO_DECODE_FRAMES   1152      // frames per decode call (1 full MPEG1 L3 frame)
#define MP3_IN_BUF_SIZE           (32 * 1024)  // HTTP input ring buffer for MP3
#define NET_AUDIO_TASK_STACK      32768     // 32KB — dr_flac uses significant stack
#define NET_AUDIO_TASK_PRIO        5        // Same as sd_player and audio_task
#define NET_AUDIO_TASK_CPU         1        // Audio processing core

//--------------------------------------------------------------------+
// Commands
//--------------------------------------------------------------------+

typedef enum {
    NET_CMD_START,
    NET_CMD_STOP,
    NET_CMD_PAUSE,
    NET_CMD_RESUME,
} net_cmd_type_t;

typedef struct {
    net_cmd_type_t type;
    char url[512];
    char codec_hint[32];
    char referer[128];   // Optional Referer header (for CDNs that require it)
} net_cmd_t;

//--------------------------------------------------------------------+
// MP3 streaming context (minimp3 frame-by-frame, no seeking)
//--------------------------------------------------------------------+

typedef struct {
    drmp3dec  dec;       // minimp3 filter state (~50 bytes)
    uint8_t  *in_buf;    // HTTP input ring buffer (PSRAM, MP3_IN_BUF_SIZE)
    size_t    in_size;   // total allocated bytes
    size_t    in_off;    // start offset of valid data
    size_t    in_len;    // valid bytes from in_off
    int16_t  *pcm_buf;   // scratch PCM output (DRMP3_MAX_SAMPLES_PER_FRAME int16)
} mp3_stream_ctx_t;

//--------------------------------------------------------------------+
// Module state
//--------------------------------------------------------------------+

typedef struct {
    // Callbacks to audio system
    net_audio_audio_cbs_t audio;

    // Command queue
    QueueHandle_t cmd_queue;

    // Current state (updated by task, read by API)
    volatile net_audio_state_t state;

    // Current stream info (updated by task)
    net_audio_info_t info;

    // Active stream handle
    http_stream_t hs;

    // Active decoder handles (at most one non-NULL at a time)
    drflac          *flac;
    mp3_stream_ctx_t *mp3_ctx;
    drwav           *wav;

    // Diagnostics
    struct {
        uint32_t decode_max_us;
        uint32_t dsp_max_us;
        uint32_t backpressure_count;
        uint32_t stream_partial;
        uint32_t error_count;
        uint64_t total_frames;
        uint64_t last_log_time_us;
    } diag;
} net_audio_t;

static net_audio_t s_net = {0};
static TaskHandle_t s_task_handle = NULL;

//--------------------------------------------------------------------+
// dr_libs HTTP callbacks (shared across codec types)
//--------------------------------------------------------------------+

static size_t drlib_read_cb(void *userdata, void *buf, size_t n)
{
    return http_stream_read((http_stream_t *)userdata, buf, n);
}

static drflac_bool32 drflac_seek_cb(void *userdata, int offset, drflac_seek_origin origin)
{
    int whence = (origin == DRFLAC_SEEK_SET) ? SEEK_SET :
                 (origin == DRFLAC_SEEK_CUR) ? SEEK_CUR : SEEK_END;
    return http_stream_seek((http_stream_t *)userdata, offset, whence) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 drflac_tell_cb(void *userdata, drflac_int64 *pCursor)
{
    *pCursor = http_stream_tell((http_stream_t *)userdata);
    return DRFLAC_TRUE;
}

static drwav_bool32 drwav_seek_cb(void *userdata, int offset, drwav_seek_origin origin)
{
    int whence = (origin == DRWAV_SEEK_SET) ? SEEK_SET : SEEK_CUR;
    return http_stream_seek((http_stream_t *)userdata, offset, whence) ? DRWAV_TRUE : DRWAV_FALSE;
}

static drwav_bool32 drwav_tell_cb(void *userdata, drwav_int64 *pCursor)
{
    *pCursor = http_stream_tell((http_stream_t *)userdata);
    return DRWAV_TRUE;
}

//--------------------------------------------------------------------+
// Decoder open / close
//--------------------------------------------------------------------+

static bool open_decoder(void)
{
    http_stream_t *hs = &s_net.hs;

    switch (hs->codec) {
        case HTTP_CODEC_FLAC:
            s_net.flac = drflac_open(
                (drflac_read_proc)drlib_read_cb,
                drflac_seek_cb,
                drflac_tell_cb,
                hs, NULL);
            if (!s_net.flac) {
                ESP_LOGE(TAG, "drflac_open failed");
                return false;
            }
            s_net.info.sample_rate     = s_net.flac->sampleRate;
            s_net.info.bits_per_sample = 32;  // drflac_read_pcm_frames_s32 always outputs left-justified int32
            s_net.info.channels        = s_net.flac->channels;
            ESP_LOGI(TAG, "FLAC: %luHz %d-bit→32 %dch",
                     (unsigned long)s_net.info.sample_rate,
                     s_net.flac->bitsPerSample,
                     s_net.info.channels);
            return true;

        case HTTP_CODEC_MP3: {
            // Use minimp3 drmp3dec_decode_frame — never needs seeking.
            // drmp3_init (high-level) uses SEEK_SET for ID3v2 and fails on HTTP streams.
            mp3_stream_ctx_t *ctx = calloc(1, sizeof(mp3_stream_ctx_t));
            if (!ctx) { ESP_LOGE(TAG, "OOM mp3_ctx"); return false; }

            ctx->in_buf = heap_caps_malloc(MP3_IN_BUF_SIZE, MALLOC_CAP_SPIRAM);
            if (!ctx->in_buf) ctx->in_buf = malloc(MP3_IN_BUF_SIZE);
            if (!ctx->in_buf) {
                free(ctx);
                ESP_LOGE(TAG, "OOM mp3 in_buf");
                return false;
            }
            ctx->in_size = MP3_IN_BUF_SIZE;

            ctx->pcm_buf = malloc(DRMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
            if (!ctx->pcm_buf) {
                free(ctx->in_buf); free(ctx);
                ESP_LOGE(TAG, "OOM mp3 pcm_buf");
                return false;
            }

            drmp3dec_init(&ctx->dec);

            // Fill initial HTTP data
            size_t got = http_stream_read(hs, ctx->in_buf,
                                          ctx->in_size < 8192 ? ctx->in_size : 8192);
            ctx->in_len = got;
            ctx->in_off = 0;

            // Probe: decode frames until we find the first real audio frame,
            // extracting sample_rate and channels. Discard PCM from probe frames
            // (< 26ms loss is inaudible on a live stream).
            bool probed = false;
            for (int attempt = 0; attempt < 4096 && !probed; attempt++) {
                // Refill if buffer is low
                if (ctx->in_len < 4) {
                    if (ctx->in_off > 0) {
                        memmove(ctx->in_buf, ctx->in_buf + ctx->in_off, ctx->in_len);
                        ctx->in_off = 0;
                    }
                    size_t space = ctx->in_size - ctx->in_len;
                    size_t chunk = space < 4096 ? space : 4096;
                    got = http_stream_read(hs, ctx->in_buf + ctx->in_len, chunk);
                    if (got == 0) break;
                    ctx->in_len += got;
                }

                drmp3dec_frame_info info = {0};
                int samples = drmp3dec_decode_frame(
                    &ctx->dec,
                    ctx->in_buf + ctx->in_off, (int)ctx->in_len,
                    ctx->pcm_buf, &info);

                if (info.frame_bytes > 0) {
                    ctx->in_off += (size_t)info.frame_bytes;
                    ctx->in_len -= (size_t)info.frame_bytes;
                    if (samples > 0 && info.sample_rate > 0) {
                        s_net.info.sample_rate     = (uint32_t)info.sample_rate;
                        s_net.info.channels        = (uint8_t)info.channels;
                        s_net.info.bits_per_sample = 32;  // decode_block outputs int32 (signal left-justified in upper 16)
                        s_net.info.bitrate_kbps    = 0;
                        probed = true;
                        ESP_LOGI(TAG, "MP3: %dHz %dch (probe frame discarded)",
                                 info.sample_rate, info.channels);
                    }
                    // else: Xing/Info/ID3 frame, no audio — keep scanning
                } else {
                    // No sync at current offset — advance 1 byte
                    if (ctx->in_len > 0) { ctx->in_off++; ctx->in_len--; }
                }
            }

            if (!probed) {
                ESP_LOGE(TAG, "MP3: no valid frame found during probe");
                free(ctx->pcm_buf); free(ctx->in_buf); free(ctx);
                return false;
            }

            // Compact after probing
            if (ctx->in_off > 0) {
                memmove(ctx->in_buf, ctx->in_buf + ctx->in_off, ctx->in_len);
                ctx->in_off = 0;
            }
            s_net.mp3_ctx = ctx;
            return true;
        }

        case HTTP_CODEC_WAV:
            s_net.wav = calloc(1, sizeof(drwav));
            if (!s_net.wav) { ESP_LOGE(TAG, "OOM drwav"); return false; }
            if (!drwav_init(s_net.wav,
                            (drwav_read_proc)drlib_read_cb,
                            drwav_seek_cb,
                            drwav_tell_cb,
                            hs, NULL)) {
                ESP_LOGE(TAG, "drwav_init failed");
                free(s_net.wav);
                s_net.wav = NULL;
                return false;
            }
            s_net.info.sample_rate     = s_net.wav->sampleRate;
            s_net.info.bits_per_sample = 32;  // drwav_read_pcm_frames_s32 always outputs left-justified int32
            s_net.info.channels        = s_net.wav->channels;
            ESP_LOGI(TAG, "WAV: %luHz %d-bit→32 %dch",
                     (unsigned long)s_net.info.sample_rate,
                     s_net.wav->bitsPerSample,
                     s_net.info.channels);
            return true;

        case HTTP_CODEC_AAC:
            ESP_LOGW(TAG, "AAC codec not yet implemented — stub");
            return false;

        case HTTP_CODEC_OGG:
            ESP_LOGW(TAG, "Ogg/Vorbis codec not yet implemented — stub (F8-E)");
            return false;

        default:
            ESP_LOGE(TAG, "Unknown codec type %d, cannot decode", hs->codec);
            return false;
    }
}

static void close_decoder(void)
{
    if (s_net.flac) { drflac_close(s_net.flac); s_net.flac = NULL; }
    if (s_net.mp3_ctx) {
        free(s_net.mp3_ctx->pcm_buf);
        free(s_net.mp3_ctx->in_buf);
        free(s_net.mp3_ctx);
        s_net.mp3_ctx = NULL;
    }
    if (s_net.wav)  { drwav_uninit(s_net.wav); free(s_net.wav); s_net.wav = NULL; }
}

//--------------------------------------------------------------------+
// Decode one block → int32 stereo interleaved
// Returns frames decoded (0 = EOF, -1 = error)
//--------------------------------------------------------------------+

static int32_t decode_block(int32_t *buf, uint32_t max_frames)
{
    if (s_net.flac) {
        uint32_t ch = s_net.info.channels;
        if (ch == 1) {
            static drflac_int32 mono_buf[NET_AUDIO_DECODE_FRAMES];
            uint32_t clamped = max_frames > NET_AUDIO_DECODE_FRAMES ? NET_AUDIO_DECODE_FRAMES : max_frames;
            drflac_uint64 frames = drflac_read_pcm_frames_s32(s_net.flac, clamped, mono_buf);
            for (uint32_t i = 0; i < (uint32_t)frames; i++) {
                buf[i * 2]     = (int32_t)mono_buf[i];
                buf[i * 2 + 1] = (int32_t)mono_buf[i];
            }
            return (int32_t)frames;
        }
        drflac_int32 *dbuf = (drflac_int32 *)buf;
        drflac_uint64 frames = drflac_read_pcm_frames_s32(s_net.flac, max_frames, dbuf);
        return (int32_t)frames;
    }

    if (s_net.mp3_ctx) {
        mp3_stream_ctx_t *ctx = s_net.mp3_ctx;

        // Loop until we decode one valid audio frame or hit EOF/error.
        // Non-audio frames (Xing, ID3 in-stream, etc.) are consumed silently.
        while (1) {
            // Compact when offset > half of buffer to reclaim space
            if (ctx->in_off > ctx->in_size / 2) {
                memmove(ctx->in_buf, ctx->in_buf + ctx->in_off, ctx->in_len);
                ctx->in_off = 0;
            }

            // Refill from HTTP if buffer is low (blocks until data arrives)
            if (ctx->in_len < 4096) {
                size_t space = ctx->in_size - ctx->in_off - ctx->in_len;
                if (space == 0) {
                    // Buffer full with no valid frame — corrupt stream, skip some
                    ESP_LOGW(TAG, "MP3: in_buf full, no sync — discarding");
                    ctx->in_len /= 2;
                    ctx->in_off  = 0;
                    space = ctx->in_size / 2;
                }
                size_t to_read = space < 4096 ? space : 4096;
                size_t got = http_stream_read(&s_net.hs,
                    ctx->in_buf + ctx->in_off + ctx->in_len, to_read);
                if (got == 0) {
                    return s_net.hs.error ? -1 : 0;  // EOF or network error
                }
                ctx->in_len += got;
            }

            drmp3dec_frame_info info = {0};
            int samples = drmp3dec_decode_frame(
                &ctx->dec,
                ctx->in_buf + ctx->in_off, (int)ctx->in_len,
                ctx->pcm_buf, &info);

            if (info.frame_bytes > 0) {
                ctx->in_off += (size_t)info.frame_bytes;
                ctx->in_len -= (size_t)info.frame_bytes;

                if (samples <= 0) continue;  // Xing/Info/padding frame

                // Valid audio — convert int16 interleaved → int32 stereo
                uint32_t ch = (uint32_t)info.channels;
                uint32_t frames_out = (uint32_t)samples;
                if (frames_out > max_frames) frames_out = max_frames;

                for (uint32_t i = 0; i < frames_out; i++) {
                    if (ch == 1) {
                        int32_t s = (int32_t)ctx->pcm_buf[i] << 16;
                        buf[i * 2]     = s;
                        buf[i * 2 + 1] = s;
                    } else {
                        buf[i * 2]     = (int32_t)ctx->pcm_buf[i * 2]     << 16;
                        buf[i * 2 + 1] = (int32_t)ctx->pcm_buf[i * 2 + 1] << 16;
                    }
                }
                return (int32_t)frames_out;
            }
            // frame_bytes == 0: not enough data yet — refill loop will handle it
        }
    }

    if (s_net.wav) {
        uint32_t ch = s_net.info.channels;
        if (ch == 1) {
            static drwav_int32 mono_buf[NET_AUDIO_DECODE_FRAMES];
            uint32_t clamped = max_frames > NET_AUDIO_DECODE_FRAMES ? NET_AUDIO_DECODE_FRAMES : max_frames;
            drwav_uint64 frames = drwav_read_pcm_frames_s32(s_net.wav, clamped, mono_buf);
            for (uint32_t i = 0; i < (uint32_t)frames; i++) {
                buf[i * 2]     = (int32_t)mono_buf[i];
                buf[i * 2 + 1] = (int32_t)mono_buf[i];
            }
            return (int32_t)frames;
        }
        drwav_int32 *dbuf = (drwav_int32 *)buf;
        drwav_uint64 frames = drwav_read_pcm_frames_s32(s_net.wav, max_frames, dbuf);
        return (int32_t)frames;
    }

    return -1;  // No active decoder
}

//--------------------------------------------------------------------+
// Diagnostics logging
//--------------------------------------------------------------------+

static void log_diag(void)
{
    uint64_t now = esp_timer_get_time();
    if (now - s_net.diag.last_log_time_us < 5000000) return;  // every 5s
    s_net.diag.last_log_time_us = now;

    StreamBufferHandle_t stream = s_net.audio.get_stream_buffer();
    size_t buf_used = stream ? (16 * 1024 - xStreamBufferSpacesAvailable(stream)) : 0;

    ESP_LOGI(TAG, "[diag] state=%d frames=%llu dec_max=%luus dsp_max=%luus "
             "backpressure=%lu partial=%lu err=%lu streambuf=%uB",
             (int)s_net.state,
             s_net.diag.total_frames,
             (unsigned long)s_net.diag.decode_max_us,
             (unsigned long)s_net.diag.dsp_max_us,
             (unsigned long)s_net.diag.backpressure_count,
             (unsigned long)s_net.diag.stream_partial,
             (unsigned long)s_net.diag.error_count,
             (unsigned)buf_used);

    s_net.diag.decode_max_us = 0;
    s_net.diag.dsp_max_us    = 0;
}

//--------------------------------------------------------------------+
// Stream lifecycle helpers
//--------------------------------------------------------------------+

static void cleanup_stream(void)
{
    close_decoder();
    http_stream_close(&s_net.hs);
    memset(&s_net.info.icy_title, 0, sizeof(s_net.info.icy_title));
    s_net.info.sample_rate     = 0;
    s_net.info.bits_per_sample = 0;
    s_net.info.channels        = 0;
    s_net.info.elapsed_ms      = 0;
}

static bool start_stream(const char *url, const char *codec_hint, const char *referer)
{
    strncpy(s_net.info.url, url, sizeof(s_net.info.url) - 1);
    s_net.info.url[sizeof(s_net.info.url) - 1] = '\0';

    s_net.state = NET_AUDIO_CONNECTING;
    ESP_LOGI(TAG, "Connecting: %s", url);

    esp_err_t err = http_stream_open(&s_net.hs, url, codec_hint, referer);
    if (err != ESP_OK) {
        s_net.state = NET_AUDIO_ERROR;
        s_net.diag.error_count++;
        return false;
    }

    strncpy(s_net.info.content_type, s_net.hs.content_type, sizeof(s_net.info.content_type) - 1);
    strncpy(s_net.info.codec, http_codec_name(s_net.hs.codec), sizeof(s_net.info.codec) - 1);

    s_net.state = NET_AUDIO_BUFFERING;
    if (!open_decoder()) {
        http_stream_close(&s_net.hs);
        s_net.state = NET_AUDIO_ERROR;
        s_net.diag.error_count++;
        return false;
    }

    // Switch audio source to NET and reconfigure I2S for this stream's format
    s_net.audio.set_producer_handle(s_task_handle);
    s_net.audio.switch_source(s_net.audio.audio_source_net,
                               s_net.info.sample_rate,
                               s_net.info.bits_per_sample);

    s_net.state = NET_AUDIO_PLAYING;
    s_net.diag.total_frames = 0;
    s_net.diag.last_log_time_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Stream started: %s %luHz %d-bit %dch",
             s_net.info.codec,
             (unsigned long)s_net.info.sample_rate,
             s_net.info.bits_per_sample,
             s_net.info.channels);

    return true;
}

//--------------------------------------------------------------------+
// Pause/resume callbacks (registered with audio_source)
//--------------------------------------------------------------------+

static void net_pause_cb(void)
{
    // Called by audio_source when switching away from NET.
    // Transition to PAUSED — decoding loop will stop consuming.
    // HTTP connection stays open.
    if (s_net.state == NET_AUDIO_PLAYING) {
        s_net.state = NET_AUDIO_PAUSED;
        ESP_LOGI(TAG, "Paused (source switch)");
    }
}

static void net_resume_cb(void)
{
    // Called by audio_source when switching back to NET.
    if (s_net.state == NET_AUDIO_PAUSED) {
        s_net.state = NET_AUDIO_PLAYING;
        // Wake the task if it's sleeping in the pause loop
        if (s_task_handle) {
            xTaskNotifyGive(s_task_handle);
        }
        ESP_LOGI(TAG, "Resumed (source switch)");
    }
}

//--------------------------------------------------------------------+
// Main decode loop (runs while state == NET_AUDIO_PLAYING)
//--------------------------------------------------------------------+

// Decode buffer: 1024 stereo frames × 2ch × 4 bytes = 8KB
static int32_t s_decode_buf[NET_AUDIO_DECODE_FRAMES * 2];

static void run_decode_loop(void)
{
    uint64_t start_ms = esp_timer_get_time() / 1000;

    while (s_net.state == NET_AUDIO_PLAYING) {
        // Check for incoming commands (non-blocking)
        net_cmd_t cmd;
        if (xQueueReceive(s_net.cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
                case NET_CMD_STOP:
                    ESP_LOGI(TAG, "Stop command received during playback");
                    cleanup_stream();
                    s_net.audio.switch_source(s_net.audio.audio_source_none, 0, 0);
                    s_net.state = NET_AUDIO_IDLE;
                    return;

                case NET_CMD_PAUSE:
                    s_net.state = NET_AUDIO_PAUSED;
                    ESP_LOGI(TAG, "Paused");
                    return;  // exit decode loop; task will re-enter when resumed

                case NET_CMD_START:
                    // New URL while already playing — restart
                    ESP_LOGI(TAG, "New stream while playing, restarting");
                    cleanup_stream();
                    s_net.audio.switch_source(s_net.audio.audio_source_none, 0, 0);
                    s_net.state = NET_AUDIO_IDLE;
                    // Re-queue the start command for processing in main loop
                    xQueueSendToFront(s_net.cmd_queue, &cmd, 0);
                    return;

                default: break;
            }
        }

        // Backpressure: wait if stream buffer is almost full
        StreamBufferHandle_t stream = s_net.audio.get_stream_buffer();
        size_t frame_bytes = NET_AUDIO_DECODE_FRAMES * 2 * sizeof(int32_t);
        size_t space = xStreamBufferSpacesAvailable(stream);
        if (space < frame_bytes) {
            s_net.diag.backpressure_count++;
            // Wait for consumer (feeder task) to drain some bytes
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }

        // Decode one block
        uint64_t t0 = esp_timer_get_time();
        uint32_t max_frames = NET_AUDIO_DECODE_FRAMES;

        int32_t frames = decode_block(s_decode_buf, max_frames);
        uint64_t t1 = esp_timer_get_time();

        uint32_t dec_us = (uint32_t)(t1 - t0);
        if (dec_us > s_net.diag.decode_max_us) s_net.diag.decode_max_us = dec_us;

        if (frames <= 0) {
            if (frames < 0) {
                ESP_LOGE(TAG, "Decode error: %ld", (long)frames);
                s_net.diag.error_count++;
            } else {
                ESP_LOGI(TAG, "Stream EOF after %llu frames", s_net.diag.total_frames);
            }
            cleanup_stream();
            s_net.audio.switch_source(s_net.audio.audio_source_none, 0, 0);
            s_net.state = NET_AUDIO_IDLE;
            return;
        }

        s_net.diag.total_frames += frames;

        // Apply DSP chain
        t0 = esp_timer_get_time();
        s_net.audio.process_audio(s_decode_buf, (uint32_t)frames);
        t1 = esp_timer_get_time();
        uint32_t dsp_us = (uint32_t)(t1 - t0);
        if (dsp_us > s_net.diag.dsp_max_us) s_net.diag.dsp_max_us = dsp_us;

        // Write to StreamBuffer
        size_t byte_count = (size_t)frames * 2 * sizeof(int32_t);
        size_t sent = xStreamBufferSend(stream, s_decode_buf, byte_count, 0);
        if (sent < byte_count) {
            s_net.diag.stream_partial++;
        }

        // Update elapsed time
        uint32_t sr = s_net.info.sample_rate;
        if (sr > 0) {
            s_net.info.elapsed_ms = (uint32_t)((s_net.diag.total_frames * 1000ULL) / sr);
        }

        // Copy ICY title from HTTP stream (radio track changes)
        if (s_net.hs.icy_title[0]) {
            strncpy(s_net.info.icy_title, s_net.hs.icy_title,
                    sizeof(s_net.info.icy_title) - 1);
        }

        log_diag();
    }

    (void)start_ms;
}

//--------------------------------------------------------------------+
// Main task
//--------------------------------------------------------------------+

static void net_audio_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Task started on CPU%d", xPortGetCoreID());

    s_task_handle = xTaskGetCurrentTaskHandle();

    while (1) {
        net_cmd_t cmd;

        // IDLE: block until START command
        if (xQueueReceive(s_net.cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        if (cmd.type == NET_CMD_START) {
            if (!start_stream(cmd.url, cmd.codec_hint, cmd.referer)) {
                ESP_LOGE(TAG, "Failed to start stream");
                s_net.state = NET_AUDIO_IDLE;
                continue;
            }
            run_decode_loop();
            // After decode loop exits, handle PAUSED state
            while (s_net.state == NET_AUDIO_PAUSED) {
                // Sleep until resumed or stopped
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
                // Check for commands while paused
                if (xQueueReceive(s_net.cmd_queue, &cmd, 0) == pdTRUE) {
                    if (cmd.type == NET_CMD_STOP) {
                        cleanup_stream();
                        s_net.audio.switch_source(s_net.audio.audio_source_none, 0, 0);
                        s_net.state = NET_AUDIO_IDLE;
                    } else if (cmd.type == NET_CMD_RESUME) {
                        // Re-claim audio source before resuming.
                        // If paused by an external source steal (USB/SD called
                        // audio_source_switch which triggered net_pause_cb), the
                        // source is now USB/SD — we must switch it back to NET and
                        // re-configure I2S to the stream's format.
                        // If paused internally (NET_CMD_PAUSE, source already NET),
                        // audio_source_switch hits the same-source early-return → no-op.
                        s_net.audio.set_producer_handle(s_task_handle);
                        s_net.audio.switch_source(s_net.audio.audio_source_net,
                                                   s_net.info.sample_rate,
                                                   s_net.info.bits_per_sample);
                        s_net.state = NET_AUDIO_PLAYING;
                        run_decode_loop();
                    } else if (cmd.type == NET_CMD_START) {
                        // New stream while paused — restart
                        cleanup_stream();
                        if (start_stream(cmd.url, cmd.codec_hint, cmd.referer)) {
                            run_decode_loop();
                        } else {
                            s_net.state = NET_AUDIO_IDLE;
                        }
                    }
                }
            }
        }
        // Ignore STOP/PAUSE/RESUME while IDLE
    }
}

//--------------------------------------------------------------------+
// Public API
//--------------------------------------------------------------------+

void net_audio_init(const net_audio_audio_cbs_t *cbs)
{
    memcpy(&s_net.audio, cbs, sizeof(s_net.audio));

    s_net.cmd_queue = xQueueCreate(4, sizeof(net_cmd_t));
    assert(s_net.cmd_queue);

    s_net.state = NET_AUDIO_IDLE;
    memset(&s_net.info, 0, sizeof(s_net.info));
    memset(&s_net.diag, 0, sizeof(s_net.diag));

    // Register pause/resume callbacks with audio_source via injected function pointer.
    // This avoids a component→main include dependency.
    if (cbs->register_source_cbs) {
        cbs->register_source_cbs(net_pause_cb, net_resume_cb);
    }

    ESP_LOGI(TAG, "Initialized (FLAC/MP3/WAV streaming, AAC/Ogg stubs)");
}

void net_audio_start_task(void)
{
    xTaskCreatePinnedToCore(net_audio_task, "net_audio",
                            NET_AUDIO_TASK_STACK, NULL,
                            NET_AUDIO_TASK_PRIO, NULL,
                            NET_AUDIO_TASK_CPU);
}

esp_err_t net_audio_cmd_start(const char *url, const char *codec_hint,
                               const char *referer)
{
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    net_cmd_t cmd = {.type = NET_CMD_START};
    strncpy(cmd.url, url, sizeof(cmd.url) - 1);
    if (codec_hint) strncpy(cmd.codec_hint, codec_hint, sizeof(cmd.codec_hint) - 1);
    if (referer)    strncpy(cmd.referer, referer, sizeof(cmd.referer) - 1);
    return (xQueueSend(s_net.cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE)
           ? ESP_OK : ESP_ERR_TIMEOUT;
}

void net_audio_cmd_stop(void)
{
    net_cmd_t cmd = {.type = NET_CMD_STOP};
    xQueueSend(s_net.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

void net_audio_cmd_pause(void)
{
    net_cmd_t cmd = {.type = NET_CMD_PAUSE};
    xQueueSend(s_net.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

void net_audio_cmd_resume(void)
{
    net_cmd_t cmd = {.type = NET_CMD_RESUME};
    // Also wake the task
    if (s_task_handle) xTaskNotifyGive(s_task_handle);
    xQueueSend(s_net.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

net_audio_state_t net_audio_get_state(void)
{
    return s_net.state;
}

bool net_audio_is_active(void)
{
    net_audio_state_t st = s_net.state;
    return st == NET_AUDIO_CONNECTING ||
           st == NET_AUDIO_BUFFERING  ||
           st == NET_AUDIO_PLAYING    ||
           st == NET_AUDIO_PAUSED;
}

net_audio_info_t net_audio_get_info(void)
{
    // Snapshot (not fully atomic, but good enough for display purposes)
    net_audio_info_t info;
    memcpy(&info, &s_net.info, sizeof(info));
    return info;
}
