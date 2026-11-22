#include "audio_meter.h"
#include <stdatomic.h>

enum { METER_RATE = 8000, WINDOW_MS = 20, STALE_MS = 200 };

static struct {
    unsigned generation;
    unsigned rate;
    unsigned step;
    unsigned until_sample;
    unsigned window;
    unsigned samples;
    int32_t bass[2];
    int32_t bass_mid[2];
    int32_t alpha_bass;
    int32_t alpha_bass_mid;
    uint32_t sums[4];
} meter;

static atomic_uint packed_levels;
static atomic_uint last_update_ms;

static int32_t signed_sample(const uint8_t *input, unsigned slot)
{
    return (int16_t)((unsigned)input[slot - 1] << 8 | input[slot - 2]);
}

static uint32_t magnitude(int32_t value)
{
    return (uint32_t)(value < 0 ? -value : value);
}

static unsigned meter_level(uint32_t sum, unsigned samples)
{
    unsigned average = sum / samples;
    if (average <= 12) return 0;
    return average * 255 / (average + 1250);
}

void audio_meter_feed(const uint8_t *pcm, size_t size, unsigned rate, unsigned slot,
                      unsigned generation, uint32_t now_ms)
{
    if (rate < 8000 || slot < 2 || slot > 4) return;
    if (meter.rate != rate || meter.generation != generation) {
        meter = (typeof(meter)){0};
        meter.generation = generation;
        meter.rate = rate;
        meter.step = (rate + METER_RATE - 1) / METER_RATE;
        unsigned effective_rate = rate / meter.step;
        meter.window = effective_rate * WINDOW_MS / 1000;
        // One-pole low-pass filters at approximately 160 and 2000 Hz.
        meter.alpha_bass = (1005 * 32768) / (effective_rate + 1005);
        meter.alpha_bass_mid = (12566 * 32768) / (effective_rate + 12566);
        atomic_store(&packed_levels, 0);
    }

    for (size_t index = 0; index + slot * 2 <= size; index += slot * 2) {
        if (meter.until_sample) {
            meter.until_sample--;
            continue;
        }
        meter.until_sample = meter.step - 1;
        for (unsigned ch = 0; ch < 2; ch++) {
            int32_t sample = signed_sample(pcm + index + ch * slot, slot);
            meter.bass[ch] += (sample - meter.bass[ch]) * meter.alpha_bass / 32768;
            meter.bass_mid[ch] += (sample - meter.bass_mid[ch]) * meter.alpha_bass_mid / 32768;
            meter.sums[0] += magnitude(sample);
            meter.sums[1] += magnitude(meter.bass[ch]);
            meter.sums[2] += magnitude(meter.bass_mid[ch] - meter.bass[ch]);
            meter.sums[3] += magnitude(sample - meter.bass_mid[ch]);
        }
        if (++meter.samples == meter.window) {
            unsigned value = 0;
            for (unsigned band = 0; band < 4; band++) {
                value |= meter_level(meter.sums[band], meter.samples * 2) << (band * 8);
                meter.sums[band] = 0;
            }
            atomic_store(&packed_levels, value);
            atomic_store(&last_update_ms, now_ms);
            meter.samples = 0;
        }
    }
}

audio_meter_t audio_meter_read(uint32_t now_ms)
{
    if (now_ms - atomic_load(&last_update_ms) > STALE_MS) return (audio_meter_t){0};
    unsigned value = atomic_load(&packed_levels);
    return (audio_meter_t){value & 255, (value >> 8) & 255, (value >> 16) & 255, value >> 24};
}
