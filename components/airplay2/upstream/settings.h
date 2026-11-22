#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t settings_get_device_name(char *name, size_t size);
void settings_device_name_to_hostname(const char *name, char *out, size_t size);
bool settings_airplay_v1(void);
esp_err_t settings_get_volume(float *volume_db);
esp_err_t settings_set_volume(float volume_db);
esp_err_t settings_persist_volume(void);
