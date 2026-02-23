#include "sd_player.h"
#include "audio_codecs.h"
#include "cue_parser.h"
#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"

static const char *TAG = "sd_player";

/* Returns a human-readable format name, e.g. "FLAC", "DSD64", "DSD128", "DSD256" */
static const char *format_name(const codec_info_t *info)
{
    if (info->is_dsd) {
        if (info->sample_rate <= 176400) return "DSD64";
        if (info->sample_rate <= 352800) return "DSD128";
        return "DSD256";
    }
    switch (info->format) {
        case CODEC_FORMAT_MP3:  return "MP3";
        case CODEC_FORMAT_WAV:  return "WAV";
        case CODEC_FORMAT_FLAC: return "FLAC";
        case CODEC_FORMAT_AAC:  return "AAC";
        case CODEC_FORMAT_OPUS: return "Opus";
        case CODEC_FORMAT_ALAC: return "ALAC";
        case CODEC_FORMAT_M4A:  return "M4A";  /* dispatcher, should not normally appear */
        default:                return "???";
    }
}

//--------------------------------------------------------------------+
// Command types for the player queue
//--------------------------------------------------------------------+

typedef enum {
    PLAYER_CMD_PLAY,
    PLAYER_CMD_PAUSE,
    PLAYER_CMD_RESUME,
    PLAYER_CMD_STOP,
    PLAYER_CMD_NEXT,
    PLAYER_CMD_PREV,
    PLAYER_CMD_SEEK,
} player_cmd_type_t;

typedef struct {
    player_cmd_type_t type;
    union {
        char filepath[160];
        uint32_t seek_seconds;
    };
} player_cmd_t;

//--------------------------------------------------------------------+
// Diagnostics (mirrors USB audio diag style)
//--------------------------------------------------------------------+

static volatile struct {
    uint32_t decode_max_us;
    uint32_t dsp_max_us;
    uint32_t loop_max_us;
    uint32_t decode_count;
    uint32_t backpressure_count;  // times stream buffer was full
    uint32_t stream_min;
    uint32_t stream_max;
    uint32_t stream_partial;     // partial writes to stream buffer
    uint32_t active_us;          // total time in decode+dsp+write
} s_sd_diag;

//--------------------------------------------------------------------+
// Player state
//--------------------------------------------------------------------+

static struct {
    volatile player_state_t state;
    codec_handle_t *codec;
    codec_info_t    current_info;
    char            current_file[160];
    volatile uint64_t frames_decoded;

    // Playlist (folder mode)
    char            folder_path[160];
    char           *track_names[PLAYLIST_MAX_TRACKS];
    int             track_count;
    int             track_index;

    // CUE sheet (NULL when playing standalone files)
    cue_sheet_t    *cue;
    int             cue_track_index;   // current CUE track (0-based)

    // IPC
    QueueHandle_t   cmd_queue;
    TaskHandle_t    task_handle;

    // Callbacks
    player_output_fn output;
    sd_player_audio_cbs_t audio;
} s_player;

//--------------------------------------------------------------------+
// Internal helpers
//--------------------------------------------------------------------+

static void player_close_current(void)
{
    if (s_player.codec) {
        codec_close(s_player.codec);
        s_player.codec = NULL;
    }
    if (s_player.cue) {
        free(s_player.cue);
        s_player.cue = NULL;
    }
    s_player.cue_track_index = 0;
    s_player.frames_decoded = 0;
}

static bool player_open_file(const char *filepath)
{
    player_close_current();

    s_player.codec = codec_open(filepath);
    if (!s_player.codec) {
        ESP_LOGE(TAG, "Failed to open: %s", filepath);
        return false;
    }

    const codec_info_t *info = codec_get_info(s_player.codec);
    s_player.current_info = *info;
    strncpy(s_player.current_file, filepath, sizeof(s_player.current_file) - 1);
    s_player.frames_decoded = 0;

    return true;
}

static void player_scan_folder(const char *filepath)
{
    const char *last_slash = strrchr(filepath, '/');
    if (!last_slash) return;

    size_t folder_len = last_slash - filepath;
    if (folder_len >= sizeof(s_player.folder_path)) return;

    sd_playlist_free(s_player.track_names, s_player.track_count);
    s_player.track_count = 0;
    s_player.track_index = -1;

    memcpy(s_player.folder_path, filepath, folder_len);
    s_player.folder_path[folder_len] = '\0';

    s_player.track_count = sd_playlist_scan(
        s_player.folder_path, s_player.track_names, PLAYLIST_MAX_TRACKS);
    sd_playlist_sort(s_player.track_names, s_player.track_count);

    const char *filename = last_slash + 1;
    for (int i = 0; i < s_player.track_count; i++) {
        if (strcasecmp(s_player.track_names[i], filename) == 0) {
            s_player.track_index = i;
            break;
        }
    }

    ESP_LOGI(TAG, "Playlist: %d tracks in %s, current=%d",
             s_player.track_count, s_player.folder_path, s_player.track_index);
}

static void player_build_track_path(int index, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", s_player.folder_path, s_player.track_names[index]);
}

// Try to load a CUE sheet for the given audio file or .cue path.
// If filepath ends with .cue, parse it directly.
// If filepath is an audio file, look for a matching .cue in the same folder.
// Returns true if CUE mode is active after this call.
static bool player_try_load_cue(const char *filepath)
{
    char cue_path[320];
    bool is_cue_file = false;

    const char *dot = strrchr(filepath, '.');
    if (dot && strcasecmp(dot, ".cue") == 0) {
        is_cue_file = true;
        strncpy(cue_path, filepath, sizeof(cue_path) - 1);
        cue_path[sizeof(cue_path) - 1] = '\0';
    } else {
        // Try matching .cue: /sdcard/album.flac → /sdcard/album.cue
        if (!dot) return false;
        size_t base_len = dot - filepath;
        if (base_len + 5 >= sizeof(cue_path)) return false;
        memcpy(cue_path, filepath, base_len);
        strcpy(cue_path + base_len, ".cue");
    }

    // We need sample_rate to convert CUE timestamps, but if playing a .cue
    // file we don't have it yet. Parse with rate=0 first to get the audio filename,
    // then open the audio to get sample_rate, then re-parse.
    cue_sheet_t temp_cue;
    if (!cue_parse(cue_path, 44100, &temp_cue)) return false;  // probe parse

    // Build audio file path from CUE's FILE directive
    char audio_path[320];
    if (is_cue_file) {
        const char *last_slash = strrchr(cue_path, '/');
        if (last_slash) {
            size_t dir_len = last_slash - cue_path + 1;
            if (dir_len + strlen(temp_cue.file) >= sizeof(audio_path)) return false;
            memcpy(audio_path, cue_path, dir_len);
            strcpy(audio_path + dir_len, temp_cue.file);
        } else {
            strncpy(audio_path, temp_cue.file, sizeof(audio_path) - 1);
        }
    } else {
        // Audio file was specified directly — use it
        strncpy(audio_path, filepath, sizeof(audio_path) - 1);
        audio_path[sizeof(audio_path) - 1] = '\0';
    }

    // Open the audio file to get the real sample rate
    if (!player_open_file(audio_path)) return false;

    // Re-parse with correct sample rate
    s_player.cue = malloc(sizeof(cue_sheet_t));
    if (!s_player.cue) {
        ESP_LOGE(TAG, "No memory for CUE sheet");
        return false;
    }
    if (!cue_parse(cue_path, s_player.current_info.sample_rate, s_player.cue)) {
        free(s_player.cue);
        s_player.cue = NULL;
        return false;
    }

    s_player.cue_track_index = 0;
    s_player.track_count = s_player.cue->track_count;
    s_player.track_index = 0;

    ESP_LOGI(TAG, "CUE mode: '%s' — %d tracks", s_player.cue->title, s_player.cue->track_count);
    return true;
}

static void player_start_playback(const char *filepath)
{
    // Try CUE mode first (.cue file or auto-detect matching .cue)
    bool cue_mode = player_try_load_cue(filepath);

    if (!cue_mode) {
        // Normal file mode
        if (!player_open_file(filepath)) {
            s_player.state = PLAYER_STATE_IDLE;
            return;
        }
        player_scan_folder(filepath);
    }

    // Switch audio source to SD with the file's format (always 32-bit I2S)
    ESP_LOGI(TAG, "[PLAY] Starting: %luHz %d-bit %dch → switch to SD source",
             s_player.current_info.sample_rate, s_player.current_info.bits_per_sample,
             s_player.current_info.channels);
    s_player.audio.set_producer_handle(s_player.task_handle);
    s_player.audio.switch_source(SD_AUDIO_SOURCE_SD,
                                 s_player.current_info.sample_rate, 32);

    s_player.state = PLAYER_STATE_PLAYING;

    // Reset diagnostics on new track
    memset((void *)&s_sd_diag, 0, sizeof(s_sd_diag));
    s_sd_diag.stream_min = UINT32_MAX;

    if (s_player.output) {
        if (cue_mode) {
            cue_track_t *t = &s_player.cue->tracks[0];
            s_player.output("Playing: %s [%s %luHz %d-bit]\r\n",
                s_player.cue->title[0] ? s_player.cue->title : s_player.current_file,
                format_name(&s_player.current_info),
                s_player.current_info.sample_rate,
                s_player.current_info.bits_per_sample);
            s_player.output("  Track 1/%d: %s\r\n",
                s_player.cue->track_count,
                t->title[0] ? t->title : "(untitled)");
        } else {
            s_player.output("Playing: %s [%s %luHz %d-bit %s]\r\n",
                s_player.current_file,
                format_name(&s_player.current_info),
                s_player.current_info.sample_rate,
                s_player.current_info.bits_per_sample,
                s_player.current_info.channels == 1 ? "mono" : "stereo");
        }
    }
}

static void player_stop(void)
{
    player_close_current();  // also frees CUE
    s_player.state = PLAYER_STATE_IDLE;

    // Switch back to USB audio source
    s_player.audio.switch_source(SD_AUDIO_SOURCE_USB, 0, 0);

    if (s_player.output) {
        s_player.output("Playback stopped\r\n");
    }
}

// Helper: get elapsed ms within the current CUE track
static uint32_t cue_track_elapsed_ms(void)
{
    if (!s_player.cue || s_player.current_info.sample_rate == 0) return 0;
    int idx = s_player.cue_track_index;
    uint64_t track_start = s_player.cue->tracks[idx].start_frame;
    uint64_t within_track = (s_player.frames_decoded > track_start)
                          ? s_player.frames_decoded - track_start : 0;
    return (uint32_t)(within_track * 1000ULL / s_player.current_info.sample_rate);
}

static void player_advance_track(bool forward)
{
    // ── CUE mode: seek within the single audio file ──
    if (s_player.cue) {
        if (forward) {
            s_player.cue_track_index++;
            if (s_player.cue_track_index >= s_player.cue->track_count) {
                ESP_LOGI(TAG, "CUE: end of album");
                player_stop();
                return;
            }
        } else {
            // If >3s into track, restart current; else go to previous
            if (cue_track_elapsed_ms() > 3000) {
                // Restart current track
            } else {
                s_player.cue_track_index--;
                if (s_player.cue_track_index < 0) s_player.cue_track_index = 0;
            }
        }

        uint64_t target = s_player.cue->tracks[s_player.cue_track_index].start_frame;
        if (codec_seek(s_player.codec, target)) {
            s_player.frames_decoded = target;
            StreamBufferHandle_t stream = s_player.audio.get_stream_buffer();
            if (stream) xStreamBufferReset(stream);
        }
        s_player.track_index = s_player.cue_track_index;

        // Reset diagnostics on track change
        memset((void *)&s_sd_diag, 0, sizeof(s_sd_diag));
        s_sd_diag.stream_min = UINT32_MAX;

        cue_track_t *t = &s_player.cue->tracks[s_player.cue_track_index];
        if (s_player.output) {
            s_player.output("Track %d/%d: %s\r\n",
                s_player.cue_track_index + 1, s_player.cue->track_count,
                t->title[0] ? t->title : "(untitled)");
        }
        return;
    }

    // ── Folder mode: close current file, open next ──
    if (s_player.track_count == 0) {
        player_stop();
        return;
    }

    if (forward) {
        s_player.track_index++;
        if (s_player.track_index >= s_player.track_count) {
            ESP_LOGI(TAG, "End of playlist");
            player_stop();
            return;
        }
    } else {
        uint32_t elapsed_ms = 0;
        if (s_player.current_info.sample_rate > 0) {
            elapsed_ms = (uint32_t)((s_player.frames_decoded * 1000ULL) /
                                     s_player.current_info.sample_rate);
        }
        if (elapsed_ms > 3000 && s_player.track_index >= 0) {
            codec_seek(s_player.codec, 0);
            s_player.frames_decoded = 0;
            if (s_player.output) {
                s_player.output("Restarting track\r\n");
            }
            return;
        }
        s_player.track_index--;
        if (s_player.track_index < 0) {
            s_player.track_index = 0;
        }
    }

    player_close_current();

    char path[320];
    player_build_track_path(s_player.track_index, path, sizeof(path));

    if (!player_open_file(path)) {
        player_stop();
        return;
    }

    // Reconfigure I2S if sample rate changed
    ESP_LOGI(TAG, "[TRACK] New format: %luHz %d-bit %dch → requesting I2S reconfig",
             s_player.current_info.sample_rate, s_player.current_info.bits_per_sample,
             s_player.current_info.channels);
    s_player.audio.switch_source(SD_AUDIO_SOURCE_SD,
                                 s_player.current_info.sample_rate, 32);

    s_player.state = PLAYER_STATE_PLAYING;

    // Reset diagnostics on track change
    memset((void *)&s_sd_diag, 0, sizeof(s_sd_diag));
    s_sd_diag.stream_min = UINT32_MAX;

    if (s_player.output) {
        s_player.output("Track %d/%d: %s\r\n",
            s_player.track_index + 1, s_player.track_count,
            s_player.track_names[s_player.track_index]);
    }
}

//--------------------------------------------------------------------+
// Command processing
//--------------------------------------------------------------------+

static void player_process_commands(void)
{
    player_cmd_t cmd;
    while (xQueueReceive(s_player.cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
            case PLAYER_CMD_PLAY:
                player_start_playback(cmd.filepath);
                break;

            case PLAYER_CMD_PAUSE:
                if (s_player.state == PLAYER_STATE_PLAYING) {
                    s_player.state = PLAYER_STATE_PAUSED;
                    if (s_player.output) s_player.output("Paused\r\n");
                }
                break;

            case PLAYER_CMD_RESUME:
                if (s_player.state == PLAYER_STATE_PAUSED) {
                    s_player.state = PLAYER_STATE_PLAYING;
                    if (s_player.output) s_player.output("Resumed\r\n");
                }
                break;

            case PLAYER_CMD_STOP:
                player_stop();
                break;

            case PLAYER_CMD_NEXT:
                player_advance_track(true);
                break;

            case PLAYER_CMD_PREV:
                player_advance_track(false);
                break;

            case PLAYER_CMD_SEEK: {
                if (!s_player.codec) break;
                uint64_t target_frame;
                if (s_player.cue) {
                    // Seek within current CUE track
                    uint64_t track_start = s_player.cue->tracks[s_player.cue_track_index].start_frame;
                    target_frame = track_start + (uint64_t)cmd.seek_seconds *
                                   s_player.current_info.sample_rate;
                    // Clamp to next track boundary or file end
                    uint64_t limit = s_player.current_info.total_frames;
                    if (s_player.cue_track_index + 1 < s_player.cue->track_count) {
                        limit = s_player.cue->tracks[s_player.cue_track_index + 1].start_frame;
                    }
                    if (target_frame > limit) target_frame = limit;
                } else {
                    target_frame = (uint64_t)cmd.seek_seconds *
                                   s_player.current_info.sample_rate;
                    if (target_frame > s_player.current_info.total_frames) {
                        target_frame = s_player.current_info.total_frames;
                    }
                }
                if (codec_seek(s_player.codec, target_frame)) {
                    s_player.frames_decoded = target_frame;
                    StreamBufferHandle_t stream = s_player.audio.get_stream_buffer();
                    if (stream) xStreamBufferReset(stream);
                    if (s_player.output) {
                        s_player.output("Seek to %lus\r\n", cmd.seek_seconds);
                    }
                } else {
                    if (s_player.output) s_player.output("Seek failed\r\n");
                }
                break;
            }
        }
    }
}

//--------------------------------------------------------------------+
// Player task
//--------------------------------------------------------------------+

static void sd_player_task(void *arg)
{
    (void)arg;
    static int32_t decode_buf[1024 * 2];  // 1024 stereo frames = 8192 bytes (static to save stack)
    uint32_t last_diag_us = 0;

    while (1) {
        // Sleep when not active
        if (s_player.audio.get_source() != SD_AUDIO_SOURCE_SD ||
            s_player.state == PLAYER_STATE_IDLE) {
            player_process_commands();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        player_process_commands();

        if (s_player.state == PLAYER_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check stream buffer space before decoding
        StreamBufferHandle_t stream = s_player.audio.get_stream_buffer();
        size_t space = xStreamBufferSpacesAvailable(stream);
        if (space < sizeof(decode_buf)) {
            s_sd_diag.backpressure_count++;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t t_loop = (uint32_t)esp_timer_get_time();

        // Decode
        uint32_t t_decode = t_loop;
        int32_t frames = codec_decode(s_player.codec, decode_buf, 1024);
        uint32_t decode_us = (uint32_t)esp_timer_get_time() - t_decode;

        if (frames <= 0) {
            if (frames == 0) {
                if (s_player.cue) {
                    // In CUE mode, EOF = end of the entire audio file → stop
                    ESP_LOGI(TAG, "CUE: audio file finished");
                    player_stop();
                } else {
                    ESP_LOGI(TAG, "Track finished, advancing...");
                    player_advance_track(true);
                }
            } else {
                ESP_LOGE(TAG, "Decode error, stopping");
                player_stop();
            }
            continue;
        }

        /* Apply ReplayGain (PCM only, not DSD/DoP) */
        if (!s_player.current_info.is_dsd && s_player.current_info.gain_db != 0.0f) {
            /*
             * Fixed-point Q16 gain: precompute once per gain_db value.
             * gain_q16 = 10^(gain_db/20) * 65536
             * Multiply each sample by gain_q16 using 64-bit intermediate.
             */
            static float   s_last_gain_db = 0.0f;
            static int32_t s_gain_q16     = 65536;   /* 1.0 in Q16 */
            if (s_player.current_info.gain_db != s_last_gain_db) {
                s_last_gain_db = s_player.current_info.gain_db;
                s_gain_q16 = (int32_t)(powf(10.0f, s_last_gain_db / 20.0f) * 65536.0f);
            }
            int32_t *p = decode_buf;
            int32_t  gq = s_gain_q16;
            for (int32_t i = 0; i < frames * 2; i++) {
                int64_t s = ((int64_t)p[i] * gq) >> 16;
                if      (s >  INT32_MAX) s =  INT32_MAX;
                else if (s <  INT32_MIN) s =  INT32_MIN;
                p[i] = (int32_t)s;
            }
        }

        /* Apply DSP — bypassed for DSD (DoP frames cannot be processed by biquad EQ) */
        uint32_t t_dsp = (uint32_t)esp_timer_get_time();
        if (!s_player.current_info.is_dsd) {
            s_player.audio.process_audio(decode_buf, (uint32_t)frames);
        }
        uint32_t dsp_us = (uint32_t)esp_timer_get_time() - t_dsp;

        // Write to stream buffer
        uint32_t bytes = (uint32_t)frames * 2 * sizeof(int32_t);
        size_t sent = xStreamBufferSend(stream, decode_buf, bytes, 0);
        if (sent < bytes) {
            s_sd_diag.stream_partial++;
        }

        uint32_t loop_us = (uint32_t)esp_timer_get_time() - t_loop;

        // Update diagnostics
        if (decode_us > s_sd_diag.decode_max_us) s_sd_diag.decode_max_us = decode_us;
        if (dsp_us > s_sd_diag.dsp_max_us) s_sd_diag.dsp_max_us = dsp_us;
        if (loop_us > s_sd_diag.loop_max_us) s_sd_diag.loop_max_us = loop_us;
        s_sd_diag.decode_count++;
        s_sd_diag.active_us += loop_us;

        // Track stream buffer fill level
        size_t stream_used = xStreamBufferBytesAvailable(stream);
        if (stream_used < s_sd_diag.stream_min) s_sd_diag.stream_min = (uint32_t)stream_used;
        if (stream_used > s_sd_diag.stream_max) s_sd_diag.stream_max = (uint32_t)stream_used;

        s_player.frames_decoded += frames;

        // CUE: gapless track boundary detection (audio keeps flowing!)
        if (s_player.cue) {
            int ci = s_player.cue_track_index;
            if (ci + 1 < s_player.cue->track_count) {
                uint64_t next_start = s_player.cue->tracks[ci + 1].start_frame;
                if (s_player.frames_decoded >= next_start) {
                    s_player.cue_track_index++;
                    s_player.track_index = s_player.cue_track_index;
                    cue_track_t *t = &s_player.cue->tracks[s_player.cue_track_index];
                    ESP_LOGI(TAG, "CUE: → Track %d/%d: %s",
                             s_player.cue_track_index + 1, s_player.cue->track_count,
                             t->title[0] ? t->title : "(untitled)");
                    if (s_player.output) {
                        s_player.output("→ Track %d/%d: %s\r\n",
                            s_player.cue_track_index + 1, s_player.cue->track_count,
                            t->title[0] ? t->title : "(untitled)");
                    }
                }
            }
        }

        // Diagnostics log every 2 seconds
        uint32_t now_us = (uint32_t)esp_timer_get_time();
        if (now_us - last_diag_us >= 2000000) {
            if (s_sd_diag.decode_count > 0) {
                // Calculate CPU load: active_us / 2000000 * 100
                uint32_t cpu_pct = s_sd_diag.active_us / 20000;  // /2000000*100

                // Position info
                uint32_t elapsed_s = 0;
                uint32_t duration_s = s_player.current_info.duration_ms / 1000;
                if (s_player.current_info.sample_rate > 0) {
                    elapsed_s = (uint32_t)(s_player.frames_decoded / s_player.current_info.sample_rate);
                }

                ESP_LOGI(TAG, "[SD DIAG] dec=%luus dsp=%luus loop=%luus | "
                              "stream min=%lu max=%lu bp=%lu part=%lu | "
                              "blocks=%lu cpu=%lu%% | pos=%lu/%lus",
                         s_sd_diag.decode_max_us,
                         s_sd_diag.dsp_max_us,
                         s_sd_diag.loop_max_us,
                         (s_sd_diag.stream_min == UINT32_MAX) ? 0 : s_sd_diag.stream_min,
                         s_sd_diag.stream_max,
                         s_sd_diag.backpressure_count,
                         s_sd_diag.stream_partial,
                         s_sd_diag.decode_count,
                         cpu_pct,
                         elapsed_s, duration_s);
            }
            // Reset counters
            s_sd_diag.decode_max_us = 0;
            s_sd_diag.dsp_max_us = 0;
            s_sd_diag.loop_max_us = 0;
            s_sd_diag.decode_count = 0;
            s_sd_diag.backpressure_count = 0;
            s_sd_diag.stream_partial = 0;
            s_sd_diag.stream_min = UINT32_MAX;
            s_sd_diag.stream_max = 0;
            s_sd_diag.active_us = 0;
            last_diag_us = now_us;
        }
    }
}

//--------------------------------------------------------------------+
// Public API: init and task creation
//--------------------------------------------------------------------+

void sd_player_init(player_output_fn output_fn, const sd_player_audio_cbs_t *audio_cbs)
{
    memset(&s_player, 0, sizeof(s_player));
    s_player.state = PLAYER_STATE_IDLE;
    s_player.track_index = -1;
    s_player.output = output_fn;
    s_player.audio = *audio_cbs;
    s_player.cmd_queue = xQueueCreate(4, sizeof(player_cmd_t));
    assert(s_player.cmd_queue);

    ESP_LOGI(TAG, "SD Player initialized");
}

void sd_player_start_task(void)
{
    xTaskCreatePinnedToCore(sd_player_task, "sd_play", 32768, NULL, 5,
                            &s_player.task_handle, 1);
    ESP_LOGI(TAG, "SD Player task started (CPU1, prio 5)");
}

//--------------------------------------------------------------------+
// Public API: commands
//--------------------------------------------------------------------+

static void send_cmd(player_cmd_type_t type)
{
    player_cmd_t cmd = { .type = type };
    xQueueSend(s_player.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

void sd_player_cmd_play(const char *path)
{
    player_cmd_t cmd = { .type = PLAYER_CMD_PLAY };

    if (path[0] == '/') {
        strncpy(cmd.filepath, path, sizeof(cmd.filepath) - 1);
    } else {
        snprintf(cmd.filepath, sizeof(cmd.filepath), "%s/%s", STORAGE_MOUNT_POINT, path);
    }

    xQueueSend(s_player.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

void sd_player_cmd_pause(void)  { send_cmd(PLAYER_CMD_PAUSE); }
void sd_player_cmd_resume(void) { send_cmd(PLAYER_CMD_RESUME); }
void sd_player_cmd_stop(void)   { send_cmd(PLAYER_CMD_STOP); }
void sd_player_cmd_next(void)   { send_cmd(PLAYER_CMD_NEXT); }
void sd_player_cmd_prev(void)   { send_cmd(PLAYER_CMD_PREV); }

void sd_player_cmd_seek(uint32_t seconds)
{
    player_cmd_t cmd = { .type = PLAYER_CMD_SEEK, .seek_seconds = seconds };
    xQueueSend(s_player.cmd_queue, &cmd, pdMS_TO_TICKS(100));
}

//--------------------------------------------------------------------+
// Public API: status queries
//--------------------------------------------------------------------+

player_status_t sd_player_get_status(void)
{
    player_status_t st = {0};
    st.state = s_player.state;
    strncpy(st.current_file, s_player.current_file, sizeof(st.current_file) - 1);
    st.file_info = s_player.current_info;
    st.current_frame = s_player.frames_decoded;

    if (s_player.cue && s_player.current_info.sample_rate > 0) {
        // CUE mode: times relative to current CUE track
        int ci = s_player.cue_track_index;
        uint64_t track_start = s_player.cue->tracks[ci].start_frame;
        uint64_t track_end;
        if (ci + 1 < s_player.cue->track_count) {
            track_end = s_player.cue->tracks[ci + 1].start_frame;
        } else {
            track_end = s_player.current_info.total_frames;
        }

        uint64_t within = (s_player.frames_decoded > track_start)
                         ? s_player.frames_decoded - track_start : 0;
        uint64_t track_len = (track_end > track_start) ? track_end - track_start : 0;

        st.elapsed_ms = (uint32_t)(within * 1000ULL / s_player.current_info.sample_rate);
        uint32_t duration_ms = (uint32_t)(track_len * 1000ULL / s_player.current_info.sample_rate);
        st.remaining_ms = (duration_ms > st.elapsed_ms) ? duration_ms - st.elapsed_ms : 0;

        st.track_index = s_player.cue_track_index;
        st.track_count = s_player.cue->track_count;
    } else {
        st.track_index = s_player.track_index;
        st.track_count = s_player.track_count;

        if (s_player.current_info.sample_rate > 0) {
            st.elapsed_ms = (uint32_t)((s_player.frames_decoded * 1000ULL) /
                                        s_player.current_info.sample_rate);
            if (s_player.current_info.duration_ms > st.elapsed_ms) {
                st.remaining_ms = s_player.current_info.duration_ms - st.elapsed_ms;
            }
        }
    }

    return st;
}

bool sd_player_is_active(void)
{
    return s_player.state != PLAYER_STATE_IDLE;
}

//--------------------------------------------------------------------+
// Public API: info commands
//--------------------------------------------------------------------+

void sd_player_cmd_track_info(void)
{
    if (!s_player.output) return;

    if (s_player.state == PLAYER_STATE_IDLE) {
        s_player.output("No track playing\r\n");
        return;
    }

    player_status_t st = sd_player_get_status();
    const char *state_str = (st.state == PLAYER_STATE_PLAYING) ? "PLAYING" :
                            (st.state == PLAYER_STATE_PAUSED) ? "PAUSED" : "IDLE";

    if (s_player.cue) {
        cue_track_t *t = &s_player.cue->tracks[s_player.cue_track_index];
        s_player.output("[%s] %s\r\n", state_str,
            s_player.cue->title[0] ? s_player.cue->title : st.current_file);
        if (s_player.cue->performer[0]) {
            s_player.output("  Artist: %s\r\n", s_player.cue->performer);
        }
        s_player.output("  Track %d/%d: %s\r\n",
            st.track_index + 1, st.track_count,
            t->title[0] ? t->title : "(untitled)");
        if (t->performer[0] && strcmp(t->performer, s_player.cue->performer) != 0) {
            s_player.output("  Track artist: %s\r\n", t->performer);
        }
        s_player.output("  Format: %s %luHz %d-bit\r\n",
            format_name(&st.file_info),
            st.file_info.sample_rate, st.file_info.bits_per_sample);
    } else {
        s_player.output("[%s] %s\r\n", state_str, st.current_file);
        s_player.output("  Format: %s %luHz %d-bit %s\r\n",
            format_name(&st.file_info),
            st.file_info.sample_rate, st.file_info.bits_per_sample,
            st.file_info.channels == 1 ? "mono" : "stereo");
    }

    uint32_t el_s = st.elapsed_ms / 1000;
    uint32_t el_m = el_s / 60; el_s %= 60;
    uint32_t rem_s = (st.elapsed_ms + st.remaining_ms) / 1000;
    uint32_t rem_m = rem_s / 60; rem_s %= 60;

    s_player.output("  Position: %lu:%02lu / %lu:%02lu\r\n", el_m, el_s, rem_m, rem_s);
    if (!s_player.cue) {
        s_player.output("  Track: %d/%d\r\n", st.track_index + 1, st.track_count);
    }
}

void sd_player_cmd_playlist_info(void)
{
    if (!s_player.output) return;

    if (s_player.cue) {
        // CUE mode: show track titles from CUE sheet
        s_player.output("Album: %s (%d tracks)\r\n",
            s_player.cue->title[0] ? s_player.cue->title : s_player.current_file,
            s_player.cue->track_count);
        if (s_player.cue->performer[0]) {
            s_player.output("Artist: %s\r\n", s_player.cue->performer);
        }
        for (int i = 0; i < s_player.cue->track_count; i++) {
            const char *marker = (i == s_player.cue_track_index) ? ">" : " ";
            cue_track_t *t = &s_player.cue->tracks[i];
            s_player.output(" %s %d. %s\r\n", marker, i + 1,
                t->title[0] ? t->title : "(untitled)");
        }
        return;
    }

    if (s_player.track_count == 0) {
        s_player.output("No playlist loaded\r\n");
        return;
    }

    s_player.output("Playlist: %s (%d tracks)\r\n", s_player.folder_path, s_player.track_count);
    for (int i = 0; i < s_player.track_count; i++) {
        const char *marker = (i == s_player.track_index) ? ">" : " ";
        s_player.output(" %s %d. %s\r\n", marker, i + 1, s_player.track_names[i]);
    }
}
