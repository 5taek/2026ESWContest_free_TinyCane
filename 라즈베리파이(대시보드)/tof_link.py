import struct
import threading

import serial
from PySide6.QtCore import QObject, Signal

DEFAULT_SERIAL_PORT = "/dev/tof"  # udev 심볼릭 링크(/etc/udev/rules.d/99-sew-boards.rules) - ttyACM 번호는 꽂는 순서에 따라 바뀌므로 시리얼 번호로 고정
DEFAULT_SERIAL_BAUD = 921600  # USB CDC 네이티브 방식이라 사실상 무시됨(ircamera와 동일)

PKT_RAW_GRID = 0xB2
PKT_HAPTIC_FIRED = 0xB3

GRID_ROWS = 8
GRID_COLS = 8
GRID_CELLS = GRID_ROWS * GRID_COLS  # 64

# msg_type 1B는 헤더에서 먼저 읽으므로 페이로드 포맷에서 제외.
RAW_GRID_PAYLOAD_FMT = f"<{GRID_CELLS}h"
RAW_GRID_PAYLOAD_SIZE = struct.calcsize(RAW_GRID_PAYLOAD_FMT)  # 128

HAPTIC_FIRED_PAYLOAD_FMT = "<BBh"  # height(1B) + direction(1B) + distance_mm(int16 LE)
HAPTIC_FIRED_PAYLOAD_SIZE = struct.calcsize(HAPTIC_FIRED_PAYLOAD_FMT)  # 4


class TofLink(QObject):
    # distances: 길이 64 리스트(row-major, row*GRID_COLS+col), 무효 칸은 None
    grid_received = Signal(list)
    # height(0~2), direction(0~2), distance_mm
    haptic_fired = Signal(int, int, int)
    serial_connected = Signal(bool)

    def __init__(self, serial_port=DEFAULT_SERIAL_PORT, serial_baud=DEFAULT_SERIAL_BAUD, parent=None):
        super().__init__(parent)
        self.serial_port = serial_port
        self.serial_baud = serial_baud
        self._stop = threading.Event()

    def start(self):
        threading.Thread(target=self._serial_loop, daemon=True).start()

    def stop(self):
        self._stop.set()

    def _serial_loop(self):
        while not self._stop.is_set():
            try:
                ser = serial.Serial(self.serial_port, self.serial_baud, timeout=1)
                self.serial_connected.emit(True)
                print(f"[tof] 시리얼 연결됨: {self.serial_port}")
                self._read_packets(ser)
            except Exception as e:
                print(f"[tof] 시리얼 연결 실패: {e}")
                self.serial_connected.emit(False)
                self._stop.wait(2.0)

    def _read_packets(self, ser):
        while not self._stop.is_set():
            head = ser.read(1)
            if not head:
                continue
            msg_type = head[0]
            try:
                if msg_type == PKT_RAW_GRID:
                    self._handle_raw_grid(ser)
                elif msg_type == PKT_HAPTIC_FIRED:
                    self._handle_haptic_fired(ser)
                else:
                    continue  # 동기 어긋난 바이트는 버림 (RPI_COMM.md 권장 방식)
            except TimeoutError as e:
                print(f"[tof] 시리얼 끊김/timeout: {e}")
                self.serial_connected.emit(False)
                return  # 바깥 _serial_loop에서 재연결

    def _handle_raw_grid(self, ser):
        payload = self._read_exact(ser, RAW_GRID_PAYLOAD_SIZE)
        flat = struct.unpack(RAW_GRID_PAYLOAD_FMT, payload)
        distances = [v if v >= 0 else None for v in flat]
        self.grid_received.emit(distances)

    def _handle_haptic_fired(self, ser):
        payload = self._read_exact(ser, HAPTIC_FIRED_PAYLOAD_SIZE)
        height, direction, distance_mm = struct.unpack(HAPTIC_FIRED_PAYLOAD_FMT, payload)
        self.haptic_fired.emit(height, direction, distance_mm)

    def _read_exact(self, ser, n):
        buf = b""
        while len(buf) < n:
            chunk = ser.read(n - len(buf))
            if not chunk:
                raise TimeoutError(f"시리얼 읽기 timeout ({n}바이트 기대, {len(buf)}바이트 받음)")
            buf += chunk
        return buf
