idf_component_register(
    SRCS "main.c"
         "core/display.c" "core/sdcard.c" "core/touch.c" "core/audio.c"
         "core/battery.c"
         "clock/clock.c" "slideshow/slideshow.c"
    INCLUDE_DIRS "." "core" "clock" "slideshow"
    REQUIRES esp_lcd fatfs esp_timer driver esp_adc esp_wifi esp_event esp_netif
)