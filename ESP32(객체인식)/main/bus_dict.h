// 버스 노선번호 전용 글자 사전 (CTC blank 포함)
#pragma once

static const int KOREAN_DICT_SIZE = 31;

static const char* const KOREAN_DICT_TABLE[] = {
    "",  // idx 0: blank
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "직",
    "행",
    "급",
    "순",
    "환",
    "남",
    "구",
    "동",
    "북",
    "서",
    "성",
    "수",
    "달",
    "칠",
    "곡",
    "가",
    "창",
    "팔",
    "공",
    "-",
};

inline const char* idx2char(int idx) {
    if (idx < 0 || idx >= KOREAN_DICT_SIZE) return nullptr;
    return KOREAN_DICT_TABLE[idx];
}
