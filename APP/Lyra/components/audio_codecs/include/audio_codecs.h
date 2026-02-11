/**
 * @file    audio_codecs.h
 * @date    2026-02-11
 * @brief   Public API for audio file decoders
 *
 * Will expose:
 * - audio_codec_open()      : Open file, detect format, init decoder
 * - audio_codec_decode()    : Decode next block of PCM samples
 * - audio_codec_seek()      : Seek to position
 * - audio_codec_get_info()  : Sample rate, bit depth, channels, duration
 * - audio_codec_get_meta()  : Title, artist, album, art
 * - audio_codec_close()     : Release decoder resources
 */
