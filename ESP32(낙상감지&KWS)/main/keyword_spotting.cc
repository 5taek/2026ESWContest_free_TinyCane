#include "keyword_spotting.h"

#include <cstdio>
#include <cstring>

#include "ai_inference_lock.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "keyword_spotting";
// Short timeout used while idle, watching for the first bytes of a new
// audio window. Keeps trigger latency low without a dedicated GPIO signal.
constexpr int kStartPollTimeoutMs = 30;
// Longer timeout used once a window is actively being received, so brief
// gaps in the USB stream don't abort a window that's still coming in.
constexpr int kUsbReadTimeoutMs = 100;
constexpr int kUsbChunkBytes = 512;
// If the stream goes quiet this many consecutive reads (~2s) mid-window,
// treat it as an aborted transfer and go back to watching for a new one.
constexpr int kMaxConsecutiveEmptyReads = 20;
constexpr float kResultThreshold = 0.60f;
#if FALL_DETECTION_TEST_MODE
constexpr size_t kReceiveProgressBytes = 4096;
#endif

constexpr size_t kSampleCount = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
constexpr size_t kAudioBytes = kSampleCount * sizeof(int16_t);
static float inference_audio[kSampleCount];

int GetSignalData(size_t offset, size_t length, float* output) {
  std::memcpy(output, inference_audio + offset, length * sizeof(float));
  return 0;
}

void WriteUsb(const char* message) {
  usb_serial_jtag_write_bytes(message, std::strlen(message),
                              pdMS_TO_TICKS(200));
}

// Blocks in short polling ticks until the first bytes of a new window
// arrive on USB (no GPIO trigger needed -- data arriving IS the trigger).
// Returns the number of bytes already captured at the front of the buffer.
size_t WaitForWindowStart(uint8_t* destination) {
  while (true) {
    const int received = usb_serial_jtag_read_bytes(
        destination, kUsbChunkBytes, pdMS_TO_TICKS(kStartPollTimeoutMs));
    if (received > 0) {
      return static_cast<size_t>(received);
    }
  }
}

// Fills the rest of the window after WaitForWindowStart captured the first
// chunk. Returns false if the stream goes quiet for too long before a full
// window arrives; the caller discards the partial window and starts over.
bool FinishReceivingWindow(uint8_t* destination, size_t already_received) {
  size_t total = already_received;
  int consecutive_empty = 0;
#if FALL_DETECTION_TEST_MODE
  size_t next_progress = kReceiveProgressBytes;
#endif
  while (total < kAudioBytes) {
    const size_t remaining = kAudioBytes - total;
    const int requested = remaining > kUsbChunkBytes
                              ? kUsbChunkBytes
                              : static_cast<int>(remaining);
    const int received = usb_serial_jtag_read_bytes(
        destination + total, requested, pdMS_TO_TICKS(kUsbReadTimeoutMs));
    if (received > 0) {
      total += static_cast<size_t>(received);
      consecutive_empty = 0;
#if FALL_DETECTION_TEST_MODE
      if (total >= next_progress && total < kAudioBytes) {
        ESP_LOGI(kTag, "audio receiving: %u/%u bytes",
                 static_cast<unsigned>(total),
                 static_cast<unsigned>(kAudioBytes));
        while (next_progress <= total) {
          next_progress += kReceiveProgressBytes;
        }
      }
#endif
    } else if (++consecutive_empty >= kMaxConsecutiveEmptyReads) {
      ESP_LOGW(kTag, "audio stream went quiet mid-window; discarding");
      return false;
    }
  }
#if FALL_DETECTION_TEST_MODE
  ESP_LOGI(kTag, "audio receive complete: %u/%u bytes",
           static_cast<unsigned>(total),
           static_cast<unsigned>(kAudioBytes));
#endif
  return true;
}

void RunKeywordInference() {
#if FALL_DETECTION_TEST_MODE
  ESP_LOGI(kTag, "inference started");
  ESP_LOGI(kTag, "heap before inference: free=%u internal=%u largest_internal_block=%u",
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
#endif
  auto* pcm_audio = reinterpret_cast<int16_t*>(inference_audio);
  for (size_t i = kSampleCount; i-- > 0;) {
    inference_audio[i] = static_cast<float>(pcm_audio[i]);
  }

  signal_t signal = {};
  signal.total_length = kSampleCount;
  signal.get_data = &GetSignalData;
  ei_impulse_result_t result = {};

  // The fall detector holds this same lock for one Invoke() (~10ms,
  // comfortably bounded well under a second) once per ~10s classification
  // window, so real contention here is rare and brief. Dropping an already
  // fully-captured 1s utterance is strictly worse than waiting for it --
  // the user would have to repeat themselves, costing more than the wait
  // ever could. Timeout is generous-but-bounded rather than infinite, so a
  // future bug elsewhere that leaks the lock can't wedge this task forever.
  // Must stay comfortably under the RPi5 client's own post-send response
  // timeout (rasberripi/keyword_spotting_link.py RESULT_TIMEOUT_S = 10s) --
  // otherwise the host gives up and reports a failed recognition before we
  // ever get a chance to reply.
  if (!LockAiInference(pdMS_TO_TICKS(8000))) {
    ESP_LOGW(kTag, "inference busy; dropping audio window");
    WriteUsb("KWS_BUSY\n");
    return;
  }
  const EI_IMPULSE_ERROR error = run_classifier(&signal, &result, false);
  UnlockAiInference();

  if (error != EI_IMPULSE_OK) {
    ESP_LOGE(kTag, "run_classifier failed: %d", error);
    WriteUsb("KWS_ERROR\n");
    return;
  }

  size_t best_index = 0;
  float best_score = result.classification[0].value;
  for (size_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; ++i) {
    if (result.classification[i].value > best_score) {
      best_score = result.classification[i].value;
      best_index = i;
    }
  }

  const char* label = result.classification[best_index].label;
  char response[128];
  std::snprintf(response, sizeof(response), "RESULT:%s (%.2f) %s\n", label,
                best_score,
                best_score >= kResultThreshold ? "ACCEPTED" : "REJECTED");
  WriteUsb(response);
#if FALL_DETECTION_TEST_MODE
  ESP_LOGI(kTag, "inference result: label=%s score=%.3f decision=%s", label,
           best_score,
           best_score >= kResultThreshold ? "ACCEPTED" : "REJECTED");
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; ++i) {
    ESP_LOGI(kTag, "%s=%.3f", result.classification[i].label,
             result.classification[i].value);
  }
  ESP_LOGI(kTag, "timing dsp=%d ms classification=%d ms anomaly=%d ms",
           result.timing.dsp, result.timing.classification,
           result.timing.anomaly);
#endif
  ESP_LOGI(kTag, "label=%s score=%.3f", label, best_score);
}

void KeywordTask(void*) {
  auto* destination = reinterpret_cast<uint8_t*>(inference_audio);
  while (true) {
    const size_t received = WaitForWindowStart(destination);
    ESP_LOGI(kTag, "audio window started");
    if (!FinishReceivingWindow(destination, received)) {
      continue;
    }
    RunKeywordInference();
  }
}
}  // namespace

void InitializeKeywordSpotting() {
  usb_serial_jtag_driver_config_t config = {};
  config.tx_buffer_size = 1024;
  // One audio window is exactly kAudioBytes (32000 B for a 16 kHz/1 s
  // window). A same-size buffer leaves only ~768 B of headroom, and the RX
  // ISR silently drops packets on overflow with no host backpressure -- one
  // lost byte desyncs every sample boundary in every window after it.
  // A full second window of extra headroom (2x) turned out to be more than
  // this board's internal heap (no PSRAM) can spare at boot -- it starved
  // this very allocation with ESP_ERR_NO_MEM before Wi-Fi even connected.
  // +25% is still a large improvement over the original ~2% margin and
  // fits comfortably.
  config.rx_buffer_size = kAudioBytes + kAudioBytes / 4;
  const esp_err_t result = usb_serial_jtag_driver_install(&config);
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(kTag, "USB Serial/JTAG driver init failed: %s",
             esp_err_to_name(result));
    return;
  }

  // Without this, console output (ESP_LOGI/printf from every other task)
  // keeps writing straight to the TX FIFO register instead of through this
  // driver's ISR-fed ring buffer, racing with our own write_bytes() calls on
  // the same hardware FIFO and garbling whichever side loses the race. This
  // routes ALL console output through the same ring buffer so log lines and
  // KWS responses interleave cleanly instead of corrupting each other.
  usb_serial_jtag_vfs_use_driver();

  // Pinned to core 1 and given a priority above the alert sender: Wi-Fi and
  // the fall-detection loop both live on core 0, and the sender's TLS
  // handshakes are CPU-heavy enough to stall keyword latency if they can
  // preempt this task.
  xTaskCreatePinnedToCore(KeywordTask, "keyword_spotting", 12288, nullptr, 6,
                          nullptr, 1);
}
