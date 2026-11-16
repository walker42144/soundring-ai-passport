idf_component_register(
    SRCS "main.c"
         "sr_app.c"
         "sr_cover.c"
         "sr_cover_scale.c"
         "sr_http.c"
         "sr_input.c"
         "sr_net.c"
         "sr_navigation.c"
         "sr_pair.c"
         "sr_playback.c"
         "sr_policy.c"
         "sr_progress.c"
         "sr_queue_stream.c"
         "sr_spotify.c"
         "sr_store.c"
         "sr_text.c"
         "sr_ui.c"
         "fonts/sr_font_14.c"
         "fonts/sr_font_20.c"
         "fonts/sr_font_28.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             esp_event
             esp_http_client
             esp_netif
             esp_timer
             esp_wifi
             esp_driver_usb_serial_jtag
             json
             lwip
             mbedtls
             nvs_flash
             spiffs
)
