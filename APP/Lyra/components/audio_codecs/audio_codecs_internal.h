#pragma once

#include "audio_codecs.h"
#include <stdio.h>

//--------------------------------------------------------------------+
// Decoder vtable (each format implements these)
//--------------------------------------------------------------------+

typedef struct codec_handle_s codec_handle_t;

typedef struct {
    int32_t (*decode)(codec_handle_t *h, int32_t *buf, uint32_t max_frames);
    bool    (*seek)(codec_handle_t *h, uint64_t frame_pos);
    void    (*close)(codec_handle_t *h);
} codec_vtable_t;

//--------------------------------------------------------------------+
// Decoder handle (internal structure)
//--------------------------------------------------------------------+

struct codec_handle_s {
    codec_info_t info;
    const codec_vtable_t *vt;
    FILE *file;
    union {
        // WAV decoder state (dr_wav — in-place struct, heap-allocated)
        struct {
            void *drwav;            // drwav* handle
        } wav;
        // FLAC decoder state (dr_flac — heap-allocated internally)
        struct {
            void *drflac;           // drflac* handle
        } flac;
        // MP3 decoder state (dr_mp3 — in-place struct, heap-allocated)
        struct {
            void *drmp3;            // drmp3* handle
        } mp3;
    };
};

//--------------------------------------------------------------------+
// Per-format open functions (called by codec_open dispatch)
//--------------------------------------------------------------------+

// WAV: dr_wav decoder, supports PCM/float/extensible/ADPCM
bool codec_wav_open(codec_handle_t *h);

// FLAC: dr_flac decoder
bool codec_flac_open(codec_handle_t *h);

// MP3: dr_mp3 decoder (based on minimp3)
bool codec_mp3_open(codec_handle_t *h);
