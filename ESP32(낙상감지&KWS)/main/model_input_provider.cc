#include "model_input_provider.h"

#include <cmath>
#include <cstdint>

#include "accelerometer_handler.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "model_input";
constexpr int kMaxFeatureLength = 256 * kChannelNumber;

float feature_buffer[kMaxFeatureLength] = {0.0f};
bool input_provider_ready = false;

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

bool IsSupportedInputType(TfLiteType type) {
  return type == kTfLiteFloat32 || type == kTfLiteInt8 ||
         type == kTfLiteUInt8;
}

void QuantizeToInt8(const float* source, int length, TfLiteTensor* input) {
  const float scale = input->params.scale;
  const int zero_point = input->params.zero_point;
  for (int i = 0; i < length; ++i) {
    int q = static_cast<int>(std::round(source[i] / scale)) + zero_point;
    if (q < -128) q = -128;
    if (q > 127) q = 127;
    input->data.int8[i] = static_cast<int8_t>(q);
  }
}

void QuantizeToUInt8(const float* source, int length, TfLiteTensor* input) {
  const float scale = input->params.scale;
  const int zero_point = input->params.zero_point;
  for (int i = 0; i < length; ++i) {
    int q = static_cast<int>(std::round(source[i] / scale)) + zero_point;
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    input->data.uint8[i] = static_cast<uint8_t>(q);
  }
}
}  // namespace

TfLiteStatus SetupModelInputProvider(tflite::ErrorReporter* error_reporter) {
  const TfLiteStatus status = SetupImu(error_reporter);
  input_provider_ready = status == kTfLiteOk;
  return status;
}

bool FillModelInput(TfLiteTensor* input,
                    tflite::ErrorReporter* error_reporter) {
  if (!input_provider_ready) {
    error_reporter->Report("Model input provider is not ready");
    return false;
  }
  if (input == nullptr) {
    error_reporter->Report("Model input tensor is null");
    return false;
  }
  if (!IsSupportedInputType(input->type)) {
    error_reporter->Report("Unsupported input tensor type: %d", input->type);
    return false;
  }

  const int feature_length = ElementCount(input);
  if (feature_length <= 0 || feature_length > kMaxFeatureLength) {
    error_reporter->Report("Unsupported input feature length: %d (max=%d)",
                           feature_length, kMaxFeatureLength);
    return false;
  }
  if ((feature_length % kChannelNumber) != 0) {
    error_reporter->Report("Input feature length must be a multiple of %d: %d",
                           kChannelNumber, feature_length);
    return false;
  }
  if ((input->type == kTfLiteInt8 || input->type == kTfLiteUInt8) &&
      input->params.scale == 0.0f) {
    error_reporter->Report("Invalid input quantization scale: 0");
    return false;
  }

  if (!ReadImu(error_reporter, feature_buffer, feature_length, false)) {
    ESP_LOGD(kTag,
             "waiting for IMU window: accel[g]=%.3f,%.3f,%.3f "
             "gyro[dps]=%.3f,%.3f,%.3f",
             g_latest_accel_x, g_latest_accel_y, g_latest_accel_z,
             g_latest_gyro_x, g_latest_gyro_y, g_latest_gyro_z);
    return false;
  }

  if (input->type == kTfLiteFloat32) {
    for (int i = 0; i < feature_length; ++i) {
      input->data.f[i] = feature_buffer[i];
    }
  } else if (input->type == kTfLiteInt8) {
    QuantizeToInt8(feature_buffer, feature_length, input);
  } else {
    QuantizeToUInt8(feature_buffer, feature_length, input);
  }

  ESP_LOGD(kTag,
           "filled input from IMU: accel[g]=%.3f,%.3f,%.3f "
           "gyro[dps]=%.3f,%.3f,%.3f",
           g_latest_accel_x, g_latest_accel_y, g_latest_accel_z,
           g_latest_gyro_x, g_latest_gyro_y, g_latest_gyro_z);
  return true;
}
