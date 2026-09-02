# TinyVision Cane (tinycane)

시각장애인을 위한 스마트 지팡이. ESP32-S3 4대가 각자 센서/AI 추론을 담당하고,
라즈베리파이5가 USB-CDC로 통신을 통합해 대시보드·음성 안내를 제어한다.
낙상 감지 시 Firebase를 통해 보호자 앱으로 실시간 알림을 보낸다.

## 폴더 구조

| 폴더 | 보드/역할 | 핵심 파일 |
|---|---|---|
| `ESP32(객체인식)/` | 카메라 AI 보드 | `main.cpp` — 장애물·점자블록·버스OCR 3종 TFLite Micro 추론 |
| `ESP32(TOF,DRV)/` | 장애물 거리 감지 보드 | `TOF_task.cc`(VL53L5CX 8×8 ToF) / `DRV.cc`(진동모터 구동) / `I2C_SET.cc` |
| `ESP32(길안내)/` | 길안내 보드 | `main.c` — GPS 좌표 기반 TMAP 턴바이턴 안내 |
| `ESP32(낙상감지&KWS)/` | 낙상·음성명령 보드 | `Fall_detection.cc`(IMU 낙상감지) / `keyword_spotting.cc`(음성 명령) / `fall_alert_sender.cc` |
| `라즈베리파이(대시보드)/` | 통합 게이트웨이 | `dashboard.py`(PySide6 GUI) + 보드별 `*_link.py`(시리얼 통신) |
| `APP(보호자용)/` | 보호자 모바일 앱 | Flutter/Dart, `alert_messaging_service.dart`(FCM+Firestore 알림 수신) |

## 통신 구조

4개 ESP32-S3 보드 → USB-CDC(각 보드 `usb_cdc_comm.*`) → 라즈베리파이5(`dashboard.py`가
`*_link.py` 모듈로 각 보드 시리얼 수신) → 통합 판단 → 음성/진동 출력 지시.
낙상 감지 시에만 `fall_alert_sender.cc` → 라즈베리파이 경유 → Firebase(FCM/Firestore)
→ 보호자 앱으로 푸시 알림 + GPS 위치 전달.

## 빌드

각 `ESP32(...)/` 폴더는 독립적인 ESP-IDF 프로젝트다. 보드별로 진입해서:
```
idf.py build flash monitor -p COM포트번호
```

`라즈베리파이(대시보드)/`는 Python 3 + PySide6 필요 (`pip install pyside6 pyserial`).

`APP(보호자용)/app/`는 Flutter 프로젝트 (`flutter pub get` 후 `flutter run`).
