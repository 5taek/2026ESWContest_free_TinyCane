#ifndef USB_CDC_COMM_H
#define USB_CDC_COMM_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ════════════════════════════════════════════════════════════
// ESP32 <-> 라즈베리파이 USB CDC 프로토콜
//
// ESP32가 RPi로 보내는 메시지는 항상 "1바이트 타입 코드"로 시작한다:
//
//   SMARTCANE_MSG_TYPE_RAW_GRID (0xB2) : 원본 ToF 거리 그리드 전체(8행x8열=64칸)가
//       뒤따른다. ToF가 새 측정값을 낼 때마다(최대 5Hz) 주기적으로 전송된다.
//       물체 종류 분류는 이 보드에서 안 함 — RPi가 필요하면 자체적으로 이 원본
//       그리드를 해석해서 쓴다.
//
//   SMARTCANE_MSG_TYPE_HAPTIC_FIRED (0xB3) : ESP32가 실제로 진동을 울린 시점에만
//       보내는 이벤트 알림. "지금 이 순간 사용자에게 어느 구역 경고가 나갔다"를
//       RPi가 알아야 할 때(TTS 동기화, 로깅 등) 쓴다. 주기적이지 않고 이벤트 기반.
//
// 원본 그리드 인덱스: flat_index = row*8 + col (row-major, 0~63)
//
// ⚠️ TOF0 모듈이 내부 손상으로 확정되어(TOF_task.h의 SMARTCANE_TOF0_DISABLED 참고)
// 교체 전까지 TOF1 하나만 8x8로 단독 운용 중이다. row 전체(0~7)가 TOF1 하나의
// 8x8 데이터다. TOF0 교체되면 예전처럼 두 센서 4x4씩(8행x4열=32칸)으로 되돌릴 것
// — 그때는 ROWS/COLS를 8/4로, 아래 struct 크기도 같이 되돌려야 함(이전 커밋 참고).
// ════════════════════════════════════════════════════════════
#define SMARTCANE_MSG_TYPE_RAW_GRID     0xB2
#define SMARTCANE_MSG_TYPE_HAPTIC_FIRED 0xB3

#define SMARTCANE_RAW_GRID_ROWS  8    // TOF1 단독 8x8 해상도 기준 (TOF0 잠금 모드)
#define SMARTCANE_RAW_GRID_COLS  8
#define SMARTCANE_RAW_GRID_CELLS (SMARTCANE_RAW_GRID_ROWS * SMARTCANE_RAW_GRID_COLS) // 64

#pragma pack(push, 1)
typedef struct {
    uint8_t msg_type;                                  // 항상 SMARTCANE_MSG_TYPE_RAW_GRID (0xB2)
    int16_t distance_mm[SMARTCANE_RAW_GRID_CELLS];      // row-major, 무효 칸은 -1
} smartcane_raw_grid_packet_t; // 1 + 64*2 = 129 bytes

typedef struct {
    uint8_t msg_type;       // 항상 SMARTCANE_MSG_TYPE_HAPTIC_FIRED (0xB3)
    uint8_t height;         // 0=TOP 1=MID 2=BOTTOM
    uint8_t direction;      // 0=LEFT 1=CENTER 2=RIGHT
    int16_t distance_mm;    // 트리거 시점의 거리
} smartcane_haptic_fired_packet_t; // 5 bytes
#pragma pack(pop)

esp_err_t UsbCdcComm_Init(void);

// 원본 ToF 그리드 전체(64칸)를 RPi에 전송합니다. distance_mm은 row-major(row*8+col),
// 무효 칸은 -1이어야 합니다. ToF 측정 주기(최대 5Hz)마다 호출하는 걸 전제로 합니다.
esp_err_t UsbCdcComm_SendRawGrid(const int16_t distance_mm[SMARTCANE_RAW_GRID_CELLS]);

// 실제로 진동이 발화된 시점에 해당 구역 정보를 RPi에 전송합니다.
esp_err_t UsbCdcComm_SendHapticFired(uint8_t height, uint8_t direction, int16_t distance_mm);

#ifdef __cplusplus
}
#endif

#endif // USB_CDC_COMM_H
