#include "sd_player.h"
#include "audio_codecs.h"
#include "storage.h"
#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"

static const char *TAG = "sd_player";

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

    // Playlist
    char            folder_path[160];
    char           *track_names[PLAYLIST_MAX_TRACKS];
    int             track_count;
    int             track_index;

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

static void player_start_playback(const char *filepath)
{
    if (!player_open_file(filepath)) {
        s_player.state = PLAYER_STATE_IDLE;
        return;
    }

    player_scan_folder(filepath);

    // Switch audio source to SD with the file's format (always 32-bit I2S)
    s_player.audio.set_producer_handle(s_player.task_handle);
    s_player.audio.switch_source(SD_AUDIO_SOURCE_SD,
                                 s_player.current_info.sample_rate, 32);

    s_player.state = PLAYER_STATE_PLAYING;

    // Reset diagnostics on new track
    memset((void *)&s_sd_diag, 0, sizeof(s_sd_diag));
    s_sd_diag.stream_min = UINT32_MAX;

    if (s_player.output) {
        s_player.output("Playing: %s [%s %luHz %d-bit %s]\r\n",
            s_player.current_file,
            s_player.current_info.format == CODEC_FORMAT_MP3 ? "MP3" :
            s_player.current_info.format == CODEC_FORMAT_WAV ? "WAV" : "FLAC",
            s_player.current_info.sample_rate,
            s_player.current_info.bits_per_sample,
            s_player.current_info.channels == 1 ? "mono" : "stereo");
    }
}

static void player_stop(void)
{
    player_close_current();
    s_player.state = PLAYER_STATE_IDLE;

    // Switch back to USB audio source
    s_player.audio.switch_source(SD_AUDIO_SOURCE_USB, 0, 0);

    if (s_player.output) {
        s_player.output("Playback stopped\r\n");
    }
}

static void player_advance_track(bool forward)
{
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
                uint64_t target_frame = (uint64_t)cmd.seek_seconds *
                                        s_player.current_info.sample_rate;
                if (target_frame > s_player.current_info.total_frames) {
                    target_frame = s_player.current_info.total_frames;
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
    static int32_t decode_buf[480 * 2];  // 480 stereo frames = 3840 bytes (static to save stack)
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
        int32_t frames = codec_decode(s_player.codec, decode_buf, 480);
        uint32_t decode_us = (uint32_t)esp_timer_get_time() - t_decode;

        if (frames <= 0) {
            if (frames == 0) {
                ESP_LOGI(TAG, "Track finished, advancing...");
                player_advance_track(true);
            } else {
                ESP_LOGE(TAG, "Decode error, stopping");
                player_stop();
            }
            continue;
        }

        // Apply DSP
        uint32_t t_dsp = (uint32_t)esp_timer_get_time();
        s_player.audio.process_audio(decode_buf, (uint32_t)frames);
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
    st.track_index = s_player.track_index;
    st.track_count = s_player.track_count;

    if (s_player.current_info.sample_rate > 0) {
        st.elapsed_ms = (uint32_t)((s_player.frames_decoded * 1000ULL) /
                                    s_player.current_info.sample_rate);
        if (s_player.current_info.duration_ms > st.elapsed_ms) {
            st.remaining_ms = s_player.current_info.duration_ms - st.elapsed_ms;
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

    s_player.output("[%s] %s\r\n", state_str, st.current_file);
    s_player.output("  Format: %s %luHz %d-bit %s\r\n",
        st.file_info.format == CODEC_FORMAT_MP3 ? "MP3" :
        st.file_info.format == CODEC_FORMAT_WAV ? "WAV" : "FLAC",
        st.file_info.sample_rate, st.file_info.bits_per_sample,
        st.file_info.channels == 1 ? "mono" : "stereo");

    uint32_t el_s = st.elapsed_ms / 1000;
    uint32_t el_m = el_s / 60; el_s %= 60;
    uint32_t dur_s = st.file_info.duration_ms / 1000;
    uint32_t dur_m = dur_s / 60; dur_s %= 60;

    s_player.output("  Position: %lu:%02lu / %lu:%02lu\r\n", el_m, el_s, dur_m, dur_s);
    s_player.output("  Track: %d/%d\r\n", st.track_index + 1, st.track_count);
}

void sd_player_cmd_playlist_info(void)
{
    if (!s_player.output) return;

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
