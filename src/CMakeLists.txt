idf_component_register(
    SRCS "main.c"
         "config/settings.c"
         "core/display.c" "core/sdcard.c" "core/touch.c" "core/audio.c"
         "core/battery.c" "core/status.c" "core/net.c" "core/screenshot.c"
         "ui/draw.c"
         "clock/clock.c" "slideshow/slideshow.c"
         "weather/weather.c" "weather/weather_icons.c"
         "web/webserver.c"
    INCLUDE_DIRS "." "config" "core" "ui" "clock" "slideshow" "weather" "web"
    REQUIRES esp_lcd fatfs esp_timer driver esp_adc esp_wifi esp_event esp_netif
             nvs_flash esp_https_server esp_http_client mbedtls
)

# The setup page runs over HTTPS with a self-signed certificate. The header
# holding it is generated on the build machine and git-ignored, so a private
# key never ends up in the repository. Regenerated on every configure, which
# keeps the build free of ordering tricks between the script and the compile.
set(CERTS_H "${CMAKE_CURRENT_SOURCE_DIR}/web/certs.h")
execute_process(
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_cert.sh" "${CERTS_H}"
    RESULT_VARIABLE CERTS_RESULT
    OUTPUT_QUIET
)
if(NOT CERTS_RESULT EQUAL 0)
    message(FATAL_ERROR "could not generate ${CERTS_H}: openssl missing or failed")
endif()