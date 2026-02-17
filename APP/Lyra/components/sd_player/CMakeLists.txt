idf_component_register(
    SRCS
        "sd_player.c"
        "sd_playlist.c"
        "cue_parser.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        log freertos audio_codecs storage
)
