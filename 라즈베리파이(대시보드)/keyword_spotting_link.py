import os
import queue
import subprocess
import signal as sys_signal
import threading
import time
import wave

import numpy as np
import serial
from scipy.signal import resample_poly
from PySide6.QtCore import QObject, Signal

DEFAULT_SERIAL_PORT = "/dev/keyword"  # udev 심볼릭 링크(/etc/udev/rules.d/99-sew-boards.rules) - ttyACM 번호는 꽂는 순서에 따라 바뀌므로 시리얼 번호로 고정
DEFAULT_SERIAL_BAUD = 921600
CHUNK_SIZE = 512
BUTTON_PIN = 17
BUTTON_BOUNCE_S = 0.05  # 기계식 버튼 접점 바운스로 when_pressed가 중복 호출되는 것 방지
TEMP_WAV = "/home/cane/sew/dashboard_recorded.wav"
BT_TARGET = "111"  # pw-record 타깃(블루투스 마이크 노드 ID, 승우의 Buds3 Pro).
                    # PipeWire 노드 ID라 재부팅/재페어링하면 바뀔 수 있음 - 안 되면 `wpctl status`의 Sources 항목에서 재확인.

ESP_SAMPLE_COUNT = 32000  # 1초 * 16000Hz * 2바이트
RESULT_TIMEOUT_S = 10
WRITE_TIMEOUT_S = 2  # 보드가 오디오 바이트를 안 읽어가면(예: IMU 스트리밍에 물려 있음) 무한 블로킹 대신 여기서 끊음
GUIDE_LONG_PRESS_THRESHOLD_S = 2.0  # guide_link.py의 EVT_LONG_PRESS 판정 기준(2초)과 동일하게 맞춤
READ_TIMEOUT_S = 0.2  # 배경 스레드가 os.path.exists로 포트 뽑힘을 주기적으로 확인할 수 있게 짧게 잡음

# guide_link.py와 같은 평문 줄 포맷("GPS,<lat>,<lon>\n")을 그대로 쓴다 - 보드가 낙상감지
# 알림을 보낼 때 최근 위치를 같이 실어 보낼 수 있게 준비하는 용도. GPS_UPDATE_INTERVAL_S만
# 10초로 다르게 잡음(guide_link.py는 길안내 중 1초 - 이쪽은 상시 백그라운드라 더 여유있게).
GPS_UPDATE_INTERVAL_S = 10.0

# smartcane CHS 브랜치 Fall_detection.cc의 kFallDetectedByte/kFallResolvedByte/kFallDangerByte와 동일.
# 유효 ASCII/UTF-8 범위 밖의 값이라 텍스트 줄(RESULT:...)과 섞여도 구분됨.
FALL_DETECTED_BYTE = 0xFF
FALL_RESOLVED_BYTE = 0xFE
FALL_DANGER_BYTE = 0xFD


class KeywordSpottingLink(QObject):
    listening_started = Signal()       # 버튼 누름 - 녹음 시작
    listening_stopped = Signal()       # 버튼 뗌 - 녹음 종료, 전송 + 인식 대기 시작
    keyword_recognized = Signal(object)  # "bus_ocr" | "braille" | "guide" | "emergency" | None(인식 실패/timeout)
    fall_event = Signal(str)  # "detected" | "resolved" | "danger"
    serial_connected = Signal(bool)
    button_available = Signal(bool)
    guide_short_press = Signal()  # 길안내 모드 중 GPIO17 짧게 누름 - 목적지 순환
    guide_long_press = Signal()   # 길안내 모드 중 GPIO17 길게(2초+) 누름 - 목적지 확정

    def __init__(self, serial_port=DEFAULT_SERIAL_PORT, serial_baud=DEFAULT_SERIAL_BAUD,
                 button_pin=BUTTON_PIN, parent=None):
        super().__init__(parent)
        self.serial_port = serial_port
        self.serial_baud = serial_baud
        self.button_pin = button_pin

        self._ser = None
        self._write_lock = threading.Lock()  # 오디오 청크 전송과 GPS tick이 같은 시리얼에 동시에 안 쓰이게
        self._button = None
        self._stop = threading.Event()
        self._result_lines = queue.Queue()

        self._gps_timer = None
        self._last_lat = None
        self._last_lon = None

        # 길안내 모드일 때는 GPIO17을 오디오 키워드스파팅 대신 목적지 선택용으로 재사용한다.
        # dashboard.py가 "지금 길안내 모드인가"를 알려주는 콜백을 주입해서 씀(상태 이중관리 방지).
        self.guide_mode_check = None

    # ---------------- 시작 ----------------
    def start(self):
        threading.Thread(target=self._serial_loop, daemon=True).start()
        self._init_button()
        self._arm_gps_timer()

    def stop(self):
        self._stop.set()
        if self._gps_timer:
            self._gps_timer.cancel()
            self._gps_timer = None

    # ---------------- GPS 위치 전송 (dashboard.py의 GPSLink가 새 fix를 줄 때마다 호출) ----------------
    def set_gps_fix(self, lat: float, lon: float):
        self._last_lat = lat
        self._last_lon = lon

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

    def _write_gps(self, lat: float, lon: float):
        line = f"GPS,{lat:.6f},{lon:.6f}\n".encode("ascii")
        with self._write_lock:
            if self._ser and self._ser.is_open:
                try:
                    self._ser.write(line)
                except Exception as e:
                    print(f"[keyword] GPS 전송 실패: {e}")
            # 연결 안 된 상태는 10초마다 로그가 시끄러우니 조용히 스킵(재연결되면 다음 tick부터 다시 됨)

    def _init_button(self):
        try:
            from gpiozero import Button
            self._button = Button(self.button_pin, pull_up=True, bounce_time=BUTTON_BOUNCE_S)
            self._button.when_pressed = self._on_press
            self.button_available.emit(True)
            print(f"[keyword] GPIO{self.button_pin} 버튼 초기화 완료")
        except Exception as e:
            print(f"[keyword] GPIO 초기화 실패: {e}")
            self.button_available.emit(False)

    # ---------------- 시리얼 연결 유지 + 수신 바이트 상시 감시 ----------------
    # 오디오 전송(_send_and_get_result)은 쓰기만 하고, 읽기는 이 스레드가 전담한다
    # (같은 포트를 두 스레드가 동시에 read()하면 서로 바이트를 가로챌 수 있어서 분리함).
    def _serial_loop(self):
        while not self._stop.is_set():
            try:
                self._ser = serial.Serial(self.serial_port, self.serial_baud, timeout=READ_TIMEOUT_S,
                                           write_timeout=WRITE_TIMEOUT_S)
                self._ser.reset_input_buffer()
                self.serial_connected.emit(True)
                print(f"[keyword] 시리얼 연결됨: {self.serial_port}")
                while not self._result_lines.empty():
                    try:
                        self._result_lines.get_nowait()
                    except queue.Empty:
                        break

                line_buf = bytearray()
                while not self._stop.is_set() and self._ser and self._ser.is_open:
                    try:
                        data = self._ser.read(1)
                    except (serial.SerialException, OSError) as e:
                        print(f"[keyword] 시리얼 읽기 오류: {e}")
                        break
                    if not data:
                        # read()가 READ_TIMEOUT_S 안에 빈 결과를 주면 그 틈에 포트 뽑힘을 확인
                        if not os.path.exists(self.serial_port):
                            print(f"[keyword] 시리얼 끊김 감지: {self.serial_port}")
                            break
                        continue
                    line_buf = self._handle_incoming_byte(data[0], line_buf)
                else:
                    continue
                self.serial_connected.emit(False)
                try:
                    self._ser.close()
                except Exception:
                    pass
                self._stop.wait(2.0)
            except Exception as e:
                print(f"[keyword] 시리얼 연결 실패: {e}")
                self.serial_connected.emit(False)
                self._stop.wait(2.0)

    def _handle_incoming_byte(self, byte_val, line_buf):
        if byte_val == FALL_DETECTED_BYTE:
            self.fall_event.emit("detected")
        elif byte_val == FALL_RESOLVED_BYTE:
            self.fall_event.emit("resolved")
        elif byte_val == FALL_DANGER_BYTE:
            self.fall_event.emit("danger")
        elif byte_val == 0x0A:  # '\n' - KWS 텍스트 줄 완성
            if line_buf:
                line = bytes(line_buf).decode("utf-8", errors="ignore").strip()
                if line:
                    self._result_lines.put(line)
            line_buf = bytearray()
        else:
            line_buf.append(byte_val)
        return line_buf

    # ---------------- 버튼 콜백 (gpiozero 내부 스레드에서 호출됨) ----------------
    def _on_press(self):
        if self.guide_mode_check and self.guide_mode_check():
            # 길안내 모드 중엔 오디오 녹음/인식을 아예 타지 않는다 - 순전히 누름 길이만 본다.
            threading.Thread(target=self._handle_guide_press, daemon=True).start()
            return
        self.listening_started.emit()
        threading.Thread(target=self._record_and_process, daemon=True).start()

    def _handle_guide_press(self):
        pressed_at = time.time()
        while self._button and self._button.is_pressed:
            time.sleep(0.01)
        held_s = time.time() - pressed_at
        if held_s >= GUIDE_LONG_PRESS_THRESHOLD_S:
            self.guide_long_press.emit()
        else:
            self.guide_short_press.emit()

    def _record_and_process(self):
        try:
            self._record_while_held()
            self.listening_stopped.emit()
            result = self._send_and_get_result()
            self.keyword_recognized.emit(result)
        except Exception as e:
            print(f"[keyword] 처리 오류: {e}")
            self.keyword_recognized.emit(None)

    def _record_while_held(self):
        proc = subprocess.Popen(
            ["pw-record", f"--target={BT_TARGET}", "--rate=48000", "--channels=1", TEMP_WAV],
            stderr=subprocess.DEVNULL,
        )
        while self._button and self._button.is_pressed:
            time.sleep(0.01)
        proc.send_signal(sys_signal.SIGINT)
        proc.wait()

    def _send_and_get_result(self):
        if self._ser is None or not self._ser.is_open:
            print("[keyword] 시리얼 연결 없음, 전송 스킵")
            return None

        with wave.open(TEMP_WAV, "rb") as wf:
            raw_rate = wf.getframerate()
            raw = np.frombuffer(wf.readframes(wf.getnframes()), dtype=np.int16).astype(np.float32)

        if raw_rate == 48000:
            resampled = resample_poly(raw, 1, 3)
        elif raw_rate == 32000:
            resampled = resample_poly(raw, 1, 2)
        else:
            resampled = raw

        resampled = np.clip(resampled, -32768, 32767).astype(np.int16)
        audio_data = resampled.tobytes()

        if len(audio_data) < ESP_SAMPLE_COUNT:
            print(f"[keyword] 녹음 데이터 부족: {len(audio_data)} bytes (필요: {ESP_SAMPLE_COUNT})")
            return None

        while not self._result_lines.empty():  # 이전 전송에서 남은 줄 비우기
            try:
                self._result_lines.get_nowait()
            except queue.Empty:
                break
        time.sleep(0.1)
        try:
            with self._write_lock:
                for i in range(0, ESP_SAMPLE_COUNT, CHUNK_SIZE):
                    chunk = audio_data[i:i + CHUNK_SIZE]
                    self._ser.write(chunk)
                    time.sleep(0.002)
        except serial.SerialTimeoutException:
            print(f"[keyword] 전송 타임아웃 - 보드가 데이터를 안 읽어감(offset {i}). "
                  "IMU 스트리밍 등에 물려 있을 수 있음")
            return None

        # 실제 읽기는 _serial_loop 배경 스레드가 하고, 여기서는 그 결과를 큐로 받는다.
        result_line = None
        deadline = time.time() + RESULT_TIMEOUT_S
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            try:
                line = self._result_lines.get(timeout=remaining)
            except queue.Empty:
                break
            if "RESULT:" in line:
                result_line = line
                break

        if not result_line or "REJECTED" in result_line:
            return None

        label = result_line.replace("RESULT:", "").split("(")[0].strip().lower()
        if "one" in label:
            return "bus_ocr"
        if "two" in label:
            return "braille"
        if "three" in label:
            return "guide"
        if "zero" in label:
            return "emergency"
        return None
