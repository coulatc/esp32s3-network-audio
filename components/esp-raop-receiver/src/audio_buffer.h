#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

// Audio output callback - called when audio needs to be written to hardware
typedef void (*audio_output_callback_t)(const uint8_t *data, size_t len, void *user_ctx);

// PCM tap callback - called with audio data at the moment it is sent to hardware
typedef void (*audio_pcm_tap_cb_t)(const uint8_t *data, size_t len, void *user_ctx);

// Initialize the audio buffer with output callback
void audio_buffer_init(audio_output_callback_t output_cb, void *user_ctx,
                       audio_pcm_tap_cb_t tap_cb, const atomic_int *gain_q16);

// Start audio playback task
void audio_buffer_start(void);

// Set the time when playback should actually begin
void audio_buffer_set_start_time(uint32_t playtime);

// Stop audio playback task (buffer remains allocated)
void audio_buffer_stop(void);

// Write audio data with timing information (called from RTP data callback)
// Returns true if written, false if buffer full
bool audio_buffer_write(const uint8_t *data, size_t len, uint32_t playtime);

// Flush all buffered audio
void audio_buffer_flush(void);

// Cleanup (stops playback and frees memory)
void audio_buffer_deinit(void);
