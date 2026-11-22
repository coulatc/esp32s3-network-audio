#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *name;
    const char *hostname;
    void (*playback)(bool playing);
    void (*flush)(void);
    void (*pcm)(const int16_t *pcm, size_t frames, unsigned rate);
    float (*get_volume)(void);
    void (*set_volume)(float db);
    esp_err_t (*save_volume)(void);
} airplay2_config_t;

esp_err_t airplay2_start(const airplay2_config_t *config);
esp_err_t airplay2_clear_settings(void);
