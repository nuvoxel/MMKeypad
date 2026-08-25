idf_component_register(
    SRCS "audio_test_main.c"
    # Pull board.h (pinmap) from the main firmware tree — single source of truth.
    INCLUDE_DIRS "." "../../main"
    REQUIRES driver esp_timer esp_lcd
)
