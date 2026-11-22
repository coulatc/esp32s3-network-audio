#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t airplay2_output_start(void);
bool airplay2_output_playing(void);
void airplay2_output_pcm(const int16_t *pcm, size_t frames, unsigned rate);
void airplay2_output_reset(void);
