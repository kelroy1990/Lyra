#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CUE_MAX_TRACKS 99

typedef struct {
    uint8_t  number;           // Track number (1-99)
    char     title[80];        // From TITLE directive
    char     performer[80];    // From PERFORMER directive
    uint64_t start_frame;      // PCM frame position (converted from INDEX 01 MM:SS:FF)
} cue_track_t;

typedef struct {
    char         file[160];           // Audio filename from FILE directive
    char         title[80];           // Album title
    char         performer[80];       // Album artist
    cue_track_t  tracks[CUE_MAX_TRACKS];
    int          track_count;
} cue_sheet_t;

/**
 * @brief Parse a CUE sheet file
 *
 * Reads a .cue file line by line and populates cue_sheet_t with track info.
 * The sample_rate is needed to convert CUE timestamps (MM:SS:FF at 75 Hz)
 * to PCM frame positions.
 *
 * Only single-FILE CUE sheets are supported (one audio file per .cue).
 *
 * @param cue_path    Full path to the .cue file
 * @param sample_rate Sample rate of the audio file (needed for frame conversion)
 * @param out         Caller-allocated output struct
 * @return true on success (at least one track found)
 */
bool cue_parse(const char *cue_path, uint32_t sample_rate, cue_sheet_t *out);
