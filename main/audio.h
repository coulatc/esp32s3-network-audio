#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

typedef enum { AUDIO_AIRPLAY, AUDIO_DLNA } audio_source_t;

typedef struct {
    uint32_t rate;
    uint8_t bits;
    uint8_t bytes;
} audio_format_t;

typedef struct {
    uint32_t generation;
    unsigned input_rate;
    unsigned output_rate;
    unsigned phase;
    int32_t previous[2];
    bool primed;
} audio_resampler_t;

esp_err_t audio_init(audio_format_t preferred);
void audio_set_format(audio_format_t format);
bool audio_format_available(audio_format_t format);
bool audio_ready(void);
void audio_claim(audio_source_t source);
bool audio_is_owner(audio_source_t source);
void audio_release(audio_source_t source);
bool audio_write(audio_source_t source, audio_resampler_t *state, const uint8_t *pcm,
                 size_t bytes, unsigned input_rate, unsigned channels, unsigned bits);
void audio_status(cJSON *status);
