#include "common.h"

#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_USB_CDC = "USB_CDC";
static bool s_usb_ready = false;
// Core0(TOF)가 쓰는 USB 쓰기를 보호 (지금은 그리드 전송뿐이지만, 나중에 다른 송신자가
// 추가돼도 바이트가 섞이지 않도록 뮤텍스는 유지).
static SemaphoreHandle_t s_usb_write_mutex = nullptr;

static esp_err_t usb_write_all(const uint8_t *data, size_t length)
{
    size_t written_total = 0;

    while (written_total < length) {
        const int written = usb_serial_jtag_write_bytes(
            data + written_total,
            length - written_total,
            pdMS_TO_TICKS(20));

        if (written <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        written_total += static_cast<size_t>(written);
    }

    return ESP_OK;
}

esp_err_t UsbCdcComm_Init(void)
{
    if (s_usb_ready) {
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_USB_CDC, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_usb_write_mutex == nullptr) {
        s_usb_write_mutex = xSemaphoreCreateMutex();
    }

    s_usb_ready = true;
    ESP_LOGI(TAG_USB_CDC, "USB CDC (USB Serial/JTAG) ready");
    return ESP_OK;
}

esp_err_t UsbCdcComm_SendRawGrid(const int16_t distance_mm[SMARTCANE_RAW_GRID_CELLS])
{
    if (!s_usb_ready) {
        const esp_err_t init_err = UsbCdcComm_Init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    smartcane_raw_grid_packet_t packet;
    packet.msg_type = SMARTCANE_MSG_TYPE_RAW_GRID;
    memcpy(packet.distance_mm, distance_mm, sizeof(packet.distance_mm));

    if (s_usb_write_mutex != nullptr) {
        xSemaphoreTake(s_usb_write_mutex, portMAX_DELAY);
    }
    const esp_err_t send_err = usb_write_all(reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
    if (s_usb_write_mutex != nullptr) {
        xSemaphoreGive(s_usb_write_mutex);
    }

    return send_err;
}

esp_err_t UsbCdcComm_SendHapticFired(uint8_t height, uint8_t direction, int16_t distance_mm)
{
    if (!s_usb_ready) {
        const esp_err_t init_err = UsbCdcComm_Init();
        if (init_err != ESP_OK) {
            return init_err;
        }
    }

    const smartcane_haptic_fired_packet_t packet = {
        .msg_type = SMARTCANE_MSG_TYPE_HAPTIC_FIRED,
        .height = height,
        .direction = direction,
        .distance_mm = distance_mm,
    };

    if (s_usb_write_mutex != nullptr) {
        xSemaphoreTake(s_usb_write_mutex, portMAX_DELAY);
    }
    const esp_err_t send_err = usb_write_all(reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
    if (s_usb_write_mutex != nullptr) {
        xSemaphoreGive(s_usb_write_mutex);
    }

    ESP_LOGI(TAG_USB_CDC, "[송신] B3 haptic_fired height=%u dir=%u dist=%dmm", height, direction, distance_mm);

    return send_err;
}
