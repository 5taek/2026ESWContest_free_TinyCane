/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "accelerometer_handler.h"

#include <cstdint>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr gpio_num_t kI2cSdaPin = GPIO_NUM_5;
constexpr gpio_num_t kI2cSclPin = GPIO_NUM_6;
constexpr uint32_t kI2cFreqHz = 100000;

constexpr uint8_t kLsm9ds1AgAddrCandidates[] = {0x6A, 0x6B};
constexpr uint8_t kRegWhoAmI = 0x0F;
constexpr uint8_t kExpectedWhoAmIAg = 0x68;
constexpr uint8_t kRegCtrlReg1G = 0x10;
constexpr uint8_t kRegOutXLG = 0x18;
constexpr uint8_t kRegCtrlReg4 = 0x1E;
constexpr uint8_t kRegCtrlReg5Xl = 0x1F;
constexpr uint8_t kRegCtrlReg6Xl = 0x20;
constexpr uint8_t kRegOutXL = 0x28;

constexpr int kMaxSamplesPerInference = 256;
// FS_XL = +-2g (0.061 mg/LSB per LSM9DS1 datasheet). Reverted from +-8g:
// the current trained model was collected/trained with +-2g data (impact
// spikes clipped at 2.0g), so the runtime scale must match that, not the
// physically "ideal" wider range. Revisit once data is recollected at +-8g.
constexpr float kAccelScaleGPerLsb = 0.000061f;
constexpr float kGyroScaleDpsPerLsb = 0.00875f;
constexpr int kI2cTimeoutMs = 100;
constexpr char kTag[] = "imu_handler";

bool g_imu_ready = false;
uint8_t g_ag_addr = 0;
i2c_master_bus_handle_t g_i2c_bus = nullptr;
i2c_master_dev_handle_t g_ag_dev = nullptr;
float g_feature_buffer[kMaxSamplesPerInference * kChannelNumber] = {0.0f};
int g_sample_count = 0;

esp_err_t WriteReg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(dev, payload, sizeof(payload), kI2cTimeoutMs);
}

esp_err_t ReadRegs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t* data,
                   size_t len) {
  return i2c_master_transmit_receive(dev, &reg, 1, data, len, kI2cTimeoutMs);
}

int16_t ReadInt16Le(const uint8_t* data) {
  return static_cast<int16_t>((data[1] << 8) | data[0]);
}

bool WriteChecked(tflite::ErrorReporter* error_reporter, uint8_t reg,
                  uint8_t value, const char* name) {
  esp_err_t err = WriteReg(g_ag_dev, reg, value);
  if (err != ESP_OK) {
    error_reporter->Report("Failed to configure LSM9DS1 %s: %d", name,
                           static_cast<int>(err));
    ESP_LOGE(kTag, "%s write failed: %d", name, static_cast<int>(err));
    return false;
  }
  ESP_LOGI(kTag, "%s configured", name);
  return true;
}

bool DetectLsm9ds1AgAddress(tflite::ErrorReporter* error_reporter,
                            uint8_t* out_addr) {
  for (uint8_t addr : kLsm9ds1AgAddrCandidates) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = kI2cFreqHz;

    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev) != ESP_OK) continue;

    uint8_t who = 0;
    esp_err_t read_err = ReadRegs(dev, kRegWhoAmI, &who, 1);
    ESP_LOGI(kTag, "Probe AG addr 0x%02X -> err=%d, who=0x%02X", addr,
             static_cast<int>(read_err), who);
    if (read_err == ESP_OK && who == kExpectedWhoAmIAg) {
      g_ag_dev = dev;
      *out_addr = addr;
      return true;
    }

    i2c_master_bus_rm_device(dev);
  }

  error_reporter->Report("LSM9DS1 AG not found at 0x6A/0x6B");
  return false;
}
}  // namespace

int begin_index = 0;
float g_latest_accel_x = 0.0f;
float g_latest_accel_y = 0.0f;
float g_latest_accel_z = 0.0f;
float g_latest_gyro_x = 0.0f;
float g_latest_gyro_y = 0.0f;
float g_latest_gyro_z = 0.0f;

TfLiteStatus SetupImu(tflite::ErrorReporter* error_reporter) {
  if (g_imu_ready && g_ag_dev != nullptr) {
    ESP_LOGW(kTag, "SetupImu called again, reusing existing I2C device");
    return kTfLiteOk;
  }

  ESP_LOGI(kTag, "SetupImu start (SDA=%d, SCL=%d)",
           static_cast<int>(kI2cSdaPin), static_cast<int>(kI2cSclPin));

  // Diagnostic: read the raw pin levels as plain GPIO inputs BEFORE the I2C
  // driver claims them. With the LSM9DS1 idle and pull-ups healthy, both
  // lines should read HIGH (~3.3V). A line reading LOW here means it is
  // physically stuck/shorted/miswired -- that is a wiring/hardware fault,
  // not something the I2C driver or a bus-recovery routine can fix.
  {
    gpio_config_t probe_cfg = {};
    probe_cfg.pin_bit_mask = (1ULL << kI2cSdaPin) | (1ULL << kI2cSclPin);
    probe_cfg.mode = GPIO_MODE_INPUT;
    probe_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&probe_cfg);
    vTaskDelay(pdMS_TO_TICKS(5));  // let the pull-ups settle
    // ESP_LOGW (not I) so this shows up even at the default WARN log level
    // without needing a TEST_MODE build.
    ESP_LOGW(kTag, "raw pin levels before I2C init: SDA=%d SCL=%d (1=HIGH/idle, 0=LOW/stuck)",
             gpio_get_level(kI2cSdaPin), gpio_get_level(kI2cSclPin));
  }

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = kI2cSdaPin;
  bus_cfg.scl_io_num = kI2cSclPin;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;

  esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_i2c_bus);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    error_reporter->Report("i2c_new_master_bus failed: %d", static_cast<int>(err));
    ESP_LOGE(kTag, "i2c_new_master_bus failed: %d", static_cast<int>(err));
    return kTfLiteError;
  }
  if (err == ESP_ERR_INVALID_STATE && g_i2c_bus == nullptr) {
    error_reporter->Report("i2c bus in invalid state and handle is null");
    ESP_LOGE(kTag, "i2c bus in invalid state and handle is null");
    return kTfLiteError;
  }
  ESP_LOGI(kTag, "I2C bus init done (err=%d)", static_cast<int>(err));

  if (!DetectLsm9ds1AgAddress(error_reporter, &g_ag_addr)) {
    ESP_LOGE(kTag, "LSM9DS1 AG detect failed");
    return kTfLiteError;
  }
  if (g_ag_dev == nullptr) {
    error_reporter->Report("AG device handle is null after detection");
    ESP_LOGE(kTag, "AG device handle is null after detection");
    return kTfLiteError;
  }
  ESP_LOGI(kTag, "LSM9DS1 AG detected at 0x%02X", g_ag_addr);

  // LSM9DS1 gyro: enable X/Y/Z, ODR_G=119Hz, FS_G=245 dps.
  if (!WriteChecked(error_reporter, kRegCtrlReg4, 0x38, "CTRL_REG4")) {
    return kTfLiteError;
  }
  if (!WriteChecked(error_reporter, kRegCtrlReg1G, 0x60, "CTRL_REG1_G")) {
    return kTfLiteError;
  }

  // LSM9DS1 accelerometer: enable X/Y/Z, ODR_XL=119Hz, FS_XL=+-2g.
  if (!WriteChecked(error_reporter, kRegCtrlReg5Xl, 0x38, "CTRL_REG5_XL")) {
    return kTfLiteError;
  }
  if (!WriteChecked(error_reporter, kRegCtrlReg6Xl, 0x60, "CTRL_REG6_XL")) {
    return kTfLiteError;
  }

  std::memset(g_feature_buffer, 0, sizeof(g_feature_buffer));
  g_sample_count = 0;
  begin_index = 0;
  g_imu_ready = true;
  ESP_LOGI(kTag, "SetupImu done");
  return kTfLiteOk;
}

bool ReadImu(tflite::ErrorReporter* error_reporter, float* input, int length,
             bool reset_buffer) {
  if (!g_imu_ready) return false;
  if (length <= 0 || (length % kChannelNumber) != 0) {
    error_reporter->Report("Unexpected input length: %d (must be multiple of %d)",
                           length, kChannelNumber);
    return false;
  }

  const int required_samples = length / kChannelNumber;
  if (required_samples > kMaxSamplesPerInference) {
    error_reporter->Report(
        "Input samples exceed buffer: %d > %d", required_samples,
        kMaxSamplesPerInference);
    return false;
  }

  if (reset_buffer) {
    std::memset(g_feature_buffer, 0, sizeof(g_feature_buffer));
    g_sample_count = 0;
    begin_index = 0;
  }

  uint8_t gyro_raw[6] = {0};
  uint8_t gyro_reg = static_cast<uint8_t>(kRegOutXLG | 0x80);
  esp_err_t err = ReadRegs(g_ag_dev, gyro_reg, gyro_raw, sizeof(gyro_raw));
  if (err != ESP_OK) {
    error_reporter->Report("I2C read gyro failed: %d", static_cast<int>(err));
    return false;
  }

  uint8_t accel_raw[6] = {0};
  uint8_t accel_reg = static_cast<uint8_t>(kRegOutXL | 0x80);
  err = ReadRegs(g_ag_dev, accel_reg, accel_raw, sizeof(accel_raw));
  if (err != ESP_OK) {
    error_reporter->Report("I2C read accel failed: %d", static_cast<int>(err));
    return false;
  }

  const int16_t gyro_x_raw = ReadInt16Le(&gyro_raw[0]);
  const int16_t gyro_y_raw = ReadInt16Le(&gyro_raw[2]);
  const int16_t gyro_z_raw = ReadInt16Le(&gyro_raw[4]);
  const int16_t accel_x_raw = ReadInt16Le(&accel_raw[0]);
  const int16_t accel_y_raw = ReadInt16Le(&accel_raw[2]);
  const int16_t accel_z_raw = ReadInt16Le(&accel_raw[4]);

  const float accel_x_g = accel_x_raw * kAccelScaleGPerLsb;
  const float accel_y_g = accel_y_raw * kAccelScaleGPerLsb;
  const float accel_z_g = accel_z_raw * kAccelScaleGPerLsb;
  const float gyro_x_dps = gyro_x_raw * kGyroScaleDpsPerLsb;
  const float gyro_y_dps = gyro_y_raw * kGyroScaleDpsPerLsb;
  const float gyro_z_dps = gyro_z_raw * kGyroScaleDpsPerLsb;

  std::memmove(g_feature_buffer, g_feature_buffer + kChannelNumber,
               sizeof(float) * (length - kChannelNumber));
  const int write_index = length - kChannelNumber;
  g_feature_buffer[write_index + 0] = accel_x_g;
  g_feature_buffer[write_index + 1] = accel_y_g;
  g_feature_buffer[write_index + 2] = accel_z_g;
  g_feature_buffer[write_index + 3] = gyro_x_dps;
  g_feature_buffer[write_index + 4] = gyro_y_dps;
  g_feature_buffer[write_index + 5] = gyro_z_dps;

  g_latest_accel_x = accel_x_g;
  g_latest_accel_y = accel_y_g;
  g_latest_accel_z = accel_z_g;
  g_latest_gyro_x = gyro_x_dps;
  g_latest_gyro_y = gyro_y_dps;
  g_latest_gyro_z = gyro_z_dps;

  if (g_sample_count < required_samples) {
    g_sample_count++;
    begin_index = g_sample_count * kChannelNumber;
    return false;
  }

  std::memcpy(input, g_feature_buffer, sizeof(float) * length);

  if (required_samples > 1) {
    // Non-overlapping windows for full-length classification: once a
    // complete window has been consumed, start collecting the next one
    // from scratch instead of sliding forward by a single sample. This
    // matches how the model was trained/validated in Edge Impulse Studio
    // (window size == window increase == 10s). The single-sample streaming
    // path used by the data forwarder (required_samples == 1) is untouched.
    std::memset(g_feature_buffer, 0, sizeof(g_feature_buffer));
    g_sample_count = 0;
  }
  begin_index = 0;
  return true;
}

TfLiteStatus SetupAccelerometer(tflite::ErrorReporter* error_reporter) {
  return SetupImu(error_reporter);
}

bool ReadAccelerometer(tflite::ErrorReporter* error_reporter, float* input,
                       int length, bool reset_buffer) {
  return ReadImu(error_reporter, input, length, reset_buffer);
}
