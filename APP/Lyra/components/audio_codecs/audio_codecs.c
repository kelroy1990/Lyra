/**
 * @file    audio_codecs.c
 * @date    2026-02-11
 * @brief   Audio file decoders: FLAC, MP3, WAV, AAC, DSD (DoP)
 *
 * This component will contain:
 * - Decoder abstraction layer (common interface for all formats)
 * - FLAC decoder (lossless, priority)
 * - WAV decoder (PCM, trivial)
 * - MP3 decoder (lossy, wide compatibility)
 * - AAC decoder (lossy, modern)
 * - DSD over PCM (DoP) detection and passthrough to DAC
 * - Metadata extraction (tags, album art)
 *
 * Phase: F6 (microSD playback)
 */
