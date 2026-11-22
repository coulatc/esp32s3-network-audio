#include "player.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "volume.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"

typedef struct {
    esp_http_client_handle_t http;
    unsigned generation;
    uint8_t prefix[12];
    size_t prefix_used, prefix_size;
    uint64_t position;
    bool failed;
} media_stream_t;

static const char *TAG = "dlna_player";
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static atomic_uint generation;
static player_snapshot_t status = { .state = NO_MEDIA_PRESENT };

static bool cancelled(unsigned id)
{
    return id != atomic_load(&generation) || !audio_is_owner(AUDIO_DLNA);
}

static bool can_decode(unsigned id)
{
    while (!cancelled(id)) {
        xSemaphoreTake(lock, portMAX_DELAY);
        bool paused = status.state == PAUSED;
        xSemaphoreGive(lock);
        if (!paused) return true;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    return false;
}

static size_t media_read(void *user, void *dest, size_t count)
{
    media_stream_t *s = user;
    uint8_t *out = dest;
    size_t total = 0;
    while (total < count && can_decode(s->generation)) {
        if (s->prefix_used < s->prefix_size) {
            size_t n = s->prefix_size - s->prefix_used;
            if (n > count - total) n = count - total;
            memcpy(out + total, s->prefix + s->prefix_used, n);
            s->prefix_used += n;
            total += n;
            continue;
        }
        size_t n = count - total;
        int got = esp_http_client_read(s->http, (char *)out + total, n > 4096 ? 4096 : n);
        if (got <= 0) {
            if (got < 0 && !cancelled(s->generation)) s->failed = true;
            break;
        }
        total += got;
    }
    s->position += total;
    return total;
}

static bool skip_bytes(media_stream_t *stream, uint64_t count)
{
    uint8_t discard[256];
    while (count) {
        size_t chunk = count > sizeof(discard) ? sizeof(discard) : count;
        if (media_read(stream, discard, chunk) != chunk) return false;
        count -= chunk;
    }
    return true;
}

static bool seek_forward(media_stream_t *stream, int offset, bool relative, bool from_start)
{
    if (offset < 0) return false;
    if (relative) return skip_bytes(stream, offset);
    if (from_start && (uint64_t)offset >= stream->position) {
        return skip_bytes(stream, (uint64_t)offset - stream->position);
    }
    return false;
}

static drwav_bool32 wav_seek(void *user, int offset, drwav_seek_origin origin)
{
    return seek_forward(user, offset, origin == DRWAV_SEEK_CUR, origin == DRWAV_SEEK_SET);
}

static drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin)
{
    return seek_forward(user, offset, origin == DRFLAC_SEEK_CUR, origin == DRFLAC_SEEK_SET);
}

static bool decode(media_stream_t *stream)
{
    enum { WAV, FLAC, MP3 } codec = MP3;
    if (stream->prefix_size >= 12 &&
        (!memcmp(stream->prefix, "RIFF", 4) || !memcmp(stream->prefix, "RF64", 4)) &&
        !memcmp(stream->prefix + 8, "WAVE", 4)) codec = WAV;
    if (stream->prefix_size >= 4 && !memcmp(stream->prefix, "fLaC", 4)) codec = FLAC;

    drwav wav;
    drflac *flac = NULL;
    drmp3 *mp3 = NULL;
    unsigned rate, channels, bits = 32;
    uint64_t total = 0, played = 0;
    if (codec == WAV) {
        if (!drwav_init_ex(&wav, media_read, wav_seek, NULL, NULL,
                            stream, NULL, DRWAV_SEQUENTIAL, NULL)) return false;
        channels = wav.channels;
        rate = wav.sampleRate;
        total = wav.totalPCMFrameCount;
    } else if (codec == FLAC) {
        flac = drflac_open(media_read, flac_seek, NULL, stream, NULL);
        if (!flac) return false;
        channels = flac->channels;
        rate = flac->sampleRate;
        total = flac->totalPCMFrameCount;
    } else {
        mp3 = calloc(1, sizeof(*mp3));
        if (!mp3) return false;
        if (!drmp3_init(mp3, media_read, NULL, NULL, NULL, stream, NULL)) {
            free(mp3);
            return false;
        }
        channels = mp3->channels;
        rate = mp3->sampleRate;
        bits = 16;
    }

    bool valid = (channels == 1 || channels == 2) && rate >= 8000 && rate <= 384000;
    drwav_int32 *pcm = valid ? malloc(256 * 2 * sizeof(*pcm)) : NULL;
    audio_resampler_t resampler = {0};
    while (pcm && can_decode(stream->generation)) {
        size_t frames;
        if (codec == WAV) frames = drwav_read_pcm_frames_s32(&wav, 256, pcm);
        else if (codec == FLAC) frames = drflac_read_pcm_frames_s32(flac, 256, pcm);
        else frames = drmp3_read_pcm_frames_s16(mp3, 256, (int16_t *)pcm);
        if (!frames || !can_decode(stream->generation)) break;

        xSemaphoreTake(lock, portMAX_DELAY);
        if (stream->generation == atomic_load(&generation) && status.state != PAUSED) {
            status.state = PLAYING;
            status.position = played / rate;
            status.duration = total / rate;
        }
        xSemaphoreGive(lock);

        while (!audio_ready() && can_decode(stream->generation)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (cancelled(stream->generation)) break;
        audio_write(AUDIO_DLNA, &resampler, (uint8_t *)pcm, frames * channels * (bits / 8),
                    rate, channels, bits);
        played += frames;
    }
    if (played && !cancelled(stream->generation)) {
        // Allow the final PCM blocks and the USB endpoint buffer to drain.
        TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(160);
        while (!cancelled(stream->generation) && xTaskGetTickCount() < until) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    free(pcm);
    if (mp3) {
        drmp3_uninit(mp3);
        free(mp3);
    }
    if (flac) drflac_close(flac);
    if (codec == WAV) drwav_uninit(&wav);
    return played > 0 && !stream->failed;
}

static bool fetch_media(const char *uri, unsigned id)
{
    const esp_http_client_config_t config = {
        .url = uri, .timeout_ms = 4000, .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true
    };
    media_stream_t stream = { .http = esp_http_client_init(&config), .generation = id };
    if (!stream.http) return false;

    esp_http_client_set_header(stream.http, "Accept", "audio/*, application/octet-stream");
    bool ok = false;
    for (int redirects = 0; redirects <= 4 && !cancelled(id); redirects++) {
        if (esp_http_client_open(stream.http, 0) != ESP_OK) break;
        int64_t headers = esp_http_client_fetch_headers(stream.http);
        int code = esp_http_client_get_status_code(stream.http);
        if (code >= 300 && code < 400 && redirects < 4) {
            if (esp_http_client_set_redirection(stream.http) != ESP_OK) break;
            esp_http_client_close(stream.http);
            continue;
        }
        if (headers < 0 || code < 200 || code >= 300) {
            ESP_LOGW(TAG, "HTTP media response %d", code);
            break;
        }
        stream.prefix_size = media_read(&stream, stream.prefix, sizeof(stream.prefix));
        stream.position = 0;
        if (stream.prefix_size) ok = decode(&stream);
        break;
    }
    esp_http_client_cleanup(stream.http);
    return ok;
}

static void player_task(void *unused)
{
    char *uri = malloc(MEDIA_URI_MAX);
    if (!uri) {
        ESP_LOGE(TAG, "Out of memory for media URL");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(lock, portMAX_DELAY);
        unsigned id = atomic_load(&generation);
        bool start = status.state == TRANSITIONING;
        if (start) strlcpy(uri, status.uri, MEDIA_URI_MAX);
        xSemaphoreGive(lock);
        if (!start) continue;
        bool ok = fetch_media(uri, id);
        xSemaphoreTake(lock, portMAX_DELAY);
        if (id == atomic_load(&generation)) {
            status.state = STOPPED;
            status.failed = !ok && audio_is_owner(AUDIO_DLNA);
            audio_release(AUDIO_DLNA);
        }
        xSemaphoreGive(lock);
    }
}

esp_err_t player_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock || xTaskCreate(player_task, "dlna_media", 14336, NULL, 5, &task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool player_set_uri(const char *uri)
{
    if (strlen(uri) >= MEDIA_URI_MAX ||
        (*uri && strncasecmp(uri, "http://", 7) && strncasecmp(uri, "https://", 8))) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    atomic_fetch_add(&generation, 1);
    audio_release(AUDIO_DLNA);
    strlcpy(status.uri, uri, sizeof(status.uri));
    status.state = *uri ? STOPPED : NO_MEDIA_PRESENT;
    status.position = status.duration = 0;
    status.failed = false;
    xSemaphoreGive(lock);
    xTaskNotifyGive(task);
    return true;
}

bool player_play(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = status.uri[0] && audio_ready();
    if (ok && status.state == PAUSED && audio_is_owner(AUDIO_DLNA)) {
        status.state = PLAYING;
    } else if (ok && (status.state == STOPPED || !audio_is_owner(AUDIO_DLNA))) {
        atomic_fetch_add(&generation, 1);
        status.position = status.duration = 0;
        status.failed = false;
        audio_claim(AUDIO_DLNA);
        status.state = TRANSITIONING;
    }
    xSemaphoreGive(lock);
    if (ok) xTaskNotifyGive(task);
    return ok;
}

bool player_pause(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = status.state == PLAYING || status.state == PAUSED;
    if (ok) status.state = PAUSED;
    xSemaphoreGive(lock);
    return ok;
}

void player_stop(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    atomic_fetch_add(&generation, 1);
    audio_release(AUDIO_DLNA);
    status.state = status.uri[0] ? STOPPED : NO_MEDIA_PRESENT;
    status.position = 0;
    status.failed = false;
    xSemaphoreGive(lock);
    xTaskNotifyGive(task);
}

void player_set_volume(unsigned value)
{
    volume_dlna_set(value);
}

void player_set_mute(bool muted)
{
    volume_dlna_mute(muted);
}

void player_snapshot(player_snapshot_t *copy)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *copy = status;
    xSemaphoreGive(lock);
    copy->volume = volume_dlna_get(&copy->muted);
}

const char *player_state_name(player_state_t state)
{
    static const char *const names[] = {
        "STOPPED", "TRANSITIONING", "PLAYING", "PAUSED_PLAYBACK", "NO_MEDIA_PRESENT"
    };
    return names[state];
}
