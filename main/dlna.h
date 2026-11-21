#pragma once
#include "esp_http_server.h"
#include "esp_netif.h"

esp_err_t dlna_start(httpd_handle_t server, const char *name, const uint8_t mac[6],
                     esp_netif_t *station, esp_netif_t *hotspot);
