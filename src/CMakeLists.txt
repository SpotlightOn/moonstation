idf_component_register(
    SRCS "main.c" "display.c" "sdcard.c" "touch.c" "audio.c" "slideshow.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd fatfs esp_timer driver
)