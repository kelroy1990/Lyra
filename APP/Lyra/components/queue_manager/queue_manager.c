/*
 * queue_manager.c — Cross-source playback queue for Lyra.
 *
 * Maintains an ordered list of tracks from any source (SD, Subsonic, HTTP).
 * When a track finishes, automatically starts the next one.
 * If a source is unavailable, skips with a warning (max 3 consecutive skips).
 */

#include "queue_manager.h"
#include "sd_player.h"
#include "net_audio.h"
#include "subsonic.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "queue_mgr";

#define MAX_CONSECUTIVE_ERRORS  3

//--------------------------------------------------------------------+
// Internal state
//--------------------------------------------------------------------+

static struct {
    qm_track_t   tracks[QM_MAX_TRACKS];
    int           count;
    int           current;           // -1 = no track selected
    bool          active;            // queue playback mode engaged
    int           consecutive_errors;
    qm_repeat_t   repeat_mode;
    bool          shuffle;
    int           shuffle_map[QM_MAX_TRACKS];
    int           shuffle_pos;
} s_q;

//--------------------------------------------------------------------+
// Shuffle helpers (Fisher-Yates)
//--------------------------------------------------------------------+

static void generate_shuffle_map(void)
{
    for (int i = 0; i < s_q.count; i++)
        s_q.shuffle_map[i] = i;
    for (int i = s_q.count - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        int tmp = s_q.shuffle_map[i];
        s_q.shuffle_map[i] = s_q.shuffle_map[j];
        s_q.shuffle_map[j] = tmp;
    }
    // Move current track to front so it doesn't replay immediately
    if (s_q.current >= 0) {
        for (int i = 0; i < s_q.count; i++) {
            if (s_q.shuffle_map[i] == s_q.current) {
                s_q.shuffle_map[i] = s_q.shuffle_map[0];
                s_q.shuffle_map[0] = s_q.current;
                s_q.shuffle_pos = 0;
                break;
            }
        }
    }
}

//--------------------------------------------------------------------+
// Advance logic
//--------------------------------------------------------------------+

static bool advance_queue(bool forward)
{
    if (s_q.count == 0) {
        s_q.active = false;
        return false;
    }

    if (s_q.repeat_mode == QM_REPEAT_ONE) {
        // Stay on current track
        return true;
    }

    if (forward) {
        if (s_q.shuffle) {
            s_q.shuffle_pos++;
            if (s_q.shuffle_pos >= s_q.count) {
                if (s_q.repeat_mode == QM_REPEAT_ALL) {
                    generate_shuffle_map();
                } else {
                    s_q.active = false;
                    return false;
                }
            }
            s_q.current = s_q.shuffle_map[s_q.shuffle_pos];
        } else {
            s_q.current++;
            if (s_q.current >= s_q.count) {
                if (s_q.repeat_mode == QM_REPEAT_ALL) {
                    s_q.current = 0;
                } else {
                    s_q.active = false;
                    return false;
                }
            }
        }
    } else {
        if (s_q.shuffle) {
            if (s_q.shuffle_pos > 0) {
                s_q.shuffle_pos--;
                s_q.current = s_q.shuffle_map[s_q.shuffle_pos];
            }
            // else stay on current
        } else {
            s_q.current--;
            if (s_q.current < 0) s_q.current = 0;
        }
    }

    return true;
}

//--------------------------------------------------------------------+
// Play current track (dispatch to appropriate audio source)
//--------------------------------------------------------------------+

static void play_current_track(void)
{
    if (s_q.current < 0 || s_q.current >= s_q.count) {
        s_q.active = false;
        return;
    }

    const qm_track_t *t = &s_q.tracks[s_q.current];

    ESP_LOGI(TAG, "Playing [%d/%d] \"%s\" by %s (source=%d)",
             s_q.current + 1, s_q.count, t->title, t->artist, t->source);

    switch (t->source) {
    case QM_SOURCE_SD:
        // Enable single-track mode so sd_player calls our EOF callback
        sd_player_set_single_track_mode(true);
        sd_player_cmd_play(t->file_path);
        break;

    case QM_SOURCE_SUBSONIC: {
        char url[512];
        if (subsonic_build_stream_url(t->subsonic_id, url, sizeof(url)) == ESP_OK) {
            net_audio_cmd_start(url, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "Subsonic URL build failed for %s", t->subsonic_id);
            qm_notify_track_error("Subsonic not connected");
            return;
        }
        break;
    }

    case QM_SOURCE_NET:
        net_audio_cmd_start(t->url, NULL, NULL);
        break;
    }

    s_q.consecutive_errors = 0;
}

//--------------------------------------------------------------------+
// EOF callbacks (registered with net_audio and sd_player)
//--------------------------------------------------------------------+

static void on_net_audio_eof(bool error)
{
    if (!s_q.active) return;

    // Check that the currently playing track was actually a net/subsonic track
    if (s_q.current >= 0 && s_q.current < s_q.count) {
        qm_source_t src = s_q.tracks[s_q.current].source;
        if (src != QM_SOURCE_NET && src != QM_SOURCE_SUBSONIC) return;
    }

    if (error) {
        qm_notify_track_error("Network stream error");
    } else {
        qm_notify_track_ended();
    }
}

static void on_sd_player_eof(bool error)
{
    if (!s_q.active) return;

    // Check that the currently playing track was actually an SD track
    if (s_q.current >= 0 && s_q.current < s_q.count) {
        if (s_q.tracks[s_q.current].source != QM_SOURCE_SD) return;
    }

    if (error) {
        qm_notify_track_error("SD decode error");
    } else {
        qm_notify_track_ended();
    }
}

//--------------------------------------------------------------------+
// Public API: Init
//--------------------------------------------------------------------+

void qm_init(void)
{
    memset(&s_q, 0, sizeof(s_q));
    s_q.current = -1;

    // Register EOF callbacks
    net_audio_set_eof_callback(on_net_audio_eof);
    sd_player_set_eof_callback(on_sd_player_eof);

    ESP_LOGI(TAG, "Queue manager initialized (max %d tracks)", QM_MAX_TRACKS);
}

//--------------------------------------------------------------------+
// Public API: Enqueue
//--------------------------------------------------------------------+

void qm_append(const qm_track_t *track)
{
    if (!track || s_q.count >= QM_MAX_TRACKS) {
        ESP_LOGW(TAG, "Queue full or invalid track");
        return;
    }
    memcpy(&s_q.tracks[s_q.count], track, sizeof(qm_track_t));
    s_q.count++;

    if (s_q.shuffle) generate_shuffle_map();
}

void qm_insert_next(const qm_track_t *track)
{
    if (!track || s_q.count >= QM_MAX_TRACKS) return;

    int insert_pos = s_q.current + 1;
    if (insert_pos < 0) insert_pos = 0;
    if (insert_pos > s_q.count) insert_pos = s_q.count;

    // Shift tracks after insert position
    memmove(&s_q.tracks[insert_pos + 1], &s_q.tracks[insert_pos],
            (s_q.count - insert_pos) * sizeof(qm_track_t));
    memcpy(&s_q.tracks[insert_pos], track, sizeof(qm_track_t));
    s_q.count++;

    if (s_q.shuffle) generate_shuffle_map();
}

void qm_append_batch(const qm_track_t *tracks, int count)
{
    if (!tracks) return;
    for (int i = 0; i < count && s_q.count < QM_MAX_TRACKS; i++) {
        memcpy(&s_q.tracks[s_q.count], &tracks[i], sizeof(qm_track_t));
        s_q.count++;
    }
    if (s_q.shuffle) generate_shuffle_map();
}

//--------------------------------------------------------------------+
// Public API: Playback control
//--------------------------------------------------------------------+

void qm_play(void)
{
    if (s_q.count == 0) return;
    if (s_q.current < 0) s_q.current = 0;

    s_q.active = true;
    s_q.consecutive_errors = 0;

    if (s_q.shuffle) {
        generate_shuffle_map();
        s_q.current = s_q.shuffle_map[0];
    }

    play_current_track();
}

void qm_next(void)
{
    if (!s_q.active || s_q.count == 0) return;
    if (advance_queue(true)) {
        play_current_track();
    }
}

void qm_prev(void)
{
    if (!s_q.active || s_q.count == 0) return;
    advance_queue(false);
    play_current_track();
}

void qm_jump(int index)
{
    if (index < 0 || index >= s_q.count) return;
    s_q.current = index;
    s_q.active = true;
    s_q.consecutive_errors = 0;
    play_current_track();
}

void qm_stop(void)
{
    s_q.active = false;
    // Disable single-track mode so sd_player resumes normal behavior
    sd_player_set_single_track_mode(false);
    ESP_LOGI(TAG, "Queue stopped");
}

void qm_clear(void)
{
    qm_stop();
    s_q.count = 0;
    s_q.current = -1;
    s_q.shuffle_pos = 0;
    ESP_LOGI(TAG, "Queue cleared");
}

void qm_remove(int index)
{
    if (index < 0 || index >= s_q.count) return;

    memmove(&s_q.tracks[index], &s_q.tracks[index + 1],
            (s_q.count - index - 1) * sizeof(qm_track_t));
    s_q.count--;

    // Adjust current index
    if (index < s_q.current) {
        s_q.current--;
    } else if (index == s_q.current) {
        // Removed the currently playing track
        if (s_q.current >= s_q.count) s_q.current = s_q.count - 1;
    }

    if (s_q.shuffle) generate_shuffle_map();
}

//--------------------------------------------------------------------+
// Public API: State queries
//--------------------------------------------------------------------+

int  qm_count(void)         { return s_q.count; }
int  qm_current_index(void) { return s_q.current; }
bool qm_is_active(void)     { return s_q.active; }
bool qm_get_shuffle(void)   { return s_q.shuffle; }
qm_repeat_t qm_get_repeat(void) { return s_q.repeat_mode; }

const qm_track_t *qm_get_track(int index)
{
    if (index < 0 || index >= s_q.count) return NULL;
    return &s_q.tracks[index];
}

//--------------------------------------------------------------------+
// Public API: Shuffle / Repeat
//--------------------------------------------------------------------+

void qm_set_shuffle(bool enabled)
{
    s_q.shuffle = enabled;
    if (enabled && s_q.count > 0) {
        generate_shuffle_map();
    }
    ESP_LOGI(TAG, "Shuffle %s", enabled ? "ON" : "OFF");
}

void qm_set_repeat(qm_repeat_t mode)
{
    s_q.repeat_mode = mode;
    static const char *names[] = {"off", "one", "all"};
    ESP_LOGI(TAG, "Repeat: %s", names[mode % 3]);
}

//--------------------------------------------------------------------+
// Notifications
//--------------------------------------------------------------------+

void qm_notify_track_ended(void)
{
    if (!s_q.active) return;

    ESP_LOGI(TAG, "Track ended, advancing...");
    if (advance_queue(true)) {
        play_current_track();
    } else {
        ESP_LOGI(TAG, "Queue complete");
        sd_player_set_single_track_mode(false);
    }
}

void qm_notify_track_error(const char *reason)
{
    if (!s_q.active) return;

    s_q.consecutive_errors++;
    ESP_LOGW(TAG, "Track error (%d/%d): %s",
             s_q.consecutive_errors, MAX_CONSECUTIVE_ERRORS, reason ? reason : "unknown");

    if (s_q.consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        ESP_LOGE(TAG, "%d consecutive errors — stopping queue", MAX_CONSECUTIVE_ERRORS);
        s_q.active = false;
        sd_player_set_single_track_mode(false);
        return;
    }

    // Skip to next track
    if (advance_queue(true)) {
        play_current_track();
    } else {
        sd_player_set_single_track_mode(false);
    }
}

//--------------------------------------------------------------------+
// CDC command handler
//--------------------------------------------------------------------+

void qm_handle_cdc_command(const char *sub, qm_print_fn_t print)
{
    if (!sub || !*sub) {
        print("Usage: queue <add|play|next|prev|jump|list|clear|remove|status|shuffle|repeat>\r\n");
        return;
    }

    // queue add sd <path>
    if (strncmp(sub, "add ", 4) == 0) {
        const char *rest = sub + 4;
        qm_track_t t = {0};

        if (strncmp(rest, "sd ", 3) == 0) {
            t.source = QM_SOURCE_SD;
            strncpy(t.file_path, rest + 3, sizeof(t.file_path) - 1);
            // Extract title from filename
            const char *slash = strrchr(t.file_path, '/');
            strncpy(t.title, slash ? slash + 1 : t.file_path, sizeof(t.title) - 1);
        } else if (strncmp(rest, "url ", 4) == 0) {
            t.source = QM_SOURCE_NET;
            strncpy(t.url, rest + 4, sizeof(t.url) - 1);
            strncpy(t.title, "Net stream", sizeof(t.title) - 1);
        } else if (strncmp(rest, "sub ", 4) == 0) {
            t.source = QM_SOURCE_SUBSONIC;
            strncpy(t.subsonic_id, rest + 4, sizeof(t.subsonic_id) - 1);
            strncpy(t.title, t.subsonic_id, sizeof(t.title) - 1);
        } else {
            print("Usage: queue add <sd|url|sub> <path|url|track_id>\r\n");
            return;
        }
        qm_append(&t);
        print("Added to queue [%d]: %s\r\n", s_q.count, t.title);
        return;
    }

    if (strcmp(sub, "play") == 0) {
        qm_play();
        print("Queue playing (%d tracks)\r\n", s_q.count);
        return;
    }

    if (strcmp(sub, "next") == 0) {
        qm_next();
        if (s_q.current >= 0 && s_q.current < s_q.count)
            print("Now: [%d] %s\r\n", s_q.current + 1, s_q.tracks[s_q.current].title);
        return;
    }

    if (strcmp(sub, "prev") == 0) {
        qm_prev();
        if (s_q.current >= 0 && s_q.current < s_q.count)
            print("Now: [%d] %s\r\n", s_q.current + 1, s_q.tracks[s_q.current].title);
        return;
    }

    if (strncmp(sub, "jump ", 5) == 0) {
        int idx = atoi(sub + 5) - 1;  // 1-based input
        if (idx >= 0 && idx < s_q.count) {
            qm_jump(idx);
            print("Jumped to [%d] %s\r\n", idx + 1, s_q.tracks[idx].title);
        } else {
            print("Invalid index (1-%d)\r\n", s_q.count);
        }
        return;
    }

    if (strcmp(sub, "list") == 0) {
        if (s_q.count == 0) {
            print("Queue empty\r\n");
            return;
        }
        static const char *src_names[] = {"SD", "SUB", "NET"};
        for (int i = 0; i < s_q.count; i++) {
            const qm_track_t *t = &s_q.tracks[i];
            print("%s[%d] [%s] %s",
                  (i == s_q.current && s_q.active) ? ">> " : "   ",
                  i + 1, src_names[t->source], t->title);
            if (t->artist[0]) print(" — %s", t->artist);
            print("\r\n");
        }
        return;
    }

    if (strcmp(sub, "clear") == 0) {
        qm_clear();
        print("Queue cleared\r\n");
        return;
    }

    if (strncmp(sub, "remove ", 7) == 0) {
        int idx = atoi(sub + 7) - 1;
        if (idx >= 0 && idx < s_q.count) {
            print("Removed [%d] %s\r\n", idx + 1, s_q.tracks[idx].title);
            qm_remove(idx);
        } else {
            print("Invalid index (1-%d)\r\n", s_q.count);
        }
        return;
    }

    if (strcmp(sub, "stop") == 0) {
        qm_stop();
        print("Queue stopped\r\n");
        return;
    }

    if (strcmp(sub, "status") == 0) {
        static const char *rep_names[] = {"off", "one", "all"};
        print("Queue: %d tracks, %s\r\n", s_q.count, s_q.active ? "ACTIVE" : "inactive");
        if (s_q.current >= 0 && s_q.current < s_q.count) {
            print("  Current: [%d] %s\r\n", s_q.current + 1, s_q.tracks[s_q.current].title);
        }
        print("  Shuffle: %s  Repeat: %s\r\n",
              s_q.shuffle ? "on" : "off",
              rep_names[s_q.repeat_mode % 3]);
        print("  Consecutive errors: %d\r\n", s_q.consecutive_errors);
        return;
    }

    if (strncmp(sub, "shuffle", 7) == 0) {
        const char *arg = sub + 7;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0)       qm_set_shuffle(true);
        else if (strcmp(arg, "off") == 0) qm_set_shuffle(false);
        else                              qm_set_shuffle(!s_q.shuffle);
        print("Queue shuffle: %s\r\n", s_q.shuffle ? "on" : "off");
        return;
    }

    if (strncmp(sub, "repeat", 6) == 0) {
        const char *arg = sub + 6;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "off") == 0)       qm_set_repeat(QM_REPEAT_OFF);
        else if (strcmp(arg, "one") == 0)  qm_set_repeat(QM_REPEAT_ONE);
        else if (strcmp(arg, "all") == 0)  qm_set_repeat(QM_REPEAT_ALL);
        else {
            // Cycle: off→one→all→off
            qm_set_repeat((qm_repeat_t)((s_q.repeat_mode + 1) % 3));
        }
        static const char *rep_names[] = {"off", "one", "all"};
        print("Queue repeat: %s\r\n", rep_names[s_q.repeat_mode % 3]);
        return;
    }

    print("Unknown queue command: %s\r\n", sub);
}
