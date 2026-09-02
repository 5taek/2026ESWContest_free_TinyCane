// 자동 생성됨 — data/*.wav를 voice.bin으로 합칠 때 같이 뽑은 오프셋 테이블.
// voice.bin을 "voice" 파티션(subtype 0x40)에 그대로 플래싱한 뒤,
// map_partition("voice", &sz)로 mmap해서 이 offset/len으로 바로 잘라 쓰면 됨.
// 포맷: mono, 16bit, 24000Hz PCM (WAV 헤더 없음, raw sample만) — 지금 스피커
// 설정(SPK_SAMPLE_RATE=24000)과 동일해서 디코딩/리샘플링 없이 바로 재생 가능.
//
// 클립을 추가/교체하면 data/*.wav를 다시 합쳐서 이 파일도 같이 재생성할 것
// (offset이 클립 순서/크기에 의존적이라 수동으로 건드리면 안 됨).

#pragma once
#include <cstdint>

struct VoiceClip {
    const char *key;    // 재생 요청 시 매칭시킬 문자열 (버스번호는 BUS_CANDIDATES와 동일)
    uint32_t offset;    // voice 파티션 시작 기준 바이트 오프셋
    uint32_t len;       // 바이트 길이
};

static const VoiceClip VOICE_CLIPS[] = {
    {"937",              0,        84480},
    {"503",              84480,    84480},
    {"410",              168960,   78720},
    {"410-1",            247680,   101760},
    {"북구2",              349440,   72960},
    {"장애물모드전환",          422400,   94080},
    {"버스탐지모드전환",         516480,   99840},
    {"점자블록모드전환",         616320,   105600},
    {"도착버스없음",           721920,   149760},  // "1분 이내에 도착하는 버스가 없습니다"
    {"자전거",              871680,   53760},  // 장애물종류 안내 (object_code 0x01)
    {"킥보드",              925440,   55680},  // object_code 0x02
    {"볼라드",              981120,   55680},  // object_code 0x03
    {"사람",               1036800,  48000},  // object_code 0x04
    {"길안내모드전환",          1084800,  105600},  // CMD_DESTINATION(0x03) payload 0x00
    {"학교",               1190400,  55680},  // payload 0x01
    {"버스정류장",            1246080,  61440},  // payload 0x02
    {"버스정류장으로길안내를시작합니다", 1307520,  140160},  // payload 0x03
    {"목적지에도착했습니다",       1447680,  96000},  // CMD_ARRIVED(0x05)
    {"긴급상황발생119신고요청",    1543680,  124800},  // CMD_EMERGENCY(0x04)
    {"좌회전",              1668480,  57600},  // CMD_TURN_LEFT(0x06)
    {"우회전",              1726080,  59520},  // CMD_TURN_RIGHT(0x07)
    {"계단",               1785600,  46080},  // CMD_STAIRS(0x09) — 0x08(횡단보도)는 wav 없어서 미포함
    {"정면에점자블록",          1831680,  117120},  // 점자블록 근접확정(dir="center")
    {"왼쪽에점자블록",          1948800,  111360},  // dir="left"
    {"오른쪽에점자블록",         2060160,  111360},  // dir="right"
    {"낙상감지",             2171520,  65280},  // CMD_FALL_ALERT_START(0x0A) 무한반복, CMD_FALL_ALERT_STOP(0x0B)로 정지
};
static const int VOICE_CLIPS_COUNT = 26;

// 전체 voice.bin 크기 (참고용, 파티션 크기 정할 때 여유 계산에 씀)
static const uint32_t VOICE_BIN_TOTAL_BYTES = 2236800;  // voice 파티션 2304K
