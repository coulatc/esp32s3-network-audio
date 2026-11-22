#include "airplay2.h"
#include "airplay2_host.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "audio_receiver.h"
#include "dacp_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs.h"
#include "playback_events.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "wifi.h"

#define PAIRING_NAMESPACE "s3ap2"

static airplay2_config_t host;
static atomic_bool playing;

esp_err_t settings_get_device_name(char *name, size_t size)
{
    strlcpy(name, host.name, size);
    return ESP_OK;
}

void settings_device_name_to_hostname(const char *name, char *out, size_t size)
{
    (void)name;
    strlcpy(out, host.hostname, size);
}

bool settings_airplay_v1(void)
{
    return false;
}

void wifi_get_mac_str(char *out, size_t size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t settings_get_volume(float *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    *value = host.get_volume();
    return ESP_OK;
}

esp_err_t settings_set_volume(float value)
{
    host.set_volume(value);
    return ESP_OK;
}

esp_err_t settings_persist_volume(void)
{
    return host.save_volume();
}

esp_err_t airplay2_clear_settings(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(PAIRING_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void playback_event(playback_source_t source, playback_event_t event,
                           const playback_event_data_t *data, void *unused)
{
    (void)data;
    (void)unused;
    if (source != PLAYBACK_SOURCE_AIRPLAY) return;
    if (event == PLAYBACK_EVENT_PLAYING) {
        host.playback(true);
        atomic_store(&playing, true);
    } else if (event == PLAYBACK_EVENT_PAUSED || event == PLAYBACK_EVENT_DISCONNECTED) {
        atomic_store(&playing, false);
        host.playback(false);
    }
}

bool airplay2_output_playing(void)
{
    return atomic_load(&playing);
}

void airplay2_output_pcm(const int16_t *pcm, size_t frames, unsigned rate)
{
    if (atomic_load(&playing)) host.pcm(pcm, frames, rate);
}

void airplay2_output_reset(void)
{
    host.flush();
}

esp_err_t airplay2_start(const airplay2_config_t *config)
{
    if (!config || !config->name || !config->hostname ||
        !config->playback || !config->flush || !config->pcm || !config->get_volume ||
        !config->set_volume || !config->save_volume) return ESP_ERR_INVALID_ARG;
    host = *config;
    esp_err_t rc = hap_init();
    if (rc != ESP_OK) return rc;

    rc = ptp_clock_init();
    if (rc != ESP_OK) return rc;
    rc = audio_receiver_init();
    if (rc != ESP_OK) return rc;
    if (playback_events_register(playback_event, NULL)) return ESP_ERR_NO_MEM;
    rc = airplay2_output_start();
    if (rc != ESP_OK) return rc;
    dacp_init();
    rc = rtsp_server_start();
    if (rc != ESP_OK) return rc;
    rc = mdns_airplay_init();
    if (rc != ESP_OK) rtsp_server_stop();
    return rc;
}
