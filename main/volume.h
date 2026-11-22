#pragma once

#include "audio.h"

#define VOLUME_UNITY 32768U
#define VOLUME_DEFAULT_LIMIT 50U
#define VOLUME_DEFAULT_DLNA 20U

esp_err_t volume_init(void);
esp_err_t volume_erase(void);
esp_err_t volume_save(void);
esp_err_t volume_set_limit(unsigned percent);
void volume_source_started(audio_source_t source);
void volume_pcm_started(audio_source_t source);
unsigned volume_gain(audio_source_t source);
float volume_airplay_get(void);
void volume_airplay_set(float db);
unsigned volume_dlna_get(bool *muted);
void volume_dlna_set(unsigned percent);
void volume_dlna_mute(bool muted);
void volume_status(cJSON *json);
