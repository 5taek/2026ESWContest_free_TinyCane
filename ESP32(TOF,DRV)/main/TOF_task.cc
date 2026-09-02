#include "common.h"
#include "TOF_task.h"
#include "DRV.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

static const char *TAG = "TOF";

// 거리-변화율(Δ) 기반 정지/위험해소 판정(Readme 5.1) 및 재알림 조건
// (5.2: 거리급변/구역재진입)은 update_smart_sleep()에서 처리한다.
volatile bool g_alert_sleep_active = false;
// SMARTCANE_TOF0_DISABLED는 TOF_task.h에 정의됨 (smartcane.cc도 같이 봐야 해서).

VL53L5CX_Configuration config_dev0;
VL53L5CX_Configuration config_dev1;

static const i2c_device_config_t cfg_29 = {
    .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    .device_address  = 0x29,
    .scl_speed_hz    = 400000,
    .scl_wait_us     = 0,
    .flags           = {.disable_ack_check = 0},
};

static QueueHandle_t haptic0_q = NULL; // channel 0 events (right detections)
static QueueHandle_t haptic1_q = NULL; // channel 1 events (left detections)

// 햅틱 큐 payload 특수값: 높이(0~2)가 아니라 "고장 알림을 울려라"는 뜻.
// 채널 하나를 두 태스크가 동시에 건드리면 LEDC duty가 서로 덮어쓰므로, 고장 알림도
// 반드시 이 큐를 통해 소비자 태스크가 재생해야 한다.
#define HAPTIC_EVENT_FAULT 0xFF

// ════════════════════════════════════════════════════════════
// 3x3 구역 매핑 — TOF0 잠금 모드(SMARTCANE_TOF0_DISABLED=1) 기준
//
// TOF0이 완전히 빠지고 TOF1(물리적 상단 배치) 하나가 8x8 전체를 담당한다.
// row_offset=0으로 TOF1이 8행을 전부 채우므로, "절반(half)" 개념은 더 이상 두 센서를
// 안 나누고 전부 half 0(TOF1)로 취급한다 (아래 recompute_zone_grid_locked 참고).
//
// 높이(종단, 3구역):  상단=row0(맨 윗줄 1줄) / 중단=row1-5(중간 5줄) / 하단=row6-7(맨 아랫줄 2줄)
// 방향(횡단, 3구역):  좌측=col0-1 / 정면=col2-5 / 우측=col6-7  (기존 1:2:1 비율을 8열로 확장)
//
// RPi에는 이 3x3 요약이 아니라 원본 8x8 그리드 전체(SMARTCANE_RAW_GRID_CELLS)를
// 그대로 보낸다 — 판정에 쓰는 이 3x3 구역은 ESP32 로컬 햅틱 트리거 전용.
// ════════════════════════════════════════════════════════════
#define ZONE_HEIGHT_TOP    0
#define ZONE_HEIGHT_MID    1
#define ZONE_HEIGHT_BOTTOM 2
#define ZONE_DIR_LEFT      0
#define ZONE_DIR_CENTER    1
#define ZONE_DIR_RIGHT     2

typedef struct {
    int16_t distance_mm; // -1 = invalid
    bool valid;
} raw_cell_t;

typedef struct {
    bool active;
    int16_t min_distance_mm; // -1 = 활성 아님
    uint8_t hit_count;
} zone_cell_t;

// 물리 8x8 배열 (row-major). TOF0_DISABLED 모드에서는 TOF1 혼자 8x8 전체를 채운다
// (row_offset=0 고정) — half[1](TOF0 몫)은 영원히 갱신 안 되어 stale로 남는데,
// 이게 오히려 의도대로 동작한다: 아래 half_is_stale_locked()가 죽은 센서를 자동으로
// 무시하도록 이미 설계돼 있어서, TOF0을 통째로 안 쓰는 지금 상황도 별도 처리 없이
// "센서 하나가 계속 고장난 상태"로 자연스럽게 취급된다.
static raw_cell_t g_raw_grid[SMARTCANE_RAW_GRID_ROWS][SMARTCANE_RAW_GRID_COLS];
static zone_cell_t g_zone_grid[3][3];     // [height][direction] 논리 3x3 구역 (햅틱 트리거 전용)
static SemaphoreHandle_t g_tof_state_mutex = nullptr;

// ⚠️ 센서별 데이터 신선도 (안전 관련 중요 수정).
// 예전엔 한쪽 센서가 죽어도 그 절반이 "마지막으로 읽은 값" 그대로 영원히 남았고,
// 살아있는 쪽 태스크가 evaluate_zone_grid_and_act_locked()를 돌 때마다 그 유령
// 데이터로 winner를 뽑았다.
//   - 없는 장애물로 계속 진동하거나
//   - 반대로 그 방향의 진짜 장애물을 영영 못 보거나
// 둘 다 지팡이에서는 치명적이라, 일정 시간 갱신이 없으면 해당 절반을 통째로 무효화한다.
// (5Hz = 200ms 주기이므로 600ms면 3프레임 연속 누락)
#define TOF_DATA_STALE_MS 600
static TickType_t g_half_fresh_tick[2] = { 0, 0 }; // [0]=TOF1(현재 유일하게 사용), [1]=TOF0(잠금 중, 항상 stale)

// 측정 회차 카운터. 아래 take_eval_round_locked() 참고.
static uint32_t g_half_seq[2]      = { 0, 0 };
static uint32_t g_last_eval_seq[2] = { 0, 0 };

// 틱 랩어라운드에 안전한 "now가 until 이후인가" 비교.
// (TickType_t 절대값을 그냥 < / >= 로 비교하면 49.7일마다 한 번 뒤집힌다)
static inline bool tick_reached(TickType_t now, TickType_t until)
{
    return (int32_t)(now - until) >= 0;
}

// g_tof_state_mutex 보호 하에 호출.
static bool half_is_stale_locked(int hf)
{
    if (g_half_fresh_tick[hf] == 0) return true;
    return tick_reached(xTaskGetTickCount(), g_half_fresh_tick[hf] + pdMS_TO_TICKS(TOF_DATA_STALE_MS));
}

// ⚠️ 판정을 "측정 1회차당 1번"으로 묶는다 (진동 오작동의 핵심 원인이었음).
//
// TOF0_TASK와 TOF1_TASK는 각자 자기 데이터를 채운 뒤 곧바로
// evaluate_zone_grid_and_act_locked()를 호출했다. 그런데 두 태스크는 같은 g_zone_grid를
// 보므로, TOF0이 판정한 직후 TOF1이 또 판정한다 — 이때 TOF0의 행 4개는 방금 전과
// 완전히 동일한 값이다. 즉 "측정 1번"이 "판정 2번"으로 세어졌다.
//
// 그 결과:
//   - WINNER_CONFIRM_FRAMES=2 가 사실상 무력화. 노이즈로 한 프레임 튄 값이 두 번
//     세어져서 그대로 진동까지 갔다. 시간축 노이즈 제거가 통째로 죽어 있었음.
//   - update_smart_sleep()도 2배로 불려서, 같은 데이터에 대해 delta=0(정지)으로
//     읽히는 프레임이 섞여 안정 판정이 왜곡됐다.
//
// 이제 "살아있는 센서가 전부 새 데이터를 한 번씩 준 시점"에만 판정한다.
// 죽은(stale) 센서는 기다리지 않으므로 한쪽이 고장나도 판정이 멈추지 않는다.
static bool take_eval_round_locked(void)
{
    bool any_fresh = false;

    for (int hf = 0; hf < 2; ++hf) {
        if (half_is_stale_locked(hf)) {
            continue; // 죽은 센서를 기다리며 판정을 멈추면 안 된다
        }
        if (g_half_seq[hf] == g_last_eval_seq[hf]) {
            return false; // 이 센서는 아직 이번 회차 데이터를 안 줬다
        }
        any_fresh = true;
    }

    if (!any_fresh) {
        return false; // 양쪽 다 stale — 판정할 새 데이터가 없음
    }

    g_last_eval_seq[0] = g_half_seq[0];
    g_last_eval_seq[1] = g_half_seq[1];
    return true;
}

// TOF1 단독 8x8 해상도 기준 (TOF0 잠금 모드). 위 "3x3 구역 매핑" 주석 참고.
static const int kHeightRowStart[3] = {0, 1, 6};
static const int kHeightRowEnd[3]   = {0, 5, 7};
static const int kDirColStart[3]    = {0, 2, 6};
static const int kDirColEnd[3]      = {1, 5, 7};

// Readme 4.2: 구역(높이)별 위험 판정 임계거리 — 상단이 가장 멀리서부터 위험으로 간주
// ⚠️ 2026-08-15 실측 후 상향 조정: 원래 값(2500/1800/1500)으로 실제로 걸어서 테스트해보니
// 보행 속도(약 1.2m/s) 대비 최초 경고가 너무 늦게 나와서 장애물에 부딪히기 직전에야
// 진동이 울리는 문제가 확인됨. 게다가 ToF 센서~지팡이 맨 끝 사이 수직거리가 40cm라,
// 센서 기준 임계거리에서 실제로는 400mm를 더 빼고 생각해야 함. 그만큼 여유를 두고 전부
// 500mm씩 올림. HAPTIC_COOLDOWN_MS도 같이 낮춰서 재경고 주기를 단축했음 — 이 둘은
// 세트로 튜닝된 값이라 하나만 되돌리지 말 것.
static const int16_t kHeightDangerThresholdMm[3] = { 3000, 2300, 2000 }; // top, mid, bottom

// Readme 4.1: 위험도 점수 = 거리점수 + 높이가중치 + 방향가중치
static const int kHeightWeight[3] = { 30, 20, 10 }; // top > mid > bottom
static const int kDirWeight[3]    = { 10, 20, 10 }; // left, center, right (center가 가장 높음)
// 위 가중치 값은 Readme에 순서만 명시되어 있어 임의로 잡은 값입니다 — 실측 후 튜닝 필요.
// TODO(실측): compute_zone_score()가 거리를 높이별 임계치로 정규화하기 때문에, 같은
// 물리 거리에서도 TOP이 BOTTOM보다 유리해져 높이 가중치가 사실상 두 번 걸립니다.
// 정규화 분모를 공통값으로 바꾸는 구조 수정이 필요하지만, 그러면 위 가중치를 전부
// 다시 잡아야 하므로 실측 전까지 보류합니다. (TEST_PLAN.md 5번 항목)

// ⚠️ 구역 활성 최소 hit 수 (안전 관련 중요 수정).
// 예전엔 구역 셀 수에 비례해 `zone_cells >= 4 ? 2 : 1`로 잡았는데, 결과가 뒤집혀 있었다.
// LEFT/RIGHT는 2칸짜리라 1칸만 걸려도 활성인데, 정면(MID/CENTER)은 8칸이라 2칸이
// 필요했다. 즉 볼라드처럼 가느다란 물체가 정면에 있으면 8칸 중 1칸에만 걸려서 무시되고,
// 같은 볼라드가 왼쪽으로 비켜서면 트리거되는 상태였다 — 정면 볼라드는 이 지팡이의
// 1순위 표적인데 가장 둔감한 구역이 돼 있었음.
// 이제 전 구역 1칸으로 통일하고, 노이즈는 시간축(WINNER_CONFIRM_FRAMES)에서 거른다.
// TODO(실측): 1로 두면 오탐이 늘 수 있음. 실사용에서 튀면 2로 올리되, 그때는 정면만
// 예외로 1을 유지할 것. (TEST_PLAN.md 2번 항목)
#define ZONE_MIN_HITS_REQUIRED 1

// TOF 센서 1개(8x8=64칸)의 원시 결과를 공유 배열의 [row_offset ..] 행에 채워넣는다.
// TOF0 잠금 모드에서는 TOF1이 항상 row_offset=0으로 불러 8행 전체를 혼자 채운다.
// 반드시 g_tof_state_mutex 를 잡은 상태에서 호출할 것.
// 주의: 여기서는 "센서가 유효한 값을 리턴했는가"만 판정하고, "위험한 거리인가"는 판정하지 않는다.
// (예전 코드는 여기서 120~700mm로 하드컷을 걸어서 VL53L5CX의 실제 측정범위(~4m)를
//  전혀 활용하지 못했음 — 그 버그를 고친 부분)
static void ingest_sensor_rows_locked(const VL53L5CX_ResultsData *results, int row_offset)
{
    for (int i = 0; i < VL53L5CX_RESOLUTION_8X8; ++i) {
        const int local_row = i / SMARTCANE_RAW_GRID_COLS;
        const int col = i % SMARTCANE_RAW_GRID_COLS;
        const int idx = i * VL53L5CX_NB_TARGET_PER_ZONE;
        const int16_t d = results->distance_mm[idx];
        const uint8_t st = results->target_status[idx];
        const uint8_t nt = results->nb_target_detected[i];
        const uint32_t sig = results->signal_per_spad[idx];

        // VL53L5CX 실측 유효범위(대략 40mm~4000mm)만 걸러낸다. "위험" 여부는
        // recompute_zone_grid_locked()에서 높이별 임계치(kHeightDangerThresholdMm)로 별도 판정.
        const bool valid = (nt > 0) && (st == 5 || st == 9) && (d >= 40 && d <= 4000) && (sig >= 40);

        raw_cell_t &cell = g_raw_grid[row_offset + local_row][col];
        cell.valid = valid;
        cell.distance_mm = valid ? d : (int16_t)-1;
    }

    // 이 절반이 방금 갱신됐다고 기록 (stale 판정 + 측정 회차 기준)
    const int half = row_offset ? 1 : 0;
    g_half_fresh_tick[half] = xTaskGetTickCount();
    g_half_seq[half]++;
}

// 8x8 원시 배열(TOF1 단독) -> 3x3 논리 구역으로 축약, 높이별 위험 임계거리 적용.
// g_tof_state_mutex 보호 하에 호출.
static void recompute_zone_grid_locked(void)
{
    // 갱신이 끊긴 절반은 이번 계산에서 통째로 없는 셈 친다 (위 TOF_DATA_STALE_MS 주석 참고)
    const bool half_stale[2] = { half_is_stale_locked(0), half_is_stale_locked(1) };

    for (int h = 0; h < 3; ++h) {
        const int16_t danger_threshold = kHeightDangerThresholdMm[h];

        for (int dir = 0; dir < 3; ++dir) {
            int hit_count = 0;
            int16_t min_d = -1;

            for (int r = kHeightRowStart[h]; r <= kHeightRowEnd[h]; ++r) {
                // ⚠️ TOF0 잠금 모드: 8행 전체가 TOF1(half 0) 하나뿐이다. 예전엔
                // "r < ROWS/2 ? 0 : 1"로 앞/뒤 절반을 다른 센서로 나눴는데, 지금은
                // 센서가 하나뿐이라 그 계산을 쓰면 뒷줄(BOTTOM 구역)이 엉뚱하게
                // half 1(잠긴 TOF0, 항상 stale)로 잡혀 BOTTOM이 절대 활성화 못 되는
                // 버그가 난다. 그래서 모든 행을 half 0으로 고정한다.
                if (half_stale[0]) {
                    continue;
                }
                for (int c = kDirColStart[dir]; c <= kDirColEnd[dir]; ++c) {
                    const raw_cell_t &cell = g_raw_grid[r][c];
                    // 이 높이 구역 자체의 위험 임계거리 이내인 hit만 카운트
                    if (cell.valid && cell.distance_mm <= danger_threshold) {
                        hit_count++;
                        if (min_d < 0 || cell.distance_mm < min_d) {
                            min_d = cell.distance_mm;
                        }
                    }
                }
            }

            zone_cell_t &zc = g_zone_grid[h][dir];
            zc.hit_count = (uint8_t)hit_count;
            zc.active = (hit_count >= ZONE_MIN_HITS_REQUIRED);
            zc.min_distance_mm = zc.active ? min_d : (int16_t)-1;
        }
    }
}

// ════════════════════════════════════════════════════════════
// 스마트 슬립 / 뮤트 상태 (Readme 5장) — send_obstacle_alert와
// update_smart_sleep이 공유해서 써야 하므로 두 함수보다 앞에 선언.
// ════════════════════════════════════════════════════════════
typedef struct {
    int16_t last_distance;             // 직전 프레임 대표(승자 구역) 거리(mm), -1이면 미측정
    TickType_t stable_since_tick;      // 안정(정지/멀어짐) 상태가 시작된 시각
    bool sleep_active;
} smart_sleep_state_t;

static smart_sleep_state_t s_sleep_state = { -1, 0, false };

// ════════════════════════════════════════════════════════════
// RPi로 보내는 두 가지 알림 (2026-08-15 재설계)
//
// 1) 원본 그리드 스트리밍 (0xB2) — 매 측정 회차(take_eval_round_locked 성공 시)마다
//    8x8 원본 거리값 전체를 그대로 RPi에 보낸다. 3x3 요약이 아니라 원본을 보내는 이유는
//    RPi 쪽에서 더 세밀한 판단(또는 자체 시각화/로깅)이 필요할 수 있어서다.
// 2) 햅틱 발화 이벤트 (0xB3) — 실제로 진동이 울린 순간에만, 어느 구역이 울렸는지를 보낸다.
//    RPi가 "지금 이 순간 사용자에게 경고가 나갔다"를 알아야 할 때(TTS 동기화 등) 쓴다.
//
// 둘 다 큐로 "보내라"는 신호만 던지고 전용 태스크가 USB 전송을 담당한다.
// ⚠️ 예전 구조의 문제: 생산자가 g_tof_state_mutex를 쥔 채로 직접 UsbCdcComm_Send*()를
// 호출하면, 그 안의 usb_write_all()은 RPi가 데이터를 안 읽어가면 다 쓸 때까지 계속
// 재시도하며 블로킹된다. RPi 쪽이 느리거나 죽어있으면 거리 측정 루프 전체가 같이
// 멈춰버리는 구조가 되므로, 반드시 큐로 분리해서 스냅샷만 락 안에서 뜨고 전송은 락
// 밖에서 한다.
// ════════════════════════════════════════════════════════════
typedef struct {
    uint8_t height;      // 0=TOP 1=MID 2=BOTTOM
    uint8_t direction;   // 0=LEFT 1=CENTER 2=RIGHT
    int16_t distance_mm;
} haptic_fired_event_t;

static QueueHandle_t alert_q = NULL;       // haptic_fired_event_t 1개 슬롯
static QueueHandle_t grid_stream_q = NULL; // 트리거용 더미 1바이트

// 실제로 발화한 winner 정보를 그대로 큐에 담아 보낸다 — 이미 값이 다 계산돼 있으므로
// 소비자가 g_zone_grid를 다시 읽을 필요도 없다.
static void send_obstacle_alert(uint8_t height, uint8_t direction, int16_t distance_mm)
{
    if (alert_q == NULL) {
        return;
    }
    const haptic_fired_event_t ev = { height, direction, distance_mm };
    xQueueOverwrite(alert_q, &ev);
}

// 원본 그리드 전송 트리거만 던진다 (내용은 소비자가 그 시점에 다시 읽음).
static void request_grid_stream(void)
{
    if (grid_stream_q == NULL) {
        return;
    }
    const uint8_t dummy = 1;
    xQueueOverwrite(grid_stream_q, &dummy);
}

// 햅틱 발화 이벤트(0xB3) 전송 전용 태스크. 여기서 블로킹돼도 ToF/햅틱은 계속 돈다.
void ALERT_SENDER_TASK(void *pvParameters)
{
    (void)pvParameters;
    haptic_fired_event_t ev;

    for (;;) {
        if (xQueueReceive(alert_q, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const esp_err_t send_err = UsbCdcComm_SendHapticFired(ev.height, ev.direction, ev.distance_mm);
        if (send_err != ESP_OK) {
            ESP_LOGW(TAG, "햅틱 발화 알림 전송 실패: %s", esp_err_to_name(send_err));
        }
    }
}

// 원본 그리드 스트리밍(0xB2) 전송 전용 태스크.
void GRID_STREAM_TASK(void *pvParameters)
{
    (void)pvParameters;
    uint8_t trigger;
    int16_t flat[SMARTCANE_RAW_GRID_CELLS];

    for (;;) {
        if (xQueueReceive(grid_stream_q, &trigger, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // g_tof_state_mutex는 짧게만 쥐고(스냅샷 복사만) 바로 놓는다 — USB 전송은
        // 락 밖에서 한다 (위 주석 참고: 블로킹이 ToF 루프로 번지는 걸 막기 위함).
        if (g_tof_state_mutex != nullptr) {
            xSemaphoreTake(g_tof_state_mutex, portMAX_DELAY);
        }
        for (int r = 0; r < SMARTCANE_RAW_GRID_ROWS; ++r) {
            for (int c = 0; c < SMARTCANE_RAW_GRID_COLS; ++c) {
                const raw_cell_t &cell = g_raw_grid[r][c];
                flat[r * SMARTCANE_RAW_GRID_COLS + c] = cell.valid ? cell.distance_mm : (int16_t)-1;
            }
        }
        if (g_tof_state_mutex != nullptr) {
            xSemaphoreGive(g_tof_state_mutex);
        }

        const esp_err_t send_err = UsbCdcComm_SendRawGrid(flat);
        if (send_err != ESP_OK) {
            ESP_LOGW(TAG, "원본 그리드 전송 실패: %s", esp_err_to_name(send_err));
        }
    }
}

// ════════════════════════════════════════════════════════════
// 스마트 슬립 (Readme 5.1 / 5.2)
// current_distance/any_active는 이제 "현재 승자(최고 위협) 구역"의 값을 받는다 —
// 위협 우선순위 스코어링이 추적하는 그 장애물과의 거리 변화를 보는 게 맞기 때문.
// ════════════════════════════════════════════════════════════
#define SMART_SLEEP_STABLE_BAND_MM   50    // ±5cm 이내면 "정지"로 간주
#define SMART_SLEEP_STABLE_MS        2000  // 이 정도 지속되면 슬립 진입
#define SMART_SLEEP_WAKE_JUMP_MM     150   // 이 이상 갑자기 가까워지면 즉시 깨움(충돌임박) — 실측 후 튜닝 필요

// 레인징 정지(소프트 행) 감지 — 태스크는 살아있는데 센서가 새 데이터를 안 주는 경우.
// 5Hz(200ms 주기) 기준이니 1초면 이미 5프레임 놓친 것 — 넉넉하게 잡은 값, 필요시 조정.
#define TOF_STALL_TIMEOUT_MS 1000
// 재시작을 이만큼 연속으로 해도 안 살아나면 "일시적 끊김"이 아니라 고장으로 판정한다.
// (재시작 시도 간격이 약 1초이므로 3회 ≈ 3초)
#define TOF_STALL_FAULT_RESTARTS 3

static void update_smart_sleep(int16_t current_distance, bool any_active)
{
    const TickType_t now = xTaskGetTickCount();

    // 감지 대상 자체가 없음(구역 완전 이탈) -> Readme 5.2 조건2: 즉시 슬립 해제 + 기준 리셋
    if (!any_active || current_distance < 0) {
        if (s_sleep_state.sleep_active) {
            ESP_LOGI(TAG, "스마트 슬립 해제: 구역 이탈");
        }
        s_sleep_state.sleep_active = false;
        s_sleep_state.last_distance = -1;
        s_sleep_state.stable_since_tick = now;
        g_alert_sleep_active = false;
        return;
    }

    if (s_sleep_state.last_distance < 0) {
        // 첫 측정 프레임: 비교 기준만 세팅
        s_sleep_state.last_distance = current_distance;
        s_sleep_state.stable_since_tick = now;
        return;
    }

    const int16_t delta = current_distance - s_sleep_state.last_distance; // 음수 = 가까워짐

    // Readme 5.2 조건1: 거리 급변(접근) -> 즉시 깨움
    if (delta <= -SMART_SLEEP_WAKE_JUMP_MM) {
        if (s_sleep_state.sleep_active) {
            ESP_LOGW(TAG, "스마트 슬립 해제: 거리 급변(충돌 임박) delta=%d mm", delta);
        }
        s_sleep_state.sleep_active = false;
        s_sleep_state.stable_since_tick = now;
        s_sleep_state.last_distance = current_distance;
        g_alert_sleep_active = false;
        return;
    }

    // 정지(±5cm 이내) 또는 멀어지는 중(위험 해소)이면 안정 상태로 취급
    const bool stable_or_receding = (delta >= -SMART_SLEEP_STABLE_BAND_MM);

    if (stable_or_receding) {
        const uint32_t stable_ms = (now - s_sleep_state.stable_since_tick) * portTICK_PERIOD_MS;
        if (stable_ms >= SMART_SLEEP_STABLE_MS) {
            if (!s_sleep_state.sleep_active) {
                ESP_LOGI(TAG, "스마트 슬립 진입 (정지/위험해소 %lums 지속)", (unsigned long)stable_ms);
            }
            s_sleep_state.sleep_active = true;
            g_alert_sleep_active = true;
        }
    } else {
        // 급변 임계치엔 못 미치지만 계속 접근 중인 애매한 경우: 안정 타이머 리셋
        s_sleep_state.stable_since_tick = now;
    }

    s_sleep_state.last_distance = current_distance;
}

// ════════════════════════════════════════════════════════════
// 위협 우선순위 스코어링 + Winner-Takes-All (Readme 4.1, 4.3)
// 위험도 점수 = 거리점수(0~50, 가까울수록↑) + 높이가중치 + 방향가중치
// 활성 구역 중 최고점 1개만 선정해서 그 구역으로만 반응한다.
// ════════════════════════════════════════════════════════════
static int compute_zone_score(int h, int dir, int16_t distance_mm)
{
    const int16_t threshold = kHeightDangerThresholdMm[h];
    if (distance_mm < 0 || distance_mm > threshold) {
        return -1; // 비활성
    }
    const float ratio = 1.0f - ((float)distance_mm / (float)threshold); // 0(경계)~1(바로앞)
    const int distance_score = (int)(ratio * 50.0f);
    return distance_score + kHeightWeight[h] + kDirWeight[dir];
}

typedef struct {
    bool active;          // 현재 winner가 존재하는가
    int height;
    int dir;
    uint8_t confirm_count; // 같은 winner 구역이 연속으로 유지된 프레임 수
    bool fired;            // 이 winner에 대해 이미 트리거(햅틱/TTS)를 보냈는가
    int16_t fired_distance_mm; // 마지막으로 트리거했을 때의 거리 (재발화 판단용)
} winner_track_t;

static winner_track_t g_winner_track = { false, -1, -1, 0, false, -1 };

#define WINNER_CONFIRM_FRAMES 2 // 이 정도 연속으로 같은 구역이 1등을 유지해야 실제 트리거

// ⚠️ winner 전환 히스테리시스 (경고가 아예 안 나가던 구멍).
// winner가 바뀌면 confirm_count가 1로 리셋된다. 그래서 점수가 비슷한 두 구역이
// 프레임마다 번갈아 1등을 하면 confirm_count가 영원히 2에 도달하지 못하고
// **진동이 한 번도 안 울린다.** 예를 들어 왼쪽 벽과 정면 볼라드가 비슷한 거리에 있으면
// MID/LEFT <-> MID/CENTER 가 계속 뒤바뀌면서 둘 다 경고를 못 받는 상황이 생긴다.
// 장애물이 실제로 두 개 있는 상황에서 침묵하는 건 지팡이에서 가장 위험한 실패다.
// 이제 현재 winner를 이 점수만큼 이겨야 자리를 뺏을 수 있다.
#define WINNER_SWITCH_MARGIN 5

// ⚠️ 재발화(re-fire) 임계값 — 안전 관련 중요 개선.
// 예전 로직: 같은 구역이 winner로 유지되는 동안 fired가 계속 true라서, 구역에 처음
// 진입할 때 딱 한 번만 울리고 그 뒤로는 완전히 침묵했다. 즉 사용자가 정면 볼라드를
// 향해 계속 걸어가면 임계 거리를 넘는 순간 한 번 울리고, 충돌 직전까지 아무 경고가
// 없었음. 지팡이라는 용도에서 이건 놓치면 안 되는 공백.
// 이제 같은 구역이라도 "마지막 알림 때보다 이만큼 더 가까워졌으면" 다시 울린다.
#define REFIRE_APPROACH_MM 300  // 30cm 더 접근하면 재경고 — 실측(2026-08-15 보행 테스트) 결과 적당한 값으로 확인, 유지

// ⚠️ 햅틱 쿨다운을 생산자(여기)와 소비자(HAPTIC_consumer_task)가 공유한다.
// 예전엔 쿨다운 판정이 소비자에만 있었다. 그래서 쿨다운에 걸려 진동이 실제로 안 났는데도
// 생산자는 fired = true / fired_distance_mm = 현재거리 로 기준을 갱신해버렸고, 다음
// 재경고는 거기서 다시 30cm를 더 가야 했다. 사용자가 장애물로 계속 접근하는 내내
// 경고가 뒤로 밀리는 공백이 생기는 구조 — 재발화 로직을 넣은 취지가 무너져 있었음.
// 이제 생산자가 먼저 확인해서, 울릴 채널이 하나도 준비 안 됐으면 트리거를 소비하지 않고
// 그대로 두고 다음 프레임에 다시 시도한다.
// ⚠️ 2026-08-15 실측 후 하향 조정: 보행 중(약 1.2m/s)에는 REFIRE_APPROACH_MM(300mm)
// 조건이 쿨다운보다 훨씬 빨리 채워져서(0.25초 vs 1.5초), 사실상 이 쿨다운 값이 재경고
// 주기를 결정하고 있었다. 1500ms면 그 사이 1.7~2m를 더 걸어가버려서 장애물 코앞에서야
// 다시 울리는 문제가 실측으로 확인됨 — 600ms로 낮춤. kHeightDangerThresholdMm 상향과
// 세트로 튜닝된 값이니 하나만 되돌리지 말 것.
#define HAPTIC_COOLDOWN_MS 600
// 패턴 재생 자체에 걸리는 최대 시간(TOP=500ms). 재생 시작 직전에 이만큼을 미리 얹어
// 예약해두지 않으면, 소비자가 재생 중인 동안 생산자가 "쿨다운 끝남"으로 오판한다.
#define HAPTIC_PLAY_MAX_MS 500
static volatile TickType_t g_haptic_cooldown_until[2] = { 0, 0 }; // [0]=HAPTIC0, [1]=HAPTIC1

// 3x3 구역 grid에서 최고점 구역(winner)을 뽑아 히스테리시스 확인 후
// 햅틱 트리거, 스마트슬립 갱신, TTS 알림까지 처리. g_tof_state_mutex 보호 하에 호출.
static void evaluate_zone_grid_and_act_locked(const char *source_tag)
{
    int best_score = -1;
    int best_h = -1;
    int best_dir = -1;

    for (int h = 0; h < 3; ++h) {
        for (int dir = 0; dir < 3; ++dir) {
            const zone_cell_t &zc = g_zone_grid[h][dir];
            if (!zc.active) continue;
            const int score = compute_zone_score(h, dir, zc.min_distance_mm);
            if (score > best_score) {
                best_score = score;
                best_h = h;
                best_dir = dir;
            }
        }
    }

    // 현재 winner가 아직 살아있다면, 도전자가 WINNER_SWITCH_MARGIN 이상 이겨야 교체한다.
    // (안 그러면 비슷한 점수의 두 구역이 번갈아 1등을 하면서 confirm_count가 계속 리셋돼
    //  둘 다 경고를 못 받는다 — 위 WINNER_SWITCH_MARGIN 주석 참고)
    if (best_score >= 0 && g_winner_track.active &&
        (g_winner_track.height != best_h || g_winner_track.dir != best_dir)) {
        const zone_cell_t &cur = g_zone_grid[g_winner_track.height][g_winner_track.dir];
        if (cur.active) {
            const int cur_score = compute_zone_score(g_winner_track.height, g_winner_track.dir,
                                                     cur.min_distance_mm);
            if (cur_score >= 0 && (best_score - cur_score) < WINNER_SWITCH_MARGIN) {
                best_score = cur_score;
                best_h = g_winner_track.height;
                best_dir = g_winner_track.dir;
            }
        }
    }

    bool just_fired = false;

    if (best_score < 0) {
        // 활성 구역 전무 -> winner 리셋
        g_winner_track.active = false;
        g_winner_track.height = -1;
        g_winner_track.dir = -1;
        g_winner_track.confirm_count = 0;
        g_winner_track.fired = false;
        g_winner_track.fired_distance_mm = -1;
    } else {
        const int16_t best_distance = g_zone_grid[best_h][best_dir].min_distance_mm;

        if (g_winner_track.active && g_winner_track.height == best_h && g_winner_track.dir == best_dir) {
            if (g_winner_track.confirm_count < 255) g_winner_track.confirm_count++;

            // 같은 구역이 유지되는 중이라도, 마지막 알림 때보다 눈에 띄게 가까워졌으면
            // 다시 경고할 수 있게 fired를 풀어준다 (충돌 임박 상황에서 침묵 방지).
            if (g_winner_track.fired && g_winner_track.fired_distance_mm > 0 && best_distance > 0) {
                // ⚠️ 기준거리를 "더 먼 쪽"으로 따라 올린다.
                // min_distance_mm는 구역 내 셀들의 최솟값이라 노이즈에 취약하다. 한 프레임
                // 튄 값(실제보다 훨씬 가까움)이 기준으로 박히면, 이후 정상 측정값은 전부
                // 그보다 멀어서 (기준 - 현재)가 음수가 되고 재경고가 영영 안 나온다.
                // 기준보다 계속 멀게 측정되면 기준을 끌어올려 스스로 회복하게 한다.
                // 의미도 자연스러워짐: "마지막 경고 이후 가장 멀었던 지점에서 30cm 접근하면 재경고".
                // 반대 방향(멀리 튄 노이즈)은 경고가 한 번 더 나가는 쪽이라 안전한 실패다.
                if (best_distance > g_winner_track.fired_distance_mm) {
                    g_winner_track.fired_distance_mm = best_distance;
                } else if ((g_winner_track.fired_distance_mm - best_distance) >= REFIRE_APPROACH_MM) {
                    ESP_LOGI(TAG, "재경고 조건 충족: %dmm -> %dmm (%dmm 접근)",
                             g_winner_track.fired_distance_mm, best_distance,
                             g_winner_track.fired_distance_mm - best_distance);
                    g_winner_track.fired = false;
                }
            }
        } else {
            g_winner_track.height = best_h;
            g_winner_track.dir = best_dir;
            g_winner_track.confirm_count = 1;
            g_winner_track.fired = false;
            g_winner_track.fired_distance_mm = -1;
        }
        g_winner_track.active = true;

        if (!g_winner_track.fired && g_winner_track.confirm_count >= WINNER_CONFIRM_FRAMES) {
            // 울릴 채널이 실제로 준비됐을 때만 트리거를 "소비"한다 (위 J-1 주석 참고).
            const TickType_t now_tick = xTaskGetTickCount();
            const bool ch0_ready = tick_reached(now_tick, g_haptic_cooldown_until[0]);
            const bool ch1_ready = tick_reached(now_tick, g_haptic_cooldown_until[1]);

            // ⚠️ CENTER는 반드시 양쪽이 다 준비됐을 때만 울린다.
            // 한쪽만 준비된 상태에서 울리면 그쪽 모터만 진동하는데, 사용자는 그걸
            // "왼쪽/오른쪽에 장애물"로 읽는다. 정면 장애물을 측면으로 잘못 안내하는 셈.
            // 실제로 재현되는 시나리오: 오른쪽 트리거로 HAPTIC0이 쿨다운에 들어간 직후
            // 정면 장애물이 잡히면 HAPTIC1만 울려서 "왼쪽"으로 전달된다.
            const bool ready = (best_dir == ZONE_DIR_CENTER) ? (ch0_ready && ch1_ready)
                             : (best_dir == ZONE_DIR_RIGHT)  ? ch0_ready
                                                             : ch1_ready;

            if (ready) {
                g_winner_track.fired = true;
                g_winner_track.fired_distance_mm = best_distance;
                just_fired = true;
            }
            // ready가 아니면 fired를 그대로 false로 둔다 -> 쿨다운이 풀리는 다음 프레임에
            // 다시 시도하므로, 기준거리가 울리지도 않은 채 앞당겨지는 일이 없다.
        }
    }

    if (just_fired) {
        static const char *kHeightName[3] = { "TOP", "MID", "BOTTOM" };
        static const char *kDirName[3]    = { "LEFT", "CENTER", "RIGHT" };
        ESP_LOGI(TAG, "%s -> WINNER [%s/%s] score=%d dist=%dmm",
                 source_tag, kHeightName[best_h], kDirName[best_dir], best_score,
                 g_zone_grid[best_h][best_dir].min_distance_mm);

        // Readme 4.3: 좌측 감지->좌측(HAPTIC1) / 우측 감지->우측(HAPTIC0) / 정면->양쪽
        // 큐 payload는 이제 best_h(높이) — HAPTIC_consumer_task가 이 값으로 진동 패턴을 고른다.
        const uint8_t height_payload = (uint8_t)best_h;
        if (haptic0_q == NULL || haptic1_q == NULL) {
            ESP_LOGE(TAG, "햅틱 큐가 없어 진동을 보낼 수 없음 (TOF_INIT 실패)");
        } else if (best_dir == ZONE_DIR_RIGHT) {
            xQueueOverwrite(haptic0_q, &height_payload);
        } else if (best_dir == ZONE_DIR_LEFT) {
            xQueueOverwrite(haptic1_q, &height_payload);
        } else { // CENTER - 양쪽 모터 동시 트리거
            // 두 모터를 정확히 동시에 켜면 순간 전류가 겹쳐서(2배 가까이) 전원 레일에
            // 부담을 준다는 게 확인됨 (TOP/CENTER 트리거 직후 하드행 재현됨).
            // 예전엔 여기서 vTaskDelay(25ms)로 시차를 뒀는데, g_tof_state_mutex를 쥔
            // 채로 잠드는 구조라 그 25ms 동안 다른 ToF 태스크까지 대기하게 됐음.
            // 이제 큐에는 즉시 둘 다 넣고, 실제 시차는 HAPTIC1 소비자 태스크가
            // 자체 지연(HAPTIC1_STAGGER_MS)으로 만든다 -> 크리티컬 섹션에 지연 없음.
            xQueueOverwrite(haptic0_q, &height_payload);
            xQueueOverwrite(haptic1_q, &height_payload);
        }

        if (!g_alert_sleep_active) {
            send_obstacle_alert((uint8_t)best_h, (uint8_t)best_dir,
                                g_zone_grid[best_h][best_dir].min_distance_mm);
        }
    }

    // 스마트 슬립은 "현재 winner 구역"의 거리를 기준으로 갱신 (없으면 비활성 처리)
    const bool winner_exists = g_winner_track.active;
    const int16_t winner_distance = winner_exists ? g_zone_grid[g_winner_track.height][g_winner_track.dir].min_distance_mm : (int16_t)-1;
    update_smart_sleep(winner_distance, winner_exists);
}

// 센서 고장 마스크. bind 실패 / 펌웨어 init 실패 / 런타임 지속 정지에서 세팅된다.
static volatile uint8_t g_tof_fault_mask = 0;

uint8_t TOF_GetFaultMask(void)
{
    return g_tof_fault_mask;
}

static bool TOF_bind(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *out_dev, VL53L5CX_Configuration *cfg, const char *name)
{
    if (i2c_master_bus_add_device(bus, &cfg_29, out_dev) == ESP_OK) {
        cfg->platform.handle = *out_dev;
        cfg->platform.address = 0x52; // 7-bit -> 8-bit 0x52 in previous code
        ESP_LOGI(TAG, "%s bound", name);
        return true;
    }
    ESP_LOGE(TAG, "%s bind failed", name);
    return false;
}

void TOF_INIT(void)
{
    // Bind each TOF to its dedicated I2C bus: bus_handle (I2C0) -> TOF0, bus_handle1 (I2C1) -> TOF1
    // ⚠️ 예전엔 반환값을 버려서, 바인딩이 실패하면 platform.handle이 NULL인 채로 이후
    // 모든 I2C 호출이 그대로 진행됐다. 실패는 반드시 고장 마스크에 남긴다.
    if (!TOF_bind(bus_handle, &tof_dev1, &config_dev0, "TOF0")) {
        g_tof_fault_mask |= TOF_FAULT_TOF0;
    }
    if (!TOF_bind(bus_handle1, &tof_dev2, &config_dev1, "TOF1")) {
        g_tof_fault_mask |= TOF_FAULT_TOF1;
    }

    // Create haptic event queues
    // Single-slot queue prevents backlog that causes long continuous vibration.
    haptic0_q     = xQueueCreate(1, sizeof(uint8_t));
    haptic1_q     = xQueueCreate(1, sizeof(uint8_t));
    alert_q       = xQueueCreate(1, sizeof(haptic_fired_event_t)); // 햅틱 발화(0xB3) 전송 요청용
    grid_stream_q = xQueueCreate(1, sizeof(uint8_t));              // 원본 그리드(0xB2) 전송 요청용

    // 큐 생성 실패를 방치하면 xQueueOverwrite(NULL, ...)에서 assert로 죽는다.
    // 여기서 못 만들면 진동/알림 경로가 통째로 없는 것이므로 명시적으로 남긴다.
    if (haptic0_q == NULL || haptic1_q == NULL || alert_q == NULL || grid_stream_q == NULL) {
        ESP_LOGE(TAG, "큐 생성 실패 (haptic0=%p haptic1=%p alert=%p grid_stream=%p) - 진동/알림 경로 사용 불가",
                 haptic0_q, haptic1_q, alert_q, grid_stream_q);
    }

    if (g_tof_state_mutex == NULL) {
        g_tof_state_mutex = xSemaphoreCreateMutex();
        if (g_tof_state_mutex == NULL) {
            ESP_LOGE(TAG, "ToF 상태 뮤텍스 생성 실패 - 두 센서 태스크가 잠금 없이 공유 배열을 씁니다");
        }
    }
}

static uint8_t TOF_init_with_retry(VL53L5CX_Configuration *p_dev, const char *tag)
{
    const int MAX_RETRY = 3;
    for (int attempt = 1; attempt <= MAX_RETRY; ++attempt) {
        uint8_t st = vl53l5cx_init(p_dev);
        if (st == 0) return 0;
        ESP_LOGW(TAG, "[%s] init failed (status=0x%02X), retry %d", tag, st, attempt);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "[%s] init final fail", tag);
    return 255;
}

void TOF_FIRMWARE_INIT(void)
{
    // ⚠️ 예전엔 반환값을 버렸다. 3회 재시도 끝에 실패해도 그대로 CONFIG_SETUP ->
    // start_ranging으로 진행했고, 태스크는 1초마다 stop/start를 무한 반복하기만 했다.
    // 사용자는 지팡이가 죽은 걸 알 방법이 없었음 — 안전장치에서는 이게 가장 나쁜 실패 방식.
#if !SMARTCANE_TOF0_DISABLED
    if (TOF_init_with_retry(&config_dev0, "TOF0") != 0) {
        g_tof_fault_mask |= TOF_FAULT_TOF0;
    }
#else
    ESP_LOGW(TAG, "TOF0_DISABLED=1 - TOF0 초기화 시도 자체를 건너뜀 (I2C0 오염 방지)");
#endif
    if (TOF_init_with_retry(&config_dev1, "TOF1") != 0) {
        g_tof_fault_mask |= TOF_FAULT_TOF1;
    }

    if (g_tof_fault_mask != 0) {
        ESP_LOGE(TAG, "════════════════════════════════════════════");
        ESP_LOGE(TAG, " ToF 센서 이상 (mask=0x%02X): %s%s", g_tof_fault_mask,
                 (g_tof_fault_mask & TOF_FAULT_TOF0) ? "TOF0(하단) " : "",
                 (g_tof_fault_mask & TOF_FAULT_TOF1) ? "TOF1(상단) " : "");
        ESP_LOGE(TAG, " 해당 방향의 장애물 감지가 동작하지 않습니다.");
        ESP_LOGE(TAG, " 배선(LPN/SDA/SCL)과 전원을 확인하세요.");
        ESP_LOGE(TAG, "════════════════════════════════════════════");
    }
}

// 고장이 지속되는 동안 주기적으로 사용자에게 진동으로 알린다.
// 부팅 시 한 번만 울리면 놓칠 수 있고, 주행 중에 센서가 죽는 경우도 알려야 한다.
#define TOF_FAULT_NOTIFY_PERIOD_MS 15000

void TOF_FAULT_NOTIFY_TASK(void *pvParameters)
{
    (void)pvParameters;
    const uint8_t fault_ev = HAPTIC_EVENT_FAULT;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TOF_FAULT_NOTIFY_PERIOD_MS));

        const uint8_t mask = g_tof_fault_mask;
        if (mask == 0) {
            continue;
        }

        ESP_LOGE(TAG, "ToF 센서 이상 지속 (mask=0x%02X) - 사용자 알림 진동", mask);
        if (haptic0_q != NULL) xQueueOverwrite(haptic0_q, &fault_ev);
        if (haptic1_q != NULL) xQueueOverwrite(haptic1_q, &fault_ev);
    }
}

void TOF_CONFIG_SETUP(void)
{
    // 설정이 실패하면 해상도/주기가 기대와 달라져서 g_raw_grid 매핑 자체가 틀어진다.
    // 조용히 넘어가면 원인 파악이 매우 어려운 종류라 로그로 남긴다.
    uint8_t st;
#if !SMARTCANE_TOF0_DISABLED
    // 4x4 해상도(16칸/센서, 최대 60Hz까지 가능하나 우리는 5Hz만 씀).
    // 반드시 해상도를 먼저 설정한 뒤에 주파수를 설정해야 한다(반대 순서면 검증 실패).
    if ((st = vl53l5cx_set_resolution(&config_dev0, VL53L5CX_RESOLUTION_4X4)) != 0) {
        ESP_LOGE(TAG, "TOF0 set_resolution 실패 (status=0x%02X)", st);
        g_tof_fault_mask |= TOF_FAULT_TOF0;
    }
    if ((st = vl53l5cx_set_ranging_frequency_hz(&config_dev0, 5)) != 0) {
        ESP_LOGE(TAG, "TOF0 set_ranging_frequency 실패 (status=0x%02X)", st);
        g_tof_fault_mask |= TOF_FAULT_TOF0;
    }
#endif

    // TOF0 잠금 모드에서는 TOF1 혼자 전체 판정을 담당하므로 8x8로 올려서 해상도를
    // 최대한 활용한다 (8x8은 15Hz 상한, 우리는 5Hz라 문제 없음). 두 센서를 같이 쓸 때는
    // 동시 레인징 전류 문제로 4x4로 낮췄었는데, 지금은 TOF1 하나뿐이라 그 제약이 없다.
    if ((st = vl53l5cx_set_resolution(&config_dev1, VL53L5CX_RESOLUTION_8X8)) != 0) {
        ESP_LOGE(TAG, "TOF1 set_resolution 실패 (status=0x%02X)", st);
        g_tof_fault_mask |= TOF_FAULT_TOF1;
    }
    if ((st = vl53l5cx_set_ranging_frequency_hz(&config_dev1, 5)) != 0) {
        ESP_LOGE(TAG, "TOF1 set_ranging_frequency 실패 (status=0x%02X)", st);
        g_tof_fault_mask |= TOF_FAULT_TOF1;
    }
}

static void print_tof_grid(const VL53L5CX_ResultsData *results, const char *label)
{
    printf("\n[%s] TOF 8x8 Grid (mm):\n", label);
    printf("    ");
    for (int col = 0; col < SMARTCANE_RAW_GRID_COLS; ++col) printf(" Col%d ", col);
    printf("\n");
    for (int row = 0; row < SMARTCANE_RAW_GRID_COLS; ++row) {
        printf("Row%d:", row);
        for (int col = 0; col < SMARTCANE_RAW_GRID_COLS; ++col) {
            int idx = row * SMARTCANE_RAW_GRID_COLS + col;
            printf("%6d", results->distance_mm[idx]);
        }
        printf("\n");
    }
}

// Haptic consumer (shared implementation). pvParameters = (void*)(uintptr_t)channel
// 큐로 들어오는 값(ev)은 이제 단순 트리거 플래그가 아니라 "어느 높이 구역이었는가"
// (0=TOP, 1=MID, 2=BOTTOM, haptic_pattern_t와 동일 규약)를 실어 나른다.
// CENTER 판정 시 두 모터의 기동 전류가 겹치지 않게 HAPTIC1만 살짝 늦게 시작한다.
// 사람은 20~30ms 차이는 "동시"로 느끼므로 체감상 양쪽 동시 진동이 그대로 유지된다.
// (LEFT 단독 트리거일 때도 25ms 늦어지지만 무시할 수 있는 수준)
#define HAPTIC1_STAGGER_MS 25

void HAPTIC_consumer_task(void *pvParameters)
{
    ledc_channel_t channel = (ledc_channel_t)(uintptr_t)pvParameters;
    QueueHandle_t q = (channel == HAPTIC_0_CHANNEL) ? haptic0_q : haptic1_q;
    const char *tag_name = (channel == HAPTIC_0_CHANNEL) ? "HAPTIC0" : "HAPTIC1";
    const int ch_idx = (channel == HAPTIC_0_CHANNEL) ? 0 : 1;
    const bool is_staggered = (channel == HAPTIC_1_CHANNEL);
    uint8_t ev;

    for (;;) {
        if (xQueueReceive(q, &ev, portMAX_DELAY) == pdTRUE) {
            // 고장 알림은 쿨다운을 무시하고 즉시 재생한다 (사용자가 반드시 알아야 하는 정보).
            // 쿨다운도 갱신하지 않아서 이어지는 장애물 경고를 잡아먹지 않는다.
            if (ev == HAPTIC_EVENT_FAULT) {
                ESP_LOGE(TAG, "[%s] 고장 알림 진동 재생", tag_name);
                Play_Haptic_Pattern(channel, HAPTIC_PATTERN_FAULT);
                Set_Haptic_Intensity(channel, 0);
                continue;
            }

            ESP_LOGI(TAG, "[%s] 큐 수신: height=%u", tag_name, ev);

            const TickType_t now = xTaskGetTickCount();
            if (!tick_reached(now, g_haptic_cooldown_until[ch_idx])) {
                // 생산자가 이미 걸러내므로 여기 오는 건 경합이 났을 때뿐 — 안전망으로 남겨둠
                ESP_LOGI(TAG, "[%s] 쿨다운 중이라 무시", tag_name);
                Set_Haptic_Intensity(channel, 0);
                continue;
            }

            // 재생 시작 전에 (재생시간 + 쿨다운)만큼 미리 예약해둔다. 이렇게 안 하면
            // 재생 중인 500ms 동안 생산자가 "쿨다운 끝남"으로 보고 트리거를 소비해버린다.
            g_haptic_cooldown_until[ch_idx] =
                now + pdMS_TO_TICKS(HAPTIC_PLAY_MAX_MS + HAPTIC_COOLDOWN_MS);

            // 전류 스파이크 겹침 방지용 시차 (ToF 뮤텍스 밖에서 처리됨)
            if (is_staggered) {
                vTaskDelay(pdMS_TO_TICKS(HAPTIC1_STAGGER_MS));
            }

            const haptic_pattern_t pattern =
                (ev <= HAPTIC_PATTERN_BOTTOM) ? (haptic_pattern_t)ev : HAPTIC_PATTERN_BOTTOM;
            ESP_LOGI(TAG, "[%s] Play_Haptic_Pattern 호출 (pattern=%d)", tag_name, (int)pattern);
            Play_Haptic_Pattern(channel, pattern);
            Set_Haptic_Intensity(channel, 0);

            // 실제 재생이 끝난 시점 기준으로 쿨다운을 정확히 다시 잡는다(위 예약분보다 짧아짐)
            g_haptic_cooldown_until[ch_idx] =
                xTaskGetTickCount() + pdMS_TO_TICKS(HAPTIC_COOLDOWN_MS);
        }
    }
}

void HAPTIC0_TASK(void *pvParameters) { HAPTIC_consumer_task((void*)(uintptr_t)HAPTIC_0_CHANNEL); }
void HAPTIC1_TASK(void *pvParameters) { HAPTIC_consumer_task((void*)(uintptr_t)HAPTIC_1_CHANNEL); }

void TOF0_TASK(void *pvParameters)
{
    VL53L5CX_ResultsData results;
    uint8_t is_ready = 0;

    // 이 태스크를 워치독에 등록 — 태스크 자체가 I2C 호출 안에서 완전히 얼어붙는
    // 경우(하드 행)는 이 태스크 스스로는 절대 못 풀어내고, 워치독 패닉->재부팅만이
    // 답이다. menuconfig에서 "Invoke Panic handler on Task Watchdog timeout"을
    // 켜둬야 실제로 재부팅까지 이어진다.
    esp_err_t wdt_add_err = esp_task_wdt_add(NULL);
    if (wdt_add_err != ESP_OK) {
        ESP_LOGE(TAG, "TOF0_TASK 워치독 등록 실패: %s", esp_err_to_name(wdt_add_err));
    }

    if (vl53l5cx_start_ranging(&config_dev0) != 0) {
        ESP_LOGE(TAG, "TOF0 start_ranging 실패");
        g_tof_fault_mask |= TOF_FAULT_TOF0;
    }
    TickType_t last_success_tick = xTaskGetTickCount();
    int consecutive_restarts = 0;

    for (;;) {
        esp_task_wdt_reset();
        if (vl53l5cx_check_data_ready(&config_dev0, &is_ready) == 0 && is_ready) {
            if (vl53l5cx_get_ranging_data(&config_dev0, &results) == 0) {
                last_success_tick = xTaskGetTickCount();
                // 정상 데이터가 돌아왔으면 고장 상태에서 복귀
                if (consecutive_restarts > 0 || (g_tof_fault_mask & TOF_FAULT_TOF0)) {
                    ESP_LOGW(TAG, "TOF0 정상 복귀");
                    consecutive_restarts = 0;
                    g_tof_fault_mask &= (uint8_t)~TOF_FAULT_TOF0;
                }
                print_tof_grid(&results, "TOF0");

                if (g_tof_state_mutex != nullptr) {
                    xSemaphoreTake(g_tof_state_mutex, portMAX_DELAY);
                }
                ingest_sensor_rows_locked(&results, SMARTCANE_RAW_GRID_ROWS / 2); // TOF0 = 하단 물리배치 -> row 4-7
                recompute_zone_grid_locked();
                // 판정은 "살아있는 센서가 모두 새 데이터를 준" 회차마다 1번만
                // (take_eval_round_locked() 주석 참고)
                if (take_eval_round_locked()) {
                    evaluate_zone_grid_and_act_locked("TOF0");
                    request_grid_stream(); // 매 측정 회차마다 원본 그리드도 RPi로 스트리밍
                }
                if (g_tof_state_mutex != nullptr) {
                    xSemaphoreGive(g_tof_state_mutex);
                }
            }
        }

        // 소프트 정지 감지: 태스크는 살아있는데(이 줄까지 도달했다는 게 그 증거)
        // 센서가 새 데이터를 계속 안 주는 경우 -> stop/start로 재시작 시도.
        // (태스크 자체가 얼어붙은 하드 행이면 애초에 이 줄에 도달을 못 하니
        //  이 로직과는 무관 — 그 경우는 위 워치독 패닉이 담당한다.)
        const uint32_t stalled_ms = (xTaskGetTickCount() - last_success_tick) * portTICK_PERIOD_MS;
        if (stalled_ms > TOF_STALL_TIMEOUT_MS) {
            ESP_LOGW(TAG, "TOF0 레인징 %lums 동안 정지 - 재시작 시도 (%d회째)",
                     (unsigned long)stalled_ms, consecutive_restarts + 1);
            vl53l5cx_stop_ranging(&config_dev0);
            vTaskDelay(pdMS_TO_TICKS(50));
            vl53l5cx_start_ranging(&config_dev0);
            last_success_tick = xTaskGetTickCount();

            // 재시작을 반복해도 안 살아나면 "일시적 끊김"이 아니라 고장으로 본다.
            // 이 시점부터 recompute_zone_grid_locked()가 이 절반을 stale로 버리고,
            // FAULT_NOTIFY_TASK가 사용자에게 진동으로 알린다.
            if (++consecutive_restarts >= TOF_STALL_FAULT_RESTARTS) {
                if (!(g_tof_fault_mask & TOF_FAULT_TOF0)) {
                    ESP_LOGE(TAG, "TOF0 재시작 %d회 실패 - 고장으로 판정", consecutive_restarts);
                }
                g_tof_fault_mask |= TOF_FAULT_TOF0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void TOF1_TASK(void *pvParameters)
{
    VL53L5CX_ResultsData results;
    uint8_t is_ready = 0;

    esp_err_t wdt_add_err = esp_task_wdt_add(NULL);
    if (wdt_add_err != ESP_OK) {
        ESP_LOGE(TAG, "TOF1_TASK 워치독 등록 실패: %s", esp_err_to_name(wdt_add_err));
    }

    if (vl53l5cx_start_ranging(&config_dev1) != 0) {
        ESP_LOGE(TAG, "TOF1 start_ranging 실패");
        g_tof_fault_mask |= TOF_FAULT_TOF1;
    }
    TickType_t last_success_tick = xTaskGetTickCount();
    int consecutive_restarts = 0;

    for (;;) {
        esp_task_wdt_reset();
        if (vl53l5cx_check_data_ready(&config_dev1, &is_ready) == 0 && is_ready) {
            if (vl53l5cx_get_ranging_data(&config_dev1, &results) == 0) {
                last_success_tick = xTaskGetTickCount();
                if (consecutive_restarts > 0 || (g_tof_fault_mask & TOF_FAULT_TOF1)) {
                    ESP_LOGW(TAG, "TOF1 정상 복귀");
                    consecutive_restarts = 0;
                    g_tof_fault_mask &= (uint8_t)~TOF_FAULT_TOF1;
                }
                print_tof_grid(&results, "TOF1");

                if (g_tof_state_mutex != nullptr) {
                    xSemaphoreTake(g_tof_state_mutex, portMAX_DELAY);
                }
                ingest_sensor_rows_locked(&results, 0); // TOF1 = 상단 물리배치 -> row 0-3
                recompute_zone_grid_locked();
                if (take_eval_round_locked()) {
                    evaluate_zone_grid_and_act_locked("TOF1");
                    request_grid_stream(); // 매 측정 회차마다 원본 그리드도 RPi로 스트리밍
                }
                if (g_tof_state_mutex != nullptr) {
                    xSemaphoreGive(g_tof_state_mutex);
                }
            }
        }

        const uint32_t stalled_ms = (xTaskGetTickCount() - last_success_tick) * portTICK_PERIOD_MS;
        if (stalled_ms > TOF_STALL_TIMEOUT_MS) {
            ESP_LOGW(TAG, "TOF1 레인징 %lums 동안 정지 - 재시작 시도 (%d회째)",
                     (unsigned long)stalled_ms, consecutive_restarts + 1);
            vl53l5cx_stop_ranging(&config_dev1);
            vTaskDelay(pdMS_TO_TICKS(50));
            vl53l5cx_start_ranging(&config_dev1);
            last_success_tick = xTaskGetTickCount();

            if (++consecutive_restarts >= TOF_STALL_FAULT_RESTARTS) {
                if (!(g_tof_fault_mask & TOF_FAULT_TOF1)) {
                    ESP_LOGE(TAG, "TOF1 재시작 %d회 실패 - 고장으로 판정", consecutive_restarts);
                }
                g_tof_fault_mask |= TOF_FAULT_TOF1;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}