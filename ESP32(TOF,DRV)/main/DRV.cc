#include "DRV.h"

static const char *TAG = "DRV";

static const i2c_device_config_t cfg_5A = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x5A,
    .scl_speed_hz    = 400000,
    .scl_wait_us     = 0,
    .flags           = {.disable_ack_check = 0},
};

static esp_err_t DRV_Write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev, buf, 2, 100);
}

// ════════════════════════════════════════════════════════════
// NFP-ELV1411A(AAC ELV1411A130318CA1) 공식 스펙시트 기준값:
// 정격전압 2Vrms, 공진주파수 150±5Hz, 최대입력전압(절대상한) 2.3Vrms
// RATED_VOLTAGE: Equation 5(LRA closed-loop RMS)로 2Vrms 역산 -> 89(0x59)
// OD_CLAMP: 제조사 절대 최대치 2.3Vrms를 그대로 Equation 9(LRA peak)에 대입
//           (여유분 추가 없이 보수적으로 - 모터 스펙 상한을 넘지 않도록) -> 108(0x6C)
// ════════════════════════════════════════════════════════════
#define DRV_RATED_VOLTAGE 0x59
#define DRV_OD_CLAMP      0x6C
// Control2(0x1C) 기본값 0xF5 = BIDIR_INPUT(bit7)=1(양방향).
// 양방향 모드에서는 PWM 50%가 "꺼짐"이고 0%는 "최대 역방향(급제동)"이라, 우리 코드가
// 가정하는 "0=꺼짐, 255=최대"와 안 맞았음 — intensity=0으로 끌 때마다 실제로는
// 급제동 명령이 나가서 순간 전류 스파이크가 났던 것으로 보임.
// BIDIR_INPUT=0(단방향)으로 바꿔서 0%=진짜 꺼짐, 100%=정격전압 정방향이 되게 함.
#define DRV_CONTROL2      0x75 // 0xF5에서 bit7(BIDIR_INPUT)만 0으로

// Simple LEDC-based haptic driver
void DRV_INIT(void)
{
    // Bind one DRV2605 per I2C bus
    if (i2c_master_bus_add_device(bus_handle, &cfg_5A, &DRV_dev1) == ESP_OK) {
        ESP_LOGI(TAG, "DRV0 bound on I2C0 (0x5A)");
        esp_err_t e1 = DRV_Write(DRV_dev1, 0x01, 0x03); // Mode: PWM/Analog Input
        esp_err_t e2 = DRV_Write(DRV_dev1, 0x1A, 0xB6); // Feedback: LRA mode
        esp_err_t e3 = DRV_Write(DRV_dev1, 0x1D, 0x00); // Control3: Closed-loop
        esp_err_t e4 = DRV_Write(DRV_dev1, 0x16, DRV_RATED_VOLTAGE); // Rated Voltage
        esp_err_t e5 = DRV_Write(DRV_dev1, 0x17, DRV_OD_CLAMP);      // Overdrive Clamp
        esp_err_t e6 = DRV_Write(DRV_dev1, 0x1C, DRV_CONTROL2);      // Control2: Unidirectional 입력
        if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK || e5 != ESP_OK || e6 != ESP_OK) {
            ESP_LOGE(TAG, "DRV0 레지스터 설정 실패 (mode=%s feedback=%s control3=%s rated=%s odclamp=%s control2=%s) - PWM 신호를 줘도 진동 안 할 수 있음",
                      esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4), esp_err_to_name(e5), esp_err_to_name(e6));
        }
    } else {
        ESP_LOGE(TAG, "DRV0 bind failed on I2C0");
    }

    if (i2c_master_bus_add_device(bus_handle1, &cfg_5A, &DRV_dev2) == ESP_OK) {
        ESP_LOGI(TAG, "DRV1 bound on I2C1 (0x5A)");
        esp_err_t e1 = DRV_Write(DRV_dev2, 0x01, 0x03); // Mode: PWM/Analog Input
        esp_err_t e2 = DRV_Write(DRV_dev2, 0x1A, 0xB6); // Feedback: LRA mode
        esp_err_t e3 = DRV_Write(DRV_dev2, 0x1D, 0x00); // Control3: Closed-loop
        esp_err_t e4 = DRV_Write(DRV_dev2, 0x16, DRV_RATED_VOLTAGE); // Rated Voltage
        esp_err_t e5 = DRV_Write(DRV_dev2, 0x17, DRV_OD_CLAMP);      // Overdrive Clamp
        esp_err_t e6 = DRV_Write(DRV_dev2, 0x1C, DRV_CONTROL2);      // Control2: Unidirectional 입력
        if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK || e5 != ESP_OK || e6 != ESP_OK) {
            ESP_LOGE(TAG, "DRV1 레지스터 설정 실패 (mode=%s feedback=%s control3=%s rated=%s odclamp=%s control2=%s) - PWM 신호를 줘도 진동 안 할 수 있음",
                      esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3), esp_err_to_name(e4), esp_err_to_name(e5), esp_err_to_name(e6));
        }
    } else {
        ESP_LOGE(TAG, "DRV1 bind failed on I2C1");
    }
}

void DRV_PWM_INIT(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = HAPTIC_MODE,
        .duty_resolution = HAPTIC_DUTY_RES,
        .timer_num = HAPTIC_TIMER,
        .freq_hz = HAPTIC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ch0 = {
        .gpio_num = HAPTIC_0_GPIO,
        .speed_mode = HAPTIC_MODE,
        .channel = HAPTIC_0_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HAPTIC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {.output_invert = 0},
        .deconfigure = false,
    };
    ledc_channel_config(&ch0);

    ledc_channel_config_t ch1 = {
        .gpio_num = HAPTIC_1_GPIO,
        .speed_mode = HAPTIC_MODE,
        .channel = HAPTIC_1_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HAPTIC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags = {.output_invert = 0},
        .deconfigure = false,
    };
    ledc_channel_config(&ch1);

    ledc_set_duty(HAPTIC_MODE, HAPTIC_0_CHANNEL, 0);
    ledc_update_duty(HAPTIC_MODE, HAPTIC_0_CHANNEL);
    ledc_set_duty(HAPTIC_MODE, HAPTIC_1_CHANNEL, 0);
    ledc_update_duty(HAPTIC_MODE, HAPTIC_1_CHANNEL);
}

// ════════════════════════════════════════════════════════════
// 진단용 임시 스위치: 1로 켜면 실제로 모터에 전류가 흐르지 않음 (LEDC duty 항상 0).
// 큐/로그/타이밍 등 나머지 트리거 로직은 그대로 실행됨.
// "트리거만 나면 멈춘다"는 증상이 모터 전류/전원 간섭 때문인지 확인하는 테스트로 썼음.
// 결과: 꺼놓으니 안 멈춤 -> 모터 전류/전원 간섭이 원인으로 확정됨.
// 이제 진단 끝났으니 다시 0으로 돌려서 정상 구동.
// ════════════════════════════════════════════════════════════
#define DIAG_DISABLE_ACTUAL_MOTOR_DRIVE 0

void Set_Haptic_Intensity(ledc_channel_t channel, uint8_t intensity)
{
#if DIAG_DISABLE_ACTUAL_MOTOR_DRIVE
    intensity = 0; // 진단 중: 실제 구동 전류를 흘리지 않음
#endif
    ESP_LOGI(TAG, "Set_Haptic_Intensity: channel=%d intensity=%d (DIAG_DISABLE=%d)",
             (int)channel, intensity, DIAG_DISABLE_ACTUAL_MOTOR_DRIVE);
    ledc_set_duty(HAPTIC_MODE, channel, intensity);
    ledc_update_duty(HAPTIC_MODE, channel);
}

void Play_Pulse(ledc_channel_t channel, uint8_t count)
{
    for (int i = 0; i < count; ++i) {
        Set_Haptic_Intensity(channel, 255); // 100% (부팅 셀프테스트 등에서 사용)
        vTaskDelay(pdMS_TO_TICKS(300));
        Set_Haptic_Intensity(channel, 0);
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

// Readme 4.2: 구역(높이)별 진동 패턴
// - TOP    : 짧게 세 번 (100 ON-80 OFF x3, 총 460ms — 치명적 위협)
// - MID    : 짧게 두 번 (150 ON-100 OFF-150 ON — 우회 가능 장애물)
// - BOTTOM : 짧게 한 번 (200ms ON — 바닥 장애물)
//
// ⚠️ 원래 TOP은 "500ms 길게 한 번", BOTTOM은 "200ms 한 번"으로 둘 다 단발 진동이라
// 세기(intensity)도 동일해서, 실제 손에 쥐고 테스트해보니 지속시간 차이(500ms vs 200ms)만으로는
// 구분이 잘 안 됐다. 사람이 절대적인 진동 "길이"를 판단하는 것보다 "몇 번 울렸는지" 세는 게
// 훨씬 쉽기 때문에, TOP/MID/BOTTOM을 전부 "울린 횟수(3/2/1)"로 구분되게 바꿨다.
// 460ms는 HAPTIC_PLAY_MAX_MS(500ms) 이내라 재트리거 예약 로직은 그대로 둬도 된다.
void Play_Haptic_Pattern(ledc_channel_t channel, haptic_pattern_t pattern)
{
    ESP_LOGI(TAG, "Play_Haptic_Pattern: channel=%d pattern=%d TOP=%d MID=%d BOTTOM=%d",
             (int)channel, (int)pattern, HAPTIC_INTENSITY_TOP, HAPTIC_INTENSITY_MID, HAPTIC_INTENSITY_BOTTOM);
    switch (pattern) {
    case HAPTIC_PATTERN_TOP:
        for (int i = 0; i < 3; ++i) {
            Set_Haptic_Intensity(channel, HAPTIC_INTENSITY_TOP);
            vTaskDelay(pdMS_TO_TICKS(100));
            Set_Haptic_Intensity(channel, 0);
            if (i < 2) {
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        }
        break;

    case HAPTIC_PATTERN_MID:
        Set_Haptic_Intensity(channel, HAPTIC_INTENSITY_MID);
        vTaskDelay(pdMS_TO_TICKS(150));
        Set_Haptic_Intensity(channel, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        Set_Haptic_Intensity(channel, HAPTIC_INTENSITY_MID);
        vTaskDelay(pdMS_TO_TICKS(150));
        Set_Haptic_Intensity(channel, 0);
        break;

    // 고장 알림: 80ms ON - 80ms OFF 를 4회. 장애물 패턴(연속/두번/한번)과 확실히 구분됨.
    case HAPTIC_PATTERN_FAULT:
        for (int i = 0; i < 4; ++i) {
            Set_Haptic_Intensity(channel, HAPTIC_INTENSITY_FAULT);
            vTaskDelay(pdMS_TO_TICKS(80));
            Set_Haptic_Intensity(channel, 0);
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        break;

    case HAPTIC_PATTERN_BOTTOM:
    default:
        Set_Haptic_Intensity(channel, HAPTIC_INTENSITY_BOTTOM);
        vTaskDelay(pdMS_TO_TICKS(200));
        Set_Haptic_Intensity(channel, 0);
        break;
    }
}