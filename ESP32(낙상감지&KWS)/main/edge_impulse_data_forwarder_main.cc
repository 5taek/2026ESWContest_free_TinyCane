#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "accelerometer_handler.h"
#include "esp_log.h"

#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"

namespace {
constexpr int kSampleRateHz = 25;
constexpr int kSamplePeriodMs = 1000 / kSampleRateHz;
}  // namespace

extern "C" void app_main(void) {
  static tflite::MicroErrorReporter micro_error_reporter;
  tflite::ErrorReporter* error_reporter = &micro_error_reporter;

  if (SetupImu(error_reporter) != kTfLiteOk) {
    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  // Keep initialization diagnostics visible, then emit only numeric CSV rows.
  esp_log_level_set("*", ESP_LOG_NONE);

  float sample[kChannelNumber] = {};
  bool reset_buffer = true;

  while (true) {
    if (ReadImu(error_reporter, sample, kChannelNumber, reset_buffer)) {
      reset_buffer = false;
      std::printf("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", sample[0], sample[1],
                  sample[2], sample[3], sample[4], sample[5]);
      std::fflush(stdout);
    } else {
      reset_buffer = false;
    }

    vTaskDelay(pdMS_TO_TICKS(kSamplePeriodMs));
  }
}
