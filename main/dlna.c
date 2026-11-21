#include "dlna.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "player.h"

#define DEVICE_TYPE "urn:schemas-upnp-org:device:MediaRenderer:1"
#define SERVICE_NS "urn:schemas-upnp-org:service:"
#define SINK_PROTOCOLS "http-get:*:audio/mpeg:*,http-get:*:audio/wav:*," \
                       "http-get:*:audio/x-wav:*,http-get:*:audio/flac:*," \
                       "http-get:*:audio/x-flac:*"
#define SSDP_GROUP "239.255.255.250"
#define SSDP_PORT 1900
#define SERVER_ID "ESP32-S3/1.0 UPnP/1.0 S3Audio/1.0"
#define MAX_SUBSCRIPTIONS 8

typedef enum { AV, RENDER, CONNECTION, SERVICE_COUNT } service_id_t;

static const char *const service_names[] = {"AVTransport", "RenderingControl", "ConnectionManager"};
static const char *const descriptions[] = {"/upnp/av.xml", "/upnp/rc.xml", "/upnp/cm.xml"};
static const char *const controls[] = {"/upnp/av", "/upnp/rc", "/upnp/cm"};
static const char *const events[] = {"/upnp/av/event", "/upnp/rc/event", "/upnp/cm/event"};

extern const char av_start[] asm("_binary_avtransport_xml_start");
extern const char rc_start[] asm("_binary_rendering_xml_start");
extern const char cm_start[] asm("_binary_connection_xml_start");
static const char *const scpd[] = {av_start, rc_start, cm_start};

static const char *TAG = "dlna";
static const char *display_name;
static char uuid[48];
static esp_netif_t *interfaces[2];

typedef struct {
    service_id_t service;
    uint32_t sequence;
    int64_t expiry;
    char sid[48];
    char callback[256];
} subscription_t;

static SemaphoreHandle_t subscription_lock;
static subscription_t subscriptions[MAX_SUBSCRIPTIONS];

static esp_err_t xml_response(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    httpd_resp_set_hdr(req, "EXT", "");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t description_handler(httpd_req_t *req)
{
    char *xml = NULL;
    int len = asprintf(&xml,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
        "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
        "<specVersion><major>1</major><minor>0</minor></specVersion><device>"
        "<deviceType>" DEVICE_TYPE "</deviceType>"
        "<friendlyName>%s</friendlyName><manufacturer>S3 Audio</manufacturer>"
        "<modelName>ESP32-S3 USB Audio</modelName><UDN>%s</UDN>"
        "<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC><presentationURL>/</presentationURL>"
        "<serviceList>"
        "<service><serviceType>" SERVICE_NS "AVTransport:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<SCPDURL>/upnp/av.xml</SCPDURL><controlURL>/upnp/av</controlURL>"
        "<eventSubURL>/upnp/av/event</eventSubURL></service>"
        "<service><serviceType>" SERVICE_NS "RenderingControl:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "<SCPDURL>/upnp/rc.xml</SCPDURL><controlURL>/upnp/rc</controlURL>"
        "<eventSubURL>/upnp/rc/event</eventSubURL></service>"
        "<service><serviceType>" SERVICE_NS "ConnectionManager:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
        "<SCPDURL>/upnp/cm.xml</SCPDURL><controlURL>/upnp/cm</controlURL>"
        "<eventSubURL>/upnp/cm/event</eventSubURL></service>"
        "</serviceList></device></root>", display_name, uuid);
    if (len < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory");
    esp_err_t err = xml_response(req, xml);
    free(xml);
    return err;
}

static esp_err_t scpd_handler(httpd_req_t *req)
{
    return xml_response(req, scpd[(uintptr_t)req->user_ctx]);
}

static bool tag_value(const char *xml, const char *tag, char *out, size_t size)
{
    // SOAP arguments can be prefixed. Reject nested elements in scalar arguments.
    size_t tag_len = strlen(tag);
    for (const char *start = strchr(xml, '<'); start; start = strchr(start + 1, '<')) {
        const char *p = start + 1;
        if (!isalpha((unsigned char)*p) && *p != '_') continue;
        const char *local = p;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == ':' || *p == '-') {
            if (*p == ':') local = p + 1;
            p++;
        }
        if ((size_t)(p - local) != tag_len || memcmp(local, tag, tag_len)) continue;
        const char *close = strchr(p, '>');
        if (!close) return false;
        if (close > start && close[-1] == '/') {
            *out = 0;
            return true;
        }
        start = close + 1;
        const char *end = strchr(start, '<');
        if (!end || end[1] != '/') return false;
        size_t used = 0;
        while (start < end && used + 1 < size) {
            if (*start != '&') {
                out[used++] = *start++;
                continue;
            }
            const char *entity = memchr(start, ';', end - start);
            if (!entity) return false;
            const char *const entities[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
            const char replacements[] = "&<>\"'";
            bool replaced = false;
            for (unsigned k = 0; k < sizeof(replacements) - 1; k++) {
                if ((size_t)(entity + 1 - start) == strlen(entities[k]) &&
                    !memcmp(start, entities[k], entity + 1 - start)) {
                    out[used++] = replacements[k];
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                if (start[1] != '#') return false;
                bool hex = start[2] == 'x' || start[2] == 'X';
                char *after;
                unsigned long value = strtoul(start + 2 + hex, &after, hex ? 16 : 10);
                if (after != entity || value < 0x20 || value > 0x7e) return false;
                out[used++] = (char)value;
            }
            start = entity + 1;
        }
        if (start != end) return false;
        out[used] = 0;
        return true;
    }
    return false;
}

static char *escape_xml(const char *src)
{
    char *result = malloc(strlen(src) * 6 + 1);
    if (!result) return NULL;
    char *out = result;
    for (; *src; src++) {
        const char *escaped = NULL;
        switch (*src) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        case '\'': escaped = "&apos;"; break;
        default: *out++ = *src; break;
        }
        if (escaped) {
            size_t len = strlen(escaped);
            memcpy(out, escaped, len);
            out += len;
        }
    }
    *out = 0;
    return result;
}

static esp_err_t soap(httpd_req_t *req, service_id_t service, const char *action,
                      unsigned fault, const char *fields)
{
    const char *start = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>";
    char *xml = NULL;
    int len;
    if (fault) {
        const char *description = "Action Failed";
        if (fault == 401) description = "Invalid Action";
        if (fault == 402) description = "Invalid Args";
        if (fault == 701) description = "Transition not available";
        if (fault == 706) description = "Invalid Connection Reference";
        if (fault == 718) description = "Invalid InstanceID";
        len = asprintf(&xml, "%s<s:Fault><faultcode>s:Client</faultcode>"
            "<faultstring>UPnPError</faultstring><detail>"
            "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
            "<errorCode>%u</errorCode><errorDescription>%s</errorDescription>"
            "</UPnPError></detail></s:Fault></s:Body></s:Envelope>", start, fault, description);
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else {
        len = asprintf(&xml, "%s<u:%sResponse xmlns:u=\"" SERVICE_NS "%s:1\">%s"
            "</u:%sResponse></s:Body></s:Envelope>", start, action, service_names[service],
            fields, action);
    }
    if (len < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory");
    esp_err_t err = xml_response(req, xml);
    free(xml);
    return err;
}

static void timecode(char out[20], unsigned seconds)
{
    snprintf(out, 20, "%02u:%02u:%02u", seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

static unsigned transport_action(const char *action, char *body, char **result)
{
    if (!strcmp(action, "SetAVTransportURI")) {
        char *uri = malloc(MEDIA_URI_MAX);
        if (!uri) return 501;
        bool ok = tag_value(body, "CurrentURI", uri, MEDIA_URI_MAX) && player_set_uri(uri);
        free(uri);
        return ok ? 0 : 402;
    }
    if (!strcmp(action, "Play")) {
        char speed[12];
        if (!tag_value(body, "Speed", speed, sizeof(speed)) || strcmp(speed, "1")) return 402;
        return player_play() ? 0 : 701;
    }
    if (!strcmp(action, "Pause")) return player_pause() ? 0 : 701;
    if (!strcmp(action, "Stop")) {
        player_stop();
        return 0;
    }
    player_snapshot_t *state = malloc(sizeof(*state));
    if (!state) return 501;
    player_snapshot(state);
    char duration[20], position[20];
    timecode(duration, state->duration);
    timecode(position, state->position);
    char *uri = escape_xml(state->uri);
    if (!uri) {
        free(state);
        return 501;
    }
    const char *current = player_state_name(state->state);
    int len = -1;
    unsigned fault = 0;
    if (!strcmp(action, "GetTransportInfo")) {
        len = asprintf(result, "<CurrentTransportState>%s</CurrentTransportState>"
            "<CurrentTransportStatus>%s</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>",
            current, state->failed ? "ERROR_OCCURRED" : "OK");
    } else if (!strcmp(action, "GetPositionInfo")) {
        len = asprintf(result, "<Track>%u</Track><TrackDuration>%s</TrackDuration>"
            "<TrackMetaData></TrackMetaData><TrackURI>%s</TrackURI><RelTime>%s</RelTime>"
            "<AbsTime>%s</AbsTime><RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
            state->uri[0] != 0, duration, uri, position, position);
    } else if (!strcmp(action, "GetMediaInfo")) {
        len = asprintf(result, "<NrTracks>%u</NrTracks><MediaDuration>%s</MediaDuration>"
            "<CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
            "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
            "<PlayMedium>%s</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
            "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
            state->uri[0] != 0, duration, uri, state->uri[0] ? "NETWORK" : "NONE");
    } else if (!strcmp(action, "GetDeviceCapabilities")) {
        len = asprintf(result, "<PlayMedia>NETWORK</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
            "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
    } else if (!strcmp(action, "GetTransportSettings")) {
        len = asprintf(result, "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
    } else if (!strcmp(action, "GetCurrentTransportActions")) {
        const char *actions = state->state == PLAYING ? "Pause,Stop" :
                              state->state == PAUSED ? "Play,Stop" :
                              state->state == TRANSITIONING ? "Stop" : state->uri[0] ? "Play" : "";
        len = asprintf(result, "<Actions>%s</Actions>", actions);
    } else {
        fault = 401;
    }
    free(uri);
    free(state);
    return fault ? fault : len < 0 ? 501 : 0;
}

static unsigned rendering_action(const char *action, const char *body, char **result)
{
    if (!strcmp(action, "ListPresets")) {
        return asprintf(result, "<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>") < 0 ? 501 : 0;
    }
    char value[24];
    if (!strcmp(action, "SelectPreset")) {
        if (!tag_value(body, "PresetName", value, sizeof(value)) || strcmp(value, "FactoryDefaults")) return 402;
        player_set_volume(100);
        player_set_mute(false);
        return 0;
    }
    if (!tag_value(body, "Channel", value, sizeof(value)) || strcmp(value, "Master")) return 402;
    if (!strcmp(action, "SetVolume")) {
        if (!tag_value(body, "DesiredVolume", value, sizeof(value)) || !*value ||
            strspn(value, "0123456789") != strlen(value) || strlen(value) > 3) return 402;
        unsigned volume = atoi(value);
        if (volume > 100) return 402;
        player_set_volume(volume);
        return 0;
    }
    if (!strcmp(action, "SetMute")) {
        if (!tag_value(body, "DesiredMute", value, sizeof(value))) return 402;
        if (!strcmp(value, "1") || !strcasecmp(value, "true")) player_set_mute(true);
        else if (!strcmp(value, "0") || !strcasecmp(value, "false")) player_set_mute(false);
        else return 402;
        return 0;
    }
    player_snapshot_t *state = malloc(sizeof(*state));
    if (!state) return 501;
    player_snapshot(state);
    int len = -1;
    if (!strcmp(action, "GetVolume")) len = asprintf(result, "<CurrentVolume>%u</CurrentVolume>", state->volume);
    if (!strcmp(action, "GetMute")) len = asprintf(result, "<CurrentMute>%d</CurrentMute>", state->muted);
    free(state);
    return len >= 0 ? 0 : 401;
}

static unsigned connection_action(const char *action, const char *body, char **result)
{
    int len;
    if (!strcmp(action, "GetProtocolInfo")) {
        len = asprintf(result, "<Source></Source><Sink>" SINK_PROTOCOLS "</Sink>");
    } else if (!strcmp(action, "GetCurrentConnectionIDs")) {
        len = asprintf(result, "<ConnectionIDs>0</ConnectionIDs>");
    } else if (!strcmp(action, "GetCurrentConnectionInfo")) {
        char id[16];
        if (!tag_value(body, "ConnectionID", id, sizeof(id)) || strcmp(id, "0")) return 706;
        len = asprintf(result, "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
            "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
            "<PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction><Status>OK</Status>");
    } else {
        return 401;
    }
    return len < 0 ? 501 : 0;
}

static esp_err_t soap_handler(httpd_req_t *req)
{
    service_id_t service = (uintptr_t)req->user_ctx;
    char action_header[128], prefix[96];
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", action_header, sizeof(action_header)) != ESP_OK) {
        return soap(req, service, "", 401, "");
    }
    snprintf(prefix, sizeof(prefix), SERVICE_NS "%s:1#", service_names[service]);
    char *action = action_header;
    if (*action == '"') action++;
    size_t len = strlen(action);
    if (len && action[len - 1] == '"') action[len - 1] = 0;
    if (strncmp(action, prefix, strlen(prefix))) return soap(req, service, "", 401, "");
    action += strlen(prefix);
    if (!*action || strspn(action, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") != strlen(action)) {
        return soap(req, service, "", 401, "");
    }

    if (!req->content_len || req->content_len > 32768) return soap(req, service, "", 402, "");
    char *body = malloc(req->content_len + 1);
    if (!body) return soap(req, service, "", 501, "");
    size_t total = 0;
    while (total < req->content_len) {
        int bytes = httpd_req_recv(req, body + total, req->content_len - total);
        if (bytes <= 0) {
            free(body);
            return ESP_FAIL;
        }
        total += bytes;
    }
    body[total] = 0;
    char *result = NULL;
    unsigned fault;
    if (service != CONNECTION) {
        char id[16];
        fault = !tag_value(body, "InstanceID", id, sizeof(id)) ? 402 :
                strcmp(id, "0") ? 718 : 0;
        if (!fault) {
            fault = service == AV ? transport_action(action, body, &result) :
                                    rendering_action(action, body, &result);
        }
    } else {
        fault = connection_action(action, body, &result);
    }
    free(body);
    esp_err_t err = soap(req, service, action, fault, result ? result : "");
    free(result);
    return err;
}

static bool callback_from_peer(httpd_req_t *req, char *value, char out[256])
{
    if (strncmp(value, "<http://", 8)) return false;
    char *end = strchr(value + 8, '>');
    if (!end || (size_t)(end - value - 1) >= 256) return false;
    *end = 0;
    char *address = value + 8;
    size_t count = strspn(address, "0123456789.");
    if (count < 7 || count >= INET_ADDRSTRLEN ||
        (address[count] != ':' && address[count] != '/')) return false;
    char ip[INET_ADDRSTRLEN];
    memcpy(ip, address, count);
    ip[count] = 0;
    struct in_addr parsed;
    struct sockaddr_in peer = {0};
    socklen_t size = sizeof(peer);
    if (inet_pton(AF_INET, ip, &parsed) != 1 ||
        getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &size) ||
        peer.sin_family != AF_INET || peer.sin_addr.s_addr != parsed.s_addr) return false;
    if (address[count] == ':') {
        char *after;
        unsigned port = strtoul(address + count + 1, &after, 10);
        if (!port || port > 65535 || *after != '/') return false;
    }
    if (strchr(value + 1, '\r') || strchr(value + 1, '\n')) return false;
    strlcpy(out, value + 1, 256);
    return true;
}

static esp_err_t gena_handler(httpd_req_t *req)
{
    service_id_t service = (uintptr_t)req->user_ctx;
    char sid[48], callback[300], verified[256], nt[24], timeout[32];
    bool has_sid = httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid)) == ESP_OK;
    unsigned seconds = 1800;
    if (httpd_req_get_hdr_value_str(req, "TIMEOUT", timeout, sizeof(timeout)) == ESP_OK &&
        !strncasecmp(timeout, "Second-", 7)) {
        char *after;
        unsigned long requested = strtoul(timeout + 7, &after, 10);
        if (!*after && requested && requested < seconds) seconds = requested;
    }
    if (!has_sid && (req->method != HTTP_SUBSCRIBE ||
        httpd_req_get_hdr_value_str(req, "NT", nt, sizeof(nt)) != ESP_OK ||
        strcasecmp(nt, "upnp:event") ||
        httpd_req_get_hdr_value_str(req, "CALLBACK", callback, sizeof(callback)) != ESP_OK ||
        !callback_from_peer(req, callback, verified))) {
        httpd_resp_set_status(req, "412 Precondition Failed");
        return httpd_resp_sendstr(req, "");
    }

    int64_t now = esp_timer_get_time();
    subscription_t *slot = NULL;
    xSemaphoreTake(subscription_lock, portMAX_DELAY);
    for (unsigned i = 0; i < MAX_SUBSCRIPTIONS; i++) {
        subscription_t *current = &subscriptions[i];
        if (has_sid && current->expiry > now && current->service == service &&
            !strcmp(current->sid, sid)) {
            slot = current;
            break;
        }
        if (!has_sid && !slot && current->expiry <= now) slot = current;
    }
    if (slot && req->method == HTTP_UNSUBSCRIBE) slot->expiry = 0;
    if (slot && req->method == HTTP_SUBSCRIBE) {
        if (!has_sid) {
            *slot = (subscription_t){ .service = service };
            uint32_t random[4];
            esp_fill_random(random, sizeof(random));
            snprintf(slot->sid, sizeof(slot->sid), "uuid:%08" PRIx32 "-%04x-%04x-%04x-%04x%08" PRIx32,
                random[0], (unsigned)(random[1] >> 16), (unsigned)(random[1] & 0xffff),
                (unsigned)(random[2] >> 16), (unsigned)(random[2] & 0xffff), random[3]);
            strlcpy(slot->callback, verified, sizeof(slot->callback));
        }
        slot->expiry = now + (int64_t)seconds * 1000000;
        strlcpy(sid, slot->sid, sizeof(sid));
    }
    xSemaphoreGive(subscription_lock);
    if (!slot) {
        httpd_resp_set_status(req, "412 Precondition Failed");
    } else if (req->method == HTTP_SUBSCRIBE) {
        snprintf(timeout, sizeof(timeout), "Second-%u", seconds);
        httpd_resp_set_hdr(req, "SID", sid);
        httpd_resp_set_hdr(req, "TIMEOUT", timeout);
    }
    return httpd_resp_sendstr(req, "");
}

static void notify_event(const subscription_t *sub, const player_snapshot_t *state)
{
    char *raw = NULL, *fields = NULL, *payload = NULL;
    if (sub->service == CONNECTION) {
        if (asprintf(&fields,
            "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
            "<e:property><SinkProtocolInfo>" SINK_PROTOCOLS "</SinkProtocolInfo></e:property>"
            "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property>") < 0) return;
    } else {
        int len;
        if (sub->service == AV) {
            char *uri = escape_xml(state->uri);
            if (!uri) return;
            len = asprintf(&raw, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
                "<InstanceID val=\"0\"><TransportState val=\"%s\"/>"
                "<TransportStatus val=\"%s\"/><AVTransportURI val=\"%s\"/></InstanceID></Event>",
                player_state_name(state->state), state->failed ? "ERROR_OCCURRED" : "OK", uri);
            free(uri);
        } else {
            len = asprintf(&raw, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
                "<InstanceID val=\"0\"><Volume channel=\"Master\" val=\"%u\"/>"
                "<Mute channel=\"Master\" val=\"%d\"/></InstanceID></Event>", state->volume, state->muted);
        }
        if (len < 0) return;
        char *escaped = escape_xml(raw);
        free(raw);
        if (!escaped) return;
        len = asprintf(&fields, "<e:property><LastChange>%s</LastChange></e:property>", escaped);
        free(escaped);
        if (len < 0) return;
    }
    int len = asprintf(&payload, "<?xml version=\"1.0\"?>"
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">%s</e:propertyset>", fields);
    free(fields);
    if (len < 0) return;
    const esp_http_client_config_t config = {
        .url = sub->callback, .method = HTTP_METHOD_NOTIFY, .timeout_ms = 700,
        .disable_auto_redirect = true
    };
    esp_http_client_handle_t http = esp_http_client_init(&config);
    if (http) {
        char seq[16];
        snprintf(seq, sizeof(seq), "%" PRIu32, sub->sequence);
        esp_http_client_set_header(http, "NT", "upnp:event");
        esp_http_client_set_header(http, "NTS", "upnp:propchange");
        esp_http_client_set_header(http, "SID", sub->sid);
        esp_http_client_set_header(http, "SEQ", seq);
        esp_http_client_set_header(http, "Content-Type", "text/xml; charset=utf-8");
        esp_http_client_set_post_field(http, payload, len);
        esp_http_client_perform(http);
        esp_http_client_cleanup(http);
    }
    free(payload);
}

static void event_task(void *unused)
{
    player_snapshot_t *previous = calloc(1, sizeof(*previous));
    player_snapshot_t *current = calloc(1, sizeof(*current));
    if (!previous || !current) {
        free(previous);
        free(current);
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        player_snapshot(current);
        bool changed[SERVICE_COUNT] = {
            current->state != previous->state || current->failed != previous->failed ||
            strcmp(current->uri, previous->uri),
            current->volume != previous->volume || current->muted != previous->muted,
            false
        };
        for (unsigned i = 0; i < MAX_SUBSCRIPTIONS; i++) {
            xSemaphoreTake(subscription_lock, portMAX_DELAY);
            subscription_t sub = subscriptions[i];
            bool send = sub.expiry > esp_timer_get_time() &&
                        (sub.sequence == 0 || changed[sub.service]);
            if (send) subscriptions[i].sequence = sub.sequence == UINT32_MAX ? 1 : sub.sequence + 1;
            xSemaphoreGive(subscription_lock);
            if (send) notify_event(&sub, current);
        }
        *previous = *current;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void discovery_types(const char *types[6])
{
    types[0] = "upnp:rootdevice";
    types[1] = uuid;
    types[2] = DEVICE_TYPE;
    types[3] = SERVICE_NS "AVTransport:1";
    types[4] = SERVICE_NS "RenderingControl:1";
    types[5] = SERVICE_NS "ConnectionManager:1";
}

static void ssdp_packet(int fd, const struct sockaddr_in *destination, uint32_t ip,
                        const char *type, bool notify)
{
    char packet[604], address[INET_ADDRSTRLEN];
    struct in_addr local = { .s_addr = ip };
    if (!inet_ntop(AF_INET, &local, address, sizeof(address))) return;
    bool own = !strcmp(type, uuid);
    int len;
    if (notify) {
        len = snprintf(packet, sizeof(packet),
            "NOTIFY * HTTP/1.1\r\nHOST: " SSDP_GROUP ":1900\r\n"
            "CACHE-CONTROL: max-age=180\r\nLOCATION: http://%s/dmr.xml\r\n"
            "NT: %s\r\nNTS: ssdp:alive\r\nSERVER: " SERVER_ID "\r\nUSN: %s%s%s\r\n\r\n",
            address, type, uuid, own ? "" : "::", own ? "" : type);
    } else {
        len = snprintf(packet, sizeof(packet),
            "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=180\r\nEXT:\r\n"
            "LOCATION: http://%s/dmr.xml\r\nSERVER: " SERVER_ID "\r\nST: %s\r\nUSN: %s%s%s\r\n\r\n",
            address, type, uuid, own ? "" : "::", own ? "" : type);
    }
    if (len > 0 && len < sizeof(packet)) {
        sendto(fd, packet, len, 0, (const struct sockaddr *)destination, sizeof(*destination));
    }
}

static const char *ssdp_header(char *message, const char *name, char *out, size_t size)
{
    const char *line = strchr(message, '\n');
    size_t wanted = strlen(name);
    while (line && *++line && *line != '\r' && *line != '\n') {
        const char *end = strchr(line, '\n');
        if (!end) break;
        if ((size_t)(end - line) > wanted && !strncasecmp(line, name, wanted) && line[wanted] == ':') {
            line += wanted + 1;
            while (*line == ' ' || *line == '\t') line++;
            while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t len = end - line;
            if (len >= size) return NULL;
            memcpy(out, line, len);
            out[len] = 0;
            return out;
        }
        line = end;
    }
    return NULL;
}

static void ssdp_task(void *unused)
{
    const char *types[6];
    discovery_types(types);
    struct sockaddr_in multicast = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    inet_pton(AF_INET, SSDP_GROUP, &multicast.sin_addr);
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) goto exited;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const struct timeval timeout = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(SSDP_PORT) };
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) goto exited;

    esp_netif_ip_info_t active[2] = {0};
    int64_t advertise_at[2] = {0};
    while (true) {
        int64_t now = esp_timer_get_time();
        for (unsigned i = 0; i < 2; i++) {
            esp_netif_ip_info_t info = {0};
            if (esp_netif_is_netif_up(interfaces[i])) esp_netif_get_ip_info(interfaces[i], &info);
            if (active[i].ip.addr != info.ip.addr) {
                struct ip_mreq member = { .imr_multiaddr = multicast.sin_addr };
                member.imr_interface.s_addr = active[i].ip.addr;
                if (member.imr_interface.s_addr) {
                    setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &member, sizeof(member));
                }
                active[i] = (esp_netif_ip_info_t){0};
                member.imr_interface.s_addr = info.ip.addr;
                if (info.ip.addr && !setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &member, sizeof(member))) {
                    active[i] = info;
                }
                advertise_at[i] = 0;
            }
            struct in_addr ip = { .s_addr = active[i].ip.addr };
            if (ip.s_addr && now >= advertise_at[i]) {
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ip, sizeof(ip));
                for (unsigned t = 0; t < 6; t++) ssdp_packet(fd, &multicast, ip.s_addr, types[t], true);
                advertise_at[i] = now + INT64_C(85) * 1000000;
            }
        }

        char request[980];
        struct sockaddr_in peer;
        socklen_t size = sizeof(peer);
        int received = recvfrom(fd, request, sizeof(request) - 1, 0, (struct sockaddr *)&peer, &size);
        if (received <= 0) continue;
        request[received] = 0;
        if (strncasecmp(request, "M-SEARCH * HTTP/1.1", 19)) continue;
        char search[100], man[32];
        if (!ssdp_header(request, "ST", search, sizeof(search)) ||
            !ssdp_header(request, "MAN", man, sizeof(man)) ||
            (strcasecmp(man, "\"ssdp:discover\"") && strcasecmp(man, "ssdp:discover"))) continue;
        uint32_t ip = 0;
        for (unsigned i = 0; i < 2; i++) {
            uint32_t match = active[i].ip.addr;
            uint32_t mask = active[i].netmask.addr;
            if (match && (!ip || ((peer.sin_addr.s_addr ^ match) & mask) == 0)) ip = match;
        }
        if (!ip) continue;
        for (unsigned t = 0; t < 6; t++) {
            if (!strcasecmp(search, "ssdp:all") || !strcasecmp(search, types[t])) {
                ssdp_packet(fd, &peer, ip, types[t], false);
            }
        }
    }

exited:
    ESP_LOGE(TAG, "Could not bind SSDP socket (errno %d)", errno);
    if (fd >= 0) close(fd);
    vTaskDelete(NULL);
}

esp_err_t dlna_start(httpd_handle_t server, const char *name, const uint8_t mac[6],
                     esp_netif_t *station, esp_netif_t *hotspot)
{
    display_name = name;
    snprintf(uuid, sizeof(uuid), "uuid:97f3c2bc-ed08-4b44-86a3-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    interfaces[0] = station;
    interfaces[1] = hotspot;
    subscription_lock = xSemaphoreCreateMutex();
    if (!subscription_lock) return ESP_ERR_NO_MEM;

    httpd_uri_t device = { .uri = "/dmr.xml", .method = HTTP_GET, .handler = description_handler };
    esp_err_t err = httpd_register_uri_handler(server, &device);
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < SERVICE_COUNT; i++) {
        const httpd_uri_t paths[] = {
            { descriptions[i], HTTP_GET, scpd_handler, (void *)(uintptr_t)i },
            { controls[i], HTTP_POST, soap_handler, (void *)(uintptr_t)i },
            { events[i], HTTP_SUBSCRIBE, gena_handler, (void *)(uintptr_t)i },
            { events[i], HTTP_UNSUBSCRIBE, gena_handler, (void *)(uintptr_t)i }
        };
        for (unsigned p = 0; p < sizeof(paths) / sizeof(*paths); p++) {
            err = httpd_register_uri_handler(server, &paths[p]);
            if (err != ESP_OK) return err;
        }
    }
    if (xTaskCreate(ssdp_task, "dlna_ssdp", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(event_task, "dlna_events", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "DLNA renderer %s", uuid);
    return ESP_OK;
}
