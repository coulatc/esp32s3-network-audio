#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MEDIA_URI_MAX 2048
typedef enum { STOPPED, TRANSITIONING, PLAYING, PAUSED, NO_MEDIA_PRESENT } player_state_t;
typedef struct {
    char uri[MEDIA_URI_MAX];
    player_state_t state;
    bool failed;
    bool muted;
    unsigned volume;
    unsigned position;
    unsigned duration;
} player_snapshot_t;

esp_err_t player_init(void);
bool player_set_uri(const char *uri);
bool player_play(void);
bool player_pause(void);
void player_stop(void);
void player_set_volume(unsigned value);
void player_set_mute(bool mute);
void player_snapshot(player_snapshot_t *status);
const char *player_state_name(player_state_t state);
