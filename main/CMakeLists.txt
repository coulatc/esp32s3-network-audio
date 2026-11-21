idf_component_register(SRCS "main.c" "audio.c" "player.c" "dlna.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client
                  esp_timer esp_driver_rmt lwip esp-raop-receiver stream_codecs
    EMBED_TXTFILES "index.html" "avtransport.xml" "rendering.xml" "connection.xml")
