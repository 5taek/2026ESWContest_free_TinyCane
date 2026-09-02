import math
import threading
import time

import pynmea2
import serial
from PySide6.QtCore import QObject, Signal

DEFAULT_SERIAL_PORT = "/dev/ttyAMA4"
DEFAULT_SERIAL_BAUD = 9600

MIN_NUM_SATS = 4  # 이 미만이면 단독 GPS 오차가 급격히 커짐 - 픽스 자체를 버림
MAX_HDOP = 5.0  # 수평정밀도저하율. 이 값보다 크면 위성 배치가 나빠 오차가 큰 것으로 보고 버림
MAX_SPEED_MPS = 40.0  # 도보/버스 이동을 감안한 여유치(≈144km/h) - 이보다 빠른 "이동"은 GPS 글리치로 보고 이전 값 유지

DEFAULT_HDOP = 2.0  # GGA 문장에 HDOP 필드가 비어있을 때(드묾)만 쓰는 보수적 기본값
BASE_UERE_M = 5.0  # 단독(SPS) GPS의 HDOP=1 기준 대략적 사용자등가거리오차(m) - 흔히 쓰이는 근사치
PROCESS_NOISE_STD_MPS = 1.3  # 칼만 필터 프로세스 노이즈(m/s 표준편차) - 도보 이동 정도의 불확실성을 매 tick 허용

METERS_PER_DEG_LAT = 111320.0  # 위도 1도당 거리(m) - 위도에 거의 무관하게 일정


def _haversine_m(lat1, lon1, lat2, lon2):
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


class _KalmanAxis:
    """1축(동/북 방향, 미터 단위)용 1차원 칼만 필터.

    고정 개수 이동평균과 달리 추정 분산(self.variance)이 매 갱신마다 계속 줄어들어
    정지 상태에서 시간이 지날수록 더 정밀하게 수렴한다. 측정 노이즈(measurement_std_m)를
    HDOP에서 추정해 그대로 게인 계산에 반영하므로, 위성 배치가 나쁜 순간의 픽스는
    자동으로 덜 반영된다.
    """

    def __init__(self, process_noise_std_mps: float):
        self._process_noise_std_mps = process_noise_std_mps
        self.estimate_m = None
        self.variance = None

    def update(self, measurement_m: float, measurement_std_m: float, dt_s: float) -> float:
        measurement_variance = measurement_std_m ** 2
        if self.estimate_m is None:
            self.estimate_m = measurement_m
            self.variance = measurement_variance
            return self.estimate_m

        # 예측: 마지막 갱신 이후 경과 시간만큼 도보 이동 불확실성을 더함
        predicted_variance = self.variance + (self._process_noise_std_mps * max(dt_s, 0.0)) ** 2

        # 갱신: 칼만 게인으로 예측치와 새 측정치를 신뢰도 비율대로 섞음
        gain = predicted_variance / (predicted_variance + measurement_variance)
        self.estimate_m += gain * (measurement_m - self.estimate_m)
        self.variance = (1.0 - gain) * predicted_variance
        return self.estimate_m


class GPSLink(QObject):
    fix_changed = Signal(bool, float, float)  # connected(유효 픽스 여부), 위도, 경도

    def __init__(self, serial_port=DEFAULT_SERIAL_PORT, serial_baud=DEFAULT_SERIAL_BAUD, parent=None):
        super().__init__(parent)
        self.serial_port = serial_port
        self.serial_baud = serial_baud
        self._stop = threading.Event()
        self._connected = False  # 마지막으로 emit한 connected 상태 - 중복 emit(배지 재렌더) 방지용
        self._last_accepted = None  # 이상치 판정 기준이 되는 마지막 채택 픽스(lat, lon, monotonic time)

        # 칼만 필터는 위도/경도를 직접 다루지 않고, 첫 픽스를 원점으로 하는 로컬 평면
        # (동쪽=east_m, 북쪽=north_m)에 투영해서 미터 단위로 필터링한다 - 위도/경도는
        # 축척이 서로 달라서(경도 1도의 실제 거리가 위도에 따라 변함) 그대로 필터링하면
        # 오차 추정이 왜곡된다.
        self._ref_lat = None
        self._ref_lon = None
        self._east_filter = _KalmanAxis(PROCESS_NOISE_STD_MPS)
        self._north_filter = _KalmanAxis(PROCESS_NOISE_STD_MPS)
        self._last_update_t = None

    def start(self):
        threading.Thread(target=self._serial_loop, daemon=True).start()

    def stop(self):
        self._stop.set()

    def _serial_loop(self):
        while not self._stop.is_set():
            try:
                ser = serial.Serial(self.serial_port, self.serial_baud, timeout=1)
                print(f"[gps] 시리얼 연결됨: {self.serial_port}")
                try:
                    self._read_loop(ser)
                finally:
                    ser.close()
            except Exception as e:
                print(f"[gps] 시리얼 연결 실패/끊김: {e}")
                self._emit_disconnected()
                self._stop.wait(2.0)

    def _read_loop(self, ser):
        while not self._stop.is_set():
            raw = ser.readline()
            if not raw:
                continue
            decoded = raw.decode("ascii", errors="replace").strip()
            if not decoded.startswith(("$GNGGA", "$GPGGA")):
                continue
            try:
                msg = pynmea2.parse(decoded)
            except (pynmea2.ParseError, ValueError):
                continue

            qual = getattr(msg, "gps_qual", 0)
            if not (qual and int(qual) > 0 and msg.latitude and msg.longitude):
                self._emit_disconnected()
                continue

            hdop = self._parse_hdop(msg)
            if not self._passes_accuracy_gate(msg, hdop):
                continue  # 픽스는 있지만 정확도가 나쁨 - 이번 문장은 버리고 이전 표시값 유지

            if not self._passes_outlier_gate(msg.latitude, msg.longitude):
                continue  # 직전 위치에서 물리적으로 말이 안 되는 거리 - GPS 글리치로 보고 버림

            lat, lon = self._filtered(msg.latitude, msg.longitude, hdop)
            self._connected = True
            self.fix_changed.emit(True, lat, lon)

    def _parse_hdop(self, msg):
        try:
            return float(msg.horizontal_dil)
        except (TypeError, ValueError):
            return None

    def _passes_accuracy_gate(self, msg, hdop) -> bool:
        try:
            num_sats = int(msg.num_sats)
        except (TypeError, ValueError):
            num_sats = None
        if num_sats is not None and num_sats < MIN_NUM_SATS:
            return False

        if hdop is not None and hdop > MAX_HDOP:
            return False

        return True

    def _passes_outlier_gate(self, lat, lon) -> bool:
        now = time.monotonic()
        if self._last_accepted is None:
            self._last_accepted = (lat, lon, now)
            return True

        last_lat, last_lon, last_t = self._last_accepted
        dt = max(now - last_t, 0.001)
        dist = _haversine_m(last_lat, last_lon, lat, lon)
        if dist / dt > MAX_SPEED_MPS:
            print(f"[gps] 이상치 거부: {dist:.0f}m/{dt:.1f}s (>{MAX_SPEED_MPS}m/s) - 이전 위치 유지")
            return False

        self._last_accepted = (lat, lon, now)
        return True

    def _filtered(self, lat, lon, hdop):
        if self._ref_lat is None:
            self._ref_lat, self._ref_lon = lat, lon

        m_per_deg_lon = METERS_PER_DEG_LAT * math.cos(math.radians(self._ref_lat))
        east_m = (lon - self._ref_lon) * m_per_deg_lon
        north_m = (lat - self._ref_lat) * METERS_PER_DEG_LAT

        measurement_std_m = max(hdop if hdop is not None else DEFAULT_HDOP, 1.0) * BASE_UERE_M
        now = time.monotonic()
        dt_s = 0.0 if self._last_update_t is None else now - self._last_update_t
        self._last_update_t = now

        east_filt = self._east_filter.update(east_m, measurement_std_m, dt_s)
        north_filt = self._north_filter.update(north_m, measurement_std_m, dt_s)

        filt_lat = self._ref_lat + north_filt / METERS_PER_DEG_LAT
        filt_lon = self._ref_lon + east_filt / m_per_deg_lon
        return filt_lat, filt_lon

    def _emit_disconnected(self):
        # 픽스 없는 상태는 매 문장(초당 ~1회)/재연결 시도(2초 간격)마다 반복되므로,
        # 이미 연결 안 됨으로 전환돼 있으면 다시 emit하지 않는다(배지 불필요한 재렌더 방지).
        if not self._connected:
            return
        self._connected = False
        self.fix_changed.emit(False, 0.0, 0.0)
