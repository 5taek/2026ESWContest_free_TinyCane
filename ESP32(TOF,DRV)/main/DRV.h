// Simplified haptic driver header
#ifndef __DRV_H__
#define __DRV_H__

#include "common.h"

// Haptic PWM pin mapping
#define HAPTIC_0_GPIO 7
#define HAPTIC_1_GPIO 8

#define HAPTIC_0_CHANNEL LEDC_CHANNEL_0
#define HAPTIC_1_CHANNEL LEDC_CHANNEL_1

#define HAPTIC_TIMER LEDC_TIMER_0
#define HAPTIC_MODE LEDC_LOW_SPEED_MODE
#define HAPTIC_DUTY_RES LEDC_TIMER_8_BIT
#define HAPTIC_FREQUENCY 20000

// 진동 세기(듀티, 0~255) — 전력 소모와 직결되는 값이라 여기서 한눈에 조정할 수 있게 모아둠.
// 모터 구동 순간 전류가 I2C 버스에 간섭을 일으키는 문제가 확인되어, 순간 전류를
// 줄이기 위해 낮춤 (100% -> 약 60%). 지난번 50%는 짧은 패턴에서 잘 안 느껴졌었으니
// 그보다는 조금 높게 잡음 — 실제 손에 쥐고 테스트하면서 조정 필요.
#define HAPTIC_INTENSITY_TOP    150  // ~59%
#define HAPTIC_INTENSITY_MID    150  // ~59%
#define HAPTIC_INTENSITY_BOTTOM 150  // ~59%
// 고장 알림용. 장애물 경고보다 확실히 구분돼야 해서 세기를 조금 높게 잡음.
#define HAPTIC_INTENSITY_FAULT  180  // ~71%

#ifdef __cplusplus
extern "C" {
#endif

// Readme 4.2: 구역(높이)별 진동 패턴 — 울리는 "횟수"로 구분한다 (세 번/두 번/한 번).
// TOP    = 상단(2.5m 이내) 치명적 위협 -> 짧게 세 번
// MID    = 중단(1.8m 이내) 우회 가능 장애물 -> 짧게 두 번
// BOTTOM = 하단(1.5m 이내) 바닥 장애물 -> 짧게 한 번
// FAULT = 장애물 경고가 아니라 "지팡이 자체가 고장났다"는 신호.
// 위 세 패턴과 절대 헷갈리면 안 되므로 더 빠르게 4연타로 구분한다.
typedef enum {
    HAPTIC_PATTERN_TOP = 0,
    HAPTIC_PATTERN_MID = 1,
    HAPTIC_PATTERN_BOTTOM = 2,
    HAPTIC_PATTERN_FAULT = 3,
} haptic_pattern_t;

void DRV_INIT(void);
void DRV_PWM_INIT(void);
void Set_Haptic_Intensity(ledc_channel_t channel, uint8_t intensity);
void Play_Pulse(ledc_channel_t channel, uint8_t count);
// 구역(높이)에 따라 다른 진동 패턴을 재생 (Readme 4.2). 블로킹 함수 — 패턴 재생이 끝날 때까지 리턴하지 않음.
void Play_Haptic_Pattern(ledc_channel_t channel, haptic_pattern_t pattern);

#ifdef __cplusplus
}
#endif

#endif // __DRV_H__