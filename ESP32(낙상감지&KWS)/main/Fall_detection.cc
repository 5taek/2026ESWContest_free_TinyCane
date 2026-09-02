#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <cstdint>

#include "esp_log.h"

#include "driver/usb_serial_jtag.h"

#include "accelerometer_handler.h"
#include "ai_inference_lock.h"
#include "fall_alert_sender.h"
#include "keyword_spotting.h"
#include "model_input_provider.h"

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern const unsigned char g_fall_model_data[];
extern const unsigned int g_fall_model_data_len;

namespace {
constexpr char kTag[] = "fall_detector";
constexpr int kTensorArenaSize = 80 * 1024;
constexpr int kLoopDelayMs = 40;
constexpr float kEventThreshold = 0.50f;

// Pickup is a post-event rule, not a model class. After an event, the cane is
// considered picked up when accelerometer motion is large enough to show that
// it is being handled again.
constexpr uint32_t kPickupIgnoreMs = 1500;
constexpr uint32_t kEmergencyAlertDelayMs = 10000;
constexpr float kPickupDeltaGThreshold = 0.30f;
constexpr float kPickupMagnitudeGThreshold = 1.35f;

// After an emergency alert fires, hold this state for this long before
// resuming normal fall monitoring instead of staying latched forever.
constexpr uint32_t kAlertCooldownMs = 10000;

// Single-byte, out-of-band signals to the RPi5 speaker controller. They are
// outside both ASCII and valid UTF-8, so they cannot be confused with the KWS
// text protocol (RESULT:/KWS_BUSY/KWS_ERROR) sent in the same direction. The
// host must read them as raw bytes from the shared USB Serial/JTAG connection.
constexpr uint8_t kFallDetectedByte = 0xFF;
constexpr uint8_t kFallResolvedByte = 0xFE;
constexpr uint8_t kFallDangerByte = 0xFD;

alignas(16) uint8_t tensor_arena[kTensorArenaSize];

tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* model_input = nullptr;
TfLiteTensor* model_output = nullptr;
bool setup_ok = false;
uint32_t loop_count = 0;

enum class PredictionKind {
  kUnknown,
  kNormal,
  kEvent,
};

enum class EventMonitorState {
  kIdle,
  kWaitingForPickup,
  kAlertSent,
};

struct CanePrediction {
  PredictionKind kind;
  int output_count;
  int top_index;
  float top_score;
  float normal_score;
  float event_score;
};

struct AccelSample {
  float x;
  float y;
  float z;
  float magnitude;
};

EventMonitorState event_monitor_state = EventMonitorState::kIdle;
uint32_t event_started_at_ms = 0;
uint32_t alert_sent_at_ms = 0;
bool has_previous_accel = false;
AccelSample previous_accel = {};

int ElementCount(const TfLiteTensor* tensor) {
  if (tensor == nullptr || tensor->dims == nullptr || tensor->dims->size <= 0) {
    return 0;
  }

  int count = 1;
  for (int i = 0; i < tensor->dims->size; ++i) {
    const int dim = tensor->dims->data[i];
    if (dim <= 0) {
      return 0;
    }
    count *= dim;
  }
  return count;
}

float TensorValueAsFloat(const TfLiteTensor* tensor, int index) {
  switch (tensor->type) {
    case kTfLiteFloat32:
      return tensor->data.f[index];
    case kTfLiteInt8:
      return (tensor->data.int8[index] - tensor->params.zero_point) *
             tensor->params.scale;
    case kTfLiteUInt8:
      return (tensor->data.uint8[index] - tensor->params.zero_point) *
             tensor->params.scale;
    default:
      return 0.0f;
  }
}

bool RegisterModelOps(tflite::MicroMutableOpResolver<20>* resolver) {
  TfLiteStatus status = kTfLiteOk;
  status = resolver->AddConv2D();
  status = status == kTfLiteOk ? resolver->AddDepthwiseConv2D() : status;
  status = status == kTfLiteOk ? resolver->AddFullyConnected() : status;
  status = status == kTfLiteOk ? resolver->AddMaxPool2D() : status;
  status = status == kTfLiteOk ? resolver->AddAveragePool2D() : status;
  status = status == kTfLiteOk ? resolver->AddSoftmax() : status;
  status = status == kTfLiteOk ? resolver->AddReshape() : status;
  status = status == kTfLiteOk ? resolver->AddQuantize() : status;
  status = status == kTfLiteOk ? resolver->AddDequantize() : status;
  status = status == kTfLiteOk ? resolver->AddAdd() : status;
  status = status == kTfLiteOk ? resolver->AddSub() : status;
  status = status == kTfLiteOk ? resolver->AddMul() : status;
  status = status == kTfLiteOk ? resolver->AddDiv() : status;
  status = status == kTfLiteOk ? resolver->AddMaximum() : status;
  status = status == kTfLiteOk ? resolver->AddRelu() : status;
  status = status == kTfLiteOk ? resolver->AddMean() : status;
  return status == kTfLiteOk;
}

bool IsSupportedTensorType(TfLiteType type) {
  return type == kTfLiteFloat32 || type == kTfLiteInt8 ||
         type == kTfLiteUInt8;
}

uint32_t NowMs() {
  return static_cast<uint32_t>(xTaskGetTickCount()) * portTICK_PERIOD_MS;
}

uint32_t ElapsedMs(uint32_t start_ms, uint32_t now_ms) {
  return now_ms - start_ms;
}

AccelSample CurrentAccelSample() {
  const float x = g_latest_accel_x;
  const float y = g_latest_accel_y;
  const float z = g_latest_accel_z;
  return {x, y, z, std::sqrt((x * x) + (y * y) + (z * z))};
}

float AccelDistanceG(const AccelSample& a, const AccelSample& b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  const float dz = a.z - b.z;
  return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

const char* PredictionKindName(PredictionKind kind) {
  switch (kind) {
    case PredictionKind::kNormal:
      return "NORMAL";
    case PredictionKind::kEvent:
      return "EVENT";
    default:
      return "UNKNOWN";
  }
}

void PrintTextAction(const char* action, const char* reason) {
#if FALL_DETECTION_TEST_MODE
  std::printf("%s: %s\n", action, reason);
  std::fflush(stdout);
#else
  (void)action;
  (void)reason;
#endif
}

void NotifyRpi(uint8_t signal, const char* action, const char* reason) {
  const int written =
      usb_serial_jtag_write_bytes(&signal, 1, pdMS_TO_TICKS(200));
  if (written != 1) {
    ESP_LOGW(kTag,
             "RPi5 signal 0x%02X did not send (wrote %d, action=%s)",
             static_cast<unsigned>(signal), written, action);
  }
  ESP_LOGW(kTag, "ACTION=%s signal=0x%02X reason=%s", action,
           static_cast<unsigned>(signal), reason);
}

void ReportAlertAction(const char* reason) {
  ESP_LOGE(kTag, "ACTION=SEND_GUARDIAN_ALERT reason=%s", reason);
  PrintTextAction("알림을 보냄", reason);
}

void ResetPostEventMonitor() {
  event_monitor_state = EventMonitorState::kIdle;
  event_started_at_ms = 0;
  alert_sent_at_ms = 0;
  has_previous_accel = false;
  previous_accel = {};
}

void StartPostEventMonitor(uint32_t now_ms, const char* reason) {
  if (event_monitor_state != EventMonitorState::kIdle) {
    return;
  }

  event_monitor_state = EventMonitorState::kWaitingForPickup;
  event_started_at_ms = now_ms;
  has_previous_accel = false;
  previous_accel = {};
  NotifyRpi(kFallDetectedByte, "RPI_FALL_DETECTED", reason);
}

bool DetectPickupMotion(uint32_t now_ms) {
  const AccelSample current = CurrentAccelSample();
  if (!has_previous_accel) {
    previous_accel = current;
    has_previous_accel = true;
    return false;
  }

  const float delta_g = AccelDistanceG(current, previous_accel);
  previous_accel = current;

  if (ElapsedMs(event_started_at_ms, now_ms) < kPickupIgnoreMs) {
    return false;
  }

  // delta_g catches direction/handling changes; magnitude catches a strong
  // lift, jerk, or impact after the initial event shock has been ignored.
  return delta_g >= kPickupDeltaGThreshold ||
         current.magnitude >= kPickupMagnitudeGThreshold;
}

// Only ever called while kWaitingForPickup (see UpdatePostEventMonitor).
void ResolveEventAsSafe(const char* reason) {
  ESP_LOGI(kTag, "event resolved without guardian alert: %s", reason);
  NotifyRpi(kFallResolvedByte, "RPI_FALL_RESOLVED", reason);
  QueueFallState("safe_fall", reason, 0.0f);
  PrintTextAction("알림 취소", reason);
  ResetPostEventMonitor();
}

void SendEmergencyAlert(uint32_t now_ms, const char* reason) {
  if (event_monitor_state == EventMonitorState::kAlertSent) {
    return;
  }

  event_monitor_state = EventMonitorState::kAlertSent;
  alert_sent_at_ms = now_ms;
  NotifyRpi(kFallDangerByte, "RPI_FALL_DANGER", reason);
  ReportAlertAction(reason);
  QueueFallState("danger_fall", reason, 1.0f);
}

void UpdatePostEventMonitor(uint32_t now_ms) {
  if (event_monitor_state == EventMonitorState::kWaitingForPickup) {
    if (DetectPickupMotion(now_ms)) {
      ResolveEventAsSafe("cane_picked_up_after_event");
      return;
    }

    if (ElapsedMs(event_started_at_ms, now_ms) >= kEmergencyAlertDelayMs) {
      SendEmergencyAlert(now_ms, "no_cane_pickup_after_event");
    }
    return;
  }

  if (event_monitor_state == EventMonitorState::kAlertSent) {
    if (ElapsedMs(alert_sent_at_ms, now_ms) >= kAlertCooldownMs) {
      ESP_LOGI(kTag, "alert cooldown elapsed; resuming fall monitoring");
      ResetPostEventMonitor();
    }
  }
}

bool ReadCanePrediction(const TfLiteTensor* output, CanePrediction* prediction) {
  if (output == nullptr || prediction == nullptr) {
    return false;
  }
  if (!IsSupportedTensorType(output->type)) {
    error_reporter->Report("Unsupported output tensor type: %d", output->type);
    return false;
  }

  *prediction = {};
  prediction->kind = PredictionKind::kUnknown;

  const int output_count = ElementCount(output);
  prediction->output_count = output_count;
  if (output_count <= 0) {
    error_reporter->Report("Bad output tensor element count: %d", output_count);
    return false;
  }

  prediction->top_index = 0;
  prediction->top_score = TensorValueAsFloat(output, 0);
  for (int i = 1; i < output_count; ++i) {
    const float score = TensorValueAsFloat(output, i);
    if (score > prediction->top_score) {
      prediction->top_score = score;
      prediction->top_index = i;
    }
  }

  if (output_count == 1) {
    const float event_score = TensorValueAsFloat(output, 0);
    prediction->event_score = event_score;
    prediction->normal_score = 1.0f - event_score;
    prediction->kind = event_score >= kEventThreshold ? PredictionKind::kEvent
                                                      : PredictionKind::kNormal;
    return true;
  }

  if (output_count == 2) {
    // Edge Impulse "Output features" order for this impulse is (event,
    // normal) -- confirmed in Create Impulse's classifier block -- so index
    // 0 is event and index 1 is normal, not the other way around.
    const float event_score = TensorValueAsFloat(output, 0);
    const float normal_score = TensorValueAsFloat(output, 1);
    prediction->normal_score = normal_score;
    prediction->event_score = event_score;
    prediction->kind = event_score >= kEventThreshold ? PredictionKind::kEvent
                                                       : PredictionKind::kNormal;
    return true;
  }

  error_reporter->Report(
      "Unsupported output tensor element count for normal/event model: %d",
      output_count);
  return false;
}

// Only arms the post-event monitor on a fresh model prediction. Called at
// most once per classification window (now every ~10s), unlike
// UpdatePostEventMonitor which must keep running every loop regardless.
void HandlePredictionAction(const CanePrediction& prediction) {
  if (prediction.kind == PredictionKind::kEvent) {
    StartPostEventMonitor(NowMs(), "event_detected");
  }
}

void LogTensorInfo(const char* name, const TfLiteTensor* tensor) {
  if (tensor == nullptr || tensor->dims == nullptr) {
    ESP_LOGE(kTag, "%s tensor is null", name);
    return;
  }

  ESP_LOGI(kTag, "%s tensor: dims_size=%d dims=[%d,%d,%d,%d] type=%d bytes=%d",
           name, tensor->dims->size,
           tensor->dims->size > 0 ? tensor->dims->data[0] : -1,
           tensor->dims->size > 1 ? tensor->dims->data[1] : -1,
           tensor->dims->size > 2 ? tensor->dims->data[2] : -1,
           tensor->dims->size > 3 ? tensor->dims->data[3] : -1,
           tensor->type, tensor->bytes);
}

void SetupFallDetector() {
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;

  if (SetupModelInputProvider(error_reporter) != kTfLiteOk) {
    error_reporter->Report("Set up IMU input provider failed");
    return;
  }

  if (g_fall_model_data_len <= 1) {
    ESP_LOGE(kTag, "models/fall_model.cc still contains the placeholder model");
    return;
  }

  model = tflite::GetModel(g_fall_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    error_reporter->Report(
        "Model schema version %d is not supported. Expected %d.",
        model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  static tflite::MicroMutableOpResolver<20> resolver;
  if (!RegisterModelOps(&resolver)) {
    error_reporter->Report("RegisterModelOps failed");
    return;
  }

  static tflite::MicroInterpreter static_interpreter(
      model, resolver, tensor_arena, kTensorArenaSize, nullptr, nullptr);
  interpreter = &static_interpreter;

  if (interpreter->AllocateTensors(false) != kTfLiteOk) {
    error_reporter->Report("AllocateTensors failed");
    return;
  }

  model_input = interpreter->input(0);
  model_output = interpreter->output(0);
  if (model_input == nullptr || model_output == nullptr) {
    error_reporter->Report("Model input/output tensor is null");
    return;
  }
  if (!IsSupportedTensorType(model_output->type)) {
    error_reporter->Report("Unsupported output tensor type: %d",
                           model_output->type);
    return;
  }

  LogTensorInfo("input", model_input);
  LogTensorInfo("output", model_output);
  setup_ok = true;
  ESP_LOGI(kTag, "event detector setup complete");
}

void RunFallDetectorOnce() {
  ++loop_count;

  // Pickup detection and the alert buzzer timeout are plain accelerometer/
  // timer checks, independent of the classifier. They must keep running
  // every loop (40ms) even though a new model prediction now only arrives
  // once per ~10s classification window.
  UpdatePostEventMonitor(NowMs());

  if (!setup_ok) {
    if ((loop_count % 100) == 0) {
      ESP_LOGW(kTag, "setup is not complete; waiting for valid model/input");
    }
    return;
  }

  if (!FillModelInput(model_input, error_reporter)) {
    if ((loop_count % 100) == 0) {
      ESP_LOGI(kTag, "model input is not ready");
    }
    return;
  }

  if (!LockAiInference(pdMS_TO_TICKS(2000))) {
    ESP_LOGW(kTag, "inference busy; skipping fall window");
    return;
  }
  const TfLiteStatus invoke_status = interpreter->Invoke();
  UnlockAiInference();
  if (invoke_status != kTfLiteOk) {
    error_reporter->Report("Invoke failed");
    return;
  }

  CanePrediction prediction = {};
  if (!ReadCanePrediction(model_output, &prediction)) {
    return;
  }

  ESP_LOGI(kTag,
           "state=%s outputs=%d top=%d:%.3f normal=%.3f event=%.3f",
           PredictionKindName(prediction.kind), prediction.output_count,
           prediction.top_index, prediction.top_score, prediction.normal_score,
           prediction.event_score);

  HandlePredictionAction(prediction);
}
}  // namespace

extern "C" void app_main(void) {
#if FALL_DETECTION_TEST_MODE
  esp_log_level_set("*", ESP_LOG_INFO);
#else
  // The console and KWS protocol share USB Serial/JTAG. In run mode, keep
  // stdout/ESP_LOG traffic off that channel so the RPi sees only packets
  // written explicitly by keyword_spotting.cc.
  esp_log_level_set("*", ESP_LOG_NONE);
#endif
  InitializeAiInferenceLock();
  InitializeFallAlertSender();
  SetupFallDetector();
  InitializeKeywordSpotting();
  // vTaskDelayUntil keeps the period at a true kLoopDelayMs regardless of how
  // long RunFallDetectorOnce takes (I2C reads, mutex waits, logging) -- with
  // plain vTaskDelay the real interval is kLoopDelayMs *plus* that work, so
  // the sampling rate silently drifts below the 25 Hz the model was trained
  // and windowed at.
  TickType_t last_wake_time = xTaskGetTickCount();
  while (true) {
    RunFallDetectorOnce();
    vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(kLoopDelayMs));
  }
}
