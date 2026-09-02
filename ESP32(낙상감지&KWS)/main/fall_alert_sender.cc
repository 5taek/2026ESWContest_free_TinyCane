#include "fall_alert_sender.h"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace {
constexpr char kTag[] = "fall_alert_sender";
constexpr EventBits_t kWifiConnected = BIT0;

// WIFI_EVENT_STA_DISCONNECTED used to call esp_wifi_connect() again
// immediately and unconditionally, with no delay of any kind. If the AP is
// out of range or unstable (WIFI_REASON_NO_AP_FOUND et al.), that turns
// into a near-continuous scan/connect RF duty cycle with no idle gap -- a
// real, sustained heat/power cost, not just log spam. Back off
// exponentially instead, capped, and reset once a connection succeeds.
constexpr uint32_t kReconnectBaseDelayMs = 1000;
constexpr uint32_t kReconnectMaxDelayMs = 30000;
constexpr uint32_t kReconnectMaxShift = 5;  // 1000ms << 5 = 32000ms, already past the cap

struct AlertMessage {
  char state[20];
  char reason[64];
  float score;
};

EventGroupHandle_t wifi_events = nullptr;
QueueHandle_t alert_queue = nullptr;
esp_timer_handle_t reconnect_timer = nullptr;
uint32_t reconnect_attempt = 0;

void ReconnectTimerCallback(void*) {
  esp_wifi_connect();
}

void ScheduleReconnect() {
  const uint32_t shift = reconnect_attempt < kReconnectMaxShift
                              ? reconnect_attempt
                              : kReconnectMaxShift;
  uint32_t delay_ms = kReconnectBaseDelayMs << shift;
  if (delay_ms > kReconnectMaxDelayMs) {
    delay_ms = kReconnectMaxDelayMs;
  }
  ++reconnect_attempt;
  ESP_LOGW(kTag, "reconnecting in %u ms (attempt %u)",
           static_cast<unsigned>(delay_ms),
           static_cast<unsigned>(reconnect_attempt));
  esp_timer_stop(reconnect_timer);  // no-op if not currently running
  esp_timer_start_once(reconnect_timer,
                        static_cast<uint64_t>(delay_ms) * 1000);
}

void WifiEvent(void*, esp_event_base_t base, int32_t id, void* event_data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    ESP_LOGI(kTag, "STA started; connecting to SSID=\"%s\"",
             CONFIG_FALL_WIFI_SSID);
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(wifi_events, kWifiConnected);
    const auto* disconnected =
        static_cast<wifi_event_sta_disconnected_t*>(event_data);
    ESP_LOGW(kTag, "disconnected, reason=%d",
             disconnected != nullptr ? disconnected->reason : -1);
    ScheduleReconnect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(kTag, "connected, ip=" IPSTR,
             IP2STR(&got_ip->ip_info.ip));
    reconnect_attempt = 0;
    xEventGroupSetBits(wifi_events, kWifiConnected);
  }
}

bool PostAlert(const AlertMessage& alert) {
  const EventBits_t bits = xEventGroupWaitBits(
      wifi_events, kWifiConnected, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
  if ((bits & kWifiConnected) == 0) {
    ESP_LOGE(kTag, "Wi-Fi unavailable");
    return false;
  }

  char json[256];
  const int length = std::snprintf(
      json, sizeof(json),
      "{\"deviceId\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\","
      "\"score\":%.4f,\"deviceTimeMs\":%llu}",
      CONFIG_FALL_FIREBASE_DEVICE_ID, alert.state, alert.reason, alert.score,
      static_cast<unsigned long long>(esp_timer_get_time() / 1000));
  if (length <= 0 || length >= static_cast<int>(sizeof(json))) return false;

  esp_http_client_config_t config = {};
  config.url = CONFIG_FALL_FIREBASE_FUNCTION_URL;
  config.method = HTTP_METHOD_POST;
  // esp-tls's connect timeout covers DNS + TCP + the full TLS handshake as
  // one lump sum (esp_tls.c's esp_tls_conn_new_sync loop), not a per-step
  // budget. On a phone personal hotspot the cellular backhaul alone can eat
  // several seconds, and 160 MHz (reverted from 240 MHz for heat) plus
  // PSRAM's extra access latency make the handshake's crypto/parsing slower
  // too -- 10s was getting exhausted by genuinely-still-progressing
  // connections, not stuck ones. This only raises the worst-case latency of
  // one attempt; SenderTask's own 3-attempt retry loop is unchanged.
  config.timeout_ms = 20000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return false;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "X-Device-Token",
                             CONFIG_FALL_FIREBASE_DEVICE_TOKEN);
  esp_http_client_set_post_field(client, json, length);
  const esp_err_t result = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || status < 200 || status >= 300) {
    ESP_LOGE(kTag, "Firebase failed: error=%s HTTP=%d",
             esp_err_to_name(result), status);
    return false;
  }
  ESP_LOGI(kTag, "Firebase accepted state=%s HTTP=%d", alert.state, status);
  return true;
}

void SenderTask(void*) {
  AlertMessage alert = {};
  while (true) {
    if (xQueueReceive(alert_queue, &alert, portMAX_DELAY) != pdTRUE) continue;
    for (int attempt = 1; attempt <= 3; ++attempt) {
      if (PostAlert(alert)) break;
      ESP_LOGW(kTag, "send retry %d/3", attempt);
      vTaskDelay(pdMS_TO_TICKS(attempt * 2000));
    }
  }
}
}  // namespace

void InitializeFallAlertSender() {
  if (std::strlen(CONFIG_FALL_WIFI_SSID) == 0 ||
      std::strlen(CONFIG_FALL_FIREBASE_FUNCTION_URL) == 0 ||
      std::strlen(CONFIG_FALL_FIREBASE_DEVICE_TOKEN) == 0) {
    ESP_LOGE(kTag, "Wi-Fi/Firebase configuration incomplete");
    return;
  }
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_events = xEventGroupCreate();
  alert_queue = xQueueCreate(4, sizeof(AlertMessage));

  esp_timer_create_args_t reconnect_timer_args = {};
  reconnect_timer_args.callback = &ReconnectTimerCallback;
  reconnect_timer_args.dispatch_method = ESP_TIMER_TASK;
  reconnect_timer_args.name = "wifi_reconnect";
  ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &reconnect_timer));

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEvent, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEvent, nullptr));
  wifi_config_t station = {};
  strlcpy(reinterpret_cast<char*>(station.sta.ssid), CONFIG_FALL_WIFI_SSID,
          sizeof(station.sta.ssid));
  strlcpy(reinterpret_cast<char*>(station.sta.password),
          CONFIG_FALL_WIFI_PASSWORD, sizeof(station.sta.password));
  station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Kept on core 0 next to the Wi-Fi stack, and below the keyword-spotting
  // task's priority: alert delivery already retries, so it must not preempt
  // latency-sensitive inference with its CPU-heavy TLS handshakes.
  xTaskCreatePinnedToCore(SenderTask, "fall_alert_sender", 8192, nullptr, 3,
                          nullptr, 0);
}

bool QueueFallState(const char* state, const char* reason, float score) {
  if (alert_queue == nullptr) return false;
  AlertMessage alert = {};
  strlcpy(alert.state, state, sizeof(alert.state));
  strlcpy(alert.reason, reason, sizeof(alert.reason));
  alert.score = score;
  return xQueueSend(alert_queue, &alert, 0) == pdTRUE;
}
