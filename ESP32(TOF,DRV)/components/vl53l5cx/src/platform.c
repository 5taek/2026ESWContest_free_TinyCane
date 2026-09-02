// platform.c
#include "platform.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS  2000  // 펌웨어 로드 중 폴링 시간 확보

static const char *TAG = "VL53L5CX_PAL";

uint8_t VL53L5CX_RdByte(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_value)
{
    return VL53L5CX_RdMulti(p_platform, RegisterAdress, p_value, 1);
}

uint8_t VL53L5CX_WrByte(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t value)
{
    return VL53L5CX_WrMulti(p_platform, RegisterAdress, &value, 1);
}

uint8_t VL53L5CX_RdMulti(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_values,
        uint32_t size)
{
    // VL53L5CX는 16bit 레지스터 주소 (Big-endian)
    uint8_t reg_buf[2] = {
        (uint8_t)((RegisterAdress >> 8) & 0xFF),
        (uint8_t)( RegisterAdress       & 0xFF)
    };

    esp_err_t err = i2c_master_transmit_receive(
        p_platform->handle,
        reg_buf, 2,
        p_values, size,
        I2C_TIMEOUT_MS);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RdMulti fail: reg=0x%04X size=%lu err=%d",
                 RegisterAdress, (unsigned long)size, err);
        return 255;
    }
    return 0;
}

uint8_t VL53L5CX_WrMulti(
        VL53L5CX_Platform *p_platform,
        uint16_t RegisterAdress,
        uint8_t *p_values,
        uint32_t size)
{
    // [reg_hi, reg_lo, data...] 한 번에 송신해야 함 (Repeated start 없이)
    uint8_t *buf = (uint8_t *)malloc(size + 2);
    if (buf == NULL) {
        ESP_LOGE(TAG, "WrMulti malloc fail (size=%lu)", (unsigned long)size);
        return 255;
    }

    buf[0] = (uint8_t)((RegisterAdress >> 8) & 0xFF);
    buf[1] = (uint8_t)( RegisterAdress       & 0xFF);
    memcpy(&buf[2], p_values, size);

    esp_err_t err = i2c_master_transmit(
        p_platform->handle,
        buf, size + 2,
        I2C_TIMEOUT_MS);

    free(buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WrMulti fail: reg=0x%04X size=%lu err=%d",
                 RegisterAdress, (unsigned long)size, err);
        return 255;
    }
    return 0;
}

uint8_t VL53L5CX_Reset_Sensor(VL53L5CX_Platform *p_platform)
{
    // 우리는 LPn(XSHUT) 핀을 별도로 토글해서 리셋하므로 여기선 stub
    (void)p_platform;
    return 0;
}

void VL53L5CX_SwapBuffer(uint8_t *buffer, uint16_t size)
{
    // 4바이트 단위 엔디안 스왑 (ST 권장 구현 그대로)
    uint32_t i, tmp;
    for (i = 0; i < size; i += 4) {
        tmp = (((uint32_t)buffer[i    ]) << 24)
            | (((uint32_t)buffer[i + 1]) << 16)
            | (((uint32_t)buffer[i + 2]) <<  8)
            | (((uint32_t)buffer[i + 3])      );
        memcpy(&buffer[i], &tmp, 4);
    }
}

uint8_t VL53L5CX_WaitMs(VL53L5CX_Platform *p_platform, uint32_t TimeMs)
{
    (void)p_platform;
    vTaskDelay(pdMS_TO_TICKS(TimeMs));
    return 0;
}
