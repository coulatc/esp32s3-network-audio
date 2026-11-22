#include "airplay2_host.h"
#include "audio_output.h"
#include "audio_receiver.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <stdatomic.h>

enum { FRAME_COUNT = 256, USB_PIPELINE_US = 16000 };
static atomic_uint source_rate = 44100;

void audio_output_set_source_rate(int rate)
{
    if (rate >= 8000 && rate <= 192000) atomic_store(&source_rate, rate);
}

void audio_output_flush(void)
{
    airplay2_output_reset();
}

uint32_t audio_output_get_hardware_latency_us(void)
{
    return USB_PIPELINE_US;
}

bool audio_output_get_pipeline_us(int64_t *now_us, uint32_t *pipeline_us)
{
    (void)now_us;
    (void)pipeline_us;
    return false;
}

int64_t audio_output_get_next_playout_time_ns(int64_t now_us)
{
    return (now_us + audio_output_get_hardware_latency_us()) * 1000;
}

static void render(void *unused)
{
    (void)unused;
    static int16_t pcm[FRAME_COUNT * 2];
    int64_t next_us = 0;
    while (true) {
        if (!airplay2_output_playing()) {
            next_us = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        unsigned rate = atomic_load(&source_rate);
        int64_t now_us = esp_timer_get_time();
        int64_t period = (int64_t)FRAME_COUNT * 1000000 / rate;
        if (!next_us || now_us - next_us >= period * 2) next_us = now_us;
        if (now_us < next_us) {
            unsigned wait_ms = (unsigned)(next_us - now_us + 999) / 1000;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }
        next_us += period;

        size_t frames = audio_receiver_read(pcm, FRAME_COUNT);
        if (frames) airplay2_output_pcm(pcm, frames, rate);
    }
}

esp_err_t airplay2_output_start(void)
{
    return xTaskCreatePinnedToCore(render, "airplay2_usb", 6144, NULL,
                                   AUDIO_PLAYBACK_TASK_PRIORITY, NULL, 1) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
