#include "music_lights.h"

static unsigned envelope(unsigned previous, unsigned level)
{
    return level > previous ? level : (level + previous * 3) / 4;
}

static uint32_t scale_color(uint32_t color, unsigned level)
{
    unsigned red = ((color >> 16) & 255) * level / 255;
    unsigned green = ((color >> 8) & 255) * level / 255;
    unsigned blue = (color & 255) * level / 255;
    return red << 16 | green << 8 | blue;
}

uint32_t music_lights_color(music_lights_t *state, light_mode_t mode, uint32_t color,
                            audio_meter_t input, uint32_t now_ms)
{
    if (mode == LIGHT_LEVEL) {
        state->level = envelope(state->level, input.level);
        return scale_color(color, state->level);
    }
    if (mode == LIGHT_BASS_PULSE) {
        unsigned average = state->bass_average_q8 >> 8;
        state->bass_average_q8 = (state->bass_average_q8 * 19 + (input.bass << 8)) / 20;
        if (input.bass >= 24 && input.bass > average + 18 &&
            now_ms - state->pulse_at >= 120) {
            state->pulse = 255;
            state->pulse_at = now_ms;
        } else {
            state->pulse = state->pulse * 3 / 4;
        }
        return scale_color(color, state->pulse);
    }
    state->bass = envelope(state->bass, input.bass);
    state->mid = envelope(state->mid, input.mid);
    state->treble = envelope(state->treble, input.treble);
    return state->bass << 16 | state->mid << 8 | state->treble;
}
