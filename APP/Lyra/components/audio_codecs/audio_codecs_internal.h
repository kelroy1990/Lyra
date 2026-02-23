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
        // WAV/AIFF decoder state (dr_wav — in-place struct, heap-allocated)
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
        // DSD decoder state (DSF/DFF container, outputs DoP int32_t frames)
        struct {
            void *state;            // dsd_state_t* heap-allocated
        } dsd;
        // AAC-LC/HE-AAC decoder state (opencore-aacdec, ADTS or M4A-AAC)
        struct {
            void *state;            // aac_state_t* heap-allocated
        } aac;
        // Opus decoder state (libopus, Ogg container)
        struct {
            void *state;            // opus_state_t* heap-allocated
        } opus;
        // ALAC decoder state (Apple ALACDecoder C++, M4A container)
        struct {
            void *state;            // alac_state_t* heap-allocated (C++ new)
        } alac;
    };
};

//--------------------------------------------------------------------+
// Per-format open functions (called by codec_open dispatch)
// All definitions are C functions; extern "C" ensures correct linkage
// when this header is included from C++ translation units (codec_alac.cpp).
//--------------------------------------------------------------------+

#ifdef __cplusplus
extern "C" {
#endif

// WAV/AIFF: dr_wav decoder, supports PCM/float/extensible/ADPCM/AIFF
bool codec_wav_open(codec_handle_t *h);

// FLAC: dr_flac decoder
bool codec_flac_open(codec_handle_t *h);

// MP3: dr_mp3 decoder (based on minimp3)
bool codec_mp3_open(codec_handle_t *h);

// DSD: DSF (Sony) or DFF (Philips DSDIFF) container, outputs DoP int32_t frames
bool codec_dsd_open(codec_handle_t *h);

// AAC: AAC-LC / HE-AAC in ADTS container (.aac raw bitstream)
bool codec_aac_open(codec_handle_t *h);

// Opus: Opus audio in Ogg container (.opus)
bool codec_opus_open(codec_handle_t *h);

// M4A: dispatcher for .m4a/.m4b/.mp4 — calls ALAC or AAC sub-opener
// Defined in codec_alac.cpp, exported with extern "C"
bool codec_m4a_open(codec_handle_t *h);

#ifdef __cplusplus
}
#endif
