/**
 * DFR1154 (OV3660) MJPEG 스트리밍 서버
 * 브라우저에서 http://<ESP32-IP>/ 접속
 */
// 모드별로 조건부 정의되는 함수와, 고정 크기 버퍼로의 안전한 snprintf 복사에 대한
// 컴파일러 경고를 이 파일 범위에서 비활성화한다.
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "mp3dec.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "cJSON.h"
#include <math.h>
#include <ctype.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <vector>
#include <algorithm>

#include "bus_dict.h"
#include "voice_clips.h"

static const char *TAG = "ircam";

// RPi로부터 GPS 페이로드를 아직 못 받았을 때 정류장 게이팅에 사용할 기본
// 좌표(캠퍼스 기준). GPS 페이로드가 정상 수신되면 그 값이 우선 사용된다.
#define GPS_USE_DEFAULT_COORD 1
static constexpr float GPS_DEFAULT_LAT = 35.88407372414365f;
static constexpr float GPS_DEFAULT_LON = 128.61325021409556f;

// ── IR / 조도센서 ────────────────────────────────────────────────────────────
#define IR_LED_GPIO      GPIO_NUM_47   // 940nm IR LED 4개
#define ALS_I2C_ADDR     0x53          // LTR-308 (카메라 SCCB와 같은 버스)
#define ALS_I2C_PORT     1             // 카메라 로그에서 확인된 포트

#define IR_ON_LUX        15.0f         // 이보다 어두우면 IR ON
#define IR_OFF_LUX       30.0f         // 이보다 밝으면 IR OFF (히스테리시스)
#define IR_WARMUP_MS     150           // IR 켠 뒤 촬영까지 대기 (안 하면 검은 화면)

typedef enum { IR_MODE_AUTO, IR_MODE_ON, IR_MODE_OFF } ir_mode_t;

static i2c_master_dev_handle_t s_als     = NULL;
static volatile ir_mode_t      s_ir_mode = IR_MODE_AUTO;
static volatile bool           s_ir_on   = false;
static volatile float          s_lux     = -1.0f;

// ── 점자블록 검출 모델 ───────────────────────────────────────────────────────
#define BB_INPUT_SIZE   224
#define BB_GRID_SIZE    14              // 224 입력 -> 14x14 그리드 출력(그리드당 5: dx,dy,w,h,conf)
// 2026-08-17: 실측(arena_used_bytes) 784KB 기준 +31% 여유로 축소 (기존 1200KB)
#define BB_ARENA_SIZE   (1024 * 1024)   // PSRAM에서 잡는다
#define BB_CONF_THRESH  0.7f
#define BB_INTERVAL_MS  2000            // 추론 주기

typedef struct {
    float cx, cy, w, h, conf;
    const char *dir;      // "left" / "center" / "right" / "none"
    int   ms;             // 추론 소요 시간
    bool  valid;
} bb_result_t;

static bb_result_t s_bb = { 0, 0, 0, 0, 0, "none", 0, false };

static tflite::MicroInterpreter *s_bb_interp = nullptr;
static TfLiteTensor             *s_bb_in     = nullptr;
static TfLiteTensor             *s_bb_out    = nullptr;
static uint8_t                  *s_bb_arena  = nullptr;
// 가중치는 flash mmap(RAM 0)으로 유지, arena만 실측 기반으로 축소해서 잡는다.
static const void               *s_bb_model = nullptr;

// ── 스피커 (MAX98357 I2S 앰프) ───────────────────────────────────────────────
#define SPK_BCLK_GPIO    GPIO_NUM_45
#define SPK_LRCLK_GPIO   GPIO_NUM_46
#define SPK_DIN_GPIO     GPIO_NUM_42
#define SPK_GAIN_GPIO    GPIO_NUM_41   // LOW = 15dB(최대), HIGH = 6dB
#define SPK_MODE_GPIO    GPIO_NUM_40   // LOW = shutdown, HIGH = 좌채널 재생
#define SPK_SAMPLE_RATE  24000   // 구글 TTS MP3 출력이 24kHz

static i2s_chan_handle_t s_spk = NULL;
static i2s_std_config_t  s_spk_cfg;
static SemaphoreHandle_t s_spk_lock = NULL;   // TTS/음성클립 중복 재생 방지

// 2026-08-24: 음성 재생을 별도 태스크(audio_task, core0)로 분리하는 큐.
// cane_mode_task(core1)는 이 큐에 키만 넣고 바로 리턴 — 재생 자체(블로킹, 1.5~2초)를
// 기다리지 않는다. 그동안 추론이 계속 돌 수 있게 하는 게 목적 (8.24개발기록 참고).
// 2026-08-26: "버스정류장으로길안내를시작합니다"가 UTF-8로 48바이트라 기존
// 32바이트로는 큐에서 잘려서(중간에 끊긴 멀티바이트) play_voice_clip()의
// strcmp 매칭이 실패했다 — 64로 늘려 여유 확보.
#define AUDIO_QUEUE_KEY_LEN 64
static QueueHandle_t s_audio_queue = NULL;

// 2026-08-30: CMD_FALL_ALERT_START(0x0A)~STOP(0x0B) 사이에 "낙상감지"를
// 무한 반복 재생하기 위한 플래그. audio_task가 큐가 비어 있을 때마다 이 값을
// 확인해서 켜져 있으면 계속 재큐잉한다 — 일반 큐 항목(장애물/모드전환 등)이
// 새로 들어오면 그쪽을 먼저 처리하고, 그 사이사이(최대 50ms 지연)에 반복분을
// 끼워넣는 식이라 다른 안내가 낙상반복 때문에 굶주리지 않는다.
static volatile bool s_fall_alert_active = false;

// 2026-08-18: 버스번호/모드전환 안내용 음성 클립 — flash mmap(RAM 0),
// voice_clips.h의 오프셋 테이블로 이 안에서 잘라 씀. cane_mode_task 부팅 시
// map_partition("voice", ...)으로 채워짐.
static const void *s_voice_data = nullptr;

#define WIFI_SSID "sew"
#define WIFI_PASS "shin0509"
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

// ── 핀맵 ──────────────────────────────────────────────────────────────────────
static const camera_config_t CAM_CFG = {
    .pin_pwdn     = -1,
    .pin_reset    = -1,
    .pin_xclk     = 5,
    .pin_sccb_sda = 8,
    .pin_sccb_scl = 9,
    .pin_d7 = 4,
    .pin_d6 = 6,
    .pin_d5 = 7,
    .pin_d4 = 14,
    .pin_d3 = 17,
    .pin_d2 = 21,
    .pin_d1 = 18,
    .pin_d0 = 16,
    .pin_vsync = 1,
    .pin_href  = 2,
    .pin_pclk  = 15,
    .xclk_freq_hz = 20000000,
    .ledc_timer   = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    // 추론 입력이 그레이스케일이라 센서에서 바로 GRAYSCALE로 받는다.
    // JPEG 디코딩이 필요 없어지고 메모리도 1/3이다.
    // 스트리밍용 JPEG는 video_push_task 안에서 fmt2jpg()로 그때그때 만든다
    // (웹 대시보드/MJPEG 스트림 코드는 2026-08-15 전체 삭제, 개발기록 참고).
    .pixel_format = PIXFORMAT_GRAYSCALE,
    .frame_size   = FRAMESIZE_VGA,        // 640x480
    .jpeg_quality = 12,
    .fb_count     = 2,
    .fb_location  = CAMERA_FB_IN_PSRAM,
    .grab_mode    = CAMERA_GRAB_LATEST,
    .sccb_i2c_port = -1,
};

// ── IR LED ───────────────────────────────────────────────────────────────────
static void ir_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << IR_LED_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(IR_LED_GPIO, 0);
    ESP_LOGI(TAG, "IR LED GPIO%d 초기화", IR_LED_GPIO);
}

static void spk_beep(int freq_hz, int ms, float vol);   // 전방 선언
static bool tts_speak(const char *text);
static volatile bool s_wifi_ready = false;

static void ir_set(bool on)
{
    if (s_ir_on == on) return;
    s_ir_on = on;
    gpio_set_level(IR_LED_GPIO, on ? 1 : 0);
    ESP_LOGI(TAG, "IR LED %s", on ? "ON" : "OFF");
    // 2026-08-18: "적외선 켜짐/꺼짐" 음성 안내 + 대체 비프음 제거 (요청).
    // LED 자체는 그대로 켜지고/꺼짐, 안내음만 없앰.
}

// ── 스피커 ───────────────────────────────────────────────────────────────────
static void spk_init(void)
{
    // GAIN / MODE 는 아날로그 전압으로 동작을 고르는 핀이라 GPIO로 직접 구동한다.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SPK_GAIN_GPIO) | (1ULL << SPK_MODE_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(SPK_GAIN_GPIO, 0);   // 최대 음량
    gpio_set_level(SPK_MODE_GPIO, 1);   // 앰프 enable

    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &s_spk, NULL));

    s_spk_cfg = (i2s_std_config_t){
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_GPIO,
            .ws   = SPK_LRCLK_GPIO,
            .dout = SPK_DIN_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk, &s_spk_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk));

    s_spk_lock = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "스피커 I2S 초기화 (BCLK%d LRCLK%d DIN%d)",
             SPK_BCLK_GPIO, SPK_LRCLK_GPIO, SPK_DIN_GPIO);
}

/* MP3마다 샘플레이트가 다를 수 있어 재생 직전에 맞춰준다 */
static void spk_set_rate(int rate)
{
    static int cur = SPK_SAMPLE_RATE;
    if (rate == cur || rate <= 0) return;
    i2s_channel_disable(s_spk);
    s_spk_cfg.clk_cfg.sample_rate_hz = rate;
    i2s_channel_reconfig_std_clock(s_spk, &s_spk_cfg.clk_cfg);
    i2s_channel_enable(s_spk);
    cur = rate;
}

static void spk_beep(int freq_hz, int ms, float vol)
{
    if (!s_spk) return;

    const int  n_total = SPK_SAMPLE_RATE * ms / 1000;
    const int  chunk   = 256;
    int16_t    buf[chunk];
    size_t     written;
    const float amp  = 32767.0f * vol;
    const float step = 2.0f * (float)M_PI * freq_hz / SPK_SAMPLE_RATE;

    for (int i = 0; i < n_total; i += chunk) {
        int n = (n_total - i < chunk) ? (n_total - i) : chunk;
        for (int k = 0; k < n; k++) {
            // 시작·끝을 부드럽게 해 '툭' 소리 방지
            float env = 1.0f;
            int   pos = i + k;
            if (pos < 200)              env = pos / 200.0f;
            else if (pos > n_total-200) env = (n_total - pos) / 200.0f;
            buf[k] = (int16_t)(amp * env * sinf(step * pos));
        }
        i2s_channel_write(s_spk, buf, n * sizeof(int16_t), &written, 200);
    }
}

// ── 로컬 음성 클립 재생 (버스번호/모드전환 안내) ───────────────────────────────
// voice_clips.h 테이블에서 key를 찾아 flash mmap된 voice 파티션에서 그 구간만
// 그대로 I2S로 흘려보낸다. MP3 디코딩도, PSRAM 복사도 없음 — 전부 24kHz/16bit
// mono PCM으로 통일해뒀기 때문에 spk_set_rate()도 한 번만 24000으로 맞추면 됨.
static bool play_voice_clip(const char *key)
{
    if (!s_spk || !s_spk_lock || !s_voice_data || !key) return false;

    const VoiceClip *clip = nullptr;
    for (int i = 0; i < VOICE_CLIPS_COUNT; i++) {
        if (strcmp(VOICE_CLIPS[i].key, key) == 0) { clip = &VOICE_CLIPS[i]; break; }
    }
    if (!clip) { ESP_LOGW(TAG, "음성 클립 '%s' 없음", key); return false; }

    if (xSemaphoreTake(s_spk_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return false;

    spk_set_rate(SPK_SAMPLE_RATE);   // 전부 24000Hz로 통일돼 있음

    const uint8_t *base = (const uint8_t *)s_voice_data + clip->offset;
    const size_t   chunk = 4096;
    size_t         off = 0;
    while (off < clip->len) {
        size_t n = std::min(chunk, (size_t)(clip->len - off));
        size_t written;
        i2s_channel_write(s_spk, base + off, n, &written, 1000);
        off += n;
    }

    xSemaphoreGive(s_spk_lock);
    return true;
}

// 2026-08-24: cane_mode_task(core1)가 이걸 부르면 큐에 키만 넣고 바로 리턴한다.
// 실제 재생(블로킹)은 audio_task(core0)가 큐를 비우면서 한다 — cane_mode_task는
// 그동안 멈추지 않고 추론을 계속 돌 수 있다. 큐가 꽉 찬 경우(재생이 밀리는 중에
// 새 요청이 또 들어온 경우)는 대기하지 않고 그냥 버린다 — 음성 안내가 밀려서
// 쌓이는 것보다 최신 상태 우선이 낫다는 판단.
static void queue_voice_clip(const char *key) {
    if (!s_audio_queue || !key) return;
    char buf[AUDIO_QUEUE_KEY_LEN] = {0};
    snprintf(buf, sizeof(buf), "%s", key);
    if (xQueueSend(s_audio_queue, buf, 0) != pdTRUE) {
        ESP_LOGW(TAG, "오디오 큐 꽉참 — '%s' 재생 건너뜀", key);
    }
}

static void audio_task(void *arg) {
    char buf[AUDIO_QUEUE_KEY_LEN];
    while (true) {
        // 2026-08-30: portMAX_DELAY로 대기 중이면 s_fall_alert_active가 나중에
        // true로 바뀌어도 그 블로킹 콜이 안 깨어나서(FreeRTOS 큐 대기는 타임아웃을
        // 재평가 안 함) 0x0A 받고도 이미 대기 중이던 호출이 안 풀리면 재생이
        // 무기한 늦어지는 버그가 있었다 — 실기기 로그로 확인됨(0x0A 로그는 찍히는데
        // 이후 재생 안 됨). 항상 100ms로 짧게 폴링하도록 고쳐서 플래그 변경을
        // 최대 100ms 안에 반영하게 함. 대부분 시간은 play_voice_clip()의 블로킹
        // 재생으로 이미 CPU를 안 태우므로 폴링 오버헤드는 무시 가능.
        if (xQueueReceive(s_audio_queue, buf, pdMS_TO_TICKS(100)) == pdTRUE) {
            play_voice_clip(buf);
        } else if (s_fall_alert_active) {
            play_voice_clip("낙상감지");
        }
    }
}

// ── TTS (구글 번역 TTS → MP3 → I2S) ─────────────────────────────────────────
#define TTS_UA  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
                "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"

static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
}

static bool tts_speak(const char *text)
{
    if (!s_spk || !s_spk_lock) return false;
    if (xSemaphoreTake(s_spk_lock, pdMS_TO_TICKS(3000)) != pdTRUE) return false;

    bool ok = false;

    char enc[512];
    url_encode(text, enc, sizeof(enc));

    char url[768];
    snprintf(url, sizeof(url),
             "https://translate.google.com/translate_tts"
             "?ie=UTF-8&client=tw-ob&tl=ko&q=%s", enc);

    // 버퍼는 PSRAM에 잡는다 (내부 RAM 아끼기)
    const size_t IN_SZ  = 8192;
    uint8_t *in   = (uint8_t *)heap_caps_malloc(IN_SZ, MALLOC_CAP_SPIRAM);
    int16_t *pcm  = (int16_t *)heap_caps_malloc(1152 * 2 * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    HMP3Decoder dec = MP3InitDecoder();
    esp_http_client_handle_t cli = NULL;

    if (!in || !pcm || !dec) { ESP_LOGE(TAG, "TTS 메모리 부족"); goto done; }

    {
        esp_http_client_config_t cfg = {};
        cfg.url             = url;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms      = 8000;
        cfg.buffer_size     = 2048;
        cli = esp_http_client_init(&cfg);
        if (!cli) goto done;
        esp_http_client_set_header(cli, "User-Agent", TTS_UA);
        esp_http_client_set_header(cli, "Referer", "https://translate.google.com/");

        if (esp_http_client_open(cli, 0) != ESP_OK) {
            ESP_LOGE(TAG, "TTS 연결 실패");
            goto done;
        }
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        if (status != 200) {
            ESP_LOGE(TAG, "TTS HTTP %d", status);
            goto done;
        }
    }

    {
        int  fill = 0;
        bool eof  = false;
        while (true) {
            if (!eof && fill < (int)IN_SZ) {
                int r = esp_http_client_read(cli, (char *)in + fill, IN_SZ - fill);
                if (r > 0)      fill += r;
                else if (r == 0) eof = true;
                else             break;
            }
            if (fill == 0 && eof) { ok = true; break; }

            uint8_t *p    = in;
            int      left = fill;
            int      off  = MP3FindSyncWord(p, left);
            if (off < 0) {                       // 동기 못 찾음 → 버퍼 비우고 계속
                fill = 0;
                if (eof) { ok = true; break; }
                continue;
            }
            p    += off;
            left -= off;

            int err = MP3Decode(dec, &p, &left, pcm, 0);
            if (err) {
                if (err == ERR_MP3_INDATA_UNDERFLOW && !eof) {
                    memmove(in, p, left);        // 데이터 더 받아서 재시도
                    fill = left;
                    continue;
                }
                fill = 0;
                if (eof) { ok = true; break; }
                continue;
            }

            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            spk_set_rate(fi.samprate);

            size_t written;
            i2s_channel_write(s_spk, pcm, fi.outputSamps * sizeof(int16_t),
                              &written, 1000);

            memmove(in, p, left);
            fill = left;
        }
    }

done:
    if (cli) { esp_http_client_close(cli); esp_http_client_cleanup(cli); }
    if (dec) MP3FreeDecoder(dec);
    free(in);
    free(pcm);
    xSemaphoreGive(s_spk_lock);
    if (!ok) ESP_LOGW(TAG, "TTS 실패: %s", text);
    return ok;
}

// ── LTR-308 조도센서 (카메라가 만든 I2C 버스에 얹는다) ───────────────────────
// I2C는 원래 1:N 규격이라 주소만 다르면 공존한다. 카메라 0x3C, ALS 0x53.
static esp_err_t als_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_als, buf, 2, 100);
}

static esp_err_t als_read(uint8_t reg, uint8_t *dst, size_t len)
{
    return i2c_master_transmit_receive(s_als, &reg, 1, dst, len, 100);
}

static bool als_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(ALS_I2C_PORT, &bus) != ESP_OK || !bus) {
        ESP_LOGW(TAG, "I2C 버스 핸들 획득 실패 → 조도센서 없이 동작");
        return false;
    }

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ALS_I2C_ADDR,
        .scl_speed_hz    = 100000,
        .scl_wait_us     = 0,
        .flags           = {},
    };
    if (i2c_master_bus_add_device(bus, &dev, &s_als) != ESP_OK) {
        ESP_LOGW(TAG, "조도센서 등록 실패 → 조도센서 없이 동작");
        s_als = NULL;
        return false;
    }

    // MAIN_CTRL(0x00) bit1 = ALS enable
    if (als_write(0x00, 0x02) != ESP_OK) {
        ESP_LOGW(TAG, "조도센서 응답 없음 → 조도센서 없이 동작");
        s_als = NULL;
        return false;
    }
    als_write(0x04, 0x22);   // ALS_MEAS_RATE: 18bit 해상도, 100ms 주기
    als_write(0x05, 0x01);   // ALS_GAIN: x3

    ESP_LOGI(TAG, "LTR-308 조도센서 초기화 성공");
    return true;
}

static float als_read_lux(void)
{
    if (!s_als) return -1.0f;
    uint8_t d[3];
    if (als_read(0x0D, d, 3) != ESP_OK) return -1.0f;   // ALS_DATA_0..2
    uint32_t raw = d[0] | (d[1] << 8) | ((uint32_t)(d[2] & 0x0F) << 16);
    // lux = 0.6 * raw / (gain * int_time),  gain=3, int_time=1.0 (100ms/18bit)
    return 0.6f * raw / 3.0f;
}

// ── IR 자동 제어 태스크 ──────────────────────────────────────────────────────
static void ir_task(void *arg)
{
    while (true) {
        if (s_ir_mode == IR_MODE_ON) {
            ir_set(true);
        } else if (s_ir_mode == IR_MODE_OFF) {
            ir_set(false);
        } else {
            float lux = als_read_lux();
            s_lux = lux;
            if (lux >= 0.0f) {
                if (!s_ir_on && lux < IR_ON_LUX)       ir_set(true);
                else if (s_ir_on && lux > IR_OFF_LUX)  ir_set(false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ── 점자블록 모델 ────────────────────────────────────────────────────────────
/* 파티션을 flash에 매핑해 포인터처럼 쓴다. RAM 복사가 없다. */
static const void *map_partition(const char *label, size_t *out_size)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, label);
    if (!part) {
        ESP_LOGE(TAG, "파티션 '%s' 없음", label);
        return nullptr;
    }
    const void *ptr = nullptr;
    esp_partition_mmap_handle_t h{};
    if (esp_partition_mmap(part, 0, part->size,
                           ESP_PARTITION_MMAP_DATA, &ptr, &h) != ESP_OK) {
        ESP_LOGE(TAG, "파티션 '%s' mmap 실패", label);
        return nullptr;
    }
    if (out_size) *out_size = part->size;
    return ptr;
}

static void bb_deinit(void);

// 모드 전환마다 이 arena를 해제/재할당한다 (한 번에 하나의 모델만 메모리에 있으면 되므로).
static bool bb_init(void)
{
    if (s_bb_interp) return true;  // 이미 로드돼 있으면 재사용

    // 가중치는 flash mmap 상태 그대로 사용 (cane_mode_task 부팅 시
    // map_partition으로 s_bb_model에 로드, RAM 0).
    if (!s_bb_model) return false;

    const tflite::Model *model = tflite::GetModel(s_bb_model);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "모델 스키마 버전 불일치 (%lu)", (unsigned long)model->version());
        return false;
    }

    s_bb_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, BB_ARENA_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_bb_arena) {
        ESP_LOGE(TAG, "tensor arena 할당 실패 (%d bytes)", BB_ARENA_SIZE);
        return false;
    }

    // MobileNetV2 + 회귀 헤드에 필요한 연산들.
    // gray_to_rgb 전처리가 MUL/SUB/CONCAT 등을 만들 수 있어 넉넉히 등록한다.
    // obs_init()/bus_det_init()과 동일하게, 재진입(점자블록 모드 재진입)마다
    // 다시 등록하지 않도록 최초 1회만 실행되게 가드한다 (2026-08-14 개발기록 참고).
    static tflite::MicroMutableOpResolver<16> resolver;
    static bool resolver_ready = false;
    if (!resolver_ready) {
        resolver.AddConv2D();
        resolver.AddDepthwiseConv2D();
        resolver.AddAdd();
        resolver.AddPad();
        resolver.AddReshape();
        resolver.AddFullyConnected();
        resolver.AddLogistic();
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddMul();
        resolver.AddSub();
        resolver.AddConcatenation();
        resolver.AddRelu6();
        resolver.AddMean();
        resolver.AddTranspose();
        resolver.AddStridedSlice();
        resolver_ready = true;
    }

    // 모드 전환 때마다 alloc/free를 반복하므로 function-local static이 아니라
    // 힙(new)에 만들어서 s_bb_interp로 들고 있어야 deinit에서 delete 가능하다.
    tflite::MicroInterpreter *interp = new tflite::MicroInterpreter(model, resolver, s_bb_arena, BB_ARENA_SIZE);
    if (interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors 실패 — arena 부족이거나 미지원 연산");
        delete interp;
        heap_caps_free(s_bb_arena);
        s_bb_arena = nullptr;
        return false;
    }

    s_bb_interp = interp;
    s_bb_in     = interp->input(0);
    s_bb_out    = interp->output(0);

    if (s_bb_in->type != kTfLiteInt8 || s_bb_out->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "점자블록 모델 입출력이 int8이 아님 (in=%d, out=%d)",
                 (int)s_bb_in->type, (int)s_bb_out->type);
        bb_deinit();
        return false;
    }

    ESP_LOGI(TAG, "모델 로드 완료");
    ESP_LOGI(TAG, "  입력 %dx%dx%d type=%d",
             s_bb_in->dims->data[1], s_bb_in->dims->data[2],
             s_bb_in->dims->data[3], s_bb_in->type);
    ESP_LOGI(TAG, "  출력 %d개 type=%d", s_bb_out->dims->data[1], s_bb_out->type);
    ESP_LOGI(TAG, "  arena 사용 %u / %u KB",
             (unsigned)(interp->arena_used_bytes() / 1024), BB_ARENA_SIZE / 1024);
    return true;
}

static void bb_deinit(void)
{
    if (s_bb_interp) { delete s_bb_interp; s_bb_interp = nullptr; }
    if (s_bb_arena)  { heap_caps_free(s_bb_arena); s_bb_arena = nullptr; }
    // 가중치(s_bb_model)는 mmap 포인터라 free 대상이 아님 (장애물/버스와 동일).
    s_bb_in = nullptr;
    s_bb_out = nullptr;
}

/* 640x480 그레이스케일 → 224x224, [0,1] 정규화 (bilinear) */
static void resize_gray(const uint8_t *src, int sw, int sh, float *dst, int dn)
{
    const float rx = (float)sw / dn;
    const float ry = (float)sh / dn;
    for (int y = 0; y < dn; y++) {
        float  fy = (y + 0.5f) * ry - 0.5f;
        int    y0 = (int)floorf(fy);
        float  wy = fy - y0;
        int    y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > sh - 1) y1 = sh - 1;

        for (int x = 0; x < dn; x++) {
            float fx = (x + 0.5f) * rx - 0.5f;
            int   x0 = (int)floorf(fx);
            float wx = fx - x0;
            int   x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > sw - 1) x1 = sw - 1;

            float p00 = src[y0 * sw + x0], p01 = src[y0 * sw + x1];
            float p10 = src[y1 * sw + x0], p11 = src[y1 * sw + x1];
            float top = p00 + (p01 - p00) * wx;
            float bot = p10 + (p11 - p10) * wx;
            dst[y * dn + x] = (top + (bot - top) * wy) / 255.0f;
        }
    }
}

// buf/w/h는 카메라 프레임의 복사본 — 호출부가 esp_camera_fb_return()을 먼저
// 끝내고 나서 넘겨야 한다 (fb를 추론 내내 붙잡고 있으면 다른 태스크의 프레임
// 획득이 밀림, 2026-08-14 개발기록 참고).
//
// 모델(brailledet_v1_pure_int8.tflite)은 완전 int8 양자화된 14x14 그리드
// YOLO 스타일 단일클래스 검출기다 (2026-08-15 학습 코드/실측으로 확정):
//   입력  [1,224,224,1] int8, scale/zero_point는 텐서에서 읽음
//   출력  [1,14,14,5]   int8, 셀당 (dx,dy,w,h,conf) — 시그모이드 출력이라
//         디코딩 전 정규화([0,1]) 값으로 그대로 씀 (버스 det과 동일한 인코딩)
// 그리드 전체에서 conf가 최댓값인 셀 하나만 골라 박스로 쓴다
// (학습 코드의 encode_yolo_target/evaluate_presence와 동일한 방식 — 셀당 최대 1개 객체 가정).
static void bb_infer(const uint8_t *buf, int w, int h)
{
    if (!s_bb_interp || !buf) return;

    int64_t t0 = esp_timer_get_time();

    // 1)+2) 리사이즈 + [0,1] 정규화 + int8 양자화를 한 루프에서 처리.
    // 중간 float staging 버퍼(224*224*4=196KB)를 따로 두면 내부 DRAM(.bss)을
    // 통째로 잡아먹어 링크 단계에서 dram0_0_seg 오버플로가 난다 (실측: 14,976
    // bytes 초과). obs_prepare_input/bus_prepare_det_input과 동일하게 픽셀
    // 단위로 리사이즈 값을 계산한 즉시 양자화해서 추가 버퍼 없이 처리한다.
    const float in_scale = s_bb_in->params.scale;
    const int32_t in_zp  = s_bb_in->params.zero_point;
    int8_t *in8 = s_bb_in->data.int8;
    const float rx = (float)w / BB_INPUT_SIZE;
    const float ry = (float)h / BB_INPUT_SIZE;
    for (int oy = 0; oy < BB_INPUT_SIZE; oy++) {
        float fy = (oy + 0.5f) * ry - 0.5f;
        int   y0 = (int)floorf(fy);
        float wy = fy - y0;
        int   y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > h - 1) y1 = h - 1;

        for (int ox = 0; ox < BB_INPUT_SIZE; ox++) {
            float fx = (ox + 0.5f) * rx - 0.5f;
            int   x0 = (int)floorf(fx);
            float wx = fx - x0;
            int   x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > w - 1) x1 = w - 1;

            float p00 = buf[y0 * w + x0], p01 = buf[y0 * w + x1];
            float p10 = buf[y1 * w + x0], p11 = buf[y1 * w + x1];
            float top = p00 + (p01 - p00) * wx;
            float bot = p10 + (p11 - p10) * wx;
            float val = (top + (bot - top) * wy) / 255.0f;

            int32_t q = (int32_t)lroundf(val / in_scale) + in_zp;
            q = std::max<int32_t>(-128, std::min<int32_t>(127, q));
            in8[oy * BB_INPUT_SIZE + ox] = (int8_t)q;
        }
    }

    if (s_bb_interp->Invoke() != kTfLiteOk) {
        ESP_LOGE(TAG, "추론 실패");
        return;
    }

    // 3) 출력 (1,14,14,5) int8 -> 역양자화 후 그리드 전체에서 conf 최댓값 셀 탐색
    const float out_scale = s_bb_out->params.scale;
    const int32_t out_zp  = s_bb_out->params.zero_point;
    const int8_t *raw_out = s_bb_out->data.int8;

    int best_gx = 0, best_gy = 0;
    float best_conf = -1.0f, best_dx = 0, best_dy = 0, best_w = 0, best_h = 0;

    for (int gy = 0; gy < BB_GRID_SIZE; gy++) {
        for (int gx = 0; gx < BB_GRID_SIZE; gx++) {
            int base = (gy * BB_GRID_SIZE + gx) * 5;
            float conf = (raw_out[base + 4] - out_zp) * out_scale;
            if (conf > best_conf) {
                best_conf = conf;
                best_gx = gx; best_gy = gy;
                best_dx = (raw_out[base + 0] - out_zp) * out_scale;
                best_dy = (raw_out[base + 1] - out_zp) * out_scale;
                best_w  = (raw_out[base + 2] - out_zp) * out_scale;
                best_h  = (raw_out[base + 3] - out_zp) * out_scale;
            }
        }
    }

    bb_result_t r;
    r.cx   = (best_gx + best_dx) / (float)BB_GRID_SIZE;
    r.cy   = (best_gy + best_dy) / (float)BB_GRID_SIZE;
    r.w    = best_w;
    r.h    = best_h;
    r.conf = best_conf;
    r.ms   = (int)((esp_timer_get_time() - t0) / 1000);

    if (r.conf >= BB_CONF_THRESH) {
        r.dir   = (r.cx < 0.35f) ? "left" : (r.cx > 0.65f) ? "right" : "center";
        r.valid = true;
    } else {
        r.dir   = "none";
        r.valid = false;
    }
    s_bb = r;

    ESP_LOGI(TAG, "추론 %dms  conf=%.2f  box=(%.2f,%.2f,%.2f,%.2f)  %s",
             r.ms, r.conf, r.cx, r.cy, r.w, r.h, r.dir);
}

static void bb_task(void *arg)
{
    static uint8_t *copy = nullptr;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (!copy) copy = (uint8_t *)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
            int w = fb->width, h = fb->height;
            if (copy) memcpy(copy, fb->buf, fb->len);
            esp_camera_fb_return(fb);
            if (copy) bb_infer(copy, w, h);
        }
        vTaskDelay(pdMS_TO_TICKS(BB_INTERVAL_MS));
    }
}

// ── 버스정류장 게이팅 (GPS 근접 확인 + 실시간 도착정보) ────────────────────────
// 2026-08-23: 버스모드 진입 전에, 사용자 위치에서 "가장 가까운 정류장"을
// 무조건 찾고(거리 임계값 없음 — 멀든 가깝든 그냥 최근접 하나) 거기에 1분
// 이내 도착하는 버스가 있는지만 확인한다. 없으면 버스모드(카메라 OCR)로
// 안 들어가고 장애물모드 유지. RPi 쪽 옛날 코드(smartcane 통합본,
// bus_mode())의 로직을 그대로 ESP32로 옮겨온 것 — GPS는 RPi가 갖고 있으므로
// MODE_CMD_BUS 명령에 위도/경도를 실어 보내게 프로토콜을 확장했다
// (usb_cdc_poll_cmd 아래 참고).
//
// 정류장 DB(대구, 5913개)는 공공데이터(raw_response.json의 bs 배열)에서
// bsId(uint64)+lat/lon(float32) 16바이트짜리 레코드로 미리 뽑아
// data/stations.bin으로 변환해뒀고, "stations" 파티션(128K)에 mmap해서 씀
// (모델/음성 클립이랑 동일 패턴, RAM 0). DB 갱신하려면 raw_response.json을
// 새로 받아서 변환 스크립트를 다시 돌려야 한다 — 이 코드는 정적 스냅샷임.

struct StationEntry {
    uint64_t bsId;
    float    lat;
    float    lon;
} __attribute__((packed));

// stations.bin 생성 시점 실측 개수. 파티션 크기(128K)가 아니라 이 값 기준으로
// 순회한다 — map_partition()이 돌려주는 size는 파티션 크기(패딩 포함)라
// 실제 레코드 수와 다르다(2026-08-17에 이 착각으로 PSRAM 예산을 잘못 계산한
// 적 있음, 개발기록 참고). stations.bin을 재생성하면 이 숫자도 같이 갱신할 것.
static const int STATION_COUNT = 5913;

static const void *s_stations_data = nullptr;

// 2026-08-23: "정류장이 근처에 있냐" 판단은 안 함 — 무조건 최근접 정류장을
// 쓰고 그 정류장의 도착정보만 본다. 그래서 거리 임계값 상수는 없음.
// "곧 도착"으로 볼 기준(초). TAGO API의 arrTime 단위가 초라고 가정.
static constexpr int BUS_ARRIVE_WITHIN_SEC = 300;

static const char *TAGO_API_KEY  = "f792f3796e4bc89ee569a424ee3a75b3074b2ab4b80b58d8a864610b2c5e3729";
static const char *TAGO_REALTIME_URL = "https://apis.data.go.kr/6270000/dbmsapi02/getRealtime02";

// 위경도 두 점 사이 거리(m). RPi 옛날 코드의 haversine()과 동일 공식.
static float gps_distance_m(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f;
    float phi1 = lat1 * (float)M_PI / 180.0f;
    float phi2 = lat2 * (float)M_PI / 180.0f;
    float dphi = (lat2 - lat1) * (float)M_PI / 180.0f;
    float dlmb = (lon2 - lon1) * (float)M_PI / 180.0f;
    float a = sinf(dphi / 2) * sinf(dphi / 2)
            + cosf(phi1) * cosf(phi2) * sinf(dlmb / 2) * sinf(dlmb / 2);
    float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
    return R * c;
}

// stations 파티션 전체를 선형 순회해서 최근접 정류장을 찾는다. 5913개 정도는
// 매번 다 훑어도 ESP32-S3에서 무시할 만한 시간(수백 마이크로초 수준)이라
// 별도 공간 인덱싱(그리드/kd-tree)은 안 씀 — 필요해지면 그때 추가.
static bool find_nearest_station(float my_lat, float my_lon,
                                  uint64_t *out_bs_id, float *out_dist_m) {
    if (!s_stations_data) return false;
    const StationEntry *arr = (const StationEntry *)s_stations_data;

    float best_dist = 1e30f;
    uint64_t best_id = 0;
    bool found = false;

    for (int i = 0; i < STATION_COUNT; i++) {
        float d = gps_distance_m(my_lat, my_lon, arr[i].lat, arr[i].lon);
        if (d < best_dist) { best_dist = d; best_id = arr[i].bsId; found = true; }
    }
    if (!found) return false;
    *out_bs_id  = best_id;
    *out_dist_m = best_dist;
    return true;
}

// 2026-08-28: 정류장 게이팅에서 API가 실제로 찾아준 노선번호를 이번 세션의
// 동적 후보군으로 쓴다(고정 리스트 대신). 도착시각이 서로
// BUS_CLUSTER_WINDOW_SEC 이내로 가까운 것들만 묶어서 후보로 삼고("연달아
// 오는 무리"), 그보다 멀리 떨어진 건 이번엔 후보에서 뺀다 — 예를 들어
// 180초/220초처럼 40초 차이나면 더 빨리 오는 쪽 하나만 후보가 됨. GPS/
// 정류장DB 없어서 API를 아예 못 부르는 폴백 경로에서는 옛날 고정 리스트
// (BUS_CANDIDATES)로 되돌아간다 — s_bus_use_api_candidates가 그 분기를 결정.
struct BusApiCandidate {
    char route_no[16];
    int  arr_time_sec;
    bool consumed;   // 이번 무리 안에서 이미 매칭됨 — 다음 재진입 매칭에서 제외
};
static constexpr int kBusApiCandidateMax = 8;
static BusApiCandidate s_bus_api_candidates[kBusApiCandidateMax];
static int  s_bus_api_candidate_count = 0;
static bool s_bus_use_api_candidates  = false;
static constexpr int BUS_CLUSTER_WINDOW_SEC = 30;

// TAGO 실시간 도착정보 조회 — BUS_ARRIVE_WITHIN_SEC 이내 도착하는 노선들을
// (routeNo + 그 노선의 최소 arrTime으로) 전부 모은 뒤, 가장 빨리 오는 것
// 기준 BUS_CLUSTER_WINDOW_SEC 이내인 것들만 s_bus_api_candidates[]에 채운다.
// 리턴값은 그렇게 채워진 후보 개수(0=없음). 2026-08-26엔 "노선 개수"만
// 셌었는데, 2026-08-28에 노선번호 자체 + 클러스터링까지 하도록 구조 변경.
// RPi 옛날 코드(get_arriving_buses())와 동일한 엔드포인트/파라미터/응답구조.
// 응답 버퍼는 PSRAM(일시적 사용, 호출 끝나면 바로 free — arena/가중치처럼
// 상주시키는 게 아니라서 오늘 겪은 PSRAM 예산 문제와는 무관함).
static int gather_bus_candidates(uint64_t bs_id) {
    s_bus_api_candidate_count = 0;

    char url[256];
    snprintf(url, sizeof(url), "%s?serviceKey=%s&bsId=%llu&_type=json",
             TAGO_REALTIME_URL, TAGO_API_KEY, (unsigned long long)bs_id);

    const size_t BUF_SZ = 16384;
    char *resp_buf = (char *)heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!resp_buf) { ESP_LOGE(TAG, "도착정보 응답버퍼 할당 실패"); return 0; }
    resp_buf[0] = '\0';

    struct RespCtx { char *buf; int len; size_t cap; } ctx = { resp_buf, 0, BUF_SZ };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA) {
            auto *c = (RespCtx *)evt->user_data;
            if (c->buf && (size_t)(c->len + evt->data_len) < c->cap - 1) {
                memcpy(c->buf + c->len, evt->data, evt->data_len);
                c->len += evt->data_len;
                c->buf[c->len] = '\0';
            }
        }
        return ESP_OK;
    };
    cfg.user_data = &ctx;

    // 노선별 최소 arrTime을 임시로 모은다(같은 노선 여러 arrTime 중 제일 빠른 것만).
    struct Tmp { char route_no[16]; int arr_time_sec; };
    Tmp tmp[kBusApiCandidateMax];
    int tmp_count = 0;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        esp_err_t err = esp_http_client_perform(cli);
        int status = esp_http_client_get_status_code(cli);
        if (err == ESP_OK && status == 200) {
            cJSON *root = cJSON_Parse(resp_buf);
            if (root) {
                cJSON *body  = cJSON_GetObjectItem(root, "body");
                cJSON *items = body ? cJSON_GetObjectItem(body, "items") : nullptr;
                if (items && cJSON_IsArray(items)) {
                    cJSON *route;
                    cJSON_ArrayForEach(route, items) {
                        cJSON *route_no = cJSON_GetObjectItem(route, "routeNo");
                        cJSON *arr_list = cJSON_GetObjectItem(route, "arrList");
                        if (!route_no || !cJSON_IsString(route_no)) continue;
                        if (!arr_list || !cJSON_IsArray(arr_list)) continue;

                        int best_time = -1;
                        cJSON *arr;
                        cJSON_ArrayForEach(arr, arr_list) {
                            cJSON *arr_time = cJSON_GetObjectItem(arr, "arrTime");
                            if (arr_time && cJSON_IsNumber(arr_time) &&
                                arr_time->valueint <= BUS_ARRIVE_WITHIN_SEC) {
                                if (best_time < 0 || arr_time->valueint < best_time) {
                                    best_time = arr_time->valueint;
                                }
                            }
                        }
                        if (best_time >= 0 && tmp_count < kBusApiCandidateMax) {
                            snprintf(tmp[tmp_count].route_no, sizeof(tmp[tmp_count].route_no),
                                     "%s", route_no->valuestring);
                            tmp[tmp_count].arr_time_sec = best_time;
                            tmp_count++;
                            ESP_LOGI(TAG, "[정류장체크] %s호 %d초 후 도착",
                                     route_no->valuestring, best_time);
                        }
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "[정류장체크] 도착정보 JSON 파싱 실패");
            }
        } else {
            ESP_LOGW(TAG, "[정류장체크] 도착정보 요청 실패 err=%s status=%d",
                     esp_err_to_name(err), status);
        }
        esp_http_client_cleanup(cli);
    }
    heap_caps_free(resp_buf);

    if (tmp_count == 0) return 0;

    // 도착시각 오름차순 정렬 (개수 적어서 선택정렬로 충분).
    for (int i = 0; i < tmp_count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < tmp_count; j++)
            if (tmp[j].arr_time_sec < tmp[best].arr_time_sec) best = j;
        if (best != i) { Tmp t = tmp[i]; tmp[i] = tmp[best]; tmp[best] = t; }
    }

    // 가장 빨리 오는 것 기준 30초 이내인 것들만 후보로 담는다 — 정렬돼 있으니
    // 처음으로 30초를 넘는 지점에서 끊으면 된다.
    int earliest = tmp[0].arr_time_sec;
    for (int i = 0; i < tmp_count && s_bus_api_candidate_count < kBusApiCandidateMax; i++) {
        if (tmp[i].arr_time_sec - earliest > BUS_CLUSTER_WINDOW_SEC) break;
        BusApiCandidate &c = s_bus_api_candidates[s_bus_api_candidate_count++];
        snprintf(c.route_no, sizeof(c.route_no), "%s", tmp[i].route_no);
        c.arr_time_sec = tmp[i].arr_time_sec;
        c.consumed = false;
    }
    ESP_LOGI(TAG, "[정류장체크] 후보 %d개로 좁힘 (최근접 도착 %d초 기준 %d초 이내)",
             s_bus_api_candidate_count, earliest, BUS_CLUSTER_WINDOW_SEC);

    return s_bus_api_candidate_count;
}

// ── 버스 번호 OCR (esp32ocr 프로젝트 로직 이식, RPi 없이 카메라 직결) ─────────
// esp32ocr/main.cpp를 참고해서 새로 작성한 코드. esp32ocr 폴더 자체는 건드리지 않음.
// 카메라가 이미 그레이스케일 640x480(VGA)이라 RGB 가중합 없이 바로 읽는다.

#define BUS_ORIG_W 640
#define BUS_ORIG_H 480
#define BUS_DET_W  224
#define BUS_DET_H  224
#define BUS_GRID_SIZE 14
#define BUS_REC_H 32
#define BUS_REC_W 160

static constexpr float BUS_DET_CONF_THRES = 0.65f;
static constexpr float BUS_DET_IOU_THRES  = 0.30f;

// 2026-08-17: 실측(arena_used_bytes) 기준 여유 30~50% 붙여서 축소
// (기존 det 3500KB / rec 1024KB, 실사용 783KB / 210KB)
static constexpr int kBusDetArenaSize = 1024 * 1024;
static constexpr int kBusRecArenaSize = 320 * 1024;

static uint8_t *s_bus_det_arena = nullptr;
static uint8_t *s_bus_rec_arena = nullptr;
static tflite::MicroInterpreter *s_bus_det_interp = nullptr;
static tflite::MicroInterpreter *s_bus_rec_interp = nullptr;

static const char *BUS_CANDIDATES[] = {"937", "503", "북구2", "410", "410-1"};
static const int BUS_CANDIDATES_COUNT = 5;

static char       s_bus_last_match[32] = {0};
static SemaphoreHandle_t s_bus_result_lock = NULL;

// 2026-08-26: 정류장 게이팅에서 gather_bus_candidates()(2026-08-28에
// count_buses_arriving_soon()에서 개명/확장)가 몇 대를 찾았는지 기억해두고,
// 버스모드에서 하나 매칭될 때마다 비교한다. 매칭 수가 목표 수보다 적으면
// 장애물모드로 안 돌아가고 버스모드를 새로 재진입(30초 타이머 리셋)해서
// 나머지도 잡으러 간다. GPS/정류장DB 없어서 API 자체를 못 부른 폴백
// 경로들은 1로 기본값 설정(예전처럼 한 번만 시도, 고정 리스트 매칭).
static int s_bus_target_count  = 1;
static int s_bus_matched_count = 0;

// 버스모드 매 사이클 USB-CDC 리포트용 (매칭 여부와 무관하게 매번 갱신).
static char  s_bus_report_text[64]  = {0};   // 이번 사이클에 rec이 읽은 원문 (실패시 빈 문자열)
static float s_bus_report_conf      = 0.0f;  // 이번 사이클 det box score (실패시 0)
static bool  s_bus_report_matched   = false; // 이번 사이클에 사전 후보와 매칭됐는지

// 웹 대시보드에 노출할 버스 OCR 최신 결과 (det 성공 시마다 갱신, 매칭 여부와 무관)
// 검출 사이클(0.5~2초)마다 no_det로 바로 꺼지면 폰 화면에서 거의 안 보이므로,
// 마지막 성공 후 BUS_STICKY_US 동안은 valid를 유지한다 (sticky).
static constexpr int64_t BUS_STICKY_US = 2 * 1000000;  // 2초

struct bus_result_t {
    bool    valid = false;
    float   cx = 0, cy = 0, w = 0, h = 0;   // 0~1 정규화 좌표
    float   conf = 0;
    char    text[64]  = {0};                // rec 원본 인식 결과
    char    match[32] = {0};                // 후보지 매칭 결과 (없으면 빈 문자열)
    int     ms = 0;
    int64_t last_success_us = 0;            // 마지막으로 det 성공한 시각
};
static bus_result_t s_bus_result;

struct BusBox {
    int x1, y1, x2, y2;
    float score;
};

// ---- DET 입력 전처리: 640x480 그레이스케일 -> 224x224 INT8 ----
static void bus_prepare_det_input(const uint8_t *src, int8_t *dst, float in_scale, int32_t in_zp) {
    for (int oy = 0; oy < BUS_DET_H; oy++) {
        float sy = (oy + 0.5f) * BUS_ORIG_H / BUS_DET_H - 0.5f;
        int y0 = std::max(0, static_cast<int>(std::floor(sy)));
        int y1 = std::min(BUS_ORIG_H - 1, y0 + 1);
        float fy = sy - y0;

        for (int ox = 0; ox < BUS_DET_W; ox++) {
            float sx = (ox + 0.5f) * BUS_ORIG_W / BUS_DET_W - 0.5f;
            int x0 = std::max(0, static_cast<int>(std::floor(sx)));
            int x1 = std::min(BUS_ORIG_W - 1, x0 + 1);
            float fx = sx - x0;

            auto get_gray = [&](int y, int x) -> float {
                return src[y * BUS_ORIG_W + x] / 255.0f;
            };

            float top    = get_gray(y0, x0) + (get_gray(y0, x1) - get_gray(y0, x0)) * fx;
            float bottom = get_gray(y1, x0) + (get_gray(y1, x1) - get_gray(y1, x0)) * fx;
            float val    = top + (bottom - top) * fy;

            int32_t q = static_cast<int32_t>(std::round(val / in_scale)) + in_zp;
            q = std::clamp<int32_t>(q, -128, 127);

            dst[oy * BUS_DET_W + ox] = static_cast<int8_t>(q);
        }
    }
}

// ---- REC 입력 전처리: det 박스로 크롭 -> 32x160, 비율유지+검정패딩 ----
static void bus_prepare_rec_input(const uint8_t *src, int bx1, int by1, int bx2, int by2, float *dst) {
    int bw = std::max(1, bx2 - bx1 + 1);
    int bh = std::max(1, by2 - by1 + 1);

    for (int i = 0; i < BUS_REC_H * BUS_REC_W; i++) {
        dst[i] = -1.0f;  // (0/255 - 0.5) / 0.5 = -1.0 (검정 패딩)
    }

    float scale = (float)BUS_REC_H / (float)bh;
    int target_w = static_cast<int>(std::round(bw * scale));
    if (target_w > BUS_REC_W) target_w = BUS_REC_W;
    if (target_w < 1) target_w = 1;

    auto get_gray = [&](int y, int x) -> float {
        return src[y * BUS_ORIG_W + x] / 255.0f;
    };

    for (int oy = 0; oy < BUS_REC_H; oy++) {
        float sy = by1 + (oy + 0.5f) * bh / (float)BUS_REC_H - 0.5f;
        int y0 = std::max(by1, std::min(by2, static_cast<int>(std::floor(sy))));
        int y1 = std::min(by2, y0 + 1);
        float fy = sy - y0;

        for (int ox = 0; ox < target_w; ox++) {
            float sx = bx1 + (ox + 0.5f) * bw / (float)target_w - 0.5f;
            int x0 = std::max(bx1, std::min(bx2, static_cast<int>(std::floor(sx))));
            int x1 = std::min(bx2, x0 + 1);
            float fx = sx - x0;

            float top    = get_gray(y0, x0) + (get_gray(y0, x1) - get_gray(y0, x0)) * fx;
            float bottom = get_gray(y1, x0) + (get_gray(y1, x1) - get_gray(y1, x0)) * fx;
            float val    = top + (bottom - top) * fy;

            dst[oy * BUS_REC_W + ox] = (val - 0.5f) / 0.5f;
        }
    }
}

static float bus_compute_iou(const BusBox &a, const BusBox &b) {
    int ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    int ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    int iw = std::max(0, ix2 - ix1 + 1), ih = std::max(0, iy2 - iy1 + 1);
    float inter = iw * ih;
    float area_a = (a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1);
    float area_b = (b.x2 - b.x1 + 1) * (b.y2 - b.y1 + 1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

static void bus_ctc_greedy_decode(const float *logits, int T, int C, char *out_str, size_t out_cap) {
    int prev = -1;
    size_t pos = 0;
    out_str[0] = '\0';

    for (int t = 0; t < T; t++) {
        const float *row = logits + t * C;
        int best = 0;
        float best_v = row[0];
        for (int c = 1; c < C; c++) {
            if (row[c] > best_v) { best_v = row[c]; best = c; }
        }
        if (best != prev && best != 0) {
            const char *ch = idx2char(best);
            if (ch) {
                size_t len = strlen(ch);
                if (pos + len < out_cap - 1) {
                    memcpy(out_str + pos, ch, len);
                    pos += len;
                    out_str[pos] = '\0';
                }
            }
        }
        prev = best;
    }
}

// 두 문자열의 최장 공통부분수열(LCS) 길이 — 버스번호 OCR 오인식을 허용하는
// 매칭에 씀. 후보 문자열이 짧아서(15자 이내) O(n*m) DP로 충분.
static int lcs_length(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    static int dp[17][17];
    for (int i = 0; i <= la; i++) dp[i][0] = 0;
    for (int j = 0; j <= lb; j++) dp[0][j] = 0;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++)
            dp[i][j] = (a[i - 1] == b[j - 1]) ? dp[i - 1][j - 1] + 1
                                               : std::max(dp[i - 1][j], dp[i][j - 1]);
    return dp[la][lb];
}

// 2026-08-28: 완전일치 대신 "과반수 문자 일치"로 완화 — OCR이 몇 글자
// 잘못 읽어도(예: 937→837, 410-1→410) 후보 문자열의 절반을 넘게 맞으면
// 매칭으로 인정하고, 재생은 OCR 원문이 아니라 후보의 정식 문자열로 한다.
// s_bus_use_api_candidates가 true면 API가 준 동적 후보(이미 매칭된 건 제외)만
// 보고, false면(GPS/API 못 쓴 폴백) 옛날 고정 리스트로 완전일치 매칭한다.
static const char *bus_match_candidate(const char *rec_text) {
    if (!rec_text || !rec_text[0]) return nullptr;

    if (s_bus_use_api_candidates) {
        const char *best = nullptr;
        int best_lcs = 0;
        for (int i = 0; i < s_bus_api_candidate_count; i++) {
            if (s_bus_api_candidates[i].consumed) continue;
            const char *cand = s_bus_api_candidates[i].route_no;
            int len = (int)strlen(cand);
            if (len == 0) continue;
            int lcs = lcs_length(rec_text, cand);
            if (lcs * 2 > len && lcs > best_lcs) {   // 과반수 초과
                best_lcs = lcs;
                best = cand;
            }
        }
        return best;
    }

    // 폴백: 고정 리스트, 완전일치 (예전 동작 그대로)
    for (int i = 0; i < BUS_CANDIDATES_COUNT; i++) {
        if (strcmp(rec_text, BUS_CANDIDATES[i]) == 0) return BUS_CANDIDATES[i];
    }
    return nullptr;
}

// 모드 전환마다 init/deinit으로 arena를 잡았다 놓는다 (한 번에 하나의 모델만 메모리에).
static bool bus_det_init(const void *model_ptr) {
    if (s_bus_det_interp) return true;  // 이미 로드됨

    s_bus_det_arena = (uint8_t *)heap_caps_aligned_alloc(16, kBusDetArenaSize, MALLOC_CAP_SPIRAM);
    if (!s_bus_det_arena) { ESP_LOGE(TAG, "버스 det arena 할당 실패"); return false; }

    const tflite::Model *model = tflite::GetModel(model_ptr);

    static tflite::MicroMutableOpResolver<20> resolver;
    static bool resolver_init = false;
    if (!resolver_init) {
        resolver.AddConv2D();
        resolver.AddDepthwiseConv2D();
        resolver.AddAdd();
        resolver.AddMul();
        resolver.AddRelu();
        resolver.AddRelu6();
        resolver.AddReshape();
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddHardSwish();
        resolver.AddConcatenation();
        resolver.AddLogistic();
        resolver_init = true;
    }

    tflite::MicroInterpreter *interpreter = new tflite::MicroInterpreter(model, resolver, s_bus_det_arena, kBusDetArenaSize);
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "버스 det AllocateTensors 실패");
        delete interpreter;
        heap_caps_free(s_bus_det_arena);
        s_bus_det_arena = nullptr;
        return false;
    }
    s_bus_det_interp = interpreter;
    ESP_LOGI(TAG, "버스 det 모델 로드 완료 | arena 사용 %u / %u KB",
             (unsigned)(interpreter->arena_used_bytes() / 1024), (unsigned)(kBusDetArenaSize / 1024));
    return true;
}

static void bus_det_deinit(void) {
    if (s_bus_det_interp) { delete s_bus_det_interp; s_bus_det_interp = nullptr; }
    if (s_bus_det_arena)  { heap_caps_free(s_bus_det_arena); s_bus_det_arena = nullptr; }
}

static bool bus_run_det(const void *model_ptr, const uint8_t *orig_img, BusBox *out_box) {
    if (!bus_det_init(model_ptr)) return false;

    TfLiteTensor *input  = s_bus_det_interp->input(0);
    TfLiteTensor *output = s_bus_det_interp->output(0);

    bus_prepare_det_input(orig_img, input->data.int8, input->params.scale, input->params.zero_point);

    if (s_bus_det_interp->Invoke() != kTfLiteOk) { ESP_LOGE(TAG, "버스 det Invoke 실패"); return false; }

    float out_scale = output->params.scale;
    int32_t out_zp   = output->params.zero_point;
    const int8_t *raw_out = output->data.int8;

    std::vector<BusBox> candidates;
    candidates.reserve(BUS_GRID_SIZE * BUS_GRID_SIZE);

    for (int gy = 0; gy < BUS_GRID_SIZE; gy++) {
        for (int gx = 0; gx < BUS_GRID_SIZE; gx++) {
            int base = (gy * BUS_GRID_SIZE + gx) * 5;
            float dx   = (raw_out[base + 0] - out_zp) * out_scale;
            float dy   = (raw_out[base + 1] - out_zp) * out_scale;
            float bw   = (raw_out[base + 2] - out_zp) * out_scale;
            float bh   = (raw_out[base + 3] - out_zp) * out_scale;
            float conf = (raw_out[base + 4] - out_zp) * out_scale;

            if (conf >= BUS_DET_CONF_THRES && bw <= 0.40f && bh <= 0.40f) {
                float cx = (gx + dx) / (float)BUS_GRID_SIZE;
                float cy = (gy + dy) / (float)BUS_GRID_SIZE;

                BusBox b;
                b.x1 = std::max(0, (int)((cx - bw / 2.0f) * BUS_ORIG_W));
                b.y1 = std::max(0, (int)((cy - bh / 2.0f) * BUS_ORIG_H));
                b.x2 = std::min(BUS_ORIG_W - 1, (int)((cx + bw / 2.0f) * BUS_ORIG_W));
                b.y2 = std::min(BUS_ORIG_H - 1, (int)((cy + bh / 2.0f) * BUS_ORIG_H));
                b.score = conf;
                candidates.push_back(b);
            }
        }
    }

    if (candidates.empty()) return false;

    std::sort(candidates.begin(), candidates.end(),
              [](const BusBox &a, const BusBox &b) { return a.score > b.score; });

    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<BusBox> nms_results;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (suppressed[i]) continue;
        nms_results.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); j++) {
            if (suppressed[j]) continue;
            if (bus_compute_iou(candidates[i], candidates[j]) > BUS_DET_IOU_THRES) suppressed[j] = true;
        }
    }

    if (nms_results.empty()) return false;
    *out_box = nms_results[0];
    return true;
}

static bool bus_rec_init(const void *model_ptr) {
    if (s_bus_rec_interp) return true;

    s_bus_rec_arena = (uint8_t *)heap_caps_aligned_alloc(16, kBusRecArenaSize, MALLOC_CAP_SPIRAM);
    if (!s_bus_rec_arena) { ESP_LOGE(TAG, "버스 rec arena 할당 실패"); return false; }

    const tflite::Model *model = tflite::GetModel(model_ptr);

    static tflite::MicroMutableOpResolver<20> resolver;
    static bool resolver_init = false;
    if (!resolver_init) {
        resolver.AddConv2D();
        resolver.AddDepthwiseConv2D();
        resolver.AddMaxPool2D();
        resolver.AddRelu();
        resolver.AddReshape();
        resolver.AddShape();
        resolver.AddStridedSlice();
        resolver.AddPack();
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver_init = true;
    }

    tflite::MicroInterpreter *interpreter = new tflite::MicroInterpreter(model, resolver, s_bus_rec_arena, kBusRecArenaSize);
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "버스 rec AllocateTensors 실패");
        delete interpreter;
        heap_caps_free(s_bus_rec_arena);
        s_bus_rec_arena = nullptr;
        return false;
    }
    s_bus_rec_interp = interpreter;
    ESP_LOGI(TAG, "버스 rec 모델 로드 완료 | arena 사용 %u / %u KB",
             (unsigned)(interpreter->arena_used_bytes() / 1024), (unsigned)(kBusRecArenaSize / 1024));
    return true;
}

static void bus_rec_deinit(void) {
    if (s_bus_rec_interp) { delete s_bus_rec_interp; s_bus_rec_interp = nullptr; }
    if (s_bus_rec_arena)  { heap_caps_free(s_bus_rec_arena); s_bus_rec_arena = nullptr; }
}

static bool bus_run_rec(const void *model_ptr, const uint8_t *orig_img, const BusBox &box, char *out_str, size_t out_cap) {
    if (!bus_rec_init(model_ptr)) return false;

    TfLiteTensor *input  = s_bus_rec_interp->input(0);
    TfLiteTensor *output = s_bus_rec_interp->output(0);

    bus_prepare_rec_input(orig_img, box.x1, box.y1, box.x2, box.y2, input->data.f);

    if (s_bus_rec_interp->Invoke() != kTfLiteOk) { ESP_LOGE(TAG, "버스 rec Invoke 실패"); return false; }

    int T = output->dims->data[1];
    int C = output->dims->data[2];
    bus_ctc_greedy_decode(output->data.f, T, C, out_str, out_cap);
    return true;
}

static void bus_ocr_deinit(void) { bus_det_deinit(); bus_rec_deinit(); }

// 카메라 프레임버퍼는 video_push_task도 동시에 esp_camera_fb_get()으로 가져간다.
// det+rec 추론(수백 ms) 동안 fb를 들고 있으면 카메라가 다음 프레임을 못 내줘서
// 영상 전송이 그 시간만큼 멈춘다. 그래서 받자마자 복사해서 즉시 반납하고,
// 무거운 추론은 복사본으로 한다 (카메라 점유 시간을 memcpy 수준으로 최소화).
static uint8_t *s_bus_frame_copy = nullptr;

// 모드 상태머신이 버스모드 동안 반복 호출하는 단발 시도 함수.
// 후보 매칭에 성공하면 true (호출부가 그때 모드를 끝내면 됨).
static bool bus_ocr_try_once(const void *det_model, const void *rec_model) {
    if (!s_bus_frame_copy) {
        s_bus_frame_copy = (uint8_t *)heap_caps_malloc(BUS_ORIG_W * BUS_ORIG_H, MALLOC_CAP_SPIRAM);
        if (!s_bus_frame_copy) { ESP_LOGE(TAG, "버스 프레임 복사 버퍼 할당 실패"); return false; }
    }

    int64_t t0 = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    bool got_frame = false;
    if (fb) {
        if (fb->len == (size_t)(BUS_ORIG_W * BUS_ORIG_H)) {
            memcpy(s_bus_frame_copy, fb->buf, fb->len);
            got_frame = true;
        }
        esp_camera_fb_return(fb);  // 복사 끝났으니 카메라는 바로 반납 (스트리밍 안 막음)
    }
    if (!got_frame) {
        // 프레임을 못 받은 사이클 — 리포트 값은 갱신하지 않고 이전 값 유지
        return false;
    }

    BusBox box;
    char rec_text[64] = {0};
    bool det_ok = bus_run_det(det_model, s_bus_frame_copy, &box);
    bool rec_ok = false;
    const char *matched = nullptr;

    if (det_ok) {
        rec_ok = bus_run_rec(rec_model, s_bus_frame_copy, box, rec_text, sizeof(rec_text));
        if (rec_ok) {
            ESP_LOGI(TAG, "[버스OCR 시도] %s", rec_text[0] ? rec_text : "(empty)");
            matched = bus_match_candidate(rec_text);
            if (matched) {
                ESP_LOGI(TAG, "🚌 [버스OCR 매칭] %s (원문: %s)", matched, rec_text);
                snprintf(s_bus_last_match, sizeof(s_bus_last_match), "%s", matched);
                // API 동적 후보였으면 이번 무리 안에서 소모 처리 — 재진입 시
                // 같은 노선을 다시 매칭하지 않게 (2026-08-28).
                if (s_bus_use_api_candidates) {
                    for (int i = 0; i < s_bus_api_candidate_count; i++) {
                        if (strcmp(s_bus_api_candidates[i].route_no, matched) == 0) {
                            s_bus_api_candidates[i].consumed = true;
                            break;
                        }
                    }
                }
            }
        } else {
            ESP_LOGI(TAG, "[버스OCR 시도] (rec_fail)");
        }
    } else {
        ESP_LOGI(TAG, "[버스OCR 시도] (no_det)");
    }

    // 웹 대시보드용 박스 갱신 (sticky, 마지막 성공 후 BUS_STICKY_US 유지)
    if (s_bus_result_lock && xSemaphoreTake(s_bus_result_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t now = esp_timer_get_time();
        if (det_ok) {
            s_bus_result.last_success_us = now;
            s_bus_result.cx   = (box.x1 + box.x2) / 2.0f / BUS_ORIG_W;
            s_bus_result.cy   = (box.y1 + box.y2) / 2.0f / BUS_ORIG_H;
            s_bus_result.w    = (box.x2 - box.x1) / (float)BUS_ORIG_W;
            s_bus_result.h    = (box.y2 - box.y1) / (float)BUS_ORIG_H;
            s_bus_result.conf = box.score;
            snprintf(s_bus_result.text, sizeof(s_bus_result.text), "%s", rec_ok ? rec_text : "");
            snprintf(s_bus_result.match, sizeof(s_bus_result.match), "%s", matched ? matched : "");
        }
        s_bus_result.valid = (now - s_bus_result.last_success_us) < BUS_STICKY_US;
        if (!s_bus_result.valid) {
            s_bus_result.text[0]  = '\0';
            s_bus_result.match[0] = '\0';
        }
        s_bus_result.ms = (int)((esp_timer_get_time() - t0) / 1000);
        xSemaphoreGive(s_bus_result_lock);
    }

    // 매 사이클 USB-CDC 리포트 갱신 — 매칭 여부와 무관하게 이번에 읽은 값을 그대로 반영.
    snprintf(s_bus_report_text, sizeof(s_bus_report_text), "%s", rec_ok ? rec_text : "");
    s_bus_report_conf    = det_ok ? box.score : 0.0f;
    s_bus_report_matched = (matched != nullptr);

    return matched != nullptr;
}

// ── 장애물감지 (SSD, smartcane 프로젝트 model_inference_ssd.cc 로직 이식) ──────
// smartcane 폴더 자체는 건드리지 않음. RPi에서 이미지를 당겨받던 부분만 빼고
// ircamera 자체 카메라 프레임을 직접 넣도록 바꿈. 후처리(softmax+NMS+박스디코딩)는
// torchvision SSD BoxCoder 수식 그대로 유지 (smartcane 쪽 주석의 경고사항 동일 적용).
#include "ssd_anchors.h"   // smartcane/main/ssd_anchors.h 복사본 (1602개 앵커)

#define OBS_INPUT_W 224
#define OBS_INPUT_H 224
#define OBS_NUM_CLASSES 5   // 배경(0) + 자전거/킥보드/볼라드/사람(1~4)

// 2026-08-17: 실측(arena_used_bytes) 1330KB 기준 +35% 여유로 축소 (기존 6MB,
// 실사용 21.6%뿐이라 78%가 낭비되고 있었음 — 이게 부팅 시 PSRAM 부족의 주범)
static constexpr size_t kObsArenaSize   = 1792 * 1024;
static constexpr float  kObsConfThres   = 0.30f;
static constexpr float  kObsNmsIou      = 0.45f;
static constexpr float  kObsBoxWeightXY = 10.0f;
static constexpr float  kObsBoxWeightWH = 5.0f;
static constexpr int    kObsMaxCandidates = 96;

static const char *OBS_CLASS_NAMES[] = {"자전거", "킥보드", "볼라드", "사람"};

static uint8_t *s_obs_arena = nullptr;
static tflite::MicroInterpreter *s_obs_interp = nullptr;
static TfLiteTensor *s_obs_input    = nullptr;
static TfLiteTensor *s_obs_bbox_out = nullptr;
static TfLiteTensor *s_obs_cls_out  = nullptr;

struct ObsCandidate { int class_id; float score; float x1, y1, x2, y2; float cx; };
static ObsCandidate s_obs_candidates[kObsMaxCandidates];

struct obs_result_t {
    bool  valid = false;
    int   class_id = -1;       // 0~3
    float score = 0;
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0, cx = 0;
    int   direction = 1;       // 0=left 1=center 2=right
};
static obs_result_t s_obs_result;

static void obs_deinit(void) {
    if (s_obs_interp) { delete s_obs_interp; s_obs_interp = nullptr; }
    if (s_obs_arena)  { heap_caps_free(s_obs_arena); s_obs_arena = nullptr; }
    s_obs_input = s_obs_bbox_out = s_obs_cls_out = nullptr;
}

static bool obs_configure_ops(tflite::MicroMutableOpResolver<32> *r) {
    return r->AddConv2D() == kTfLiteOk &&
           r->AddDepthwiseConv2D() == kTfLiteOk &&
           r->AddAdd() == kTfLiteOk &&
           r->AddMul() == kTfLiteOk &&
           r->AddSub() == kTfLiteOk &&
           r->AddConcatenation() == kTfLiteOk &&
           r->AddAveragePool2D() == kTfLiteOk &&
           r->AddMaxPool2D() == kTfLiteOk &&
           r->AddFullyConnected() == kTfLiteOk &&
           r->AddReshape() == kTfLiteOk &&
           r->AddPad() == kTfLiteOk &&
           r->AddPadV2() == kTfLiteOk &&
           r->AddLogistic() == kTfLiteOk &&
           r->AddSoftmax() == kTfLiteOk &&
           r->AddQuantize() == kTfLiteOk &&
           r->AddDequantize() == kTfLiteOk &&
           r->AddTransposeConv() == kTfLiteOk &&
           r->AddTranspose() == kTfLiteOk &&
           r->AddSlice() == kTfLiteOk &&
           r->AddHardSwish() == kTfLiteOk &&
           r->AddRelu() == kTfLiteOk &&
           r->AddRelu6() == kTfLiteOk &&
           r->AddMean() == kTfLiteOk &&
           r->AddResizeNearestNeighbor() == kTfLiteOk;
}

static bool obs_init(const void *model_ptr) {
    if (s_obs_interp) return true;  // 이미 로드됨

    s_obs_arena = (uint8_t *)heap_caps_malloc(kObsArenaSize, MALLOC_CAP_SPIRAM);
    if (!s_obs_arena) { ESP_LOGE(TAG, "장애물 arena(%uB) 할당 실패", (unsigned)kObsArenaSize); return false; }

    const tflite::Model *model = tflite::GetModel(model_ptr);

    static tflite::MicroMutableOpResolver<32> resolver;
    static bool resolver_ready = false;
    if (!resolver_ready) {
        if (!obs_configure_ops(&resolver)) {
            ESP_LOGE(TAG, "장애물 op resolver 구성 실패");
            heap_caps_free(s_obs_arena); s_obs_arena = nullptr;
            return false;
        }
        resolver_ready = true;
    }

    tflite::MicroInterpreter *interp = new tflite::MicroInterpreter(model, resolver, s_obs_arena, kObsArenaSize);
    if (interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "장애물 AllocateTensors 실패");
        delete interp;
        heap_caps_free(s_obs_arena); s_obs_arena = nullptr;
        return false;
    }

    s_obs_interp = interp;
    s_obs_input  = interp->input(0);
    for (size_t i = 0; i < interp->outputs_size(); i++) {
        TfLiteTensor *out = interp->output(i);
        if (!out || !out->dims || out->dims->size < 1) continue;
        int last = out->dims->data[out->dims->size - 1];
        if (last == 4) s_obs_bbox_out = out;
        else if (last == OBS_NUM_CLASSES) s_obs_cls_out = out;
    }
    if (!s_obs_bbox_out || !s_obs_cls_out) {
        ESP_LOGE(TAG, "장애물 출력 텐서(bbox/cls) 식별 실패");
        obs_deinit();
        return false;
    }
    if (s_obs_input->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "장애물 입력이 int8이 아님 (type=%d)", (int)s_obs_input->type);
        obs_deinit();
        return false;
    }

    ESP_LOGI(TAG, "장애물 모델 로드 완료 | arena %uKB 중 %uKB 사용",
             (unsigned)(kObsArenaSize / 1024), (unsigned)(interp->arena_used_bytes() / 1024));
    return true;
}

// 640x480 그레이스케일 → 224x224 int8 양자화 (bilinear, 버스 det 전처리와 동일한 방식)
static void obs_prepare_input(const uint8_t *src, int8_t *dst, float in_scale, int32_t in_zp) {
    for (int oy = 0; oy < OBS_INPUT_H; oy++) {
        float sy = (oy + 0.5f) * BUS_ORIG_H / OBS_INPUT_H - 0.5f;
        int y0 = std::max(0, (int)floorf(sy));
        int y1 = std::min(BUS_ORIG_H - 1, y0 + 1);
        float fy = sy - y0;

        for (int ox = 0; ox < OBS_INPUT_W; ox++) {
            float sx = (ox + 0.5f) * BUS_ORIG_W / OBS_INPUT_W - 0.5f;
            int x0 = std::max(0, (int)floorf(sx));
            int x1 = std::min(BUS_ORIG_W - 1, x0 + 1);
            float fx = sx - x0;

            auto g = [&](int y, int x) -> float { return src[y * BUS_ORIG_W + x] / 255.0f; };
            float top = g(y0, x0) + (g(y0, x1) - g(y0, x0)) * fx;
            float bot = g(y1, x0) + (g(y1, x1) - g(y1, x0)) * fx;
            float val = top + (bot - top) * fy;

            int32_t q = (int32_t)lroundf(val / in_scale) + in_zp;
            q = std::max<int32_t>(-128, std::min<int32_t>(127, q));
            dst[oy * OBS_INPUT_W + ox] = (int8_t)q;
        }
    }
}

static inline float obs_read_bbox(int a, int k) {
    int idx = a * 4 + k;
    if (s_obs_bbox_out->type == kTfLiteFloat32) return s_obs_bbox_out->data.f[idx];
    return ((float)s_obs_bbox_out->data.int8[idx] - s_obs_bbox_out->params.zero_point) * s_obs_bbox_out->params.scale;
}
static inline float obs_read_cls(int a, int c) {
    int idx = a * OBS_NUM_CLASSES + c;
    if (s_obs_cls_out->type == kTfLiteFloat32) return s_obs_cls_out->data.f[idx];
    return ((float)s_obs_cls_out->data.int8[idx] - s_obs_cls_out->params.zero_point) * s_obs_cls_out->params.scale;
}

static float obs_iou(const ObsCandidate &a, const ObsCandidate &b) {
    float l = std::max(a.x1, b.x1), t = std::max(a.y1, b.y1);
    float r = std::min(a.x2, b.x2), bo = std::min(a.y2, b.y2);
    float w = std::max(0.0f, r - l), h = std::max(0.0f, bo - t);
    float inter = w * h;
    float ua = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float ub = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    float uni = ua + ub - inter;
    return uni <= 0.0f ? 0.0f : inter / uni;
}

// 0=왼쪽 1=중앙 2=오른쪽
static int obs_direction(float cx) {
    if (cx < 0.33f) return 0;
    if (cx < 0.66f) return 1;
    return 2;
}

// 추론 1회 (단발 호출용). 결과는 s_obs_result에 반영.
static bool obs_run_once(const uint8_t *orig_img) {
    if (!s_obs_interp) return false;

    obs_prepare_input(orig_img, s_obs_input->data.int8, s_obs_input->params.scale, s_obs_input->params.zero_point);
    if (s_obs_interp->Invoke() != kTfLiteOk) { ESP_LOGE(TAG, "장애물 Invoke 실패"); return false; }

    const float kLogitMargin = logf(kObsConfThres / (1.0f - kObsConfThres));
    int cand_count = 0;

    for (int a = 0; a < SSD_ANCHOR_COUNT; a++) {
        float logit_bg = obs_read_cls(a, 0);
        float max_fg = -1e30f;
        for (int c = 1; c < OBS_NUM_CLASSES; c++) {
            float l = obs_read_cls(a, c);
            if (l > max_fg) max_fg = l;
        }
        if ((max_fg - logit_bg) < kLogitMargin) continue;

        float probs[OBS_NUM_CLASSES];
        float max_logit = -1e30f;
        for (int c = 0; c < OBS_NUM_CLASSES; c++) { probs[c] = obs_read_cls(a, c); if (probs[c] > max_logit) max_logit = probs[c]; }
        float sum_exp = 0.0f;
        for (int c = 0; c < OBS_NUM_CLASSES; c++) { probs[c] = expf(probs[c] - max_logit); sum_exp += probs[c]; }

        for (int c = 1; c < OBS_NUM_CLASSES; c++) {
            float score = probs[c] / sum_exp;
            if (score < kObsConfThres || cand_count >= kObsMaxCandidates) continue;

            float ax = ssd_anchors[a][0], ay = ssd_anchors[a][1];
            float aw = ssd_anchors[a][2], ah = ssd_anchors[a][3];
            float dx = obs_read_bbox(a, 0) / kObsBoxWeightXY;
            float dy = obs_read_bbox(a, 1) / kObsBoxWeightXY;
            float dw = obs_read_bbox(a, 2) / kObsBoxWeightWH;
            float dh = obs_read_bbox(a, 3) / kObsBoxWeightWH;
            float pcx = ax + dx * aw, pcy = ay + dy * ah;
            float pw = aw * expf(dw), ph = ah * expf(dh);

            ObsCandidate &cd = s_obs_candidates[cand_count++];
            cd.class_id = c - 1;
            cd.score = score;
            cd.x1 = std::max(0.0f, std::min(1.0f, pcx - pw / 2));
            cd.y1 = std::max(0.0f, std::min(1.0f, pcy - ph / 2));
            cd.x2 = std::max(0.0f, std::min(1.0f, pcx + pw / 2));
            cd.y2 = std::max(0.0f, std::min(1.0f, pcy + ph / 2));
            cd.cx = std::max(0.0f, std::min(1.0f, pcx));
        }
    }

    // 점수 내림차순 정렬 + NMS (smartcane과 동일, 후보 최대 96개라 선택정렬로 충분)
    for (int i = 0; i < cand_count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < cand_count; j++) if (s_obs_candidates[j].score > s_obs_candidates[best].score) best = j;
        if (best != i) { ObsCandidate tmp = s_obs_candidates[i]; s_obs_candidates[i] = s_obs_candidates[best]; s_obs_candidates[best] = tmp; }
    }
    bool suppressed[kObsMaxCandidates] = {false};
    int kept = 0;
    for (int i = 0; i < cand_count; i++) {
        if (suppressed[i]) continue;
        if (kept != i) s_obs_candidates[kept] = s_obs_candidates[i];
        ObsCandidate sel = s_obs_candidates[kept];
        kept++;
        for (int j = i + 1; j < cand_count; j++) {
            if (suppressed[j] || s_obs_candidates[j].class_id != sel.class_id) continue;
            if (obs_iou(sel, s_obs_candidates[j]) > kObsNmsIou) suppressed[j] = true;
        }
    }

    obs_result_t r;
    if (kept > 0) {
        const ObsCandidate &best = s_obs_candidates[0];
        r.valid = true;
        r.class_id = best.class_id;
        r.score = best.score;
        r.x1 = best.x1; r.y1 = best.y1; r.x2 = best.x2; r.y2 = best.y2; r.cx = best.cx;
        r.direction = obs_direction(best.cx);
        ESP_LOGI(TAG, "[장애물] %s score=%.2f dir=%d", OBS_CLASS_NAMES[best.class_id], best.score, r.direction);
    } else {
        ESP_LOGI(TAG, "[장애물] 탐지 없음");
    }
    s_obs_result = r;
    return kept > 0;
}

// ── RPi ttyACM1 USB-CDC 프로토콜 + 모드 상태머신 ──────────────────────────────
// RPi -> ESP32 : 1바이트 명령. 0x01=버스모드 진입, 0x02=점자블록모드 진입.
//                (아무것도 안 보내면 계속 장애물모드 유지/복귀)
// ESP32 -> RPi : 결과 패킷. 첫 바이트로 종류 구분.
//   0xB0 장애물 [object_code 1B][direction_code 1B][conf0-100 1B]
//       object_code: 0x01자전거 0x02킥보드 0x03볼라드 0x04사람 0x05미상
//       direction_code: 0x01왼쪽 0x02중앙 0x03오른쪽
//   0xB1 버스   [len 1B][번호 텍스트 len바이트][conf0-100 1B][matched 1B(0/1)]
//       matched=0: 아직 사전 후보와 미매칭(진행중 추정값), 1: 사전 매칭 확정(버스모드 종료)
//   0xB2 점자블록 [있음여부 1B(0/1)][conf0-100 1B][근접여부 1B(0/1)]
//       버스(0xB1)와 동일한 원리로 매 사이클 전송됨 — 근접여부=1이 버스의 matched와
//       같은 "확정" 신호(점자블록모드 종료), 0이면 아직 진행중인 추정값 (2026-08-15)
#include "driver/usb_serial_jtag.h"

#define MODE_CMD_BUS     0x01
#define MODE_CMD_BRAILLE 0x02
// 2026-08-25: RPi가 ToF 활성방향과 AI(0xB0) 방향이 일치할 때만 보내는 트리거.
// [0x10][object_code 1B] 총 2바이트 — object_code는 0xB0에서 쓰는 값 그대로
// 재사용(0x01자전거 0x02킥보드 0x03볼라드 0x04사람). 모드 상관없이 받는 즉시
// 해당 객체 음성 클립을 큐잉해서 스피커로 알려준다 (장애물종류안내음성.md 참고).
#define CMD_OBSTACLE_CONFIRM 0x10

// 2026-08-26: 길안내(라즈베리파이가 GPS/버튼입력으로 직접 판단, ESP32는
// 상태머신 안 건드리고 스피커 안내만 담당 — CMD_OBSTACLE_CONFIRM과 동일한
// "그냥 알려주면 재생만" 패턴). [0x03][payload 1B] 항상 페이로드 동반:
//   0x00 = 길안내모드 진입 ("길안내모드전환")
//   0x01 = 목적지 순환 → 학교 ("학교")
//   0x02 = 목적지 순환 → 버스정류장 ("버스정류장")
//   0x03 = 목적지 확정(2초 이상 누름) ("버스정류장으로길안내를시작합니다")
#define CMD_DESTINATION       0x03
// 페이로드 없음, 받는 즉시 "긴급상황발생119신고요청" 재생
#define CMD_EMERGENCY         0x04
// 페이로드 없음, 받는 즉시 "목적지에도착했습니다" 재생
#define CMD_ARRIVED           0x05

// 2026-08-26: 별도 "guide" 모듈이 raw 0x03~0x06으로 보내는 좌회전/우회전/
// 횡단보도/계단을, RPi가 ircamera 프로토콜(0x03~0x05 이미 사용중)과 안
// 겹치게 0x06~0x09로 리매핑해서 보냄. 전부 페이로드 없음.
#define CMD_TURN_LEFT          0x06   // "좌회전"
#define CMD_TURN_RIGHT         0x07   // "우회전"
#define CMD_CROSSWALK          0x08   // 횡단보도 — wav 아직 없음, 값만 예약
#define CMD_STAIRS             0x09   // "계단"

// 2026-08-30: IMU MCU가 낙상 감지/해제/위급을 판단해서 RPi로 알리고(0xFF/0xFE/0xFD,
// IMU 자체 프로토콜 — ircamera랑 별개 채널), RPi가 그중 감지/해제 두 시점만
// ircamera로 릴레이하는 명령. 페이로드 없음.
//   0x0A 수신: "낙상감지" 무한 반복 재생 시작 (audio_task가 큐 비었을 때마다 계속 재큐잉)
//   0x0B 수신: 반복 정지. 기존 CMD_EMERGENCY(0x04, "긴급상황발생119신고요청")는
//              위급 판정(RPI_FALL_DANGER) 시 RPi가 그대로 별도로 보내는 것으로 유지 —
//              이 두 명령이랑 무관한 단발성 재생.
#define CMD_FALL_ALERT_START    0x0A
#define CMD_FALL_ALERT_STOP     0x0B

#define RESULT_MSG_OBSTACLE 0xB0
#define RESULT_MSG_BUS       0xB1
#define RESULT_MSG_BRAILLE    0xB2
// 2026-08-26: ESP32가 RPi 명령 없이 스스로 장애물모드로 "복귀"하는 경우
// (버스매칭성공/버스타임아웃/점자블록근접확정/점자블록타임아웃, 전부
// enter_mode(OBSTACLE)로 귀결됨) RPi가 이걸 사전에 알 방법이 없어서 추가.
// 버스/점자블록 진입은 RPi가 직접 명령한 거라 이미 알고 있으므로 안 보냄 —
// 그래서 페이로드 없이 1바이트만: "지금부터 장애물모드"라는 뜻 고정.
// (8.26개발기록 참고)
#define RESULT_MSG_MODE      0xB3

enum class CaneMode { OBSTACLE, BUS, BRAILLE };
static CaneMode s_mode = CaneMode::OBSTACLE;
static int64_t  s_mode_enter_us = 0;

// 버스모드 타임아웃은 60초. 여러 대가 연달아 도착하는 경우에도 enter_mode(BUS)
// 재진입 시 s_mode_enter_us가 리셋돼 매번 새 60초 창을 받는다.
static constexpr int64_t kBusModeTimeoutUs     = 60LL * 1000000;
// 점자블록 타임아웃은 30초로 설정 — 짧으면 근접 확정 전에 재전환이 잦아져
// 오히려 장애물감지가 꺼진 시간이 늘어나므로, 안정적인 인식을 우선했다.
static constexpr int64_t kBrailleModeTimeoutUs = 30LL  * 1000000;

static const void *s_obs_model     = nullptr;
static const void *s_bus_det_model = nullptr;
static const void *s_bus_rec_model = nullptr;

static bool s_usb_cdc_ready = false;

static void usb_cdc_init(void) {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    s_usb_cdc_ready = true;
}

static void usb_cdc_write(const void *data, size_t len) {
    usb_serial_jtag_write_bytes((const uint8_t *)data, len, pdMS_TO_TICKS(1000));
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
}

// ── ESP 로그를 USB CDC(RPi용 채널)로도 내보내기 (2026-08-15) ─────────────────
// PC용 UART 콘솔은 카메라 EV-EOF-OVF 스팸(전원 문제로 추정, 개발기록 참고) 때문에
// 로그가 안 뜨거나 묻히는데, 라즈베리파이용 USB CDC 채널은 이미 안정적으로 동작
// 중이라 여기에 로그를 같이 실어서 라즈베리파이 쪽에서 보게 한다.
// 결과 패킷(0xB0/0xB1/0xB2)과 안 겹치게 로그는 항상 0xFE로 시작한다 —
// 수신 측(ircamera_link.py)에서 첫 바이트로 분기해서 처리해야 함 (RPi 쪽은 사용자가 직접 수정).
#define LOG_MSG_MARKER 0xFE

static vprintf_like_t s_orig_log_vprintf = nullptr;

static int usb_cdc_log_vprintf(const char *fmt, va_list args) {
    // 원래 콘솔(UART0) 출력은 그대로 유지
    va_list args_orig;
    va_copy(args_orig, args);
    int ret = s_orig_log_vprintf ? s_orig_log_vprintf(fmt, args_orig) : vprintf(fmt, args_orig);
    va_end(args_orig);

    if (s_usb_cdc_ready) {
        static char buf[257];
        buf[0] = LOG_MSG_MARKER;
        int n = vsnprintf(buf + 1, sizeof(buf) - 1, fmt, args);
        if (n > 0) {
            if (n >= (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 2;
            // 결과 패킷(usb_cdc_write)과 달리 논블로킹으로 던진다 — 블로킹하면
            // 카메라 오버플로처럼 로그가 폭주하는 상황에서 시스템 전체가 멈출 수 있다.
            // 유실돼도 괜찮은 디버그 로그이므로 실패/부분전송은 그냥 무시한다.
            usb_serial_jtag_write_bytes((const uint8_t *)buf, 1 + n, 0);
        }
    }
    return ret;
}

// 논블로킹: 명령 1바이트 와 있으면 true
static bool usb_cdc_poll_cmd(uint8_t *out_cmd) {
    uint8_t b;
    int n = usb_serial_jtag_read_bytes(&b, 1, 0);
    if (n == 1) { *out_cmd = b; return true; }
    return false;
}

// 2026-08-23: MODE_CMD_BUS 뒤에 위도/경도(float32 LE 각 4B, 총 8B)가 붙어서
// 옴 — RPi가 한 번의 write()로 [0x01][lat 4B][lon 4B]를 같이 보내는 걸
// 전제로, 커맨드 바이트 받은 직후 짧게 블로킹해서 나머지 8바이트를 마저
// 읽는다. RPi 전송과 시간차가 크면(수백 ms 이상) 실패할 수 있음 — 그런
// 경우가 생기면 타임아웃을 늘릴 것.
static bool usb_cdc_read_gps_payload(float *out_lat, float *out_lon) {
    uint8_t buf[8];
    int total = 0;
    int64_t deadline = esp_timer_get_time() + 300 * 1000;  // 300ms
    while (total < 8 && esp_timer_get_time() < deadline) {
        int n = usb_serial_jtag_read_bytes(buf + total, 8 - total, pdMS_TO_TICKS(50));
        if (n > 0) total += n;
    }
    if (total < 8) {
        ESP_LOGW(TAG, "GPS 페이로드 수신 실패 (received %d/8 bytes)", total);
        return false;
    }
    memcpy(out_lat, buf, 4);
    memcpy(out_lon, buf + 4, 4);
    return true;
}

// 2026-08-25: CMD_OBSTACLE_CONFIRM 뒤에 붙는 object_code 1바이트를 읽는다.
// usb_cdc_read_gps_payload()와 동일한 짧은 블로킹 패턴 (최대 300ms).
static bool usb_cdc_read_obstacle_payload(uint8_t *out_code) {
    int64_t deadline = esp_timer_get_time() + 300 * 1000;
    while (esp_timer_get_time() < deadline) {
        int n = usb_serial_jtag_read_bytes(out_code, 1, pdMS_TO_TICKS(50));
        if (n == 1) return true;
    }
    ESP_LOGW(TAG, "장애물 확인 트리거 페이로드 수신 실패");
    return false;
}

// 0xB0 프로토콜의 object_code(main.cpp 주석 1625번째 줄 근방 참고)를
// voice_clips.h 클립 키로 매핑. RPi(ircamera_link.py)의 OBJECT_NAMES와
// 값이 동일해야 한다 — 바뀌면 양쪽 다 같이 고칠 것.
static const char *obstacle_voice_key(uint8_t object_code) {
    switch (object_code) {
        case 0x01: return "자전거";
        case 0x02: return "킥보드";
        case 0x03: return "볼라드";
        case 0x04: return "사람";
        default:   return nullptr;   // 0x05(미상) 등은 안내할 게 없음
    }
}

// 2026-08-26: CMD_DESTINATION 뒤에 붙는 페이로드 1바이트를 읽는다.
// usb_cdc_read_obstacle_payload()와 동일한 짧은 블로킹 패턴 (최대 300ms).
static bool usb_cdc_read_destination_payload(uint8_t *out_code) {
    int64_t deadline = esp_timer_get_time() + 300 * 1000;
    while (esp_timer_get_time() < deadline) {
        int n = usb_serial_jtag_read_bytes(out_code, 1, pdMS_TO_TICKS(50));
        if (n == 1) return true;
    }
    ESP_LOGW(TAG, "길안내 트리거 페이로드 수신 실패");
    return false;
}

// CMD_DESTINATION payload -> voice_clips.h 키 매핑.
static const char *destination_voice_key(uint8_t payload) {
    switch (payload) {
        case 0x00: return "길안내모드전환";
        case 0x01: return "학교";
        case 0x02: return "버스정류장";
        case 0x03: return "버스정류장으로길안내를시작합니다";
        default:   return nullptr;
    }
}

static void send_obstacle_result(void) {
    uint8_t object_code = 0x05, dir_code = 0x02, conf100 = 0;
    if (s_obs_result.valid) {
        object_code = (uint8_t)(s_obs_result.class_id + 1);     // 1~4
        dir_code    = (uint8_t)(s_obs_result.direction + 1);     // 1~3
        conf100     = (uint8_t)(s_obs_result.score * 100.0f);
    }
    uint8_t pkt[4] = { RESULT_MSG_OBSTACLE, object_code, dir_code, conf100 };
    usb_cdc_write(pkt, sizeof(pkt));
}

static void send_bus_result(const char *text, float conf, bool matched) {
    uint8_t len = (uint8_t)strnlen(text, 31);
    uint8_t conf100 = (uint8_t)(conf * 100.0f);
    uint8_t pkt[2 + 31 + 2];
    pkt[0] = RESULT_MSG_BUS;
    pkt[1] = len;
    memcpy(pkt + 2, text, len);
    pkt[2 + len]     = conf100;
    pkt[2 + len + 1] = (uint8_t)(matched ? 1 : 0);
    usb_cdc_write(pkt, 2 + len + 2);
}

static void send_braille_result(bool present, float conf, bool near) {
    uint8_t pkt[4] = { RESULT_MSG_BRAILLE, (uint8_t)(present ? 1 : 0),
                        (uint8_t)(conf * 100.0f), (uint8_t)(near ? 1 : 0) };
    usb_cdc_write(pkt, sizeof(pkt));
}

static void enter_mode(CaneMode m) {
    // 한 번에 하나의 모델만 메모리에 있으면 되므로, 새 모드에 필요없는 건 다 내린다.
    if (m != CaneMode::OBSTACLE) obs_deinit();
    if (m != CaneMode::BUS)      { bus_det_deinit(); bus_rec_deinit(); }
    if (m != CaneMode::BRAILLE)  bb_deinit();

    // init 실패(대개 PSRAM 단편화로 arena 재할당 실패) 시 조용히 넘어가지 않고
    // 로그를 남기고 1회 재시도한다. 그래도 실패하면 해당 모드 감지가 꺼진 채로
    // 진행되지만, 최소한 다음에 멈춤이 재현됐을 때 시리얼 로그로 원인을 짚을 수
    // 있게 한다 (2026-08-14 개발기록 참고).
    if (m == CaneMode::OBSTACLE) {
        // 2026-08-26: 점자블록(s_bb)과 동일한 이유로 리셋 — 장애물모드 재진입
        // 직후 obs_run_once()가 이번 사이클에 실패(모델 로드 실패/Invoke 실패)
        // 해도 send_obstacle_result()는 무조건 호출되므로, 리셋 안 하면 몇 분
        // 전(모드 전환 전) 묵은 값이 새 감지인 것처럼 RPi로 나갈 수 있었다.
        // 하필 모드 전환 직후가 arena 재할당이 제일 잘 실패하는 타이밍이라
        // 실제로 겪을 수 있는 경로였음 (8.26개발기록 참고).
        s_obs_result = obs_result_t{};

        // RPi 명령 없이 ESP32가 스스로 장애물모드로 복귀하는 경우를 RPi가
        // 알 수 있게 알려준다 (버스/점자블록 진입은 RPi가 이미 알고 있어서
        // 안 보냄 — 위 RESULT_MSG_MODE 주석 참고). 페이로드 없음.
        uint8_t mode_pkt[1] = { RESULT_MSG_MODE };
        usb_cdc_write(mode_pkt, sizeof(mode_pkt));
    }
    if (m == CaneMode::OBSTACLE && s_obs_model) {
        if (!obs_init(s_obs_model)) {
            ESP_LOGE(TAG, "장애물모델 로드 실패 (여유 PSRAM %uKB) — 재시도",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            if (!obs_init(s_obs_model)) {
                ESP_LOGE(TAG, "장애물모델 재시도도 실패 — 장애물 감지 비활성 상태로 진행");
            }
        }
    }
    if (m == CaneMode::BUS) {
        bool det_ok = s_bus_det_model ? bus_det_init(s_bus_det_model) : true;
        bool rec_ok = s_bus_rec_model ? bus_rec_init(s_bus_rec_model) : true;
        if (!det_ok || !rec_ok) {
            ESP_LOGE(TAG, "버스모델 로드 실패 (det=%d rec=%d, 여유 PSRAM %uKB)",
                     det_ok, rec_ok,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
    }
    if (m == CaneMode::BRAILLE) {
        // ⚠️ s_bb는 static 전역이라 리셋 안 하면 지난 세션의 값(특히 near=true로 끝난
        // 경우)이 그대로 남는다. bb_init()이 실패하거나 첫 사이클에 카메라 프레임이
        // 아직 준비 안 됐으면 bb_infer()가 못 돌아서 이 묵은 값으로 near를 계산하게
        // 되고, 그러면 새로 진입하자마자 아무것도 안 보고 바로 장애물모드로 튕겨나간다
        // (2026-08-15 개발기록 참고). 진입할 때마다 무조건 무효화해서 이 값이 새어들지
        // 않게 한다.
        s_bb = bb_result_t{ 0, 0, 0, 0, 0, "none", 0, false };

        if (!bb_init()) {
            ESP_LOGE(TAG, "점자블록모델 로드 실패 (여유 PSRAM %uKB) — 재시도",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            if (!bb_init()) {
                ESP_LOGE(TAG, "점자블록모델 재시도도 실패 — 점자블록 감지 비활성 상태로 진행");
            }
        }
    }

    s_mode = m;
    s_mode_enter_us = esp_timer_get_time();
    ESP_LOGI(TAG, "모드 전환 -> %s",
             m == CaneMode::OBSTACLE ? "장애물" : m == CaneMode::BUS ? "버스" : "점자블록");

    // 모드 전환 음성 안내 (로컬 클립, blocking — cane_mode_task를 그 재생
    // 시간(~1.5~2초)만큼 붙잡는다. 매 프레임마다가 아니라 모드 전환 시
    // 1회뿐이라 지금은 수용 가능한 수준으로 판단, 나중에 부담되면 별도
    // 오디오 태스크+큐로 분리할 것).
    // 2026-08-24: 큐잉으로 변경 — 재생(1.5~2초) 끝날 때까지 cane_mode_task를
    // 붙잡지 않는다 (audio_task가 core0에서 실제 재생 담당, 위 queue_voice_clip 참고).
    if (m == CaneMode::OBSTACLE) queue_voice_clip("장애물모드전환");
    else if (m == CaneMode::BUS) queue_voice_clip("버스탐지모드전환");
    else if (m == CaneMode::BRAILLE) queue_voice_clip("점자블록모드전환");
}

static void cane_mode_task(void *arg) {
    usb_cdc_init();

    // 가중치 4개(장애물/버스det/버스rec/점자블록)는 모두 flash mmap(RAM 0)으로
    // 로드하고, arena는 실측 기반으로 축소해서 잡는다 (예: 장애물 1792KB).
    size_t sz = 0;
    s_obs_model     = map_partition("ssdmodel", &sz);
    s_bus_det_model = map_partition("detmodel", &sz);
    s_bus_rec_model = map_partition("recmodel", &sz);
    s_bb_model      = map_partition("bbmodel", &sz);
    s_voice_data    = map_partition("voice", &sz);
    s_stations_data = map_partition("stations", &sz);
    if (!s_obs_model)     ESP_LOGW(TAG, "ssdmodel 파티션 없음 — 장애물감지 비활성화");
    if (!s_bus_det_model || !s_bus_rec_model) ESP_LOGW(TAG, "버스 모델 파티션 없음 — 버스모드 비활성화");
    if (!s_bb_model)      ESP_LOGW(TAG, "bbmodel 파티션 없음 — 점자블록감지 비활성화");
    if (!s_voice_data)    ESP_LOGW(TAG, "voice 파티션 없음 — 음성 안내 비활성화");
    if (!s_stations_data) ESP_LOGW(TAG, "stations 파티션 없음 — 버스정류장 게이팅 비활성화(바로 버스모드 진입)");

    enter_mode(CaneMode::OBSTACLE);  // 기본 모드 (여기서 "장애물모드전환" 음성도 재생됨)

    while (true) {
        uint8_t cmd;
        if (usb_cdc_poll_cmd(&cmd)) {
            if (cmd == MODE_CMD_BUS && s_mode != CaneMode::BUS) {
                // 2026-08-23 수정: "정류장이 근처에 있냐"는 판단 안 함 — 무조건
                // 사용자 위치에서 가장 가까운 정류장을 찾고(멀든 가깝든 상관
                // 없음), 그 정류장에 1분 이내 도착하는 버스가 있는지만 본다.
                // 없으면 버스모드 진입 안 하고 장애물모드 유지.
                float lat, lon;
                bool got_gps = usb_cdc_read_gps_payload(&lat, &lon);
#if GPS_USE_DEFAULT_COORD
                if (!got_gps) {
                    ESP_LOGW(TAG, "GPS 페이로드 없음 — 기본 좌표 사용 (%.6f, %.6f)",
                             GPS_DEFAULT_LAT, GPS_DEFAULT_LON);
                    lat = GPS_DEFAULT_LAT;
                    lon = GPS_DEFAULT_LON;
                    got_gps = true;
                }
#endif
                if (!got_gps) {
                    ESP_LOGW(TAG, "GPS 페이로드 없음 — 버스모드 게이팅 스킵하고 바로 진입 (고정 리스트 폴백)");
                    s_bus_use_api_candidates = false;
                    s_bus_target_count = 1; s_bus_matched_count = 0;
                    enter_mode(CaneMode::BUS);
                } else if (!s_stations_data) {
                    // stations 파티션이 없으면 게이팅 자체가 불가능하니 예전처럼 바로 진입
                    ESP_LOGW(TAG, "stations 파티션 없음 — 고정 리스트 폴백으로 바로 진입");
                    s_bus_use_api_candidates = false;
                    s_bus_target_count = 1; s_bus_matched_count = 0;
                    enter_mode(CaneMode::BUS);
                } else {
                    uint64_t bs_id = 0;
                    float dist_m = 0;
                    if (!find_nearest_station(lat, lon, &bs_id, &dist_m)) {
                        ESP_LOGW(TAG, "[정류장체크] 정류장 DB 비어있음 — 고정 리스트 폴백으로 바로 진입");
                        s_bus_use_api_candidates = false;
                        s_bus_target_count = 1; s_bus_matched_count = 0;
                        enter_mode(CaneMode::BUS);
                    } else {
                        int soon_count = gather_bus_candidates(bs_id);
                        if (soon_count <= 0) {
                            ESP_LOGI(TAG, "[정류장체크] 최근접 정류장(%.0fm, bsId=%llu) 도착예정 버스 없음",
                                     dist_m, (unsigned long long)bs_id);
                            queue_voice_clip("도착버스없음");
                        } else {
                            ESP_LOGI(TAG, "[정류장체크] 통과 (%.0fm, bsId=%llu) — %d대(무리) 도착예정, 버스모드 진입",
                                     dist_m, (unsigned long long)bs_id, soon_count);
                            s_bus_use_api_candidates = true;
                            s_bus_target_count = soon_count; s_bus_matched_count = 0;
                            enter_mode(CaneMode::BUS);
                        }
                    }
                }
            }
            else if (cmd == MODE_CMD_BRAILLE && s_mode != CaneMode::BRAILLE) enter_mode(CaneMode::BRAILLE);
            else if (cmd == CMD_OBSTACLE_CONFIRM) {
                uint8_t obj_code = 0;
                if (usb_cdc_read_obstacle_payload(&obj_code)) {
                    const char *key = obstacle_voice_key(obj_code);
                    if (key) queue_voice_clip(key);
                    else ESP_LOGW(TAG, "알 수 없는 장애물 확인 코드 0x%02X", obj_code);
                }
            }
            else if (cmd == CMD_DESTINATION) {
                uint8_t payload = 0;
                if (usb_cdc_read_destination_payload(&payload)) {
                    const char *key = destination_voice_key(payload);
                    if (key) queue_voice_clip(key);
                    else ESP_LOGW(TAG, "알 수 없는 길안내 페이로드 0x%02X", payload);
                }
            }
            else if (cmd == CMD_EMERGENCY) {
                queue_voice_clip("긴급상황발생119신고요청");
            }
            else if (cmd == CMD_ARRIVED) {
                queue_voice_clip("목적지에도착했습니다");
            }
            else if (cmd == CMD_TURN_LEFT) {
                queue_voice_clip("좌회전");
            }
            else if (cmd == CMD_TURN_RIGHT) {
                queue_voice_clip("우회전");
            }
            else if (cmd == CMD_STAIRS) {
                queue_voice_clip("계단");
            }
            else if (cmd == CMD_CROSSWALK) {
                // 2026-08-26: 횡단보도 wav 아직 없음 — 값은 예약해두고 로그만 남김.
                // wav 생기면 voice.bin/voice_clips.h에 추가하고 이 분기도 queue_voice_clip으로 교체.
                ESP_LOGW(TAG, "횡단보도 안내 음성 미준비 (CMD_CROSSWALK 수신됨)");
            }
            else if (cmd == CMD_FALL_ALERT_START) {
                s_fall_alert_active = true;
                ESP_LOGI(TAG, "낙상 알림 반복 시작 (0x0A)");
            }
            else if (cmd == CMD_FALL_ALERT_STOP) {
                s_fall_alert_active = false;
                ESP_LOGI(TAG, "낙상 알림 반복 정지 (0x0B)");
            }
        }

        int64_t elapsed = esp_timer_get_time() - s_mode_enter_us;

        if (s_mode == CaneMode::OBSTACLE) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                static uint8_t *obs_copy = nullptr;
                if (!obs_copy) obs_copy = (uint8_t *)heap_caps_malloc(BUS_ORIG_W * BUS_ORIG_H, MALLOC_CAP_SPIRAM);
                if (obs_copy && fb->len == (size_t)(BUS_ORIG_W * BUS_ORIG_H)) {
                    memcpy(obs_copy, fb->buf, fb->len);
                    esp_camera_fb_return(fb);
                    obs_run_once(obs_copy);
                    send_obstacle_result();
                } else {
                    esp_camera_fb_return(fb);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(300));

        } else if (s_mode == CaneMode::BUS) {
            bool matched = bus_ocr_try_once(s_bus_det_model, s_bus_rec_model);
            // 매칭 여부와 무관하게 매 사이클 리포트 전송 — RPi는 matched 바이트로
            // "아직 진행중인 추정값"과 "사전 매칭 확정값"을 구분한다.
            send_bus_result(matched ? s_bus_last_match : s_bus_report_text,
                             s_bus_report_conf, matched);
            if (matched) {
                queue_voice_clip(s_bus_last_match);   // "937" 등 버스번호 안내
                s_bus_matched_count++;
                // 2026-08-26: 매칭된 순간 바로 전환하지 않고 1초 지연 (요청).
                // 정류장 게이팅에서 잡힌 도착예정 대수(s_bus_target_count)를
                // 아직 다 못 채웠으면 장애물모드로 안 돌아가고 버스모드를
                // 새로 재진입 — enter_mode(BUS)가 이미 로드된 det/rec 모델은
                // 그대로 두고(m==BUS라 deinit 안 함) 타이머만 30초로 리셋하고
                // "버스탐지모드전환" 음성도 다시 재생해준다.
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (s_bus_matched_count < s_bus_target_count) {
                    ESP_LOGI(TAG, "버스 %d/%d대 매칭 — 버스모드 재진입",
                             s_bus_matched_count, s_bus_target_count);
                    enter_mode(CaneMode::BUS);
                } else {
                    enter_mode(CaneMode::OBSTACLE);
                }
            } else if (elapsed > kBusModeTimeoutUs) {
                ESP_LOGI(TAG, "버스모드 30초 타임아웃 — 장애물모드 복귀");
                enter_mode(CaneMode::OBSTACLE);   // 타임아웃은 지연 없이 바로 포기
            }
            vTaskDelay(pdMS_TO_TICKS(200));

        } else { // BRAILLE
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                // 장애물 모드와 동일한 패턴: 복사만 하고 fb는 즉시 반납한 뒤,
                // 무거운 추론(리사이즈+Invoke)은 복사본으로 한다. fb를 추론 내내
                // 붙잡고 있으면 video_push_task가 새 프레임을 못 받아 타임아웃 남
                // (2026-08-14 개발기록 참고).
                static uint8_t *bb_copy = nullptr;
                if (!bb_copy) bb_copy = (uint8_t *)heap_caps_malloc(BUS_ORIG_W * BUS_ORIG_H, MALLOC_CAP_SPIRAM);
                if (bb_copy && fb->len == (size_t)(BUS_ORIG_W * BUS_ORIG_H)) {
                    memcpy(bb_copy, fb->buf, fb->len);
                    int w = fb->width, h = fb->height;
                    esp_camera_fb_return(fb);
                    bb_infer(bb_copy, w, h);
                } else {
                    esp_camera_fb_return(fb);
                }
            }
            // 버스 모드와 동일한 원리: 확실해질(=근접 확정) 때까지 매 사이클 결과를
            // 계속 보내고, near가 되거나 타임아웃일 때만 장애물모드로 복귀한다.
            // near가 버스의 matched와 같은 역할 — "감지됨"만으로는 즉시 안 돌아가고,
            // "충분히 가까이 왔다"는 확정 신호가 있어야 돌아간다. 이러면 재전환 빈도가
            // 자연스럽게 낮아져서 6MB 장애물 arena 재구성도 덜 일어난다 (2026-08-15 개발기록).
            bool near = s_bb.valid && (s_bb.w * s_bb.h > 0.15f);
            send_braille_result(s_bb.valid, s_bb.conf, near);
            if (near) {
                // 2026-08-26: 근접확정 시 방향(s_bb.dir: left/center/right)에 맞는
                // 클립 안내 추가 — 그동안 이 케이스엔 음성이 아예 없었음.
                if (strcmp(s_bb.dir, "left") == 0)       queue_voice_clip("왼쪽에점자블록");
                else if (strcmp(s_bb.dir, "right") == 0) queue_voice_clip("오른쪽에점자블록");
                else                                      queue_voice_clip("정면에점자블록");
                // 확정된 순간 바로 전환하지 않고 1초 지연 (요청).
                vTaskDelay(pdMS_TO_TICKS(1000));
                enter_mode(CaneMode::OBSTACLE);
            } else if (elapsed > kBrailleModeTimeoutUs) {
                enter_mode(CaneMode::OBSTACLE);   // 타임아웃은 지연 없이 바로 포기
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// ── RPi로 영상 푸시 (박스 오버레이 JPEG, TCP push) ────────────────────────────
// RPi IP를 하드코딩하지 않고 mDNS(cane.local)로 매번 다시 찾는다 — 휴대폰
// 핫스팟처럼 접속할 때마다 IP가 바뀌는 환경에서도 RPi 쪽 호스트네임만
// "cane"으로 고정해두면(예: sudo hostnamectl set-hostname cane) 코드 수정 없이 붙는다.
// 프레임마다 [4바이트 LE 길이][JPEG 바이트열]을 계속 보낸다.
// 현재 활성 모드의 박스(장애물/버스/점자블록)를 그레이스케일 프레임에 직접
// 그려 넣은 뒤 JPEG 인코딩해서 보낸다 (RPi 쪽에서 따로 박스를 그릴 필요 없음).
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "mdns.h"

// RPi 호스트네임을 "cane"으로 고정해두면(Raspberry Pi OS는 avahi가 기본 설치돼 있어
// `sudo hostnamectl set-hostname cane` 한 번이면 cane.local로 바로 잡힘) 핫스팟이
// 바뀌어 IP가 달라져도 코드를 안 고치고 그때그때 mDNS로 찾아간다.
#define VIDEO_PUSH_HOST "cane"
#define VIDEO_PUSH_PORT 8090

static void draw_box_gray(uint8_t *buf, int W, int H,
                           float nx1, float ny1, float nx2, float ny2, uint8_t val) {
    int x1 = std::max(0, std::min(W - 1, (int)(nx1 * W)));
    int y1 = std::max(0, std::min(H - 1, (int)(ny1 * H)));
    int x2 = std::max(0, std::min(W - 1, (int)(nx2 * W)));
    int y2 = std::max(0, std::min(H - 1, (int)(ny2 * H)));
    if (x2 <= x1 || y2 <= y1) return;

    const int t = 3; // 테두리 두께(px)
    for (int dy = 0; dy < t; dy++) {
        if (y1 + dy < H) memset(buf + (y1 + dy) * W + x1, val, x2 - x1);
        if (y2 - dy >= 0 && y2 - dy < H) memset(buf + (y2 - dy) * W + x1, val, x2 - x1);
    }
    for (int y = y1; y <= y2 && y < H; y++) {
        for (int dx = 0; dx < t; dx++) {
            if (x1 + dx < W) buf[y * W + x1 + dx] = val;
            if (x2 - dx >= 0 && x2 - dx < W) buf[y * W + x2 - dx] = val;
        }
    }
}

static int s_video_sock = -1;

static bool video_push_connect(void) {
    if (s_video_sock >= 0) return true;

    // 연결 시도마다 mDNS로 다시 물어본다 — 핫스팟이 바뀌어 RPi IP가 달라져도
    // cane.local 이름만 살아있으면 알아서 새 IP로 찾아간다.
    esp_ip4_addr_t rpi_ip = {};
    if (mdns_query_a(VIDEO_PUSH_HOST, 2000, &rpi_ip) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS 조회 실패 (%s.local) — 잠시 후 재시도", VIDEO_PUSH_HOST);
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(VIDEO_PUSH_PORT);
    addr.sin_addr.s_addr = rpi_ip.addr;

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return false;
    }
    s_video_sock = sock;
    ESP_LOGI(TAG, "영상 푸시 연결됨 (" IPSTR ":%d)", IP2STR(&rpi_ip), VIDEO_PUSH_PORT);
    return true;
}

static void video_push_disconnect(void) {
    if (s_video_sock >= 0) { close(s_video_sock); s_video_sock = -1; }
}

static void video_push_task(void *arg) {
    static uint8_t *frame_copy = nullptr;
    if (!frame_copy) frame_copy = (uint8_t *)heap_caps_malloc(BUS_ORIG_W * BUS_ORIG_H, MALLOC_CAP_SPIRAM);

    while (true) {
        // 영상 전송(video_push_task)은 활성 모드와 무관하게 항상 돈다.
        if (!frame_copy) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        if (!video_push_connect()) {
            vTaskDelay(pdMS_TO_TICKS(2000));  // RPi 연결 안 됐으면 잠시 후 재시도
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if (fb->len != (size_t)(BUS_ORIG_W * BUS_ORIG_H)) { esp_camera_fb_return(fb); continue; }
        memcpy(frame_copy, fb->buf, fb->len);
        esp_camera_fb_return(fb);  // 복사 끝났으니 바로 반납 (다른 태스크 카메라 사용 안 막음)

        // 현재 활성 모드의 박스만 그려 넣는다
        if (s_mode == CaneMode::OBSTACLE && s_obs_result.valid) {
            draw_box_gray(frame_copy, BUS_ORIG_W, BUS_ORIG_H,
                          s_obs_result.x1, s_obs_result.y1, s_obs_result.x2, s_obs_result.y2, 255);
        } else if (s_mode == CaneMode::BUS && s_bus_result.valid) {
            draw_box_gray(frame_copy, BUS_ORIG_W, BUS_ORIG_H,
                          s_bus_result.cx - s_bus_result.w / 2, s_bus_result.cy - s_bus_result.h / 2,
                          s_bus_result.cx + s_bus_result.w / 2, s_bus_result.cy + s_bus_result.h / 2, 255);
        } else if (s_mode == CaneMode::BRAILLE && s_bb.valid) {
            draw_box_gray(frame_copy, BUS_ORIG_W, BUS_ORIG_H,
                          s_bb.cx - s_bb.w / 2, s_bb.cy - s_bb.h / 2,
                          s_bb.cx + s_bb.w / 2, s_bb.cy + s_bb.h / 2, 255);
        }

        uint8_t *jpg = nullptr;
        size_t jpg_len = 0;
        if (fmt2jpg(frame_copy, BUS_ORIG_W * BUS_ORIG_H, BUS_ORIG_W, BUS_ORIG_H,
                    PIXFORMAT_GRAYSCALE, 80, &jpg, &jpg_len)) {
            uint32_t len_le = (uint32_t)jpg_len;  // ESP32는 리틀엔디안이라 그대로 씀
            int wr1 = send(s_video_sock, &len_le, sizeof(len_le), 0);
            int wr2 = (wr1 == (int)sizeof(len_le)) ? send(s_video_sock, jpg, jpg_len, 0) : -1;
            if (wr1 != (int)sizeof(len_le) || wr2 != (int)jpg_len) {
                ESP_LOGW(TAG, "영상 푸시 전송 실패 — 재연결");
                video_push_disconnect();
            }
            free(jpg);
        }

        vTaskDelay(pdMS_TO_TICKS(200));  // 대략 5fps (20fps로 시도해봤으나 체감 차이 없어서 원복, 2026-08-15)
    }
}

// ── WiFi 이벤트 핸들러 ──────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 재연결 중...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP 주소: " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "브라우저에서 http://" IPSTR "/ 접속", IP2STR(&e->ip_info.ip));
        s_wifi_ready = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASS, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

// ── 메인 ──────────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    // 로그를 USB CDC(라즈베리파이 채널)로도 내보내는 후킹 — 가장 먼저 등록해야
    // 그 이후의 모든 ESP_LOG*가 다 걸린다. usb_cdc_init()(cane_mode_task 안에서
    // 호출됨) 전까지는 s_usb_cdc_ready=false라 원래 UART 콘솔로만 나간다.
    s_orig_log_vprintf = esp_log_set_vprintf(usb_cdc_log_vprintf);

    // 캐시 크기 변경(2026-08-14: I 16→32KB, D 32→64KB, D라인 32→64B) 전후로
    // 내부 DRAM 여유가 얼마나 줄었는지 실측하기 위한 로그. 문제 되면 이 숫자 보고
    // sdkconfig 캐시 설정을 원상복구할 것.
    ESP_LOGI(TAG, "부팅 시 내부 DRAM 여유: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    ESP_ERROR_CHECK(nvs_flash_init());

    ir_gpio_init();
    spk_init();

    // 2026-08-24: 음성 재생 전용 태스크 — video_push_task와 같은 core0에 고정한다.
    // cane_mode_task(core1, 추론)와는 물리적으로 분리돼서 음성 재생(블로킹) 중에도
    // 추론이 안 멈춘다. video_push_task(우선순위2)보다 살짝 높은 우선순위(3)를 줘서
    // 오디오가 필요할 때 JPEG 인코딩보다 먼저 스케줄되게 한다 (완전히 안 겹치는
    // 건 아니지만, core1의 추론과 얽히는 것보다 훨씬 가벼운 충돌 — 8.24개발기록 참고).
    s_audio_queue = xQueueCreate(4, AUDIO_QUEUE_KEY_LEN);
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4096, NULL, 3, NULL, 0);

    // 카메라를 먼저 초기화해야 한다. 조도센서와 I2C 버스를 공유하는데
    // 순서가 바뀌면 버스 설정이 충돌한다.
    ESP_LOGI(TAG, "카메라 초기화 중...");
    ESP_ERROR_CHECK(esp_camera_init(&CAM_CFG));
    ESP_LOGI(TAG, "카메라 초기화 성공  PID=0x%x",
             esp_camera_sensor_get()->id.PID);

    // 2026-08-18: vflip(상하반전) 빼고 hmirror(좌우반전)만 적용 (요청).
    {
        sensor_t *s = esp_camera_sensor_get();
        int hm_ret = s->set_hmirror(s, 1);
        ESP_LOGI(TAG, "hmirror 설정 결과=%d (0=성공)", hm_ret);
    }

    // s_bus_result_lock은 대시보드(웹)가 항상 참조하므로, 태스크 생성 전에 먼저 만든다.
    s_bus_result_lock = xSemaphoreCreateMutex();

    // 카메라가 만든 I2C 버스에 조도센서를 얹는다 (실패해도 계속 진행)
    als_init();
    xTaskCreate(ir_task, "ir_task", 8192, NULL, 4, NULL);   // TTS 디코딩 여유

    // 장애물(기본)/버스/점자블록 모드 상태머신 — 한 번에 하나의 모델만 메모리에 올려두고
    // RPi(ttyACM1)의 0x01/0x02 명령으로 모드를 전환한다.
    // core1에 고정 — video_push_task(core0)와 코어를 분리해서, 추론(Invoke())이
    // CPU를 오래 쥐고 있어도 영상 전송 태스크가 스케줄링에서 밀리지 않게 한다.
    // (xTaskCreate로 코어 미지정 시 스케줄러가 둘을 같은 코어에 몰아넣을 수 있고,
    // 실제로 점자블록 모드에서 그 증상이 재현됨 — 2026-08-14 개발기록 참고.)
    xTaskCreatePinnedToCore(cane_mode_task, "cane_mode_task", 16384, NULL, 3, NULL, 1);
    // WiFi 연결 — video_push_task보다 반드시 먼저 해야 한다. wifi_init() 안에서
    // esp_netif_init()이 lwIP tcpip_thread를 띄우는데, 그 전에 socket()을 부르면
    // "Invalid mbox" assert로 죽는다 (2026-08-12에 실제로 겪음, 개발기록 참고).
    ESP_LOGI(TAG, "WiFi 연결 중...");
    wifi_init();

    // video_push_task가 cane.local을 mDNS로 조회하므로 그 전에 초기화해야 한다.
    // ESP32 자기 자신을 이름으로 광고할 필요는 없음 — RPi(cane)를 찾아가는 client 역할만 함.
    esp_err_t mdns_err = mdns_init();
    if (mdns_err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init 실패 (%d) — cane.local 조회 불가", (int)mdns_err);
    }

    // RPi로 영상을 TCP 푸시(video_push_task)한다.
    // core0에 고정 — cane_mode_task(core1)와 분리해서 추론 중에도 영상 전송이
    // CPU를 계속 받을 수 있게 한다.
    xTaskCreatePinnedToCore(video_push_task, "video_push_task", 8192, NULL, 2, NULL, 0);
}
