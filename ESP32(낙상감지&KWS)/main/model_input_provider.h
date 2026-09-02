#ifndef FALL_DETECTION_MAIN_MODEL_INPUT_PROVIDER_H_
#define FALL_DETECTION_MAIN_MODEL_INPUT_PROVIDER_H_

#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/core/api/error_reporter.h"

TfLiteStatus SetupModelInputProvider(tflite::ErrorReporter* error_reporter);

// Read the IMU, maintain a sliding window, and fill one model input tensor.
// Return true when input is ready for Invoke(), or false while the window warms up.
bool FillModelInput(TfLiteTensor* input,
                    tflite::ErrorReporter* error_reporter);

#endif  // FALL_DETECTION_MAIN_MODEL_INPUT_PROVIDER_H_
