idf_component_register(SRCS "library.c" "lib_storage.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES json settings esp_timer queue_manager)
