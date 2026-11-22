#include "volume.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define SETTINGS_NAMESPACE "s3audio"
#define VOLUME_KEY "volume_v1"
#define MUTE_CDB (-14400)
#define DEFAULT_AIRPLAY_CDB (-1800)
#define STARTUP_GUARD_US 2500000LL
#define SAVE_DELAY_MS 1200

typedef struct {
    int16_t airplay_cdb;
    uint8_t dlna_percent;
    uint8_t limit;
} volume_settings_t;

static volume_settings_t settings = {
    DEFAULT_AIRPLAY_CDB, VOLUME_DEFAULT_DLNA, VOLUME_DEFAULT_LIMIT
};
static volume_settings_t saved;
static SemaphoreHandle_t lock;
static TaskHandle_t save_task;
static atomic_uint gain[2];
static int64_t first_pcm[2] = {-1, -1};
static bool dlna_muted, erasing;

static unsigned percent_gain(unsigned percent)
{
    return percent * VOLUME_UNITY / 100;
}

static unsigned airplay_gain(int cdb)
{
    if (cdb <= MUTE_CDB) return 0;
    return (unsigned)(VOLUME_UNITY * powf(10.0f, cdb / 2000.0f));
}

static void apply_locked(void)
{
    unsigned ceiling = percent_gain(settings.limit);
    unsigned airplay = airplay_gain(settings.airplay_cdb);
    unsigned dlna = percent_gain(dlna_muted ? 0 : settings.dlna_percent);
    atomic_store(&gain[AUDIO_AIRPLAY], airplay < ceiling ? airplay : ceiling);
    atomic_store(&gain[AUDIO_DLNA], dlna < ceiling ? dlna : ceiling);
}

static esp_err_t write_locked(const volume_settings_t *value)
{
    if (erasing || !memcmp(value, &saved, sizeof(saved))) return ESP_OK;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, VOLUME_KEY, value, sizeof(*value));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) saved = *value;
    return err;
}

esp_err_t volume_save(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = write_locked(&settings);
    xSemaphoreGive(lock);
    return err;
}

static void save_worker(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_DELAY_MS))) {}
        esp_err_t err = volume_save();
        if (err != ESP_OK) ESP_LOGW("volume", "Could not save volume: %s", esp_err_to_name(err));
    }
}

static int16_t db_to_cdb(float db)
{
    if (db <= MUTE_CDB / 100.0f) return MUTE_CDB;
    if (db >= 0.0f) return 0;
    return (int16_t)lroundf(db * 100.0f);
}

esp_err_t volume_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;

    nvs_handle_t nvs;
    bool found = false;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        volume_settings_t stored;
        size_t size = sizeof(stored);
        found = nvs_get_blob(nvs, VOLUME_KEY, &stored, &size) == ESP_OK &&
            size == sizeof(stored) && stored.airplay_cdb >= MUTE_CDB &&
            stored.airplay_cdb <= 0 && stored.dlna_percent <= 100 && stored.limit <= 100;
        if (found) settings = stored;
        nvs_close(nvs);
    }
    // Keep the AirPlay 2 volume saved by the preceding firmware.
    if (!found && nvs_open("s3ap2", NVS_READONLY, &nvs) == ESP_OK) {
        float db;
        size_t size = sizeof(db);
        if (nvs_get_blob(nvs, "volume_db", &db, &size) == ESP_OK && size == sizeof(db) &&
            isfinite(db) && db >= MUTE_CDB / 100.0f && db <= 0.0f) {
            settings.airplay_cdb = db_to_cdb(db);
        }
        nvs_close(nvs);
    }
    saved = settings;
    apply_locked();
    return xTaskCreate(save_worker, "save_volume", 3072, NULL, 3, &save_task) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t volume_erase(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, VOLUME_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err == ESP_OK) erasing = true;
    xSemaphoreGive(lock);
    return err;
}

esp_err_t volume_set_limit(unsigned percent)
{
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    volume_settings_t next = settings;
    next.limit = percent;
    esp_err_t err = erasing ? ESP_ERR_INVALID_STATE : write_locked(&next);
    if (err == ESP_OK) {
        settings = next;
        apply_locked();
    }
    xSemaphoreGive(lock);
    return err;
}

void volume_source_started(audio_source_t source)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    first_pcm[source] = -1;
    xSemaphoreGive(lock);
}

void volume_pcm_started(audio_source_t source)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (audio_is_owner(source) && first_pcm[source] < 0) first_pcm[source] = esp_timer_get_time();
    xSemaphoreGive(lock);
}

static bool accept_increase_locked(audio_source_t source)
{
    return audio_is_owner(source) && first_pcm[source] >= 0 &&
        esp_timer_get_time() - first_pcm[source] >= STARTUP_GUARD_US;
}

unsigned volume_gain(audio_source_t source)
{
    return atomic_load(&gain[source]);
}

float volume_airplay_get(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned effective = volume_gain(AUDIO_AIRPLAY);
    bool capped = effective < airplay_gain(settings.airplay_cdb);
    float db = settings.airplay_cdb / 100.0f;
    if (capped) db = effective ? 20.0f * log10f((float)effective / VOLUME_UNITY) : -144.0f;
    xSemaphoreGive(lock);
    return db;
}

void volume_airplay_set(float db)
{
    if (!isfinite(db)) return;
    int16_t cdb = db_to_cdb(db);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!erasing && cdb != settings.airplay_cdb &&
        (cdb < settings.airplay_cdb || accept_increase_locked(AUDIO_AIRPLAY))) {
        settings.airplay_cdb = cdb;
        apply_locked();
        xTaskNotifyGive(save_task);
    }
    xSemaphoreGive(lock);
}

unsigned volume_dlna_get(bool *muted)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *muted = dlna_muted || !settings.limit;
    unsigned percent = settings.dlna_percent < settings.limit ? settings.dlna_percent : settings.limit;
    xSemaphoreGive(lock);
    return percent;
}

void volume_dlna_set(unsigned percent)
{
    if (percent > 100) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!erasing && percent != settings.dlna_percent &&
        (percent < settings.dlna_percent || accept_increase_locked(AUDIO_DLNA))) {
        settings.dlna_percent = percent;
        apply_locked();
        xTaskNotifyGive(save_task);
    }
    xSemaphoreGive(lock);
}

void volume_dlna_mute(bool muted)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    dlna_muted = muted;
    apply_locked();
    xSemaphoreGive(lock);
}

void volume_status(cJSON *json)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned limit = settings.limit;
    unsigned dlna = settings.dlna_percent;
    int cdb = settings.airplay_cdb;
    xSemaphoreGive(lock);
    cJSON_AddNumberToObject(json, "volumeLimit", limit);
    cJSON_AddNumberToObject(json, "airplayVolumeDb", cdb / 100.0);
    cJSON_AddNumberToObject(json, "dlnaVolume", dlna);
}
