#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "dlna.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "airplay2.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "music_lights.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "player.h"
#include "volume.h"

#define AP_PASSWORD "S3audio-setup"
#define SETTINGS_KEY "settings_v1"
#define BOOT_BUTTON GPIO_NUM_0
#define BOOT_RESET_HOLD_MS 5000
#define BOOT_BUTTON_POLL_MS 20

typedef struct {
    char ssid[33];
    char password[65];
    uint32_t rate;
    uint32_t rgb;
    uint8_t bits;
    uint8_t led_mode;
    uint8_t brightness;
} settings_t;

static const char *TAG = "s3_audio";
static settings_t settings = { .rgb = 0x18A8FF, .led_mode = 2, .brightness = 20 };
static atomic_uint led_settings, led_brightness, led_output_rgb;
static atomic_bool station_connected, ap_enabled, airplay_playing, airplay_available;
static esp_netif_t *station, *hotspot;
static char ap_name[32], device_name[32], hostname[32];
static uint8_t mac[6];
static audio_resampler_t airplay_pcm;
static TaskHandle_t network_task_handle;
static bool restarting;
static SemaphoreHandle_t settings_write_lock;

static void load_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("s3audio", NVS_READONLY, &handle) != ESP_OK) return;
    settings_t saved;
    size_t size = sizeof(saved);
    if (nvs_get_blob(handle, SETTINGS_KEY, &saved, &size) == ESP_OK && size == sizeof(saved) &&
        saved.ssid[32] == 0 && saved.password[64] == 0 && saved.led_mode <= LIGHT_SPECTRUM &&
        saved.brightness <= 100 && saved.rgb <= 0xFFFFFF) settings = saved;
    nvs_close(handle);
}

static esp_err_t save_settings(const settings_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("s3audio", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, SETTINGS_KEY, value, sizeof(*value));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) settings = *value;
    return err;
}

static esp_err_t erase_settings(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("s3audio", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, SETTINGS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void boot_reset_task(void *unused)
{
    const TickType_t poll = pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS);
    const TickType_t hold = pdMS_TO_TICKS(BOOT_RESET_HOLD_MS);

    // Arm only after release, even if the button was still held at startup.
    while (gpio_get_level(BOOT_BUTTON) == 0) vTaskDelay(poll);
    for (;;) {
        TickType_t pressed_at = xTaskGetTickCount();
        while (gpio_get_level(BOOT_BUTTON) == 0 &&
               xTaskGetTickCount() - pressed_at < hold) {
            vTaskDelay(poll);
        }
        if (gpio_get_level(BOOT_BUTTON) != 0) {
            vTaskDelay(poll);
            continue;
        }

        xSemaphoreTake(settings_write_lock, portMAX_DELAY);
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (!restarting) {
            err = erase_settings();
            if (err == ESP_OK) {
                esp_err_t volume = volume_erase();
                if (volume != ESP_OK) {
                    ESP_LOGW(TAG, "Could not clear volume settings: %s", esp_err_to_name(volume));
                }
                esp_err_t pairing = airplay2_clear_settings();
                if (pairing != ESP_OK) {
                    ESP_LOGW(TAG, "Could not clear AirPlay settings: %s", esp_err_to_name(pairing));
                }
                restarting = true;
            }
        }
        xSemaphoreGive(settings_write_lock);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Factory settings restored. Release BOOT to restart.");
        } else if (err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Could not reset settings: %s", esp_err_to_name(err));
        }

        // GPIO0 is a boot strapping pin. Wait for a stable release before reboot.
        unsigned released = 0;
        while (released < 3) {
            vTaskDelay(poll);
            released = gpio_get_level(BOOT_BUTTON) ? released + 1 : 0;
        }
        if (err == ESP_OK) esp_restart();
    }
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        atomic_store(&station_connected, true);
        ESP_LOGI(TAG, "Web console http://" IPSTR "/", IP2STR(&event->ip_info.ip));
    } else if ((base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        atomic_store(&station_connected, false);
    } else {
        return;
    }
    if (network_task_handle) xTaskNotifyGive(network_task_handle);
}

static void network_task(void *arg)
{
    (void)arg;
    bool configured = settings.ssid[0] != 0;
    TickType_t last_attempt = xTaskGetTickCount() - pdMS_TO_TICKS(10000);
    while (true) {
        bool online = atomic_load(&station_connected);
        if (online == atomic_load(&ap_enabled)) {
            esp_err_t err = esp_wifi_set_mode(online ? WIFI_MODE_STA :
                                              configured ? WIFI_MODE_APSTA : WIFI_MODE_AP);
            if (err == ESP_OK) atomic_store(&ap_enabled, !online);
        }
        TickType_t now = xTaskGetTickCount();
        if (!online && configured && now - last_attempt >= pdMS_TO_TICKS(10000)) {
            esp_wifi_connect();
            last_attempt = now;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    hotspot = esp_netif_create_default_wifi_ap();
    station = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(station, hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(hotspot, hostname));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    bool configured = settings.ssid[0] != 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(configured ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    wifi_config_t ap = { .ap = {
        .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK
    }};
    memcpy(ap.ap.ssid, ap_name, strlen(ap_name));
    ap.ap.ssid_len = strlen(ap_name);
    memcpy(ap.ap.password, AP_PASSWORD, sizeof(AP_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    if (configured) {
        wifi_config_t sta = { .sta = { .threshold.authmode = WIFI_AUTH_OPEN,
                                     .pmf_cfg = { .capable = true } }};
        memcpy(sta.sta.ssid, settings.ssid, strlen(settings.ssid));
        memcpy(sta.sta.password, settings.password, strlen(settings.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    atomic_store(&ap_enabled, true);
    ESP_LOGI(TAG, "Setup AP: %s, http://192.168.4.1/", ap_name);
    if (xTaskCreate(network_task, "wifi_retry", 4096, NULL, 4, &network_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Could not start Wi-Fi reconnect task");
    }
}

static void update_led(void)
{
    atomic_store(&led_settings, settings.rgb | (uint32_t)settings.led_mode << 24);
    atomic_store(&led_brightness, settings.brightness);
}

static void led_task(void *unused)
{
    led_strip_handle_t strip;
    const led_strip_config_t config = {
        .strip_gpio_num = 48, .max_leds = 1, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB
    };
    const led_strip_rmt_config_t rmt = { .resolution_hz = 10000000 };
    if (led_strip_new_rmt_device(&config, &rmt, &strip) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO48 WS2812 setup failed");
        vTaskDelete(NULL);
        return;
    }

    uint32_t previous = UINT32_MAX;
    light_mode_t last_mode = LIGHT_OFF;
    music_lights_t effects = {0};
    while (true) {
        unsigned packed = atomic_load(&led_settings);
        unsigned rgb = packed & 0xFFFFFF;
        light_mode_t mode = (packed >> 24) & 255;
        uint32_t now_ms = esp_timer_get_time() / 1000;
        if (mode != last_mode) {
            effects = (music_lights_t){0};
            last_mode = mode;
        }
        unsigned brightness = atomic_load(&led_brightness);
        if (mode == LIGHT_OFF) rgb = 0;
        if (mode == LIGHT_STATUS) {
            player_snapshot_t player;
            player_snapshot(&player);
            if (!atomic_load(&station_connected) && atomic_load(&ap_enabled)) {
                rgb = (now_ms / 400) % 2 ? 0xFF8200 : 0;
            } else if ((atomic_load(&airplay_playing) && audio_is_owner(AUDIO_AIRPLAY)) ||
                       player.state == PLAYING) {
                rgb = 0x08EA78;
            } else if (!audio_ready()) {
                rgb = 0xE63939;
            }
        }
        bool reactive = mode >= LIGHT_LEVEL;
        if (reactive) rgb = music_lights_color(&effects, mode, rgb, audio_meter_read(now_ms), now_ms);
        unsigned r = ((rgb >> 16) & 255) * brightness / 100;
        unsigned g = ((rgb >> 8) & 255) * brightness / 100;
        unsigned b = (rgb & 255) * brightness / 100;
        uint32_t applied = r << 16 | g << 8 | b;
        if (applied != previous) {
            if (led_strip_set_pixel(strip, 0, r, g, b) == ESP_OK && led_strip_refresh(strip) == ESP_OK) {
                atomic_store(&led_output_rgb, applied);
                previous = applied;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(reactive ? 20 : 80));
    }
}

static void airplay_audio(const int16_t *samples, size_t frames, unsigned rate)
{
    audio_write(AUDIO_AIRPLAY, &airplay_pcm, (const uint8_t *)samples,
                frames * sizeof(*samples) * 2, rate, 2, 16);
}

static void airplay_playback(bool playing)
{
    if (playing) audio_claim(AUDIO_AIRPLAY);
    else audio_release(AUDIO_AIRPLAY);
    atomic_store(&airplay_playing, playing);
}

static void airplay_flush(void)
{
    if (audio_is_owner(AUDIO_AIRPLAY)) audio_claim(AUDIO_AIRPLAY);
}

extern const char index_start[] asm("_binary_index_html_start");
extern const char index_end[] asm("_binary_index_html_end");

static esp_err_t homepage(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_start, index_end - index_start - 1);
}

static esp_err_t json_reply(httpd_req_t *req, cJSON *obj)
{
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory");
    char ip[16] = "";
    esp_netif_ip_info_t info;
    if (atomic_load(&station_connected) && esp_netif_get_ip_info(station, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    char color[8];
    snprintf(color, sizeof(color), "#%06lx", (unsigned long)settings.rgb);
    cJSON_AddStringToObject(json, "name", device_name);
    cJSON_AddStringToObject(json, "hostname", hostname);
    cJSON_AddStringToObject(json, "wifi", settings.ssid);
    cJSON_AddStringToObject(json, "ip", ip);
    cJSON_AddStringToObject(json, "hotspot", ap_name);
    cJSON_AddBoolToObject(json, "apEnabled", atomic_load(&ap_enabled));
    cJSON_AddBoolToObject(json, "airplay", atomic_load(&airplay_available));
    cJSON_AddStringToObject(json, "source", audio_is_owner(AUDIO_AIRPLAY) ? "AirPlay" :
                                          audio_is_owner(AUDIO_DLNA) ? "DLNA" : "");
    cJSON_AddNumberToObject(json, "rate", settings.rate);
    cJSON_AddNumberToObject(json, "bits", settings.bits);
    cJSON_AddNumberToObject(json, "mode", settings.led_mode);
    cJSON_AddNumberToObject(json, "brightness", settings.brightness);
    cJSON_AddStringToObject(json, "color", color);
    cJSON_AddNumberToObject(json, "ledRgb", atomic_load(&led_output_rgb));
    volume_status(json);
    audio_status(json);
    return json_reply(req, json);
}

static void restart_task(void *unused)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    volume_save();
    esp_restart();
}

static bool number(const cJSON *object, const char *key, int max, int *dst)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > max ||
        value->valuedouble != value->valueint) return false;
    *dst = value->valueint;
    return true;
}

static bool update_settings(const char *uri, cJSON *json, settings_t *next)
{
    if (!strcmp(uri, "/api/wifi")) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
            strlen(ssid->valuestring) > 32) return false;
        size_t len = strlen(password->valuestring);
        if (!ssid->valuestring[0] && len) return false;
        if (len && (len < 8 || len > 64)) return false;
        if (len == 64 && strspn(password->valuestring, "0123456789abcdefABCDEF") != 64) return false;
        strlcpy(next->ssid, ssid->valuestring, sizeof(next->ssid));
        strlcpy(next->password, password->valuestring, sizeof(next->password));
        return true;
    }
    if (!strcmp(uri, "/api/audio")) {
        int rate, bits;
        if (!number(json, "rate", 192000, &rate) || !number(json, "bits", 32, &bits) ||
            !audio_format_available((audio_format_t){rate, bits, 0})) return false;
        next->rate = rate;
        next->bits = bits;
        return true;
    }

    const cJSON *color = cJSON_GetObjectItemCaseSensitive(json, "color");
    int mode, brightness;
    if (!number(json, "mode", LIGHT_SPECTRUM, &mode) || !number(json, "brightness", 100, &brightness) ||
        !cJSON_IsString(color) || strlen(color->valuestring) != 7 || color->valuestring[0] != '#' ||
        strspn(color->valuestring + 1, "0123456789abcdefABCDEF") != 6) return false;
    next->rgb = strtoul(color->valuestring + 1, NULL, 16);
    next->led_mode = mode;
    next->brightness = brightness;
    return true;
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    char type[40];
    char body[512];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0 || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    }
    size_t used = 0;
    while (used < req->content_len) {
        int read = httpd_req_recv(req, body + used, req->content_len - used);
        if (read <= 0) return ESP_FAIL;
        used += read;
    }
    body[used] = 0;
    cJSON *json = cJSON_ParseWithLengthOpts(body, used + 1, NULL, true);
    settings_t next = settings;
    bool volume = !strcmp(req->uri, "/api/volume");
    int limit = 0;
    bool valid = cJSON_IsObject(json) && (volume ? number(json, "volumeLimit", 100, &limit) :
                                                  update_settings(req->uri, json, &next));
    cJSON_Delete(json);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings");

    xSemaphoreTake(settings_write_lock, portMAX_DELAY);
    if (restarting) {
        xSemaphoreGive(settings_write_lock);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Restart pending");
    }
    esp_err_t err = volume ? volume_set_limit(limit) : save_settings(&next);
    if (err != ESP_OK) {
        xSemaphoreGive(settings_write_lock);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
    }
    bool wifi = !strcmp(req->uri, "/api/wifi");
    if (wifi) {
        if (xTaskCreate(restart_task, "wifi_restart", 3072, NULL, 3, NULL) != pdPASS) {
            xSemaphoreGive(settings_write_lock);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Saved. Restart the device.");
        }
        restarting = true;
    } else if (!volume) {
        audio_set_format((audio_format_t){settings.rate, settings.bits, 0});
        update_led();
    }
    xSemaphoreGive(settings_write_lock);
    return status_handler(req);
}

static httpd_handle_t web_start(void)
{
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = homepage },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = settings_handler },
        { .uri = "/api/audio", .method = HTTP_POST, .handler = settings_handler },
        { .uri = "/api/volume", .method = HTTP_POST, .handler = settings_handler },
        { .uri = "/api/led", .method = HTTP_POST, .handler = settings_handler }
    };
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.max_open_sockets = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 4;
    config.send_wait_timeout = 4;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    for (size_t i = 0; i < sizeof(routes) / sizeof(*routes); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    return server;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    load_settings();
    ESP_ERROR_CHECK(volume_init());
    settings_write_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(settings_write_lock ? ESP_OK : ESP_ERR_NO_MEM);
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(xTaskCreate(boot_reset_task, "boot_reset", 3072, NULL, 4, NULL) == pdPASS ?
                    ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(ap_name, sizeof(ap_name), "S3-Audio-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(device_name, sizeof(device_name), "S3 Audio %02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(hostname, sizeof(hostname), "s3-audio-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_ERROR_CHECK(player_init());
    update_led();
    xTaskCreate(led_task, "status_led", 5100, NULL, 3, NULL);

    wifi_start();
    esp_err_t err = audio_init((audio_format_t){settings.rate, settings.bits, 0});
    if (err != ESP_OK) ESP_LOGE(TAG, "USB audio init: %s", esp_err_to_name(err));

    httpd_handle_t server = web_start();
    err = dlna_start(server, device_name, mac, station, hotspot);
    if (err != ESP_OK) ESP_LOGE(TAG, "DLNA init: %s", esp_err_to_name(err));

    const airplay2_config_t config = {
        .name = device_name, .hostname = hostname,
        .playback = airplay_playback, .flush = airplay_flush, .pcm = airplay_audio,
        .get_volume = volume_airplay_get, .set_volume = volume_airplay_set, .save_volume = volume_save
    };
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 2 * 1024 * 1024) {
        ESP_LOGE(TAG, "AirPlay requires at least 2 MB of configured PSRAM");
        return;
    }
    // Let PTP join on the station interface when an existing network connects.
    bool configured = settings.ssid[0] != 0;
    for (unsigned i = 0; configured && i < 50 && !atomic_load(&station_connected); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    err = airplay2_start(&config);
    atomic_store(&airplay_available, err == ESP_OK);
    if (err != ESP_OK) ESP_LOGE(TAG, "AirPlay init: %s", esp_err_to_name(err));
}
