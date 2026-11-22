#include "audio.h"
#include "audio_meter.h"
#include "esp_timer.h"
#include "volume.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/uac2_host.h"
#include "usb/uac_host.h"

#define OUTPUT_PACKET_MAX 600
#define PCM_BLOCK_SIZE 768
#define PCM_QUEUE_BLOCKS 10
#define UAC2_RING_BYTES 6144
#define UAC1_RING_BYTES 8192
#define USB_WRITER_CORE 0
#define MAX_FORMATS 48
#define GAIN_RAMP_MS 180
#define GAIN_RAMP_SHIFT 8

typedef enum { UAC1 = 1, UAC2 = 2 } usb_class_t;
typedef struct {
    usb_class_t protocol;
    uint8_t address, interface;
} connection_t;

typedef struct {
    uint32_t generation;
    uint32_t queued_at_us;
    audio_source_t source;
    size_t length;
    uint8_t data[PCM_BLOCK_SIZE];
} pcm_block_t;

static const char *TAG = "usb_audio";
static const uint32_t rates[] = {44100, 48000, 32000, 88200, 96000, 24000, 22050,
                                 16000, 11025, 8000, 64000, 176400, 192000};
static QueueHandle_t connections, pcm_queue;
static SemaphoreHandle_t lock;
static atomic_uint generation = 1, active_encoding, written_bytes, written_gain;
static atomic_uint usb_path_us, usb_underrun_bytes, usb_write_errors, pcm_queue_drops;
static atomic_int owner = -1;
static atomic_bool detached, stream_failed;
static audio_format_t preferred, supported[MAX_FORMATS];
static unsigned supported_count, requested_version;
static unsigned active_class;
static const char *message = "等待 USB DAC";

static int32_t read_sample(const uint8_t *at, unsigned bytes)
{
    // PCM is little-endian; align samples to the most significant bits.
    uint32_t value = (uint32_t)at[bytes - 1] << 24 | (uint32_t)at[bytes - 2] << 16;
    if (bytes >= 3) value |= (uint32_t)at[bytes - 3] << 8;
    if (bytes == 4) value |= at[0];
    return (int32_t)value;
}

static void write_sample(uint8_t *at, int32_t sample, audio_format_t format)
{
    uint32_t mask = UINT32_MAX << (32 - format.bits);
    uint32_t packed = ((uint32_t)sample & mask) >> (32 - format.bytes * 8);
    at[0] = packed;
    at[1] = packed >> 8;
    if (format.bytes >= 3) at[2] = packed >> 16;
    if (format.bytes == 4) at[3] = packed >> 24;
}

static unsigned limit_block(pcm_block_t *block, audio_format_t format, unsigned current)
{
    unsigned target = volume_gain(block->source) << GAIN_RAMP_SHIFT;
    if (current > target) current = target;
    if (!target) {
        memset(block->data, 0, block->length);
        return 0;
    }
    if (current == (VOLUME_UNITY << GAIN_RAMP_SHIFT)) return current;
    if (!format.bits) format.bits = 32;

    // Start new audio from zero and ramp increases. Apply decreases on the next block.
    unsigned step = (VOLUME_UNITY << GAIN_RAMP_SHIFT) * 100 / (format.rate * (GAIN_RAMP_MS / 10));
    unsigned stride = format.bytes * 2;
    for (size_t i = 0; i < block->length; i += stride) {
        if (target - current < step) current = target;
        else current += step;
        unsigned q15 = current >> GAIN_RAMP_SHIFT;
        for (unsigned ch = 0; ch < 2; ch++) {
            uint8_t *sample = block->data + i + ch * format.bytes;
            int32_t scaled = (int64_t)read_sample(sample, format.bytes) * q15 / VOLUME_UNITY;
            write_sample(sample, scaled, format);
        }
    }
    return current;
}

static bool same_format(audio_format_t a, audio_format_t b)
{
    return a.rate == b.rate && a.bits == b.bits;
}

static audio_format_t decode_format(unsigned value)
{
    return (audio_format_t){value >> 8, (value >> 3) & 31, value & 7};
}

static void set_active(audio_format_t format)
{
    // Encode 32 valid bits as zero when a four-byte slot is active.
    unsigned value = format.rate << 8 | (format.bits & 31) << 3 | format.bytes;
    atomic_store(&active_encoding, 0);
    atomic_fetch_add(&generation, 1);
    atomic_store(&active_encoding, value);
    xQueueReset(pcm_queue);
}

static void publish_options(const audio_format_t *list, unsigned count, unsigned protocol,
                            const char *status)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (count) memcpy(supported, list, count * sizeof(*list));
    supported_count = count;
    active_class = protocol;
    message = status;
    xSemaphoreGive(lock);
}

static void add_format(audio_format_t *list, unsigned *count, audio_format_t format)
{
    for (unsigned i = 0; i < *count; i++) {
        if (same_format(list[i], format)) return;
    }
    if (*count < MAX_FORMATS) list[(*count)++] = format;
}

static void uac2_connected(uint8_t addr, uint8_t iface, uac2_host_driver_event_t event, void *arg)
{
    if (event != UAC2_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    connection_t connection = { UAC2, addr, iface };
    xQueueSend(connections, &connection, 0);
}

static void uac1_connected(uint8_t addr, uint8_t iface, uac_host_driver_event_t event, void *arg)
{
    if (event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) return;
    connection_t connection = { UAC1, addr, iface };
    xQueueSend(connections, &connection, 0);
}

static void uac2_event(uac2_host_device_handle_t dev, uac2_host_device_event_t event, void *arg)
{
    if (event == UAC2_HOST_DEVICE_EVENT_DISCONNECTED) atomic_store(&detached, true);
    if (event == UAC2_HOST_DEVICE_EVENT_STREAM_ERROR) atomic_store(&stream_failed, true);
}

static void uac1_event(uac_host_device_handle_t dev, uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) atomic_store(&detached, true);
}

static unsigned uac2_formats(uac2_host_device_handle_t dev, audio_format_t *list)
{
    uac2_sample_rate_range_t ranges[UAC2_MAX_SAMPLE_RATE_RANGES];
    uint8_t range_count = UAC2_MAX_SAMPLE_RATE_RANGES;
    if (uac2_host_device_get_sample_rate_range(dev, ranges, &range_count) != ESP_OK ||
        !range_count) {
        uint32_t current;
        if (uac2_host_device_get_sample_rate(dev, &current) != ESP_OK) return 0;
        ranges[0] = (uac2_sample_rate_range_t){current, current, 0};
        range_count = 1;
    }

    unsigned count = 0;
    for (unsigned a = 1; a <= 32; a++) {
        uac2_host_dev_alt_param_t alt;
        if (uac2_host_get_device_alt_param(dev, a, &alt) != ESP_OK) continue;
        if (alt.channels != 2 || alt.ep_max_packet_size > OUTPUT_PACKET_MAX ||
            alt.sub_slot_size < 2 || alt.sub_slot_size > 4 ||
            (alt.bit_resolution != 16 && alt.bit_resolution != 24 &&
             alt.bit_resolution != 32)) continue;
        for (unsigned r = 0; r < sizeof(rates) / sizeof(*rates); r++) {
            uint32_t rate = rates[r];
            if (((rate + 999) / 1000) * 2 * alt.sub_slot_size > alt.ep_max_packet_size) continue;
            for (unsigned n = 0; n < range_count; n++) {
                uac2_sample_rate_range_t range = ranges[n];
                if (rate >= range.min && rate <= range.max &&
                    (!range.res || (rate - range.min) % range.res == 0)) {
                    add_format(list, &count, (audio_format_t){rate, alt.bit_resolution, alt.sub_slot_size});
                    break;
                }
            }
        }
    }
    return count;
}

static bool uac1_supports_rate(const uac_host_dev_alt_param_t *alt, unsigned rate)
{
    if (!alt->sample_freq_type) {
        return rate >= alt->sample_freq_lower && rate <= alt->sample_freq_upper;
    }
    for (unsigned i = 0; i < alt->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
        if (alt->sample_freq[i] == rate) return true;
    }
    return false;
}

static unsigned uac1_formats(uac_host_device_handle_t dev, audio_format_t *list)
{
    unsigned count = 0;
    for (unsigned a = 1; a <= 32; a++) {
        uac_host_dev_alt_param_t alt;
        if (uac_host_get_device_alt_param(dev, a, &alt) != ESP_OK) break;
        if (alt.format != 1 || alt.channels != 2 || (alt.bit_resolution != 16 && alt.bit_resolution != 24 &&
                                  alt.bit_resolution != 32)) continue;
        unsigned slot = alt.subframe_size;
        if (slot != alt.bit_resolution / 8) continue;
        for (unsigned r = 0; r < sizeof(rates) / sizeof(*rates); r++) {
            uint32_t rate = rates[r];
            if (((rate + 999) / 1000) * 2 * slot > OUTPUT_PACKET_MAX) continue;
            if (uac1_supports_rate(&alt, rate)) {
                add_format(list, &count, (audio_format_t){rate, alt.bit_resolution, slot});
            }
        }
    }
    return count;
}

static esp_err_t start_stream(uac2_host_device_handle_t two, uac_host_device_handle_t one,
                              audio_format_t *fmt)
{
    if (two) {
        uac2_host_stream_config_t options = {
            .channels = 2, .bit_resolution = fmt->bits, .sample_freq = fmt->rate
        };
        esp_err_t err = uac2_host_device_start(two, &options);
        if (err != ESP_OK) return err;
        uac2_host_stream_info_t info;
        err = uac2_host_device_get_stream_info(two, &info);
        if (err != ESP_OK) {
            uac2_host_device_stop(two);
            return err;
        }
        fmt->bytes = info.sub_slot_size;
        return ESP_OK;
    }
    uac_host_stream_config_t options = {
        .channels = 2, .bit_resolution = fmt->bits, .sample_freq = fmt->rate
    };
    return uac_host_device_start(one, &options);
}

static void process_device(connection_t connection)
{
    uac2_host_device_handle_t two = NULL;
    uac_host_device_handle_t one = NULL;
    audio_format_t options[MAX_FORMATS];
    atomic_store(&detached, false);
    atomic_store(&stream_failed, false);

    unsigned count;
    esp_err_t err;
    if (connection.protocol == UAC2) {
        uac2_host_device_config_t config = {
            .addr = connection.address, .iface_num = connection.interface,
            .buffer_size = UAC2_RING_BYTES, .callback = uac2_event
        };
        err = uac2_host_device_open(&config, &two);
        if (err != ESP_OK) goto open_failed;
        count = uac2_formats(two, options);
    } else {
        uac_host_device_config_t config = {
            .addr = connection.address, .iface_num = connection.interface,
            .buffer_size = UAC1_RING_BYTES, .callback = uac1_event
        };
        err = uac_host_device_open(&config, &one);
        if (err != ESP_OK) goto open_failed;
        count = uac1_formats(one, options);
    }

    publish_options(options, count, connection.protocol, "正在读取 USB 音频格式");
    unsigned applied = UINT32_MAX;
    bool streaming = false;
    pcm_block_t block;
    uint32_t output_generation = 0;
    unsigned current_gain = 0;
    bool reported_pcm = false;
    int64_t next_usb_sample = 0;
    while (!atomic_load(&detached)) {
        xSemaphoreTake(lock, portMAX_DELAY);
        audio_format_t target = preferred;
        unsigned version = requested_version;
        xSemaphoreGive(lock);

        if (applied != version || atomic_exchange(&stream_failed, false)) {
            set_active((audio_format_t){0});
            if (streaming) {
                err = two ? uac2_host_device_stop(two) : uac_host_device_stop(one);
                if (err != ESP_OK) break;
                streaming = false;
            }
            applied = version;
            audio_format_t candidate = {0};
            // First try the configured tuple, then 48/44.1 kHz at 16/24 bit, then other tuples.
            const audio_format_t order[] = { target, {48000, 16, 0}, {44100, 16, 0},
                                                   {48000, 24, 0}, {44100, 24, 0} };
            bool tried[MAX_FORMATS] = {0};
            for (unsigned pass = 0; pass <= sizeof(order) / sizeof(*order) &&
                                    !streaming && !atomic_load(&detached); pass++) {
                for (unsigned i = 0; i < count && !streaming && !atomic_load(&detached); i++) {
                    if (tried[i] || (pass < sizeof(order) / sizeof(*order) &&
                                     !same_format(options[i], order[pass]))) continue;
                    tried[i] = true;
                    candidate = options[i];
                    err = start_stream(two, one, &candidate);
                    streaming = err == ESP_OK;
                    if (!streaming) ESP_LOGW(TAG, "%lu Hz / %u bit: %s",
                                              candidate.rate, candidate.bits, esp_err_to_name(err));
                }
            }
            if (streaming) {
                set_active(candidate);
                ESP_LOGI(TAG, "UAC%u: %lu Hz / %u bit, %u-byte slots",
                         connection.protocol, candidate.rate, candidate.bits, candidate.bytes);
            }
            const char *status = "DAC 没有可用的双声道 PCM 格式";
            if (streaming) {
                status = !target.rate || same_format(target, candidate) ?
                    "USB DAC 已就绪" : "所选格式不可用，已启用可用格式";
            }
            publish_options(options, count, connection.protocol, status);
        }

        if (xQueueReceive(pcm_queue, &block, pdMS_TO_TICKS(20)) == pdTRUE &&
            streaming && audio_is_owner(block.source) && block.generation == atomic_load(&generation)) {
            if (output_generation != block.generation) {
                output_generation = block.generation;
                current_gain = 0;
                reported_pcm = false;
            }
            audio_format_t format = decode_format(atomic_load(&active_encoding));
            current_gain = limit_block(&block, format, current_gain);
            esp_err_t wrote = two ? uac2_host_device_write(two, block.data, block.length, 40) :
                                    uac_host_device_write(one, block.data, block.length, 40);
            if (wrote == ESP_OK) {
                int64_t now = esp_timer_get_time();
                atomic_fetch_add(&written_bytes, block.length);
                atomic_store(&written_gain, current_gain >> GAIN_RAMP_SHIFT);
                if (!reported_pcm && block.generation == atomic_load(&generation)) {
                    volume_pcm_started(block.source);
                    reported_pcm = true;
                }
                audio_meter_feed(block.data, block.length, format.rate, format.bytes,
                                 block.generation, now / 1000);
                if (two && now >= next_usb_sample) {
                    next_usb_sample = now + 50000;
                    uac2_host_buffer_status_t buf;
                    if (uac2_host_device_get_buffer_status(two, &buf) == ESP_OK) {
                        uint32_t duration = (uint64_t)block.length * 1000000 / (format.rate * format.bytes * 2);
                        uint32_t ahead = buf.ring_us > duration ? buf.ring_us - duration : 0;
                        atomic_store(&usb_path_us, (uint32_t)now - block.queued_at_us + ahead + buf.in_flight_us);
                        atomic_store(&usb_underrun_bytes, buf.underrun_bytes);
                    }
                }
            } else {
                atomic_fetch_add(&usb_write_errors, 1);
            }
        }
    }

    set_active((audio_format_t){0});
    publish_options(NULL, 0, 0, "等待 USB DAC");
    err = two ? uac2_host_device_close(two) : uac_host_device_close(one);
    if (err != ESP_OK) ESP_LOGW(TAG, "Close: %s", esp_err_to_name(err));
    return;

open_failed:
    ESP_LOGW(TAG, "UAC%u open: %s", connection.protocol, esp_err_to_name(err));
}

static void host_events(void *arg)
{
    while (true) {
        uint32_t flags;
        if (usb_host_lib_handle_events(portMAX_DELAY, &flags) == ESP_OK &&
            (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) usb_host_device_free_all();
    }
}

static void audio_task(void *arg)
{
    connection_t connection;
    while (xQueueReceive(connections, &connection, portMAX_DELAY) == pdTRUE) {
        process_device(connection);
    }
}

esp_err_t audio_init(audio_format_t initial)
{
    preferred = initial;
    lock = xSemaphoreCreateMutex();
    connections = xQueueCreate(8, sizeof(connection_t));
    pcm_queue = xQueueCreate(PCM_QUEUE_BLOCKS, sizeof(pcm_block_t));
    if (!lock || !connections || !pcm_queue) return ESP_ERR_NO_MEM;

    const usb_host_config_t host = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    esp_err_t err = usb_host_install(&host);
    if (err != ESP_OK) return err;
    if (xTaskCreatePinnedToCore(host_events, "usb_events", 4096, NULL, 21, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const uac2_host_driver_config_t two = {
        .callback = uac2_connected, .create_background_task = true,
        .task_priority = 19, .stack_size = 6144, .core_id = 0
    };
    const uac_host_driver_config_t one = {
        .callback = uac1_connected, .create_background_task = true,
        .task_priority = 18, .stack_size = 5120, .core_id = 0
    };
    esp_err_t uac2_err = uac2_host_install(&two);
    esp_err_t uac1_err = uac_host_install(&one);
    if (uac2_err != ESP_OK) ESP_LOGE(TAG, "UAC2 install: %s", esp_err_to_name(uac2_err));
    if (uac1_err != ESP_OK) ESP_LOGE(TAG, "UAC1 install: %s", esp_err_to_name(uac1_err));
    if (uac1_err != ESP_OK && uac2_err != ESP_OK) return uac2_err;
    return xTaskCreatePinnedToCore(audio_task, "usb_audio", 6144, NULL, 10, NULL,
                                   USB_WRITER_CORE) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

void audio_set_format(audio_format_t format)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!same_format(preferred, format)) {
        preferred = format;
        requested_version++;
    }
    xSemaphoreGive(lock);
}

bool audio_format_available(audio_format_t format)
{
    if (!format.rate && !format.bits) return true;
    if (!lock) return false;
    bool found = false;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (unsigned i = 0; i < supported_count; i++) found |= same_format(supported[i], format);
    xSemaphoreGive(lock);
    return found;
}

bool audio_ready(void)
{
    return atomic_load(&active_encoding) != 0;
}

static void clear_pending(void)
{
    atomic_fetch_add(&generation, 1);
    if (pcm_queue) xQueueReset(pcm_queue);
}

void audio_claim(audio_source_t source)
{
    if (!audio_is_owner(source)) volume_source_started(source);
    atomic_store(&owner, (int)source);
    clear_pending();
}

bool audio_is_owner(audio_source_t source)
{
    return atomic_load(&owner) == (int)source;
}

void audio_release(audio_source_t source)
{
    int expected = source;
    if (atomic_compare_exchange_strong(&owner, &expected, -1)) clear_pending();
}

static bool queue_block(audio_source_t source, pcm_block_t *block)
{
    // The renderer produces 256 frames at a time. Give the independent USB
    // writer a brief chance to drain a full queue without waiting a full block.
    TickType_t wait = pdMS_TO_TICKS(source == AUDIO_DLNA ? 75 : 3);
    if (!audio_is_owner(source) || block->generation != atomic_load(&generation)) return false;
    block->queued_at_us = (uint32_t)esp_timer_get_time();
    if (xQueueSend(pcm_queue, block, wait) == pdTRUE) {
        block->length = 0;
        return true;
    }
    atomic_fetch_add(&pcm_queue_drops, 1);
    return false;
}

bool audio_write(audio_source_t source, audio_resampler_t *state, const uint8_t *pcm,
                 size_t length, unsigned input_rate, unsigned channels, unsigned bits)
{
    if (!input_rate || (channels != 1 && channels != 2) ||
        (bits != 16 && bits != 24 && bits != 32)) return false;
    uint32_t current_gen = atomic_load(&generation);
    audio_format_t out = decode_format(atomic_load(&active_encoding));
    if (!audio_is_owner(source) || !out.rate || !pcm_queue) return false;
    if (!out.bits) out.bits = 32;
    if (state->generation != current_gen || state->input_rate != input_rate ||
        state->output_rate != out.rate) {
        *state = (audio_resampler_t){ .generation = current_gen, .input_rate = input_rate,
                                     .output_rate = out.rate,
                                     .phase_scale = ((UINT64_C(1) << 47) + out.rate / 2) / out.rate };
    }

    unsigned input_slot = bits / 8, stride = input_slot * channels;
    unsigned output_stride = out.bytes * 2;
    pcm_block_t block = { .generation = current_gen, .source = source };
    if (input_rate == out.rate && channels == 2 && bits == out.bits && input_slot == out.bytes) {
        length -= length % stride;
        while (length) {
            size_t n = length < sizeof(block.data) ? length : sizeof(block.data);
            memcpy(block.data, pcm, n);
            block.length = n;
            if (!queue_block(source, &block)) return false;
            pcm += n;
            length -= n;
        }
        return true;
    }
    for (size_t pos = 0; pos + stride <= length; pos += stride) {
        int32_t sample[2];
        sample[0] = read_sample(pcm + pos, input_slot);
        sample[1] = channels == 2 ? read_sample(pcm + pos + input_slot, input_slot) : sample[0];
        if (!state->primed) {
            memcpy(state->previous, sample, sizeof(sample));
            state->phase = out.rate;
            state->primed = true;
        }
        while (state->phase <= out.rate) {
            // Q31 interpolation with one shared weight for both channels. The
            // reciprocal is computed only when the rates or stream change.
            bool at_sample = state->phase == out.rate;
            uint32_t fraction = at_sample ? 0 : (state->phase * state->phase_scale + (1U << 15)) >> 16;
            for (unsigned ch = 0; ch < 2; ch++) {
                int64_t delta = (int64_t)sample[ch] - state->previous[ch];
                int32_t result = at_sample ? sample[ch] :
                    state->previous[ch] + ((delta * fraction) >> 31);
                write_sample(block.data + block.length, result, out);
                block.length += out.bytes;
            }
            if (block.length + output_stride > sizeof(block.data) && !queue_block(source, &block)) {
                state->primed = false;
                state->phase = 0;
                return false;
            }
            state->phase += input_rate;
        }
        state->phase -= out.rate;
        memcpy(state->previous, sample, sizeof(sample));
    }
    return !block.length || queue_block(source, &block);
}

static void cpu_load_status(cJSON *status)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && !CONFIG_FREERTOS_SMP && !CONFIG_FREERTOS_UNICORE
    // Status requests are serialized by the HTTP server.
    static int64_t last_sample;
    static uint64_t previous_idle[2];
    static unsigned load[2];
    int64_t now = esp_timer_get_time();
    if (now - last_sample >= 500000) {
        uint64_t span = now - last_sample;
        for (int core = 0; core < 2; core++) {
            uint64_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
            uint64_t elapsed_idle = idle - previous_idle[core];
            load[core] = elapsed_idle >= span ? 0 : (span - elapsed_idle) * 100 / span;
            previous_idle[core] = idle;
        }
        last_sample = now;
    }
    cJSON_AddNumberToObject(status, "cpu0Load", load[0]);
    cJSON_AddNumberToObject(status, "cpu1Load", load[1]);
#else
    (void)status;
#endif
}

void audio_status(cJSON *status)
{
    cJSON *options = cJSON_AddArrayToObject(status, "formats");
    if (!lock || !options) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    cJSON_AddStringToObject(status, "usb", message);
    cJSON_AddNumberToObject(status, "uac", active_class);
    cJSON_AddNumberToObject(status, "usbBytesWritten", atomic_load(&written_bytes));
    cJSON_AddNumberToObject(status, "usbWriteErrors", atomic_load(&usb_write_errors));
    cJSON_AddNumberToObject(status, "pcmQueueDrops", atomic_load(&pcm_queue_drops));
    if (audio_ready() && active_class == UAC2 && atomic_load(&owner) >= 0) {
        cJSON_AddNumberToObject(status, "usbPathUs", atomic_load(&usb_path_us));
        cJSON_AddNumberToObject(status, "usbUnderrunBytes", atomic_load(&usb_underrun_bytes));
    }
    double gain = audio_ready() && atomic_load(&owner) >= 0 ? atomic_load(&written_gain) : 0;
    cJSON_AddNumberToObject(status, "usbVolumePercent", gain * 100 / VOLUME_UNITY);
    audio_format_t active = decode_format(atomic_load(&active_encoding));
    cJSON_AddNumberToObject(status, "activeRate", active.rate);
    cJSON_AddNumberToObject(status, "activeBits", active.rate && !active.bits ? 32 : active.bits);
    for (unsigned i = 0; i < supported_count; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) break;
        cJSON_AddNumberToObject(entry, "rate", supported[i].rate);
        cJSON_AddNumberToObject(entry, "bits", supported[i].bits);
        cJSON_AddItemToArray(options, entry);
    }
    xSemaphoreGive(lock);
    cpu_load_status(status);
}
