idf_component_register(SRCS "main.c" "audio.c" "audio_meter.c" "music_lights.c" "player.c" "dlna.c" "volume.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
                  esp_timer mbedtls esp_driver_rmt esp_driver_gpio lwip airplay2 stream_codecs
    EMBED_TXTFILES "index.html" "avtransport.xml" "rendering.xml" "connection.xml")
