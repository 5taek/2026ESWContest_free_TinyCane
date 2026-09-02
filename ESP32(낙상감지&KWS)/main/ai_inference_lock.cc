#include "ai_inference_lock.h"

#include "esp_log.h"

namespace {
constexpr char kTag[] = "ai_inference_lock";
SemaphoreHandle_t inference_mutex = nullptr;
}

void InitializeAiInferenceLock() {
  if (inference_mutex == nullptr) {
    inference_mutex = xSemaphoreCreateMutex();
    if (inference_mutex == nullptr) {
      ESP_LOGE(kTag, "failed to create inference mutex");
    }
  }
}

bool LockAiInference(TickType_t timeout_ticks) {
  return inference_mutex != nullptr &&
         xSemaphoreTake(inference_mutex, timeout_ticks) == pdTRUE;
}

void UnlockAiInference() {
  if (inference_mutex != nullptr) {
    xSemaphoreGive(inference_mutex);
  }
}
