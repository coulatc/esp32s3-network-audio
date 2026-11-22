// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "rtsp_server.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_receiver.h"
#include "audio_output.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_handlers.h"
#include "rtsp_message.h"
#include "rtsp_rsa.h"

#include "ntp_clock.h"
#include "ptp_clock.h"
#include "playback_events.h"
#include "settings.h"
#include "dacp_client.h"

static const char *TAG = "rtsp_server";

#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE   ((size_t)256 * 1024)

#define RTSP_CLIENT_COUNT 4
#define CLIENT_STACK_SIZE 8192
#define SERVER_STACK_SIZE 4096

static int server_socket = -1;
static TaskHandle_t server_task_handle = NULL;
static atomic_bool server_running;
static SemaphoreHandle_t request_lock;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

// Client slot for tracking connections
typedef struct {
  rtsp_conn_t *conn;
  TaskHandle_t task;
  int socket;
  atomic_bool should_stop;
} client_slot_t;

static client_slot_t clients[RTSP_CLIENT_COUNT];
static client_slot_t *playback_owner;

// Flag set by the play/pause button to tell the grace period loop
// to send a DACP resume command and keep waiting for reconnect.
static atomic_bool s_resume_requested;

// Dispatch, stream handoff and cleanup share this lock. Socket reads and
// the legacy DACP grace period stay outside it.
bool rtsp_playback_is_owner(const rtsp_conn_t *conn) {
  return playback_owner && playback_owner->conn == conn;
}

static void stop_playback(rtsp_conn_t *conn) {
  ptp_clock_remove_peer(conn);
  audio_receiver_stop();
  audio_receiver_end_session();
  audio_output_flush();
  ntp_clock_stop();
  ptp_clock_end_session();
  ptp_clock_init(); // Resume passive PTP if legacy RAOP had stopped it.
}

static void clear_playback_owner(void) {
  playback_owner = NULL;
  dacp_clear_session();
  playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_DISCONNECTED,
                       NULL);
}

bool rtsp_playback_claim(rtsp_conn_t *conn) {
  if (rtsp_playback_is_owner(conn)) return true;
  client_slot_t *slot = NULL;
  for (size_t i = 0; i < RTSP_CLIENT_COUNT; i++) {
    if (clients[i].conn == conn) {
      slot = &clients[i];
      break;
    }
  }
  if (!slot || slot->should_stop) return false;
  if (playback_owner) {
    ESP_LOGI(TAG, "Replacing playback session after a new audio request");
    playback_owner->should_stop = true;
    if (playback_owner->socket >= 0) {
      shutdown(playback_owner->socket, SHUT_RDWR);
      stop_playback(playback_owner->conn);
    }
    clear_playback_owner();
  }
  playback_owner = slot;
  if (conn->dacp_id[0] && conn->active_remote[0]) {
    dacp_set_session(conn->dacp_id, conn->active_remote, conn->client_ip);
  }
  playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_CONNECTED, NULL);
  return true;
}

void airplay_set_volume(float volume_db) {
  if (!request_lock) return;
  xSemaphoreTake(request_lock, portMAX_DELAY);
  if (playback_owner) rtsp_conn_set_volume(playback_owner->conn, volume_db);
  xSemaphoreGive(request_lock);
}

int32_t airplay_get_volume_q15(void) {
  if (!request_lock) return 16384;
  xSemaphoreTake(request_lock, portMAX_DELAY);
  int32_t result = playback_owner ? rtsp_conn_get_volume_q15(playback_owner->conn) : 16384;
  xSemaphoreGive(request_lock);
  return result;
}

void rtsp_server_request_resume(void) {
  s_resume_requested = true;
}

uint16_t airplay_rtsp_port(void) {
  return settings_airplay_v1() ? 5000 : 7000;
}

// Helper to grow buffer
static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size,
                            size_t data_len) {
  (void)old_size;
  uint8_t *new_buf =
      heap_caps_malloc(new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!new_buf) {
    new_buf = malloc(new_size);
  }
  if (!new_buf) {
    return NULL;
  }
  if (old_buf && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  free(old_buf);
  return new_buf;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (!header_end) {
      break;
    }

    size_t header_len = (size_t)(header_end - buffer) + 4;
    char *header_str = malloc(header_len + 1);
    if (!header_str) {
      *buf_len = 0;
      break;
    }
    memcpy(header_str, buffer, header_len);
    header_str[header_len] = '\0';

    int content_len = rtsp_parse_content_length(header_str);
    if (content_len < 0) {
      content_len = 0;
    }

    size_t total_len = header_len + (size_t)content_len;
    if (total_len > RTSP_BUFFER_LARGE || *buf_len < total_len) {
      free(header_str);
      if (total_len > RTSP_BUFFER_LARGE) {
        *buf_len = 0;
      }
      break;
    }

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary (buffer capacity > total_len).
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    xSemaphoreTake(request_lock, portMAX_DELAY);
    if (!slot->should_stop) {
      rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    }
    xSemaphoreGive(request_lock);
    buffer[total_len] = saved;
    free(header_str);

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

static void close_client_slot(client_slot_t *slot) {
  if (slot->socket >= 0) close(slot->socket);
  slot->socket = -1;
  slot->conn = NULL;
  slot->should_stop = false;
  slot->task = NULL;
}

// Client task
static void client_task(void *pvParameters) {
  int slot_idx = (int)(intptr_t)pvParameters;
  client_slot_t *slot = &clients[slot_idx];
  rtsp_conn_t *conn = rtsp_conn_create();
  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  uint8_t *buffer = conn ? malloc(buf_capacity) : NULL;
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate connection resources");
    rtsp_conn_free(conn);
    xSemaphoreTake(request_lock, portMAX_DELAY);
    close_client_slot(slot);
    xSemaphoreGive(request_lock);
    vTaskDelete(NULL);
    return;
  }

  // Get client IP address for timing requests.
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) == 0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
  }
  xSemaphoreTake(request_lock, portMAX_DELAY);
  slot->conn = conn;
  xSemaphoreGive(request_lock);

  size_t buf_len = 0;

  // Socket timeout for stop signal responsiveness
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(slot->socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  // Disable Nagle: RTSP control commands (volume, pause) are tiny and must
  // not wait for coalescing/delayed-ACK, which adds tens to hundreds of ms of
  // latency to every command on this connection.
  int nodelay = 1;
  setsockopt(slot->socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        if (buf_len >= buf_capacity - 1024) {
          size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                            : buf_capacity * 2;
          if (new_cap > RTSP_BUFFER_LARGE) {
            goto cleanup;
          }
          uint8_t *new_buf =
              grow_buffer(buffer, buf_capacity, new_cap, buf_len);
          if (!new_buf) {
            goto cleanup;
          }
          buffer = new_buf;
          buf_capacity = new_cap;
        }

        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, buffer + buf_len, buf_capacity - buf_len);
        if (block_len <= 0) {
          if (slot->should_stop || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            goto cleanup;
          }
          continue;
        }

        buf_len += (size_t)block_len;
        process_rtsp_buffer(slot, buffer, &buf_len);
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len >= buf_capacity - 1024) {
      size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                        : buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) {
        break;
      }
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        break;
      }
      buffer = new_buf;
      buf_capacity = new_cap;
    }

    ssize_t recv_len =
        recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
  }

cleanup:
  ESP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  free(buffer);

  xSemaphoreTake(request_lock, portMAX_DELAY);
  close(slot->socket);
  slot->socket = -1;
  bool grace = false;
  if (playback_owner == slot) {
    stop_playback(conn);
    grace = conn->protocol_version == 1 && conn->dacp_id[0] &&
            conn->active_remote[0] && !slot->should_stop;
    if (grace) {
      s_resume_requested = false;
      playback_events_emit(PLAYBACK_SOURCE_AIRPLAY, PLAYBACK_EVENT_PAUSED,
                           NULL);
    } else {
      clear_playback_owner();
    }
  }
  xSemaphoreGive(request_lock);

  // Preserve classic RAOP pause detection without blocking the control server.
  if (grace) {
    bool stay = true;
    int polls = 6; // First allow three seconds for mDNS to settle.
    while (stay && !slot->should_stop) {
      for (int i = 0; i < polls && !slot->should_stop && !s_resume_requested; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      if (slot->should_stop) break;
      if (atomic_exchange(&s_resume_requested, false)) {
        dacp_send_playpause();
      } else {
        stay = dacp_probe_service();
      }
      polls = 10;
    }
    xSemaphoreTake(request_lock, portMAX_DELAY);
    if (playback_owner == slot) clear_playback_owner();
    xSemaphoreGive(request_lock);
  }

  // Event sockets, crypto and other connection state belong to this client.
  // The slot is unavailable for reuse until its local cleanup completes.
  xSemaphoreTake(request_lock, portMAX_DELAY);
  slot->conn = NULL;
  xSemaphoreGive(request_lock);
  rtsp_conn_free(conn);
  xSemaphoreTake(request_lock, portMAX_DELAY);
  close_client_slot(slot);
  xSemaphoreGive(request_lock);
  vTaskDelete(NULL);
}

static void server_task(void *pvParameters) {
  (void)pvParameters;

  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  for (size_t i = 0; i < RTSP_CLIENT_COUNT; i++) clients[i].socket = -1;

  server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(airplay_rtsp_port());

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  if (listen(server_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", airplay_rtsp_port());
  server_running = true;

  while (server_running) {
    int new_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      continue;
    }

    // Accept discovery, pairing and auxiliary control sockets without
    // touching an existing playback session. Only audio SETUP claims it.
    xSemaphoreTake(request_lock, portMAX_DELAY);
    int free_slot = -1;
    for (int i = 0; i < RTSP_CLIENT_COUNT; i++) {
      if (clients[i].task == NULL) {
        free_slot = i;
        break;
      }
    }
    if (free_slot >= 0) {
      client_slot_t *slot = &clients[free_slot];
      slot->socket = new_socket;
      slot->should_stop = false;
      if (xTaskCreate(client_task, "rtsp_client", CLIENT_STACK_SIZE,
                      (void *)(intptr_t)free_slot, 5, &slot->task) != pdPASS) {
        close_client_slot(slot);
      }
    } else {
      ESP_LOGW(TAG, "No free RTSP client slot");
      close(new_socket);
    }
    xSemaphoreGive(request_lock);
  }

  bool pending;
  do {
    pending = false;
    xSemaphoreTake(request_lock, portMAX_DELAY);
    for (size_t i = 0; i < RTSP_CLIENT_COUNT; i++) {
      client_slot_t *slot = &clients[i];
      if (slot->task != NULL) {
        pending = true;
        slot->should_stop = true;
        if (slot->socket >= 0) shutdown(slot->socket, SHUT_RDWR);
      }
    }
    xSemaphoreGive(request_lock);
    if (pending) vTaskDelay(pdMS_TO_TICKS(20));
  } while (pending);

  if (server_socket >= 0) {
    close(server_socket);
    server_socket = -1;
  }

  server_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool rtsp_server_wait_for_task_stopped(int timeout_ticks) {
  while (server_task_handle != NULL && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return server_task_handle == NULL;
}

esp_err_t rtsp_server_start(void) {
  if (server_task_handle != NULL) {
    if (server_running) {
      return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "RTSP server task still stopping, waiting");
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGE(TAG, "Previous RTSP server task did not stop");
      return ESP_ERR_INVALID_STATE;
    }
  }

  if (!request_lock) request_lock = xSemaphoreCreateMutex();
  if (!request_lock) return ESP_ERR_NO_MEM;

  // Parsing the key and deriving the first blinding pair costs the best part
  // of a second; a sender waiting on its OPTIONS response gives up before
  // that. Do it before the listener exists so it cannot land mid-request.
  rsa_init();

  BaseType_t task_ret =
      xTaskCreate(server_task, "rtsp_server", SERVER_STACK_SIZE, NULL, 5,
                  &server_task_handle);
  if (task_ret != pdPASS || server_task_handle == NULL) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

void rtsp_server_stop(void) {
  server_running = false;

  if (server_socket >= 0) {
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    server_socket = -1;
  }

  if (server_task_handle != NULL) {
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGW(TAG, "RTSP server task did not exit within timeout");
    }
  }
}
