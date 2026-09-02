#include "common.h"
#include "esp_task_wdt.h"

static const char *TAG_MAIN = "MAIN";

// ════════════════════════════════════════════════════════════
// 측정 모드 — RPi 없이 USB-C 하나로 로그를 보면서 ToF 동작을 확인하기 위한 설정
//
// 이 보드는 USB-C가 네이티브 USB Serial/JTAG이고 UART 브리지가 없다. 그래서 로그를
// 보려면 콘솔을 USB Serial/JTAG로 켜야 하는데, 그 페리페럴은 평소 RPi 통신
// (UsbCdcComm)이 쓰는 것과 같다. 둘을 동시에 켜면:
//   - 콘솔은 레지스터 직접 접근(no-driver) 경로로 쓰고
//   - UsbCdcComm은 usb_serial_jtag 드라이버(링버퍼+ISR) 경로로 쓴다
// 즉 같은 엔드포인트에 두 주체가 달라붙어 출력이 섞이고, 드라이버 RX ISR이 콘솔로
// 갈 바이트를 가로챈다. (드라이버 이중 설치 자체는 IDF v6.0에서 ESP_ERR_INVALID_STATE로
//  막히지만, 그건 이 문제와 별개다)
//
// 그래서 측정 모드에서는 USB를 콘솔에게 통째로 넘긴다:
//   - UsbCdcComm_Init() 안 함
//   - ALERT_SENDER_TASK 안 띄움 (이 태스크가 전송 시점에 UsbCdcComm_Init을 다시 부름)
// ToF / 햅틱 / 워치독 / 고장알림은 전부 그대로 동작한다.
//
// ▶ 1로 켤 때 menuconfig에서 같이 바꿀 것:
//     Component config > ESP System Settings > Channel for console output
//       = "USB Serial/JTAG Controller"
//   되돌릴 때는 다시 "Default: UART0" + secondary "No secondary console".
//
// ⚠️ 측정 모드에서는 RPi 연동 테스트가 불가능하다.
// ════════════════════════════════════════════════════════════
#define SMARTCANE_MEASURE_MODE 0

// ⚠️ 개발용 스위치 — 알려진 센서 고장(예: TOF1 모듈 불량)이 있는 상태로 계속
// 개발/테스트할 때, 부팅마다 + 15초마다 울리는 고장 알림 진동이 방해가 될 수 있어
// 끌 수 있게 만든 것. **실제 사용자에게 전달하기 전에는 반드시 0으로 되돌릴 것** —
// 이건 "지팡이 절반이 고장났다"는 걸 사용자에게 알리는 유일한 경로라 꺼두면
// 진짜 고장이 나도 사용자가 모르게 됨.
#define SMARTCANE_SUPPRESS_FAULT_HAPTIC 0

// ════════════════════════════════════════════════════════════
// 태스크 워치독 (TWDT) 초기화
//
// ⚠️ 이게 없으면 TOF 태스크의 esp_task_wdt_add()가 통째로 무효였다.
// sdkconfig에 CONFIG_ESP_TASK_WDT_INIT이 꺼져 있어서(부팅 시 자동 초기화 안 함)
// TWDT가 한 번도 초기화된 적이 없었고, 그 상태에서 esp_task_wdt_add()는
// ESP_ERR_INVALID_STATE로 실패한다. 즉 "I2C 안에서 하드행 -> 패닉 -> 재부팅"이라는
// 유일한 복구 경로가 아예 없는 상태였음. 안전장치에서는 치명적이라 여기서 직접 켠다.
//
// idle_core_mask = 0 : idle 태스크는 감시하지 않는다. (Core1엔 지금 아무 태스크도
//   안 올라가 있어 idle이 잘 돌지만, 나중에 Core1에 오래 걸리는 작업이 붙어도
//   오탐 재부팅이 안 나도록 안전하게 꺼둔다.)
// trigger_panic = true : 타임아웃 시 패닉. sdkconfig에
//   CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y가 이미 켜져 있어 그대로 재부팅으로 이어진다.
// timeout 5초 : TOF 루프가 200ms 주기로 esp_task_wdt_reset()을 부르므로 충분히 여유 있다.
// ════════════════════════════════════════════════════════════
#define TWDT_TIMEOUT_MS 5000

static void init_task_watchdog(void)
{
    const esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = TWDT_TIMEOUT_MS,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };

    esp_err_t err = esp_task_wdt_init(&twdt_cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        // 이미 초기화된 경우(향후 CONFIG_ESP_TASK_WDT_INIT을 켜면 여기로 온다)
        err = esp_task_wdt_reconfigure(&twdt_cfg);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "태스크 워치독 초기화 실패: %s - 하드행 시 자동 재부팅이 안 됩니다",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_MAIN, "태스크 워치독 활성화 (timeout=%dms, panic=on)", TWDT_TIMEOUT_MS);
    }
}

static void start_task_pinned(TaskFunction_t task, const char *name, uint32_t stack_size, UBaseType_t priority, BaseType_t core_id)
{
    if (xTaskCreatePinnedToCore(task, name, stack_size, NULL, priority, NULL, core_id) != pdPASS) {
        ESP_LOGE(TAG_MAIN, "%s task create failed", name);
    }
}

extern "C" void app_main(void)
{
    // 태스크가 esp_task_wdt_add()를 부르기 전에 반드시 먼저 초기화돼 있어야 한다.
    init_task_watchdog();

    gpio_set_direction((gpio_num_t)TOF1_LPN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TOF1_LPN_PIN, 1);
    gpio_set_direction((gpio_num_t)TOF2_LPN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TOF2_LPN_PIN, 1);
    
    // 센서 내부 펌웨어가 켜질 시간을 반드시 기다려야 합니다.
    vTaskDelay(pdMS_TO_TICKS(50));
    I2C_master_INIT();
    // Quick I2C scan on both buses to verify devices respond
    I2C_SCAN(bus_handle, "I2C0");
    I2C_SCAN(bus_handle1, "I2C1");
    TOF_INIT();

    // ⚠️ DRV0 레지스터 설정이 부팅마다 mode/feedback/control3만 골라서 실패하는 게
    // 확인됨(뒤쪽 3개 rated/odclamp/control2는 항상 성공) — 실패/성공이 쓰기 순서 그대로
    // 앞/뒤로 깔끔하게 갈리는 패턴이라, 전원 레일이 아직 다 안정되기 전에 첫 I2C 쓰기가
    // 나가는 타이밍 문제일 가능성을 의심해 여기서 한 번 더 지연을 준다. 원인이 배선
    // 접촉불량이면 이 지연은 효과가 없을 것 — 그 경우 커넥터 재점검이 맞는 방향.
    vTaskDelay(pdMS_TO_TICKS(100));
    DRV_INIT();
    DRV_PWM_INIT();
    // boot self-test for haptics
    Play_Pulse(HAPTIC_0_CHANNEL, 1);
    Play_Pulse(HAPTIC_1_CHANNEL, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    TOF_FIRMWARE_INIT();
    TOF_CONFIG_SETUP();

    // ToF 센서가 죽었으면 사용자에게 즉시 알린다.
    // 아직 햅틱 소비자 태스크를 만들기 전이라 LEDC 채널 경합이 없으므로 직접 재생해도 안전하다.
    // (부팅 후에는 TOF_FAULT_NOTIFY_TASK가 15초마다 큐를 통해 반복 알림)
    const uint8_t boot_fault = TOF_GetFaultMask();
    if (boot_fault != 0) {
#if SMARTCANE_SUPPRESS_FAULT_HAPTIC
        ESP_LOGW(TAG_MAIN, "⚠️ ToF 센서 이상(mask=0x%02X) - SUPPRESS_FAULT_HAPTIC=1 이라 진동 생략", boot_fault);
#else
        ESP_LOGE(TAG_MAIN, "⚠️ ToF 센서 이상으로 부팅 (mask=0x%02X) - 고장 진동 재생", boot_fault);
        Play_Haptic_Pattern(HAPTIC_0_CHANNEL, HAPTIC_PATTERN_FAULT);
        Play_Haptic_Pattern(HAPTIC_1_CHANNEL, HAPTIC_PATTERN_FAULT);
#endif
    }

#if !SMARTCANE_MEASURE_MODE
    // ⚠️ 콘솔이 USB Serial/JTAG로 켜져 있으면 이 호출이 같은 페리페럴을 놓고 다툰다.
    // 라즈베리파이를 연결해서 쓸 때는 콘솔을 UART0 + "No secondary console"로 둘 것.
    if (UsbCdcComm_Init() != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "USB CDC init failed at boot");
    }
#else
    ESP_LOGW(TAG_MAIN, "════════════════════════════════════════════");
    ESP_LOGW(TAG_MAIN, " 측정 모드: USB를 콘솔에 양보합니다");
    ESP_LOGW(TAG_MAIN, " - RPi 통신(0xB2 grid 스트리밍/0xB3 햅틱발화) 비활성");
    ESP_LOGW(TAG_MAIN, " - ToF/햅틱/워치독/고장알림은 정상 동작");
    ESP_LOGW(TAG_MAIN, "════════════════════════════════════════════");
#endif

    ESP_LOGI(TAG_MAIN, "\n════ 데이터 스트림 태스크 생성 ════");

#if !SMARTCANE_TOF0_DISABLED
    start_task_pinned(TOF0_TASK, "TOF0_TASK", 8192, 5, 0);
#else
    ESP_LOGW(TAG_MAIN, "TOF0_DISABLED=1 - TOF0_TASK 미기동 (TOF1 8x8 단독 모드)");
#endif
    start_task_pinned(TOF1_TASK, "TOF1_TASK", 8192, 5, 0);
    start_task_pinned(HAPTIC0_TASK, "HAPTIC0", 4096, 5, 0);
    start_task_pinned(HAPTIC1_TASK, "HAPTIC1", 4096, 5, 0);
#if !SMARTCANE_MEASURE_MODE
    // ToF가 감지한 알림을 USB로 실제 전송하는 태스크들.
    // 예전엔 ToF 태스크가 뮤텍스를 쥔 채로 직접 USB write를 했는데, RPi가 느리거나
    // 안 읽으면 그 동안 거리 측정 루프 전체가 멈추는 구조였음 -> 분리함.
    // (측정 모드에서는 안 띄운다 — 이 태스크들이 전송 시점에 UsbCdcComm_Init을 다시 부르기 때문.
    //  알림 큐는 xQueueOverwrite라 소비자가 없어도 ToF는 막히지 않는다.)
    start_task_pinned(ALERT_SENDER_TASK, "ALERT_TX", 4096, 3, 0);
    // 원본 그리드는 129바이트짜리 구조체를 스택에 두고 직렬화하므로 여유 있게 스택을 준다.
    start_task_pinned(GRID_STREAM_TASK, "GRID_STREAM", 4096, 3, 0);
#endif
#if !SMARTCANE_SUPPRESS_FAULT_HAPTIC
    // 센서 고장이 지속되면 15초마다 구분되는 진동으로 사용자에게 알리는 태스크.
    // 우선순위를 낮게 둬서 실제 장애물 경고 경로를 방해하지 않는다.
    start_task_pinned(TOF_FAULT_NOTIFY_TASK, "TOF_FAULT", 2560, 2, 0);
#else
    ESP_LOGW(TAG_MAIN, "SUPPRESS_FAULT_HAPTIC=1 - 15초 고장 알림 진동 태스크 미기동");
#endif

    ESP_LOGI(TAG_MAIN, "✓ 부팅 시퀀스 완료\n");
}