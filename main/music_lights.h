#pragma once

#include "audio_meter.h"
#include <stdint.h>

typedef enum {
    LIGHT_OFF,
    LIGHT_SOLID,
    LIGHT_STATUS,
    LIGHT_LEVEL,
    LIGHT_BASS_PULSE,
    LIGHT_SPECTRUM,
} light_mode_t;

typedef struct {
    unsigned level;
    unsigned bass;
    unsigned mid;
    unsigned treble;
    unsigned bass_average_q8;
    unsigned pulse;
    uint32_t pulse_at;
} music_lights_t;

uint32_t music_lights_color(music_lights_t *state, light_mode_t mode, uint32_t color,
                            audio_meter_t input, uint32_t now_ms);
