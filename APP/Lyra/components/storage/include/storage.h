/**
 * @file    storage.h
 * @date    2026-02-11
 * @brief   Public API for microSD storage and filesystem
 *
 * Will expose:
 * - storage_init()           : Init SDIO, mount filesystem
 * - storage_is_mounted()     : Card presence check
 * - storage_list_files()     : Enumerate audio files in directory
 * - storage_open_file()      : Open file for reading (used by codecs)
 * - storage_get_free_space() : Available space on card
 */
