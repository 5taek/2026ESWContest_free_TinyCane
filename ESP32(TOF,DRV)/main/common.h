#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "vl53l5cx_api.h"
#include "usb_cdc_comm.h"
#include "TOF_task.h"
#include "DRV.h"
#include "I2C_SET.h"

// ════════════════════════════════════════════════════════════
// 핀 정의 (필요시 변경)
// ════════════════════════════════════════════════════════════
#define I2C_SDA_PIN     5
#define I2C_SCL_PIN     6
#define I2C1_SDA_PIN    3
#define I2C1_SCL_PIN    4
#define TOF1_LPN_PIN    GPIO_NUM_1
#define TOF2_LPN_PIN    GPIO_NUM_2

#ifdef __cplusplus
}
#endif

#endif // COMMON_H