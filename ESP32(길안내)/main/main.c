#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h" // 라즈베리파이5 GPS 수신용 USB-CDC

#define TAG "TMAP_NAV"
#define MATH_PI 3.14159265358979323846

#define WIFI_SSID "sew"
#define WIFI_PASS "shin0509"
#define TMAP_APP_KEY "TJZqUnpN8j2rXMNOIex7u2gWb2GV6ieX9DUln7rP"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// 라즈베리파이5 <-> ESP32-S3 제어 신호 (콘솔 로그와 같은 USB-CDC 회선 공유)
// RPi -> ESP32: 목적지 선택
#define CMD_DEST_1 0x01
#define CMD_DEST_2 0x02
// ESP32 -> RPi: 상태 통지
// 0xF5~0xFF는 UTF-8에 등장할 수 없는 바이트라 로그와 겹치지 않는다.
#define SIGNAL_NEXT_DIRECTION 0xFE // 다음 방향 안내
#define SIGNAL_ARRIVED 0xFF        // 최종 목적지 도착
// 길안내 이벤트 신호. 0x00~0x1F는 ASCII 제어문자 영역이라 로그(0x20 이상)와 겹치지 않는다.
#define SIGNAL_TURN_LEFT 0x03  // 좌회전
#define SIGNAL_TURN_RIGHT 0x04 // 우회전
#define SIGNAL_CROSSWALK 0x05  // 횡단보도
#define SIGNAL_STAIRS 0x06     // 계단
// 경로 이탈 / 재탐색 신호
#define SIGNAL_OFF_ROUTE 0x07      // 경로 이탈 확정
#define SIGNAL_REROUTING 0x08      // 재탐색 시작
#define SIGNAL_REROUTE_DONE 0x09   // 재탐색 성공, 새 경로로 안내 재개
#define SIGNAL_REROUTE_FAILED 0x0A // 재탐색 실패 (백오프 후 재시도 예정)
#define SIGNAL_BACK_ON_ROUTE 0x0B  // 재탐색 전에 사용자가 스스로 경로로 복귀

#define MAX_WAYPOINTS 100
#define MAX_ROUTE_LINE_POINTS 300 // TMAP LineString(보행로 형상) 정점 최대 개수
#define HTTP_RESP_BUF_SIZE 20480

// 길안내/이탈 판정 파라미터
#define ANNOUNCE_DISTANCE_M 5.0   // 이 거리 안으로 들어오면 안내 1회 발송
#define ARRIVAL_RADIUS_M 5.0      // 안내지점 도달 인정 반경
#define OFF_ROUTE_DISTANCE_M 20.0 // 경로선에서 이만큼 벗어나면 이탈 후보
#define ON_ROUTE_DISTANCE_M 15.0  // 이 안으로 들어오면 이탈 해제 (히스테리시스)
#define OFF_ROUTE_CONFIRM_CNT 4   // 4회(=4초) 연속돼야 이탈 확정
#define REROUTE_COOLDOWN_MS 15000 // 재탐색 성공 직후 재발동 방지
#define REROUTE_RETRY_BASE_MS 10000 // 재탐색 실패 시 백오프 시작값
#define REROUTE_RETRY_MAX_MS 60000
#define GPS_FRESH_MS 5000      // 이 시간 넘게 GPS가 없으면 이탈 판정 중단
#define PASS_MARGIN_SEGMENTS 2 // 안내지점 통과 판정 여유 (경로선 정점 개수)
#define PASS_CONFIRM_CNT 2
// 주기 안내 로그 간격. navigation_task 루프 자체는 1Hz를 유지할 것
// (루프를 늘리면 OFF_ROUTE_CONFIRM_CNT 4회가 4초가 아니게 된다)
#define GUIDANCE_LOG_INTERVAL_MS 5000

typedef struct { double lon; double lat; int turnType; int facilityType; char facilityName[32]; char description[128]; } Waypoint_t;
typedef struct { double lat; double lon; } LatLon_t;

Waypoint_t route_points[MAX_WAYPOINTS];
int total_waypoints = 0;
int current_target_idx = 0;
static TaskHandle_t navigation_task_handle = NULL;

// 목적지 선택 명령을 route_request_task로 넘기는 깊이 1 큐.
// 명령마다 태스크를 새로 만들면 두 요청이 route_points/route_line을 동시에 덮어쓴다.
static QueueHandle_t route_cmd_q = NULL;

// 이탈 판정 기준선. LineString 피처를 이어붙인 실제 보행로 폴리라인.
static LatLon_t route_line[MAX_ROUTE_LINE_POINTS];
static int route_line_count = 0;
// 각 안내지점에 대응하는 경로선 정점 인덱스 (통과 판정용)
static int waypoint_line_idx[MAX_WAYPOINTS];

// 재탐색 때 목적지를 알아야 해서 전역으로 둔다
static int current_dest_idx = -1;
// navigation_task를 협조적으로 멈추기 위한 플래그.
// HTTP/TLS 요청 중에 vTaskDelete하면 소켓과 힙이 샌다.
static volatile bool nav_stop_requested = false;

// 라즈베리파이가 0x01/0x02로 고르는 목적지
typedef struct { double lat; double lon; const char *name; } Destination_t;
static const Destination_t DESTINATIONS[] = {
    { 35.88407372414365, 128.61325021409556, "목적지1(버스정류장)" },
    { 35.88708878857774, 128.6121873917122, "목적지2(학교)" },
};
#define DESTINATION_COUNT (sizeof(DESTINATIONS) / sizeof(DESTINATIONS[0]))

void navigation_task(void *pvParameters);
void fetch_tmap_pedestrian_route(double dest_lat, double dest_lon);
void start_navigation_to_destination(int dest_idx);
static bool try_reroute(void);

// 현재 위치. GPS 수신 전에는 이 기본값으로 첫 경로를 탐색한다.
double current_my_lon = 128.61161779437046;
double current_my_lat = 35.887123468705354;
bool has_real_gps_fix = false;

// 마지막 GPS 수신 시각. GPS 끊김과 실제 경로 이탈을 구분하는 데 쓴다.
static volatile TickType_t last_gps_tick = 0;

// Xtensa에서 double은 원자적으로 읽히지 않는다. 위/경도가 서로 다른 샘플에서
// 섞여 읽히면 이탈 판정이 수백 m 튀어서 잘못된 재탐색이 걸린다.
static portMUX_TYPE gps_mux = portMUX_INITIALIZER_UNLOCKED;

static void gps_update(double lat, double lon) {
    portENTER_CRITICAL(&gps_mux);
    current_my_lat = lat;
    current_my_lon = lon;
    last_gps_tick = xTaskGetTickCount();
    portEXIT_CRITICAL(&gps_mux);
}

// 위/경도를 한 벌로 읽어온다. fresh가 거짓이면 이탈 판정을 하지 않는다.
static void gps_snapshot(double *lat, double *lon, bool *fresh) {
    portENTER_CRITICAL(&gps_mux);
    double snap_lat = current_my_lat;
    double snap_lon = current_my_lon;
    TickType_t snap_tick = last_gps_tick;
    bool has_fix = has_real_gps_fix;
    portEXIT_CRITICAL(&gps_mux);

    if (lat) *lat = snap_lat;
    if (lon) *lon = snap_lon;
    if (fresh) {
        *fresh = has_fix &&
                 (xTaskGetTickCount() - snap_tick) < pdMS_TO_TICKS(GPS_FRESH_MS);
    }
}

const char *current_position_source(void) {
    return has_real_gps_fix ? "실시간 GPS" : "기본값(하드코딩)";
}

// 라즈베리파이로 상태 신호 1바이트 전송
static void usb_send_signal(uint8_t signal) {
    usb_serial_jtag_write_bytes(&signal, 1, pdMS_TO_TICKS(100));
}

/* 길안내 이벤트 판정 (TMAP 보행자 경로 코드)
 *   turnType: 12/16/17 좌회전, 13/18/19 우회전, 211~217 횡단보도
 *   계단/횡단보도는 facilityType 숫자 대신 facilityName 문자열로 판정한다.
 *   TMAP 문서의 facilityType 코드표가 예시와 서로 어긋나 신뢰할 수 없다.
 */
static bool is_turn_left(int turnType) {
    return turnType == 12 || turnType == 16 || turnType == 17;
}
static bool is_turn_right(int turnType) {
    return turnType == 13 || turnType == 18 || turnType == 19;
}
static bool is_crosswalk(const Waypoint_t *wp) {
    return strstr(wp->facilityName, "횡단보도") != NULL ||
           (wp->turnType >= 211 && wp->turnType <= 217);
}
static bool is_stairs(const Waypoint_t *wp) {
    return strstr(wp->facilityName, "계단") != NULL;
}

// 안내지점에서 할 동작을 한 줄 문구로 만든다.
// 한 지점이 회전과 횡단보도를 겸할 수 있어 해당되는 걸 이어붙인다.
static void maneuver_label(const Waypoint_t *wp, char *out, size_t out_size) {
    const char *turn = "직진";
    if (is_turn_left(wp->turnType)) turn = "좌회전";
    else if (is_turn_right(wp->turnType)) turn = "우회전";

    snprintf(out, out_size, "%s%s%s", turn,
             is_crosswalk(wp) ? " + 횡단보도" : "",
             is_stairs(wp) ? " + 계단" : "");
}

// 한 지점이 여러 이벤트를 가질 수 있으므로 해당되는 신호를 모두 보낸다.
static void send_guidance_event_signals(const Waypoint_t *wp) {
    if (is_turn_left(wp->turnType)) {
        ESP_LOGI(TAG, "📤 [이벤트 신호] 좌회전 (0x03)");
        usb_send_signal(SIGNAL_TURN_LEFT);
    } else if (is_turn_right(wp->turnType)) {
        ESP_LOGI(TAG, "📤 [이벤트 신호] 우회전 (0x04)");
        usb_send_signal(SIGNAL_TURN_RIGHT);
    }

    if (is_crosswalk(wp)) {
        ESP_LOGI(TAG, "📤 [이벤트 신호] 횡단보도 (0x05)");
        usb_send_signal(SIGNAL_CROSSWALK);
    }

    if (is_stairs(wp)) {
        ESP_LOGI(TAG, "📤 [이벤트 신호] 계단 (0x06)");
        usb_send_signal(SIGNAL_STAIRS);
    }
}

// TMAP 응답 원문을 로그에 찍기 전 출력 가능한 ASCII만 남긴다.
// 실패 응답의 바이트가 제어 신호값과 겹쳐 라즈베리파이로 새어나가는 걸 막는다.
static void sanitize_for_log(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    for (; src[i] != '\0' && i < dst_size - 1; i++) {
        unsigned char ch = (unsigned char)src[i];
        dst[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    dst[i] = '\0';
}

double calculate_distance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * MATH_PI / 180.0;
    double dLon = (lon2 - lon1) * MATH_PI / 180.0;
    double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * MATH_PI / 180.0) * cos(lat2 * MATH_PI / 180.0) * sin(dLon / 2) * sin(dLon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

// 점에서 선분까지의 거리(m). 세그먼트마다 haversine을 돌리면 비싸서,
// A를 원점으로 미터 좌표로 바꾼 뒤 투영하는 국소 평면 근사를 쓴다.
static double point_to_segment_meters(double plat, double plon,
                                      double alat, double alon,
                                      double blat, double blon) {
    const double M_PER_DEG_LAT = 110540.0;
    double m_per_deg_lon = 111320.0 * cos(plat * MATH_PI / 180.0);

    double px = (plon - alon) * m_per_deg_lon, py = (plat - alat) * M_PER_DEG_LAT;
    double bx = (blon - alon) * m_per_deg_lon, by = (blat - alat) * M_PER_DEG_LAT;

    double seg_len2 = bx * bx + by * by;
    double t = (seg_len2 <= 0.0) ? 0.0 : (px * bx + py * by) / seg_len2;
    if (t < 0.0) t = 0.0;
    else if (t > 1.0) t = 1.0;

    double dx = px - t * bx, dy = py - t * by;
    return sqrt(dx * dx + dy * dy);
}

// 경로선까지의 최단거리(m)와 가장 가까운 세그먼트의 시작 정점 인덱스.
// 경로선 정점이 2개 미만이면 -1.0을 반환한다.
static double distance_to_route_meters(double lat, double lon, int *out_seg_idx) {
    if (route_line_count < 2) {
        if (out_seg_idx) *out_seg_idx = -1;
        return -1.0;
    }

    double best = -1.0;
    int best_idx = 0;
    for (int i = 0; i < route_line_count - 1; i++) {
        double d = point_to_segment_meters(lat, lon,
                                           route_line[i].lat, route_line[i].lon,
                                           route_line[i + 1].lat, route_line[i + 1].lon);
        if (best < 0.0 || d < best) {
            best = d;
            best_idx = i;
        }
    }
    if (out_seg_idx) *out_seg_idx = best_idx;
    return best;
}

// 각 안내지점에 가장 가까운 경로선 정점을 미리 구해둔다. 경로를 받을 때마다 1회.
static void build_waypoint_line_index(void) {
    for (int i = 0; i < total_waypoints; i++) {
        waypoint_line_idx[i] = 0;
        if (route_line_count < 1) continue;

        double best = -1.0;
        for (int j = 0; j < route_line_count; j++) {
            double d = calculate_distance(route_points[i].lat, route_points[i].lon,
                                          route_line[j].lat, route_line[j].lon);
            if (best < 0.0 || d < best) {
                best = d;
                waypoint_line_idx[i] = j;
            }
        }
    }
}

/* 라즈베리파이5가 보내는 GPS 문자열 파싱
 *   "GPS,<위도>,<경도>\n"  (예: "GPS,35.887123,128.611617\n")
 *   NMEA -> 십진수(DD.DDDD) 변환은 라즈베리파이 쪽에서 끝내고 보낸다.
 */
bool parse_gps_line(const char *line, double *lat, double *lon) {
    if (strncmp(line, "GPS,", 4) != 0) return false;

    double parsed_lat, parsed_lon;
    if (sscanf(line + 4, "%lf,%lf", &parsed_lat, &parsed_lon) != 2) return false;

    // 위경도 범위를 벗어나면 노이즈로 보고 버린다
    if (parsed_lat < -90.0 || parsed_lat > 90.0 || parsed_lon < -180.0 || parsed_lon > 180.0) return false;

    *lat = parsed_lat;
    *lon = parsed_lon;
    return true;
}

// 목적지 명령을 큐에서 하나씩 꺼내 순차 처리하는 상주 태스크.
// GPS 수신 루프가 TMAP HTTP 요청(최대 15초)에 막히지 않게 하고,
// start_navigation_to_destination이 겹쳐 실행되는 것도 막아준다.
void route_request_task(void *pvParameters) {
    int dest_idx;
    while (1) {
        if (xQueueReceive(route_cmd_q, &dest_idx, portMAX_DELAY) == pdTRUE) {
            start_navigation_to_destination(dest_idx);
        }
    }
}

/* USB-CDC로 라즈베리파이5의 GPS/명령 데이터를 읽는 태스크
 *   - XIAO ESP32-S3의 USB-C 포트는 콘솔 로그 출력에도 쓰여서 드라이버가 이미
 *     설치돼 있을 수 있다. 그 경우 재설치 없이 RX만 읽는다.
 *   - GPS 줄("GPS,lat,lon\n")과 목적지 명령(0x01/0x02) 바이트가 같은 회선으로 섞여
 *     들어온다. 명령은 개행 없는 raw 1바이트라 줄 버퍼에 쌓기 전에 걸러낸다.
 */
void gps_usb_task(void *pvParameters) {
    ESP_LOGI(TAG, "🛰️ 라즈베리파이5 GPS/명령 데이터 수신 대기 중... (USB-CDC)");

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
    }

    uint8_t rx_data[128];
    char line_buffer[256];
    int line_pos = 0;

    while (1) {
        int len = usb_serial_jtag_read_bytes(rx_data, sizeof(rx_data) - 1, pdMS_TO_TICKS(20));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t raw = rx_data[i];

                if (raw == CMD_DEST_1 || raw == CMD_DEST_2) {
                    int dest_idx = (raw == CMD_DEST_1) ? 0 : 1;
                    // xQueueOverwrite는 블록하지 않아 수신 루프가 멈추지 않고,
                    // 대기 중인 명령을 덮어써서 항상 최신 명령이 이긴다.
                    xQueueOverwrite(route_cmd_q, &dest_idx);
                    continue;
                }

                char c = (char)raw;
                if (c == '\n' || c == '\r') {
                    if (line_pos > 0) {
                        line_buffer[line_pos] = '\0';

                        double real_lat, real_lon;
                        if (parse_gps_line(line_buffer, &real_lat, &real_lon)) {
                            if (!has_real_gps_fix) {
                                has_real_gps_fix = true;
                                ESP_LOGI(TAG, "✅ [GPS 최초 수신] 기본값 위치 -> 실제 위치로 전환합니다.");
                            }
                            gps_update(real_lat, real_lon);
                            ESP_LOGI(TAG, "🌍 [실시간 GPS] 위도: %f, 경도: %f", real_lat, real_lon);
                        }
                        line_pos = 0;
                    }
                } else {
                    if (line_pos < sizeof(line_buffer) - 1) {
                        line_buffer[line_pos++] = c;
                    } else {
                        // 개행 없이 노이즈가 버퍼를 채운 경우, 버리고 새로 시작
                        ESP_LOGW(TAG, "⚠️ GPS 라인 버퍼 오버플로우, 리셋합니다.");
                        line_pos = 0;
                    }
                }
            }
        }
    }
}

/* 실시간 내비게이션 태스크 (1Hz) */
void navigation_task(void *pvParameters) {
    ESP_LOGI(TAG, "🚀 [실시간 길 안내 태스크] 시작됨!");

    if (!has_real_gps_fix) {
        ESP_LOGW(TAG, "⚠️ 아직 GPS 실측값을 못 받아서 기본값 위치(%.6f, %.6f) 기준으로 안내를 시작합니다.",
                 current_my_lat, current_my_lon);
    }

    if (total_waypoints > 0) {
        ESP_LOGI(TAG, "=============================================");
        ESP_LOGI(TAG, "📣 길 안내를 시작합니다.");
        ESP_LOGI(TAG, "👉 [초기 방향] %s", route_points[0].description);
        ESP_LOGI(TAG, "=============================================\n");
    }

    int off_route_count = 0, pass_count = 0;
    bool off_route = false;
    TickType_t next_reroute_allowed = 0;
    uint32_t retry_ms = REROUTE_RETRY_BASE_MS;
    TickType_t next_guidance_log = 0; // 0이면 첫 루프에서 기다리지 않고 바로 안내
    int last_logged_idx = -1;         // 목표가 바뀌면 타이머와 무관하게 즉시 안내
    int announced_idx = -1;           // 음성 안내를 이미 보낸 지점 (지점당 1회)

    while (!nav_stop_requested && current_target_idx < total_waypoints) {
        double my_lat, my_lon;
        bool fresh;
        gps_snapshot(&my_lat, &my_lon, &fresh);

        // 실측 GPS와 경로선이 모두 있을 때만 이탈/통과 판정을 한다.
        // can_judge가 거짓이면 (A)(B)(C)를 건너뛰고 도착 판정만 남는다.
        int seg_idx = -1;
        double off_dist = fresh ? distance_to_route_meters(my_lat, my_lon, &seg_idx) : -1.0;
        bool can_judge = (off_dist >= 0.0);

        /* (A) 이탈 판정. 히스테리시스 + 연속 확인으로 GPS 튐에 의한 오탐을 거른다 */
        if (can_judge) {
            if (!off_route) {
                if (off_dist > OFF_ROUTE_DISTANCE_M) {
                    if (++off_route_count >= OFF_ROUTE_CONFIRM_CNT) {
                        off_route = true;
                        ESP_LOGW(TAG, "=============================================");
                        ESP_LOGW(TAG, "⚠️ [경로 이탈] 경로에서 %.1fm 벗어났습니다!", off_dist);
                        ESP_LOGW(TAG, "=============================================\n");
                        usb_send_signal(SIGNAL_OFF_ROUTE);
                    }
                } else {
                    off_route_count = 0;
                }
            } else if (off_dist < ON_ROUTE_DISTANCE_M) {
                // 재탐색이 걸리기 전에 스스로 경로로 돌아온 경우
                off_route = false;
                off_route_count = 0;
                retry_ms = REROUTE_RETRY_BASE_MS;
                ESP_LOGI(TAG, "✅ [경로 복귀] 다시 경로 위로 돌아왔습니다. 기존 안내를 계속합니다.");
                usb_send_signal(SIGNAL_BACK_ON_ROUTE);
            }
        } else {
            off_route_count = 0;
        }

        /* (B) 재탐색. 쿨다운/백오프를 지킨다 */
        if (off_route && xTaskGetTickCount() >= next_reroute_allowed) {
            if (try_reroute()) {
                off_route = false;
                off_route_count = 0;
                pass_count = 0;
                // current_target_idx가 0으로 리셋되므로 이 둘도 같이 되돌린다.
                // 안 그러면 새 경로의 첫 지점 안내가 묻힌다.
                announced_idx = -1;
                last_logged_idx = -1;
                retry_ms = REROUTE_RETRY_BASE_MS;
                next_reroute_allowed = xTaskGetTickCount() + pdMS_TO_TICKS(REROUTE_COOLDOWN_MS);
                continue;
            }
            next_reroute_allowed = xTaskGetTickCount() + pdMS_TO_TICKS(retry_ms);
            retry_ms = (retry_ms * 2 > REROUTE_RETRY_MAX_MS) ? REROUTE_RETRY_MAX_MS : retry_ms * 2;
        }

        /* (C) 안내지점 통과 감지. 경로 위에 있을 때만 동작한다.
         *   ARRIVAL_RADIUS_M 밖으로 지나치면 current_target_idx가 영영 멈춰
         *   안내가 끊기므로, 이미 지나간 지점은 안내 없이 건너뛴다. */
        if (!off_route && can_judge &&
            seg_idx > waypoint_line_idx[current_target_idx] + PASS_MARGIN_SEGMENTS) {
            if (++pass_count >= PASS_CONFIRM_CNT) {
                ESP_LOGW(TAG, "⏭️ [안내지점 통과] '%s'를 이미 지나쳐 다음 지점으로 넘어갑니다.",
                         route_points[current_target_idx].description);
                current_target_idx++;
                pass_count = 0;
                continue;
            }
        } else {
            pass_count = 0;
        }

        Waypoint_t target = route_points[current_target_idx];
        double distance = calculate_distance(my_lat, my_lon, target.lat, target.lon);

        /* (D) 주기 안내. 5초마다, 목표 지점이 바뀌면 즉시 한 번 더.
         *   GPS 오차가 5~10m라 도달 반경만 보고 안내하면 통째로 침묵할 수 있어
         *   도달 여부와 무관하게 현재 상태를 계속 알린다.
         *   라즈베리파이로 가는 음성 신호는 (E)에서 지점당 1회만 보낸다. */
        if (xTaskGetTickCount() >= next_guidance_log || current_target_idx != last_logged_idx) {
            next_guidance_log = xTaskGetTickCount() + pdMS_TO_TICKS(GUIDANCE_LOG_INTERVAL_MS);
            last_logged_idx = current_target_idx;

            if (off_route) {
                // 이탈 상태에서 "43m 앞 좌회전"을 알리면 엉뚱한 방향으로 유도하게 된다
                ESP_LOGW(TAG, "🧭 [안내] 경로를 이탈했습니다. 경로선에서 %.0fm 이격 | 재탐색 대기 중", off_dist);
            } else if (!fresh) {
                ESP_LOGW(TAG, "🧭 [안내] GPS 신호를 기다리는 중 (%s 기준) | 다음: %s",
                         current_position_source(), target.description);
            } else {
                char label[48];
                maneuver_label(&target, label, sizeof(label));
                if (can_judge) {
                    ESP_LOGI(TAG, "🧭 [안내 %d/%d] %.0fm 앞 %s | %s | 경로선 이격 %.0fm",
                             current_target_idx + 1, total_waypoints,
                             distance, label, target.description, off_dist);
                } else {
                    // 경로선이 없으면 이격 거리를 계산할 수 없어 생략
                    ESP_LOGI(TAG, "🧭 [안내 %d/%d] %.0fm 앞 %s | %s",
                             current_target_idx + 1, total_waypoints,
                             distance, label, target.description);
                }
            }
        }

        /* (E) 접근 안내. ANNOUNCE_DISTANCE_M 안으로 들어오면 지점당 1회 발송.
         *   도달 반경까지 기다리면 GPS 오차로 그 관문을 못 밟고 지나쳐
         *   회전 안내를 아예 못 받는 구간이 생긴다.
         *   이탈 중이거나 GPS가 끊겼으면 보내지 않는다. */
        if (!off_route && fresh && distance <= ANNOUNCE_DISTANCE_M &&
            announced_idx != current_target_idx) {
            announced_idx = current_target_idx;
            ESP_LOGI(TAG, "=============================================");
            ESP_LOGI(TAG, "👉 [방향 안내 | %.0fm 앞] %s", distance, target.description);
            if (is_crosswalk(&target)) ESP_LOGW(TAG, "🚨 [부저 ON 띠띠띠!] 전방에 횡단보도가 있습니다!");
            else if (is_stairs(&target)) ESP_LOGW(TAG, "🚨 [부저 ON 띠띠띠!] 전방에 계단이 있습니다! 조심하세요!");
            ESP_LOGI(TAG, "=============================================\n");
            usb_send_signal(SIGNAL_NEXT_DIRECTION);
            send_guidance_event_signals(&target);
        }

        /* (F) 도달 판정. 안내는 (E)에서 이미 나갔으므로 인덱스만 전진시킨다 */
        if (distance <= ARRIVAL_RADIUS_M) {
            current_target_idx++;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (nav_stop_requested) {
        // 새 목적지 요청으로 중단된 경우라 도착 신호를 보내면 안 된다
        ESP_LOGI(TAG, "🛑 기존 안내를 중단합니다.");
    } else {
        ESP_LOGI(TAG, "🏁 목적지 부근에 도착했습니다. 안내를 종료합니다.");
        usb_send_signal(SIGNAL_ARRIVED);
    }
    navigation_task_handle = NULL; // 자기 자신을 지우기 전에 핸들부터 정리
    vTaskDelete(NULL);
}


/* Wi-Fi 및 TMAP API */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS, }, };
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wifi_config); esp_wifi_start();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

typedef struct { char *buffer; int len; bool truncated; } response_data_t;
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    response_data_t *resp = (response_data_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && resp->buffer) {
        if (resp->len + evt->data_len < HTTP_RESP_BUF_SIZE) {
            memcpy(resp->buffer + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buffer[resp->len] = '\0';
        } else {
            // 잘린 데이터는 JSON 파싱을 실패시킨다. 원인을 알 수 있게 표시해둔다.
            resp->truncated = true;
        }
    }
    return ESP_OK;
}

/* 경로 요청과 파싱을 나눈 건 재탐색 실패 시 기존 경로를 지키기 위해서다.
 * 응답을 먼저 받아 확인하고, 쓸 만한 경로일 때만 route_points/route_line을 갈아끼운다.
 */

// HTTP 요청만 수행. 성공 시 malloc된 응답 본문(호출자가 free), 실패 시 NULL.
static char *tmap_request_route_body(double dest_lat, double dest_lon) {
    char *local_response_buffer = (char *)malloc(HTTP_RESP_BUF_SIZE);
    if (!local_response_buffer) return NULL;
    memset(local_response_buffer, 0, HTTP_RESP_BUF_SIZE);
    response_data_t resp = { .buffer = local_response_buffer, .len = 0, .truncated = false };

    esp_http_client_config_t config = {
        .url = "https://apis.openapi.sk.com/tmap/routes/pedestrian?version=1&format=json",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "appKey", TMAP_APP_KEY);

    double start_lat, start_lon;
    gps_snapshot(&start_lat, &start_lon, NULL);
    ESP_LOGI(TAG, "📍 출발지 위치 기준: %s (위도: %f, 경도: %f)",
             current_position_source(), start_lat, start_lon);

    char post_data[256];
    snprintf(post_data, sizeof(post_data),
        "{\"startX\":\"%f\",\"startY\":\"%f\",\"endX\":\"%f\",\"endY\":\"%f\",\"startName\":\"출발지\",\"endName\":\"목적지\",\"reqCoordType\":\"WGS84GEO\",\"resCoordType\":\"WGS84GEO\"}",
        start_lon, start_lat, dest_lon, dest_lat);

    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        char safe_body[256];
        sanitize_for_log(local_response_buffer, safe_body, sizeof(safe_body));
        ESP_LOGE(TAG, "❌ TMAP 요청 실패! err=%s, status_code=%d, 응답: %s",
                 esp_err_to_name(err), status_code, safe_body);
        free(local_response_buffer);
        return NULL;
    }

    if (resp.truncated) {
        ESP_LOGE(TAG, "❌ TMAP 응답이 수신 버퍼(%d바이트)를 넘겨 잘렸습니다. 경로가 너무 길거나 HTTP_RESP_BUF_SIZE를 키워야 합니다.",
                 HTTP_RESP_BUF_SIZE);
        free(local_response_buffer);
        return NULL;
    }

    return local_response_buffer;
}

// 응답 본문을 파싱해 route_points/route_line/waypoint_line_idx를 채우고 안내지점 수를 반환.
// 0을 반환할 때는 기존 경로 배열을 건드리지 않는다.
static int parse_route_body(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        char safe_body[256];
        sanitize_for_log(body, safe_body, sizeof(safe_body));
        ESP_LOGE(TAG, "❌ TMAP 응답 JSON 파싱 실패! 원본 응답: %s", safe_body);
        return 0;
    }

    cJSON *features = cJSON_GetObjectItem(root, "features");
    int array_size = cJSON_IsArray(features) ? cJSON_GetArraySize(features) : 0;

    // 쓸 만한 안내지점이 하나라도 있는지 먼저 본다.
    // 여기서 걸러야 재탐색이 빈 응답을 받아도 기존 경로가 날아가지 않는다.
    int usable_points = 0;
    for (int i = 0; i < array_size; i++) {
        cJSON *geometry = cJSON_GetObjectItem(cJSON_GetArrayItem(features, i), "geometry");
        cJSON *type = geometry ? cJSON_GetObjectItem(geometry, "type") : NULL;
        if (type && cJSON_IsString(type) && strcmp(type->valuestring, "Point") == 0) usable_points++;
    }
    if (usable_points == 0) {
        ESP_LOGE(TAG, "❌ TMAP 응답에 안내 지점이 없습니다. (features %d개)", array_size);
        cJSON_Delete(root);
        return 0;
    }

    // 여기서부터 성공이 확정이므로 기존 경로를 갈아끼운다.
    total_waypoints = 0;
    route_line_count = 0;
    current_target_idx = 0;
    bool point_overflow = false, line_overflow = false;

    for (int i = 0; i < array_size; i++) {
        cJSON *feature = cJSON_GetArrayItem(features, i);
        cJSON *geometry = cJSON_GetObjectItem(feature, "geometry");
        cJSON *properties = cJSON_GetObjectItem(feature, "properties");
        if (!geometry) continue;

        cJSON *type = cJSON_GetObjectItem(geometry, "type");
        if (!type || !cJSON_IsString(type)) continue;
        const char *geo_type = type->valuestring;
        cJSON *coords = cJSON_GetObjectItem(geometry, "coordinates");
        if (!coords) continue;

        if (properties && strcmp(geo_type, "Point") == 0) {
            if (cJSON_GetArraySize(coords) != 2) continue;
            if (total_waypoints >= MAX_WAYPOINTS) { point_overflow = true; continue; }

            // facilityType/facilityName은 Point가 아니라 이어지는 LineString의 속성이다.
            // 여기선 비워두고 아래 LineString 분기에서 직전 지점으로 거슬러 올라가 채운다.
            Waypoint_t *wp = &route_points[total_waypoints];
            wp->lon = cJSON_GetArrayItem(coords, 0)->valuedouble;
            wp->lat = cJSON_GetArrayItem(coords, 1)->valuedouble;
            cJSON *turn = cJSON_GetObjectItem(properties, "turnType");
            wp->turnType = turn ? turn->valueint : 0;
            wp->facilityType = 0;
            wp->facilityName[0] = '\0';
            wp->description[0] = '\0';
            cJSON *desc = cJSON_GetObjectItem(properties, "description");
            if (desc && desc->valuestring) {
                strncpy(wp->description, desc->valuestring, sizeof(wp->description) - 1);
                wp->description[sizeof(wp->description) - 1] = '\0';
            }
            total_waypoints++;
        } else if (strcmp(geo_type, "LineString") == 0) {
            // 실제 보행로 형상. 이탈 판정의 유일한 기준선이다.
            int n = cJSON_GetArraySize(coords);
            for (int j = 0; j < n; j++) {
                cJSON *pair = cJSON_GetArrayItem(coords, j);
                if (cJSON_GetArraySize(pair) != 2) continue;
                if (route_line_count >= MAX_ROUTE_LINE_POINTS) { line_overflow = true; break; }
                route_line[route_line_count].lon = cJSON_GetArrayItem(pair, 0)->valuedouble;
                route_line[route_line_count].lat = cJSON_GetArrayItem(pair, 1)->valuedouble;
                route_line_count++;
            }

            // 응답은 Point -> LineString -> Point 순으로 교차되고, 각 LineString은
            // 직전 Point에서 출발하는 구간이다. 그 구간의 시설물을 직전 지점에 붙여둬야
            // 사용자가 그 구간을 걷기 직전에 미리 경고할 수 있다.
            if (properties && total_waypoints > 0) {
                Waypoint_t *prev = &route_points[total_waypoints - 1];
                cJSON *ftype = cJSON_GetObjectItem(properties, "facilityType");
                cJSON *fname = cJSON_GetObjectItem(properties, "facilityName");
                if (ftype) prev->facilityType = ftype->valueint;
                if (fname && fname->valuestring) {
                    strncpy(prev->facilityName, fname->valuestring, sizeof(prev->facilityName) - 1);
                    prev->facilityName[sizeof(prev->facilityName) - 1] = '\0';
                }
            }
        }
    }

    cJSON_Delete(root);

    if (point_overflow) ESP_LOGW(TAG, "⚠️ 안내 지점이 MAX_WAYPOINTS(%d)를 넘어 잘렸습니다.", MAX_WAYPOINTS);
    if (line_overflow) ESP_LOGW(TAG, "⚠️ 경로선 정점이 MAX_ROUTE_LINE_POINTS(%d)를 넘어 잘렸습니다. 경로 뒷부분의 이탈 판정이 부정확해집니다.", MAX_ROUTE_LINE_POINTS);

    build_waypoint_line_index();

    ESP_LOGI(TAG, "✅ 경로 저장 완료! (안내 지점 %d개, 경로선 정점 %d개)", total_waypoints, route_line_count);
    if (route_line_count < 2) {
        ESP_LOGW(TAG, "⚠️ 경로선을 얻지 못해 경로 이탈 감지/재탐색이 비활성화됩니다.");
    }
    // 시설물 구간을 미리 찍어둔다. is_stairs/is_crosswalk가 facilityName 문자열로
    // 판정하므로 TMAP이 어떤 문자열을 보내는지 확인할 수 있어야 한다.
    for (int i = 0; i < total_waypoints; i++) {
        if (route_points[i].facilityName[0] != '\0') {
            ESP_LOGI(TAG, "🏗️ [시설물] 지점 %d '%s' 다음 구간: %s (facilityType=%d)%s%s",
                     i, route_points[i].description, route_points[i].facilityName,
                     route_points[i].facilityType,
                     is_stairs(&route_points[i]) ? " -> 계단 판정" : "",
                     is_crosswalk(&route_points[i]) ? " -> 횡단보도 판정" : "");
        }
    }
    return total_waypoints;
}

void fetch_tmap_pedestrian_route(double dest_lat, double dest_lon) {
    char *body = tmap_request_route_body(dest_lat, dest_lon);
    if (!body) return;
    parse_route_body(body);
    free(body);
}

// 현재 위치를 출발지로 같은 목적지까지 경로를 다시 받는다.
// 실패하면 기존 경로를 유지한 채 false를 반환한다.
static bool try_reroute(void) {
    if (current_dest_idx < 0 || current_dest_idx >= (int)DESTINATION_COUNT) return false;
    Destination_t dest = DESTINATIONS[current_dest_idx];

    ESP_LOGW(TAG, "🔄 [재탐색] 현재 위치에서 %s 까지 경로를 다시 계산합니다...", dest.name);
    usb_send_signal(SIGNAL_REROUTING);

    char *body = tmap_request_route_body(dest.lat, dest.lon);
    if (!body) {
        ESP_LOGE(TAG, "❌ [재탐색 실패] 기존 경로를 유지한 채 잠시 후 다시 시도합니다.");
        usb_send_signal(SIGNAL_REROUTE_FAILED);
        return false;
    }

    int n = parse_route_body(body); // 0이면 기존 경로는 그대로 남는다
    free(body);
    if (n == 0) {
        ESP_LOGE(TAG, "❌ [재탐색 실패] 기존 경로를 유지한 채 잠시 후 다시 시도합니다.");
        usb_send_signal(SIGNAL_REROUTE_FAILED);
        return false;
    }

    ESP_LOGI(TAG, "✅ [재탐색 완료] 새 경로 %d개 지점으로 안내를 이어갑니다.", n);
    ESP_LOGI(TAG, "👉 [초기 방향] %s", route_points[0].description);
    usb_send_signal(SIGNAL_REROUTE_DONE);
    return true;
}

// 라즈베리파이가 고른 목적지로 안내를 (재)시작
void start_navigation_to_destination(int dest_idx) {
    if (dest_idx < 0 || dest_idx >= (int)DESTINATION_COUNT) return;

    Destination_t dest = DESTINATIONS[dest_idx];
    if (dest.lat == 0.0 && dest.lon == 0.0) {
        ESP_LOGE(TAG, "❌ %s 좌표가 아직 설정되지 않았습니다. main.c의 DESTINATIONS 배열을 채워주세요.", dest.name);
        return;
    }

    ESP_LOGI(TAG, "🎯 [목적지 선택] %s (으)로 경로를 요청합니다.", dest.name);

    // 안내 중인 태스크가 있으면 멈추고 새 목적지로 다시 시작한다.
    // 재탐색 중 HTTP/TLS 요청을 vTaskDelete로 끊으면 소켓과 mbedTLS 컨텍스트가 새므로
    // 플래그를 세우고 스스로 빠져나올 때까지 기다린다.
    if (navigation_task_handle != NULL) {
        ESP_LOGI(TAG, "🔁 기존 안내를 중단하고 새 목적지로 다시 안내를 시작합니다.");
        nav_stop_requested = true;
        for (int i = 0; i < 400 && navigation_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(50)); // 최대 20초 대기 (HTTP 타임아웃 15초를 넘김)
        }
        if (navigation_task_handle != NULL) { // 최후 수단
            ESP_LOGW(TAG, "⚠️ 기존 안내 태스크가 응답하지 않아 강제 종료합니다.");
            vTaskDelete(navigation_task_handle);
            navigation_task_handle = NULL;
        }
        nav_stop_requested = false;
    }

    total_waypoints = 0;
    route_line_count = 0;
    current_target_idx = 0;
    current_dest_idx = dest_idx;

    fetch_tmap_pedestrian_route(dest.lat, dest.lon);

    if (total_waypoints > 0) {
        // 재탐색 때 이 태스크가 HTTP + TLS + cJSON을 직접 돌려서 스택 8192가 필요하다
        xTaskCreate(&navigation_task, "navi_task", 8192, NULL, 5, &navigation_task_handle);
    } else {
        ESP_LOGE(TAG, "❌ 경로를 가져오지 못해 안내를 시작할 수 없습니다.");
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_sta();

    // gps_usb_task가 NULL 큐에 쓰지 않도록 큐를 먼저 만든다
    route_cmd_q = xQueueCreate(1, sizeof(int));
    if (route_cmd_q == NULL) {
        ESP_LOGE(TAG, "❌ 목적지 명령 큐 생성 실패");
        return;
    }
    // 경로 요청 전담 태스크. HTTP + TLS + cJSON을 돌려서 스택 8192가 필요하다.
    xTaskCreate(&route_request_task, "route_req_task", 8192, NULL, 5, NULL);

    // GPS/명령 수신. 목적지 신호(0x01/0x02)가 오면 위 큐로 넘긴다.
    // 부팅만으로는 경로를 요청하지 않는다.
    xTaskCreate(&gps_usb_task, "gps_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "📡 키워드스파팅(라파5)의 목적지 선택 신호(0x01=버스정류장, 0x02=학교)를 기다립니다...");
}