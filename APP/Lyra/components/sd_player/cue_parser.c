#include "cue_parser.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <esp_log.h>

static const char *TAG = "cue_parser";

// Extract quoted string: TITLE "Some Title" → Some Title
static bool extract_quoted(const char *line, char *out, size_t out_size)
{
    const char *q1 = strchr(line, '"');
    if (!q1) return false;
    const char *q2 = strchr(q1 + 1, '"');
    if (!q2) return false;

    size_t len = q2 - q1 - 1;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, q1 + 1, len);
    out[len] = '\0';
    return true;
}

// Parse INDEX 01 MM:SS:FF → PCM frame position
// CUE frames are at 75 Hz (CD sector rate)
static bool parse_index(const char *line, uint32_t sample_rate, uint64_t *frame_out)
{
    // Find "INDEX 01" (we only care about INDEX 01, not INDEX 00)
    const char *p = strstr(line, "INDEX");
    if (!p) return false;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;

    int index_num = atoi(p);
    if (index_num != 1) return false;

    // Skip index number
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;

    // Parse MM:SS:FF
    int mm = 0, ss = 0, ff = 0;
    if (sscanf(p, "%d:%d:%d", &mm, &ss, &ff) != 3) return false;

    // Convert to PCM frames: ((MM*60 + SS) * 75 + FF) * sample_rate / 75
    uint64_t total_cd_frames = ((uint64_t)mm * 60 + ss) * 75 + ff;
    *frame_out = total_cd_frames * sample_rate / 75;
    return true;
}

bool cue_parse(const char *cue_path, uint32_t sample_rate, cue_sheet_t *out)
{
    if (!cue_path || !out || sample_rate == 0) return false;

    memset(out, 0, sizeof(*out));

    FILE *f = fopen(cue_path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", cue_path);
        return false;
    }

    char line[256];
    int current_track = -1;  // -1 = header section, 0+ = inside TRACK
    bool in_track = false;

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing whitespace
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
               line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }

        // Skip leading whitespace
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        // FILE "filename.flac" WAVE
        if (strncasecmp(p, "FILE", 4) == 0 && (p[4] == ' ' || p[4] == '\t')) {
            if (out->file[0] == '\0') {  // Only take the first FILE
                extract_quoted(p, out->file, sizeof(out->file));
            }
            continue;
        }

        // TRACK nn AUDIO
        if (strncasecmp(p, "TRACK", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            const char *num_start = p + 5;
            while (*num_start == ' ' || *num_start == '\t') num_start++;
            int tnum = atoi(num_start);
            if (tnum > 0 && out->track_count < CUE_MAX_TRACKS) {
                current_track = out->track_count;
                out->tracks[current_track].number = (uint8_t)tnum;
                out->track_count++;
                in_track = true;
            }
            continue;
        }

        // TITLE "text"
        if (strncasecmp(p, "TITLE", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            if (in_track && current_track >= 0) {
                extract_quoted(p, out->tracks[current_track].title,
                              sizeof(out->tracks[current_track].title));
            } else {
                extract_quoted(p, out->title, sizeof(out->title));
            }
            continue;
        }

        // PERFORMER "text"
        if (strncasecmp(p, "PERFORMER", 9) == 0 && (p[9] == ' ' || p[9] == '\t')) {
            if (in_track && current_track >= 0) {
                extract_quoted(p, out->tracks[current_track].performer,
                              sizeof(out->tracks[current_track].performer));
            } else {
                extract_quoted(p, out->performer, sizeof(out->performer));
            }
            continue;
        }

        // INDEX 01 MM:SS:FF
        if (strncasecmp(p, "INDEX", 5) == 0 && current_track >= 0) {
            parse_index(p, sample_rate, &out->tracks[current_track].start_frame);
            continue;
        }
    }

    fclose(f);

    if (out->track_count == 0 || out->file[0] == '\0') {
        ESP_LOGW(TAG, "CUE parse failed: tracks=%d file='%s'", out->track_count, out->file);
        return false;
    }

    ESP_LOGI(TAG, "Parsed CUE: '%s' — %d tracks, file='%s'",
             out->title, out->track_count, out->file);
    return true;
}
