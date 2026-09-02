import threading

import serial
from PySide6.QtCore import QObject, Signal

DEFAULT_SERIAL_PORT = "/dev/guide"
DEFAULT_SERIAL_BAUD = 921600  # 다른 ESP32-S3 보드(ircamera/keyword)와 동일하게 가정

# ---- 보드 -> 라즈베리파이 ----
EVT_SHORT_PRESS = 0x01
EVT_LONG_PRESS = 0x02
EVT_EVENT = 0xAA

# 길안내 경로 이벤트 + 도착(2026-08-26 확인 - smartcane 저장소 OHT 브랜치 main/main.c 기준, 실제
# 펌웨어 값). 위 EVT_SHORT_PRESS/EVT_LONG_PRESS/EVT_EVENT(짧게/길게 누름, 0xAA)는 아직
# "임시 프로토콜"이라 실기판과 안 맞지만, 이 다섯 개는 main.c의 SIGNAL_* 값과 확인됐으므로
# 우선 이것만 실제 값으로 맞춰둔다. 도착은 원래 여기 EVT_ARRIVED=0xAB로 있었는데, 실제
# 보드는 0xAB가 아니라 SIGNAL_ARRIVED(0xFF)를 보내서 도착 시 speaker 릴레이가 안 됐음 - 실측으로
# 확인 후 수정(2026-08-26).
SIGNAL_TURN_LEFT = 0x03
SIGNAL_TURN_RIGHT = 0x04
SIGNAL_CROSSWALK = 0x05
SIGNAL_STAIRS = 0x06
SIGNAL_ARRIVED = 0xFF

# ---- 라즈베리파이 -> 보드 ----
# 숫자값이 EVT_SHORT_PRESS/EVT_LONG_PRESS와 겹치지만 방향(수신 vs 송신)이 달라 실제 충돌은 없다.
# 이름을 별도로 둔 건 로그/디버깅할 때 헷갈리지 않게 하기 위함.
CMD_DEST_BUS_STOP = 0x01
CMD_DEST_SCHOOL = 0x02
# GPS는 커맨드 바이트 없이 평문 줄로 보낸다("GPS,<lat>,<lon>\n") - _write_gps() 참고.

GPS_UPDATE_INTERVAL_S = 1.0


class GuideLink(QObject):
    serial_connected = Signal(bool)
    short_press = Signal()     # 짧게 눌렀다 뗌 - 목적지 순환
    long_press = Signal()      # 2초 이상 길게 누름 확정 - 길안내 시작
    event_occurred = Signal()  # 경로 중 이벤트 발생 - ircamera 스피커로 릴레이용
    arrived = Signal()         # 목적지 도착
    turn_left = Signal()       # 좌회전 안내
    turn_right = Signal()      # 우회전 안내
    crosswalk = Signal()       # 횡단보도 안내
    stairs = Signal()          # 계단 안내

    def __init__(self, serial_port=DEFAULT_SERIAL_PORT, serial_baud=DEFAULT_SERIAL_BAUD, parent=None):
        super().__init__(parent)
        self.serial_port = serial_port
        self.serial_baud = serial_baud

        self._ser = None
        self._ser_lock = threading.Lock()
        self._stop = threading.Event()

        self._gps_timer = None
        self._last_lat = None
        self._last_lon = None

    # ---------------- 시작/종료 ----------------
    def start(self):
        threading.Thread(target=self._serial_loop, daemon=True).start()

    def stop(self):
        self._stop.set()
        self.stop_navigation()

    # ---------------- 시리얼 수신 루프 ----------------
    def _serial_loop(self):
        while not self._stop.is_set():
            try:
                with self._ser_lock:
                    # write_timeout을 안 주면 GPS 주기 전송 중 보드가 안 읽어갈 때 무한 블로킹될 수 있다.
                    self._ser = serial.Serial(self.serial_port, self.serial_baud, timeout=1, write_timeout=1)
                self.serial_connected.emit(True)
                print(f"[guide] 시리얼 연결됨: {self.serial_port}")
                try:
                    self._read_loop(self._ser)
                finally:
                    with self._ser_lock:
                        if self._ser:
                            self._ser.close()
                        self._ser = None
            except Exception as e:
                print(f"[guide] 시리얼 연결 실패/끊김: {e}")
                self.serial_connected.emit(False)
                self._stop.wait(2.0)

    def _read_loop(self, ser):
        while not self._stop.is_set():
            head = ser.read(1)
            if not head:
                continue
            evt = head[0]
            if evt == EVT_SHORT_PRESS:
                self.short_press.emit()
            elif evt == EVT_LONG_PRESS:
                self.long_press.emit()
            elif evt == EVT_EVENT:
                self.event_occurred.emit()
            elif evt == SIGNAL_ARRIVED:
                self.stop_navigation()
                self.arrived.emit()
            elif evt == SIGNAL_TURN_LEFT:
                self.turn_left.emit()
            elif evt == SIGNAL_TURN_RIGHT:
                self.turn_right.emit()
            elif evt == SIGNAL_CROSSWALK:
                self.crosswalk.emit()
            elif evt == SIGNAL_STAIRS:
                self.stairs.emit()
            else:
                # 알려진 이벤트 바이트가 아니면 ESP_LOGI 콘솔 로그 줄이다(ircamera_link.py의
                # _handle_log_line과 동일한 이유 - 같은 USB-CDC 회선을 콘솔 로그랑 공유함).
                # 개행까지 읽어서 콘솔에만 출력한다.
                self._handle_log_line(ser, head)

    def _handle_log_line(self, ser, first_byte):
        line = bytearray(first_byte)
        while not self._stop.is_set():
            b = ser.read(1)
            if not b:
                break
            line += b
            if b == b"\n":
                break
        text = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if text:
            print(f"[guide:fw] {text}")

    # ---------------- 목적지 확정 + GPS 위치 스트리밍 ----------------
    def update_position(self, lat: float, lon: float):
        """GPSLink에서 새 위치가 들어올 때마다 호출 - 다음 1초 tick에 이 값을 보낸다."""
        self._last_lat = lat
        self._last_lon = lon

    def start_navigation(self, dest_code: int):
        """목적지 확정 바이트(CMD_DEST_BUS_STOP/CMD_DEST_SCHOOL)를 1회 보내고,
        그 뒤로 GPS_UPDATE_INTERVAL_S 주기로 현재 위치 전송을 시작한다."""
        threading.Thread(target=self._write_byte, args=(dest_code,), daemon=True).start()
        self._arm_gps_timer()

    def stop_navigation(self):
        if self._gps_timer:
            self._gps_timer.cancel()
            self._gps_timer = None

    def _arm_gps_timer(self):
        if self._gps_timer:
            self._gps_timer.cancel()
        self._gps_timer = threading.Timer(GPS_UPDATE_INTERVAL_S, self._send_gps_tick)
        self._gps_timer.daemon = True
        self._gps_timer.start()

    def _send_gps_tick(self):
        if self._last_lat is not None and self._last_lon is not None:
            self._write_gps(self._last_lat, self._last_lon)
        self._arm_gps_timer()  # threading.Timer는 1회성이라 매 tick마다 다시 건다

    def _write_byte(self, value: int):
        with self._ser_lock:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.write(bytes([value]))
                except Exception as e:
                    print(f"[guide] 전송 실패: {e}")
            else:
                print("[guide] 시리얼 연결 안 됨, 전송 스킵")

    def _write_gps(self, lat: float, lon: float):
        line = f"GPS,{lat:.6f},{lon:.6f}\n".encode("ascii")
        with self._ser_lock:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.write(line)
                except Exception as e:
                    print(f"[guide] GPS 전송 실패: {e}")
            # 연결 안 된 상태는 매초 로그가 시끄러우니 조용히 스킵(재연결되면 다음 tick부터 다시 됨)
