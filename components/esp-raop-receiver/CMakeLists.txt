idf_component_register(
    SRCS "src/raop_core.c" "src/raop.c" "src/rtp.c" "src/util.c"
         "src/dmap_parser.c" "src/audio_buffer.c" "src/alac_decoder.cpp"
         "alac/ALACBitUtilities.c" "alac/ALACDecoder.cpp" "alac/EndianPortable.c"
         "alac/ag_dec.c" "alac/dp_dec.c" "alac/matrix_dec.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src" "private_include" "alac"
    REQUIRES esp_netif mdns mbedtls nvs_flash
    PRIV_REQUIRES esp_timer lwip pthread esp_hal_security)
