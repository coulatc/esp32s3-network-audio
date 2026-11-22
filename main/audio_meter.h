#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t level;
    uint8_t bass;
    uint8_t mid;
    uint8_t treble;
} audio_meter_t;

void audio_meter_feed(const uint8_t *pcm, size_t size, unsigned rate, unsigned slot_bytes,
                      unsigned generation, uint32_t now_ms);
audio_meter_t audio_meter_read(uint32_t now_ms);
