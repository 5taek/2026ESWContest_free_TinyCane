import subprocess
import sys

from PySide6.QtCore import Qt, QTimer, Signal, QObject
from PySide6.QtGui import QImage, QPixmap, QColor, QPainter
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QVBoxLayout, QHBoxLayout,
    QGridLayout, QFrame, QGraphicsOpacityEffect, QSizePolicy,
)

from gps_link import GPSLink
from guide_link import CMD_DEST_BUS_STOP, CMD_DEST_SCHOOL, GuideLink
from ircamera_link import (
    GUIDE_VOICE_BUS_STOP,
    GUIDE_VOICE_CONFIRM,
    GUIDE_VOICE_ENTER,
    GUIDE_VOICE_SCHOOL,
    IrCameraLink,
    OBJECT_CODES,
)
from keyword_spotting_link import KeywordSpottingLink
from tof_link import GRID_CELLS, GRID_COLS, GRID_ROWS, TofLink

# 블루투스 이어폰 MAC 주소 목록. 페어링된 기기가 여러 개(승우의 Buds3 Pro / DST-S02)라
# 그때그때 어느 쪽을 쓸지 바뀔 수 있어서, 배지는 "이 중 하나라도 연결돼 있으면 연결됨"으로
# 본다(2026-08-26). `bluetoothctl paired-devices`로 목록 확인 가능.
BT_EARPHONE_MACS = ["7C:7B:BF:B9:D7:76", "3C:71:57:81:77:C9"]  # 승우의 Buds3 Pro, DST-S02

MODE_BRAILLE = "braille"
MODE_BUS_OCR = "bus_ocr"
MODE_OBSTACLE = "obstacle"
# 길안내 모드(MODE_GUIDE)는 ircamera 보드의 ModeBar(장애물/버스OCR/점자블록, 0x01/0x02 전환)와는
# 무관한 별도 트랙이다 - 키워드스파팅으로 진입하고, 진행 상태는 guide 보드(/dev/guide) 버튼
# 이벤트로 갱신되므로 ModeBar에는 넣지 않고 KeywordStatus 결과 표시 + 별도 GuideStatus 패널로만 다룬다.
MODE_GUIDE = "guide"

# "four" 키워드 인식 시 뜨는 응급상황 연출용 - 실제로 119에 신고하지는 않고, 화면 전체에
# 빨간 반투명 오버레이 + 문구만 5초간 띄운다(EmergencyOverlay). ircamera/TOF 어느 쪽과도
# 무관한 순수 UI 트랙이라 ModeBar/MODE_LABELS/MODE_COLORS에는 안 넣는다.
MODE_EMERGENCY = "emergency"
EMERGENCY_OVERLAY_DURATION_MS = 5000

MODE_LABELS = {
    MODE_BRAILLE: "점자블록 모드",
    MODE_BUS_OCR: "버스 OCR 모드",
    MODE_OBSTACLE: "장애물 감지 모드",
    MODE_GUIDE: "길안내 모드",
}

MODE_COLORS = {
    MODE_BRAILLE: "#5b8def",
    MODE_BUS_OCR: "#e8a33d",
    MODE_OBSTACLE: "#3ddc84",
    MODE_GUIDE: "#c084fc",
}

GUIDE_DESTINATIONS = ["학교", "버스정류장"]  # 짧게 누를 때마다 이 리스트를 순환한다. 늘리려면 여기만 추가하면 됨(2026-08-26: 집 -> 버스정류장으로 교체).

BG = "#0f1115"
PANEL = "#181b21"
PANEL_BORDER = "#262a33"
TEXT_MAIN = "#eef1f6"
TEXT_SUB = "#8a90a0"
GREEN = "#3ddc84"
RED = "#e5484d"


# ============================================================
# 연결 상태 배지 (GPS / 블루투스 이어폰)
# ============================================================
class StatusBadge(QFrame):
    def __init__(self, icon: str, label: str, parent=None):
        super().__init__(parent)
        self._connected = False

        self.setObjectName("StatusBadge")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(14, 8, 14, 8)
        layout.setSpacing(8)

        self.dot = QLabel("●")
        self.dot.setFixedWidth(14)

        self.icon_label = QLabel(icon)
        self.text_label = QLabel(label)
        self.state_label = QLabel("연결 안 됨")

        layout.addWidget(self.dot)
        layout.addWidget(self.icon_label)
        layout.addWidget(self.text_label)
        layout.addStretch(1)
        layout.addWidget(self.state_label)

        self.set_connected(False)

    def set_connected(self, connected: bool):
        self._connected = connected
        color = GREEN if connected else RED
        text = "연결됨" if connected else "연결 안 됨"
        self.dot.setStyleSheet(f"color: {color}; font-size: 14px;")
        self.state_label.setText(text)
        self.state_label.setStyleSheet(f"color: {color}; font-weight: 600;")
        self.setStyleSheet(f"""
            #StatusBadge {{
                background-color: {PANEL};
                border: 1px solid {PANEL_BORDER};
                border-radius: 10px;
            }}
        """)



# ============================================================
# 블루투스 이어폰 연결 상태 - bluetoothctl info로 주기적 확인 (호출당 ~15ms라 메인 스레드에서 돌려도 됨)
# ============================================================
class BluetoothWatcher(QObject):
    connected_changed = Signal(bool)

    def __init__(self, macs: list, poll_interval_ms: int = 3000, parent=None):
        super().__init__(parent)
        self.macs = macs
        self._last = None
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._check)
        self._timer.start(poll_interval_ms)
        # 생성 직후에는 아직 connected_changed를 아무도 구독 안 했을 수 있으니
        # 이번 이벤트루프 턴이 끝난 뒤(=호출부가 connect()까지 마친 뒤)로 첫 체크를 미룬다.
        QTimer.singleShot(0, self._check)

    def _check(self):
        # 등록된 MAC 중 하나라도 연결돼 있으면 연결됨으로 본다 - 어느 이어폰을 실제로
        # 쓰고 있는지는 안 가리고, 페어링된 후보 여러 개 중 지금 붙어있는 걸 그냥 찾는다.
        connected = False
        for mac in self.macs:
            try:
                result = subprocess.run(
                    ["bluetoothctl", "info", mac],
                    capture_output=True, text=True, timeout=2,
                )
                if "Connected: yes" in result.stdout:
                    connected = True
                    break
            except (subprocess.TimeoutExpired, FileNotFoundError):
                pass

        if connected != self._last:
            self._last = connected
            self.connected_changed.emit(connected)


# ============================================================
# 모드 표시 (점자블록 / 버스 OCR / 장애물 감지)
# ============================================================
class ModeBar(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("ModeBar")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(4)

        self._chips = {}
        for mode in (MODE_BRAILLE, MODE_BUS_OCR, MODE_OBSTACLE):
            chip = QLabel(MODE_LABELS[mode])
            chip.setAlignment(Qt.AlignCenter)
            chip.setFixedHeight(44)
            layout.addWidget(chip, 1)
            self._chips[mode] = chip

        self.set_active(MODE_OBSTACLE)

    def set_active(self, active_mode: str):
        for mode, chip in self._chips.items():
            if mode == active_mode:
                color = MODE_COLORS[mode]
                chip.setStyleSheet(f"""
                    background-color: {color};
                    color: #0f1115;
                    font-weight: 700;
                    font-size: 15px;
                    border-radius: 10px;
                """)
            else:
                chip.setStyleSheet(f"""
                    background-color: {PANEL};
                    color: {TEXT_SUB};
                    font-weight: 500;
                    font-size: 15px;
                    border: 1px solid {PANEL_BORDER};
                    border-radius: 10px;
                """)


# ============================================================
# 카메라 프리뷰 (MJPEG 프레임이 들어올 자리, 지금은 placeholder)
# ============================================================
class CameraPreview(QLabel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("CameraPreview")
        self.setMinimumSize(480, 360)
        self.setAlignment(Qt.AlignCenter)
        self.setStyleSheet(f"""
            #CameraPreview {{
                background-color: #05060a;
                border: 1px solid {PANEL_BORDER};
                border-radius: 12px;
                color: {TEXT_SUB};
                font-size: 15px;
            }}
        """)
        self.set_no_signal()

    def set_no_signal(self):
        self.setText("📷  카메라 스트림 대기 중…\n(MJPEG / Wi-Fi)")

    def set_frame(self, image: QImage):
        pix = QPixmap.fromImage(image).scaled(
            self.size(), Qt.KeepAspectRatio, Qt.SmoothTransformation
        )
        self.setPixmap(pix)


# ============================================================
# OCR / AI 결과 패널
# ============================================================
class ResultPanel(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("ResultPanel")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(6)

        title = QLabel("버스 번호 OCR 결과")
        title.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px; font-weight: 600;")

        self.value_label = QLabel("—")
        self.value_label.setStyleSheet(f"color: {TEXT_MAIN}; font-size: 34px; font-weight: 800;")

        self.conf_label = QLabel("신뢰도 —")
        self.conf_label.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px;")

        layout.addWidget(title)
        layout.addWidget(self.value_label)
        layout.addWidget(self.conf_label)

        self.setStyleSheet(f"""
            #ResultPanel {{
                background-color: {PANEL};
                border: 1px solid {PANEL_BORDER};
                border-radius: 12px;
            }}
        """)

    def show_result(self, text: str, confidence: float, matched: bool = True):
        self.value_label.setText(text if text else "—")
        color = GREEN if matched else TEXT_MAIN
        self.value_label.setStyleSheet(f"color: {color}; font-size: 34px; font-weight: 800;")
        status = "확정" if matched else "인식중..."
        self.conf_label.setText(f"신뢰도 {confidence * 100:.0f}% · {status}")

    def clear_result(self):
        self.value_label.setText("—")
        self.value_label.setStyleSheet(f"color: {TEXT_MAIN}; font-size: 34px; font-weight: 800;")
        self.conf_label.setText("신뢰도 —")


# ============================================================
# 장애물 실시간 상태 (0xB0 패킷마다 ~300ms 간격으로 계속 갱신, 배너처럼 반짝이진 않음)
# ============================================================
class ObstacleStatus(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("ObstacleStatus")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(6)

        title = QLabel("장애물 실시간 감지")
        title.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px; font-weight: 600;")

        self.value_label = QLabel("감지된 장애물 없음")
        self.value_label.setStyleSheet(f"color: {TEXT_MAIN}; font-size: 20px; font-weight: 700;")

        self.conf_label = QLabel("")
        self.conf_label.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px;")

        layout.addWidget(title)
        layout.addWidget(self.value_label)
        layout.addWidget(self.conf_label)

        self.setStyleSheet(f"""
            #ObstacleStatus {{
                background-color: {PANEL};
                border: 1px solid {PANEL_BORDER};
                border-radius: 12px;
            }}
        """)

    def update_status(self, object_name: str, direction_name: str, confidence: int):
        if object_name == "미상":
            self.clear()
        else:
            self.value_label.setText(f"{direction_name} · {object_name}")
            self.conf_label.setText(f"신뢰도 {confidence}%")

    def clear(self):
        self.value_label.setText("감지된 장애물 없음")
        self.conf_label.setText("")


# ============================================================
# 점자블록 실시간 상태 (0xB2 패킷 - present/conf/near)
# ============================================================
class BrailleStatus(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("BrailleStatus")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(6)

        title = QLabel("점자블록 실시간 감지")
        title.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px; font-weight: 600;")

        self.value_label = QLabel("감지된 점자블록 없음")
        self.value_label.setStyleSheet(f"color: {TEXT_MAIN}; font-size: 20px; font-weight: 700;")

        self.conf_label = QLabel("")
        self.conf_label.setStyleSheet(f"color: {TEXT_SUB}; font-size: 13px;")

        layout.addWidget(title)
        layout.addWidget(self.value_label)
        layout.addWidget(self.conf_label)

        self.setStyleSheet(f"""
            #BrailleStatus {{
                background-color: {PANEL};
                border: 1px solid {PANEL_BORDER};
                border-radius: 12px;
            }}
        """)

    def update_status(self, present: bool, confidence: int, near: bool):
        if not present:
            self.clear()
        else:
            distance = "근접" if near else "일반 거리"
            self.value_label.setText(f"점자블록 감지됨 · {distance}")
            self.conf_label.setText(f"신뢰도 {confidence}%")

    def clear(self):
        self.value_label.setText("감지된 점자블록 없음")
        self.conf_label.setText("")


# ============================================================
# TOF 8x8 원본 거리 그리드 (0xB2 패킷, 129바이트 고정 - tof_link.TofLink / RPI_COMM.md)
# TOF0 모듈이 내부 손상으로 확정되어 교체 전까지 TOF1 하나만 8x8 단독 운용 중(2026-08-28~).
# 인덱스는 row*GRID_COLS+col (row-major)로 위젯의 (row, col)에 그대로 대응된다:
#   row 0=맨 윗줄  row 1-5=중간  row 6-7=맨 아랫줄 2줄
#   col 0-1=LEFT(좌측)  col 2-5=CENTER(정면)  col 6-7=RIGHT(우측)
# ============================================================
TOF_NEAR_MM = 300  # 이보다 가까우면 위험(빨강), 그 밖의 유효 칸은 주의(주황)

# ircamera(AI모드) 0xB0의 direction_name(한글)을 TOF 열(dir) 인덱스로 매핑.
# TOF_task.cc의 haptic 판정 dir 정의(0=LEFT 1=CENTER 2=RIGHT)와 동일한 순서 -
# 0xB3(햅틱 발화 이벤트)의 direction 필드에 그대로 대응됨.
DIRECTION_TO_COL = {"왼쪽": 0, "중앙": 1, "오른쪽": 2}


class TofGridWidget(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("TofGridWidget")
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(10)

        title = QLabel("TOF 장애물 그리드")
        title.setStyleSheet(f"color: {TEXT_SUB}; font-size: 11px; font-weight: 600;")
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setSpacing(4)
        self._cells = []
        for row in range(GRID_ROWS):
            grid.setRowStretch(row, 1)
            for col in range(GRID_COLS):
                cell = QLabel("")
                cell.setAlignment(Qt.AlignCenter)
                cell.setMinimumSize(26, 24)
                cell.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
                cell.setStyleSheet(self._cell_style(PANEL_BORDER))
                grid.addWidget(cell, row, col)
                self._cells.append(cell)
                if row == 0:
                    grid.setColumnStretch(col, 1)
        layout.addLayout(grid, 1)

        self.setStyleSheet(f"""
            #TofGridWidget {{
                background-color: {PANEL};
                border: 1px solid {PANEL_BORDER};
                border-radius: 12px;
            }}
        """)

    def _cell_style(self, bg: str) -> str:
        return f"background-color: {bg}; border-radius: 6px; color: {TEXT_MAIN}; font-size: 11px; font-weight: 700;"

    def update_grid(self, distances):
        """TofLink.grid_received(distances) 그대로 연결. 길이 64 리스트(row-major,
        row*GRID_COLS+col), 무효 칸은 None (tof_link.py 참고)."""
        for idx, dist in enumerate(distances):
            cell = self._cells[idx]
            if dist is not None:
                color = RED if dist < TOF_NEAR_MM else "#e8a33d"
                cell.setText(str(dist))
                cell.setStyleSheet(self._cell_style(color))
            else:
                cell.setText("")
                cell.setStyleSheet(self._cell_style(PANEL_BORDER))

    def clear(self):
        self.update_grid([None] * GRID_CELLS)


# ============================================================
# TOF 진동 피드백 표시 (0xB3 패킷 - ESP32가 winner-takes-all로 판정해서 실제로
# 진동을 울린 순간만 온다). height(TOP/MID/BOTTOM)+direction(LEFT/CENTER/RIGHT)
# 두 축을 UP/DOWN/LEFT/CENTER/RIGHT 5개 노드짜리 십자(+) 모양으로 표시한다:
#   direction은 해당 LEFT/CENTER/RIGHT 노드를 그대로 켜고,
#   height가 TOP/BOTTOM이면 UP/DOWN 노드도 같이 켜진다(MID는 별도 노드 없음 -
#   중앙 CENTER 노드가 이미 그 역할). 예: TOP-LEFT는 UP+LEFT가 동시에 켜짐.
# ============================================================
TOF_ZONE_HEIGHT_LABELS = ("TOP", "MID", "BOT")
TOF_ZONE_DIRECTION_LABELS = ("L", "C", "R")
TOF_DIRECTION_HEADERS = ("LEFT", "CENTER", "RIGHT")
TOF_ZONE_FLASH_MS = 1200  # 진동 표시 유지 시간 - 다음 이벤트 전까지 계속 켜져 있지 않게 일정 시간 후 원복


class TofZoneFireWidget(QFrame):
    """
    TOF 햅틱 피드백 표시.

    제목/상태 문구/바깥 박스는 제거하고
    UP / LEFT / CENTER / RIGHT / DOWN 5개 노드만 십자 형태로 표시한다.

    실제 진동 이벤트가 발생하면 해당 방향 노드를 빨간색으로 잠시 점등한다.
    예:
      TOP-LEFT  -> UP + LEFT
      TOP-CENTER -> UP + CENTER
      MID-RIGHT -> RIGHT
      BOT-CENTER -> DOWN + CENTER
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("TofZoneFireWidget")

        # 바깥 패널 없이 십자 표시만 깔끔하게 배치
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        cross = QGridLayout()
        cross.setSpacing(6)

        self._nodes = {}

        node_positions = {
            "UP": (0, 1),
            "LEFT": (1, 0),
            "CENTER": (1, 1),
            "RIGHT": (1, 2),
            "DOWN": (2, 1),
        }

        for name, (r, c) in node_positions.items():
            node = QLabel(name)
            node.setAlignment(Qt.AlignCenter)
            node.setMinimumSize(55, 36)
            node.setSizePolicy(
                QSizePolicy.Expanding,
                QSizePolicy.Expanding
            )
            node.setStyleSheet(self._node_style(PANEL_BORDER))

            cross.addWidget(node, r, c)
            self._nodes[name] = node

        layout.addStretch(1)
        layout.addLayout(cross)
        layout.addStretch(1)

        # 실제 진동 이벤트가 발생했을 때 표시를 일정 시간 유지
        self._clear_timer = QTimer(self)
        self._clear_timer.setSingleShot(True)
        self._clear_timer.timeout.connect(self._clear_flash)

        self._active_nodes = []

        # 위젯 자체의 배경/테두리 제거
        self.setStyleSheet("""
            #TofZoneFireWidget {
                background-color: transparent;
                border: none;
            }
        """)

    def _node_style(self, bg: str) -> str:
        return (
            f"background-color: {bg}; "
            "border-radius: 8px; "
            f"color: {TEXT_SUB}; "
            "font-size: 11px; "
            "font-weight: 700;"
        )

    def flash(self, height: int, direction: int, distance_mm: int):
        if not (0 <= height < 3 and 0 <= direction < 3):
            return

        self._clear_flash()

        # 좌/중앙/우 방향은 해당 방향 노드를 활성화
        active = [TOF_DIRECTION_HEADERS[direction]]

        # 높이에 따라 UP/DOWN도 같이 활성화
        if height == 0:
            active.append("UP")
        elif height == 2:
            active.append("DOWN")

        for name in active:
            self._nodes[name].setStyleSheet(self._node_style(RED))

        self._active_nodes = active
        self._clear_timer.start(TOF_ZONE_FLASH_MS)

    def _clear_flash(self):
        for name in self._active_nodes:
            if name in self._nodes:
                self._nodes[name].setStyleSheet(
                    self._node_style(PANEL_BORDER)
                )
        self._active_nodes = []

    def clear(self):
        self._clear_timer.stop()
        self._clear_flash()


# ============================================================
# 키워드스파팅 상태 표시 (GPIO17 버튼 누름 -> "말씀하세요" -> "인식 중" -> 결과)
# 심사위원이 "지금 뭘 하고 있는지" 바로 알 수 있게 항상 보이는 상태줄로 둔다.
# ============================================================
class KeywordStatus(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("KeywordStatus")
        self.setFixedHeight(48)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(18, 0, 18, 0)

        self.label = QLabel("")
        self.label.setStyleSheet("font-size: 15px; font-weight: 700;")
        layout.addWidget(self.label)
        layout.addStretch(1)

        self._result_timer = QTimer(self)
        self._result_timer.setSingleShot(True)
        self._result_timer.timeout.connect(self.show_idle)

        self.show_idle()

    def _set(self, text: str, color: str):
        self.label.setText(text)
        self.label.setStyleSheet(f"font-size: 15px; font-weight: 700; color: {color};")
        self.setStyleSheet(f"""
            #KeywordStatus {{ background-color: {PANEL}; border-radius: 10px; border: 2px solid {color}; }}
        """)

    def show_idle(self):
        self._set("🎤  버튼을 누르고 말씀하세요", TEXT_SUB)

    def show_listening(self):
        self._result_timer.stop()
        self._set("🎤  듣는 중…", GREEN)

    def show_processing(self):
        self._set("🧠  키워드 인식 중…", "#e8a33d")

    def show_result(self, mode):
        if mode == MODE_BUS_OCR:
            self._set("✅  '버스' 인식 → 버스 OCR 모드로 전환", MODE_COLORS[MODE_BUS_OCR])
        elif mode == MODE_BRAILLE:
            self._set("✅  '점자블록' 인식 → 점자블록 모드로 전환", MODE_COLORS[MODE_BRAILLE])
        elif mode == MODE_GUIDE:
            self._set("✅  '길안내' 인식 → 길안내 모드로 전환", MODE_COLORS[MODE_GUIDE])
        elif mode == MODE_EMERGENCY:
            self._set("🚨  '넷' 인식 → 도움 요청", RED)
        else:
            self._set("❓  인식 실패, 다시 시도해주세요", RED)
        self._result_timer.start(2500)


# ============================================================
# 길안내 모드 상태 (키워드 "three" 인식 후 진입 - guide 보드(/dev/guide) 버튼 이벤트로 갱신)
# 짧게 누름: GUIDE_DESTINATIONS를 순환하며 표시 / 길게 누름(2초+): 길안내 시작 메시지 표시
# ============================================================
class GuideStatus(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("GuideStatus")
        self.setFixedHeight(48)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(18, 0, 18, 0)

        self.label = QLabel("")
        self.label.setStyleSheet("font-size: 15px; font-weight: 700;")
        layout.addWidget(self.label)
        layout.addStretch(1)

        self._set("🧭  길안내 모드 - 버튼을 눌러 목적지를 선택하세요", MODE_COLORS[MODE_GUIDE])
        self.setVisible(False)

        # 좌회전/우회전/횡단보도/계단 신호가 오면 3초간만 문구를 덮어썼다가, 그 전에
        # 표시하고 있던 문구(길안내 시작/대기/목적지선택 등)로 자동 복귀시키기 위한 상태.
        self._steady_text = ""
        self._steady_color = MODE_COLORS[MODE_GUIDE]
        self._turn_timer = QTimer(self)
        self._turn_timer.setSingleShot(True)
        self._turn_timer.timeout.connect(self._revert_to_steady)

    def _set(self, text: str, color: str):
        self.label.setText(text)
        self.label.setStyleSheet(f"font-size: 15px; font-weight: 700; color: {color};")
        self.setStyleSheet(f"""
            #GuideStatus {{ background-color: {PANEL}; border-radius: 10px; border: 2px solid {color}; }}
        """)

    def _set_steady(self, text: str, color: str):
        """일반 상태 전환용. 진행 중이던 3초 임시표시는 취소하고 새 상태를 '복귀 지점'으로 기억한다."""
        self._turn_timer.stop()
        self._steady_text = text
        self._steady_color = color
        self._set(text, color)

    def _revert_to_steady(self):
        self._set(self._steady_text, self._steady_color)

    def show_waiting(self):
        self._set_steady("🧭  길안내 모드 - 버튼을 눌러 목적지를 선택하세요", MODE_COLORS[MODE_GUIDE])

    def show_destination(self, name: str):
        self._set_steady(f"📍  목적지: {name} (2초 이상 누르면 길안내 시작)", MODE_COLORS[MODE_GUIDE])

    def show_started(self, name: str):
        self._set_steady(f"🚶  길안내를 시작합니다 - {name}", GREEN)

    def show_arrived(self):
        self._set_steady("🎉  목적지에 도착했습니다", GREEN)

    def show_turn_signal(self, text: str):
        """좌회전/우회전/횡단보도/계단 - 3초간만 표시하고 이전 상태(steady)로 자동 복귀."""
        self._set(text, GREEN)
        self._turn_timer.start(3000)


# ============================================================
# 이벤트 알림 배너 (장애물 감지 / 낙상 감지) - 이모지로 재미있게
# ============================================================
class EventBanner(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("EventBanner")
        self.setFixedHeight(64)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(18, 0, 18, 0)

        self.label = QLabel("")
        self.label.setStyleSheet("font-size: 20px; font-weight: 700;")
        layout.addWidget(self.label)
        layout.addStretch(1)

        self._effect = QGraphicsOpacityEffect(self)
        self.setGraphicsEffect(self._effect)
        self._effect.setOpacity(0.0)

        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.timeout.connect(self._hide)

        self._hide()

    def _hide(self):
        self._effect.setOpacity(0.0)
        self.setStyleSheet(f"""
            #EventBanner {{ background-color: {PANEL}; border-radius: 12px; border: 1px solid {PANEL_BORDER}; }}
        """)

    def flash(self, text: str, color: str, duration_ms: int = 2600):
        self.label.setText(text)
        self.label.setStyleSheet(f"font-size: 20px; font-weight: 700; color: {color};")
        self.setStyleSheet(f"""
            #EventBanner {{ background-color: {PANEL}; border-radius: 12px; border: 2px solid {color}; }}
        """)
        self._effect.setOpacity(1.0)
        self._hide_timer.start(duration_ms)

    def obstacle(self, label: str, confidence: float):
        self.flash(f"⚠️  장애물 감지 — {label} ({confidence * 100:.0f}%)", "#e8a33d")

    def braille(self, present: bool, confidence: int, near: bool):
        if not present:
            self.flash("🟨  점자블록 인식 안 됨", TEXT_SUB)
        elif near:
            self.flash(f"🟨  점자블록 인식됨 (근접, {confidence}%)", MODE_COLORS[MODE_BRAILLE])
        else:
            self.flash(f"🟨  점자블록 인식됨 ({confidence}%)", MODE_COLORS[MODE_BRAILLE])

    def crosswalk(self):
        self.flash("🚦  횡단보도 인식됨", "#5b8def")

    def fall(self):
        # ESP32 쪽 유예시간(kPickupIgnoreMs)이 최대 10초라, 그 안에 0xFE/0xFD로 넘어가기 전에
        # 배너가 먼저 사라지지 않도록 넉넉히 잡고, 실제로는 clear_fall()이 명시적으로 먼저 끈다.
        self.flash("🚨  낙상 감지! 확인이 필요합니다", RED, duration_ms=15000)

    def clear_fall(self):
        self._hide_timer.stop()
        self._hide()


# ============================================================
# 응급상황 연출 (키워드 "four" 인식 시) - 실제로 119에 신고하지는 않고, 창 전체에
# 빨간 반투명 오버레이를 씌워서 뒤 대시보드가 흐릿하게 비치게 하고 EMERGENCY_OVERLAY_DURATION_MS
# 뒤 자동으로 사라진다.
# ============================================================
class EmergencyOverlay(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet("background-color: rgba(178, 24, 24, 195);")

        layout = QVBoxLayout(self)
        layout.setAlignment(Qt.AlignCenter)
        layout.setSpacing(14)

        # 🚨(회전경광등, 컬러 이모지)는 이 시스템 폰트에 글리프가 없어서 배경색이 칠해진
        # 네모(tofu)로 깨져 보였다 - 컬러 이모지 폰트 없이도 그리는 경고삼각형(⚠, VS16 없는
        # 단독 코드포인트)으로 바꿔서 실제 글리프가 뜨게 한다.
        title = QLabel("⚠ 도움 요청")
        title.setAlignment(Qt.AlignCenter)
        title.setStyleSheet("color: white; background: transparent; font-size: 48px; font-weight: 800;")

        subtitle = QLabel("119에 신고중...")
        subtitle.setAlignment(Qt.AlignCenter)
        subtitle.setStyleSheet("color: white; background: transparent; font-size: 26px; font-weight: 600;")

        layout.addWidget(title)
        layout.addWidget(subtitle)

        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.timeout.connect(self.hide)

        self.hide()

    def trigger(self, duration_ms=EMERGENCY_OVERLAY_DURATION_MS):
        if self.parentWidget():
            self.setGeometry(self.parentWidget().rect())
        self.raise_()
        self.show()
        self._hide_timer.start(duration_ms)


# ============================================================
# 메인 윈도우
# ============================================================
class DashboardWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("보조기기 대시보드")
        # TOF 그리드(TofGridWidget) 추가 후 side 컬럼 실제 필요 높이가 커져서
        # 기존 620이면 그리드가 sizeHint보다 작게 눌려 렌더링된다(행이 뭉개져 보임).
        # 여기서 min height를 올려도 부족하면 show() 이후 resize(sizeHint())로 보정한다.
        self.setMinimumSize(1000, 870)
        self.setStyleSheet(f"background-color: {BG}; color: {TEXT_MAIN}; font-family: 'Pretendard', 'Malgun Gothic', sans-serif;")

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(20, 20, 20, 20)
        root.setSpacing(14)

        # central의 자식으로 둬야 central과 같이 리사이즈/스크롤되는 좌표계를 공유한다.
        # 평소엔 숨겨져 있다가 trigger_emergency()가 불릴 때만 central 전체를 덮는다.
        self.emergency_overlay = EmergencyOverlay(central)

        # 상단 1행: GPS / 블루투스 이어폰 / 영상 스트림
        top_bar = QHBoxLayout()
        top_bar.setSpacing(10)
        self.gps_badge = StatusBadge("🛰️", "GPS")
        self.gps_coord_label = QLabel("")
        self.gps_coord_label.setStyleSheet(f"color: {TEXT_SUB}; font-size: 12px;")
        self.bt_badge = StatusBadge("🎧", "블루투스 이어폰")
        self.video_badge = StatusBadge("📡", "영상 스트림")
        top_bar.addStretch(1)
        top_bar.addWidget(self.gps_badge)
        top_bar.addWidget(self.gps_coord_label)
        top_bar.addWidget(self.bt_badge)
        top_bar.addWidget(self.video_badge)
        top_bar.addStretch(1)
        root.addLayout(top_bar)

        # 상단 2행: 3개 ESP32-S3 보드 연결 상태
        board_bar = QHBoxLayout()
        board_bar.setSpacing(10)
        self.keyword_badge = StatusBadge("🎙️", "키워드+IMU")
        self.serial_badge = StatusBadge("🔌", "AI모드")
        self.tof_badge = StatusBadge("📏", "TOF")
        self.guide_badge = StatusBadge("🧭", "길안내모드")
        board_bar.addStretch(1)
        board_bar.addWidget(self.keyword_badge)
        board_bar.addWidget(self.serial_badge)
        board_bar.addWidget(self.tof_badge)
        board_bar.addWidget(self.guide_badge)
        board_bar.addStretch(1)
        root.addLayout(board_bar)

        # 모드 바 + 키워드스파팅 상태
        self.mode_bar = ModeBar()
        root.addWidget(self.mode_bar)

        self.keyword_status = KeywordStatus()
        root.addWidget(self.keyword_status)

        self.guide_status = GuideStatus()
        root.addWidget(self.guide_status)

        # 본문: 카메라 프리뷰 + 결과 패널
        body = QHBoxLayout()
        body.setSpacing(14)

        self.camera_preview = CameraPreview()
        body.addWidget(self.camera_preview, 3)

        side = QVBoxLayout()
        side.setSpacing(14)
        self.result_panel = ResultPanel()        # 버스 OCR 모드 전용
        self.obstacle_status = ObstacleStatus()   # 장애물 감지 모드 전용(ircamera 0xB0, 물체분류)
        self.braille_status = BrailleStatus()     # 점자블록 모드 전용
        side.addWidget(self.result_panel)
        side.addWidget(self.obstacle_status)
        side.addWidget(self.braille_status)
        # TOF 진동 발화(0xB3)와 8x8 원본 그리드(0xB2) - 모드와 무관하게 항상 인식결과 박스
        # 아래쪽에 표시. 발화 표시가 그리드보다 위에 오게 배치.
        self.tof_zone_fire = TofZoneFireWidget()
        side.addWidget(self.tof_zone_fire)
        self.tof_grid = TofGridWidget()
        # side 컬럼의 남는 세로 공간을 빈 스트레치가 아니라 그리드 쪽으로 몰아준다 -
        # 그래야 8행이 눌리지 않고 실제로 늘어나서 칸 안 숫자가 다 보인다.
        side.addWidget(self.tof_grid, 1)
        body.addLayout(side, 2)

        # 모드별로 정확히 하나의 결과 패널만 보이게 한다 - 이전 모드의 값이
        # 화면에 남아있는 걸 막기 위해 set_mode()에서 매번 셋 다 리셋하고 스위칭한다.
        self._mode_panels = {
            MODE_OBSTACLE: self.obstacle_status,
            MODE_BUS_OCR: self.result_panel,
            MODE_BRAILLE: self.braille_status,
        }

        root.addLayout(body, 1)

        # 하단: 이벤트 배너
        self.event_banner = EventBanner()
        root.addWidget(self.event_banner)

        self._last_obstacle_key = None
        # TOF+AI모드 방향일치 확인 트리거(0x10, main() 참고)용 - 그리드가 올 때마다 갱신되고,
        # AI모드 0xB0가 올 때 "이 방향 칸이 지금 활성인가"를 즉시 조회하는 용도라 여기 저장해둔다.
        # 0xB3(햅틱 발화 이벤트)의 마지막 direction만 들고 있는다 - 옛 0xB1은 이미 "확정된
        # 위협 구역" 스냅샷이라 다음 이벤트가 올 때까지 그대로 유지했는데, 0xB3도 같은
        # 이벤트 기반 성격이라 동일하게 다음 이벤트/연결끊김 전까지 유지한다.
        self._latest_tof_direction = None
        self._guide_dest_index = -1
        # GPIO17을 목적지선택용으로 가로챌지 여부 - guide_status의 화면 표시 여부와는
        # 별개로 관리한다(과거엔 guide_status.isVisible()을 같이 썼는데, "시작합니다"
        # 이후에도 패널이 계속 보여있어서 길안내 시작 후에도 키워드스파팅으로 못
        # 돌아오는 버그가 있었다).
        self._guide_button_active = False
        self.set_mode(MODE_OBSTACLE)  # 초기 표시 상태를 ModeBar 기본값(장애물 감지)에 맞춘다

    # ---- 외부에서 상태를 밀어넣는 인터페이스 (나중에 실데이터 연결 지점) ----
    def set_gps_connected(self, connected: bool):
        self.gps_badge.set_connected(connected)

    def set_gps_fix(self, connected: bool, lat: float, lon: float):
        self.gps_badge.set_connected(connected)
        self.gps_coord_label.setText(f"{lat:.6f}, {lon:.6f}" if connected else "")

    def set_bt_connected(self, connected: bool):
        self.bt_badge.set_connected(connected)

    def set_mode(self, mode: str):
        self.mode_bar.set_active(mode)

        # 현재 모드에 해당하는 패널만 보이게 하고, 나머지는 감춘다.
        for panel_mode, panel in self._mode_panels.items():
            panel.setVisible(panel_mode == mode)

        # 모드가 바뀌면 셋 다 리셋한다 - 이전 모드에서 남은 값이 다음 모드 진입 시
        # (혹은 다시 돌아왔을 때) 잠깐이라도 화면에 남아있지 않게 하기 위함.
        self.result_panel.clear_result()
        self.obstacle_status.clear()
        self.braille_status.clear()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self.emergency_overlay.isVisible():
            self.emergency_overlay.setGeometry(self.centralWidget().rect())

    def trigger_emergency(self):
        self.emergency_overlay.trigger()

    def set_camera_frame(self, image: QImage):
        self.camera_preview.set_frame(image)

    def show_ocr_result(self, text: str, confidence: float, matched: bool = True):
        self.result_panel.show_result(text, confidence, matched)

    def show_obstacle_event(self, label: str, confidence: float):
        self.event_banner.obstacle(label, confidence)

    def show_crosswalk_event(self):
        self.event_banner.crosswalk()

    def show_fall_event(self):
        self.event_banner.fall()

    def hide_fall_event(self):
        self.event_banner.clear_fall()

    def set_ircamera_serial_connected(self, connected: bool):
        self.serial_badge.set_connected(connected)

    def set_ircamera_video_connected(self, connected: bool):
        self.video_badge.set_connected(connected)

    def handle_obstacle_packet(self, object_name: str, direction_name: str, confidence: int):
        """0xB0 패킷(~300ms 간격, 미탐지 포함)마다 호출됨.
        실시간 상태는 매번 갱신하되, 배너 반짝임은 '없음 -> 감지' 전환(rising edge)에서만 띄운다."""
        self.obstacle_status.update_status(object_name, direction_name, confidence)

        key = (object_name, direction_name)
        if object_name == "미상":
            self._last_obstacle_key = None
        elif key != self._last_obstacle_key:
            self._last_obstacle_key = key
            self.show_obstacle_event(f"{direction_name} {object_name}", confidence / 100)

    def show_braille_result(self, present: bool, confidence: int, near: bool):
        self.event_banner.braille(present, confidence, near)
        self.braille_status.update_status(present, confidence, near)

    def set_keyword_serial_connected(self, connected: bool):
        self.keyword_badge.set_connected(connected)

    def set_tof_connected(self, connected: bool):
        self.tof_badge.set_connected(connected)
        if not connected:
            self.tof_grid.clear()
            self.tof_zone_fire.clear()
            self._latest_tof_direction = None  # 끊긴 동안의 낡은 값으로 확인 트리거가 오발동하지 않게

    def update_tof_grid(self, distances):
        self.tof_grid.update_grid(distances)

    def handle_tof_haptic_fired(self, height: int, direction: int, distance_mm: int):
        self._latest_tof_direction = direction
        self.tof_zone_fire.flash(height, direction, distance_mm)

    def tof_direction_active(self, direction_name: str) -> bool:
        """AI모드(0xB0)의 direction_name("왼쪽"/"중앙"/"오른쪽")과 TOF의 마지막 햅틱 발화
        방향(0xB3)이 같은 열이면 "TOF도 같은 방향에서 위협을 확정했다"로 본다. 0xB2(원본
        그리드)는 구역 분류가 없는 순수 거리값이라 여기 쓰기엔 안 맞음 - 확정 판단은
        ESP32가 winner-takes-all로 이미 해준 0xB3 이벤트를 그대로 따라간다."""
        col = DIRECTION_TO_COL.get(direction_name)
        if col is None:
            return False
        return self._latest_tof_direction == col

    def set_guide_connected(self, connected: bool):
        self.guide_badge.set_connected(connected)

    def enter_guide_mode(self):
        self._guide_dest_index = -1
        self._guide_button_active = True
        self.guide_status.show_waiting()
        self.guide_status.setVisible(True)

    def guide_cycle_destination(self):
        if not self._guide_button_active:
            return  # 길안내 모드 진입 전(키워드 "three" 인식 전) 버튼 이벤트는 무시
        self._guide_dest_index = (self._guide_dest_index + 1) % len(GUIDE_DESTINATIONS)
        self.guide_status.show_destination(GUIDE_DESTINATIONS[self._guide_dest_index])

    def guide_start(self):
        """목적지 확정 시 UI를 갱신하고, 확정된 목적지 이름을 돌려준다(호출부가 guide_link로
        확정 바이트 전송 + GPS 스트리밍 시작을 트리거하는 데 씀). 목적지를 아직 하나도
        선택 안 한 상태의 롱프레스는 무시하고 None을 돌려준다.
        확정되면 GPIO17을 즉시 키워드스파팅으로 돌려준다 - 길안내는 이제 시작됐으니
        버튼을 계속 목적지선택용으로 잡아둘 이유가 없다(guide_status 패널 자체는
        "시작합니다" 문구를 보여주려고 계속 보이는 채로 둔다)."""
        if not self._guide_button_active or self._guide_dest_index < 0:
            return None
        name = GUIDE_DESTINATIONS[self._guide_dest_index]
        self.guide_status.show_started(name)
        self._guide_button_active = False
        return name

    def guide_arrived(self):
        self._guide_dest_index = -1
        self._guide_button_active = False
        self.guide_status.show_arrived()

    def guide_turn_signal(self, text: str):
        self.guide_status.show_turn_signal(text)


def main():
    app = QApplication(sys.argv)
    window = DashboardWindow()

    # ---- ircamera (ACM1: 장애물/버스OCR/점자블록 + TCP:8090 영상) ----
    link = IrCameraLink()
    link.frame_received.connect(window.set_camera_frame)
    link.mode_changed.connect(window.set_mode)
    link.obstacle_detected.connect(window.handle_obstacle_packet)

    # AI모드(0xB0)와 TOF(그리드)가 같은 방향에서 동시에 뭔가를 보고하면, RPi가 판단한
    # 객체코드를 0x10 트리거로 ircamera에 확정해준다(TTS로 그대로 말하게). debounce는
    # 방향이 아니라 "객체 종류"로만 건다 - 방향은 TOF 햅틱이 이미 실시간으로 알려주고
    # 있어서, 같은 물체가 방향만 바뀌며 계속 잡히는 동안 재트리거할 필요가 없다.
    _last_confirmed_object = None

    def on_obstacle_for_confirm(object_name, direction_name, confidence):
        nonlocal _last_confirmed_object
        if object_name == "미상":
            _last_confirmed_object = None  # 감지가 끊기면 다음에 다시 잡혔을 때 재트리거되도록 리셋
            return
        if not window.tof_direction_active(direction_name):
            return
        object_code = OBJECT_CODES.get(object_name)
        if object_code is None or object_code == _last_confirmed_object:
            return
        _last_confirmed_object = object_code
        link.send_obstacle_confirm(object_code)

    link.obstacle_detected.connect(on_obstacle_for_confirm)
    link.bus_result.connect(lambda text, conf, matched: window.show_ocr_result(text, conf / 100, matched))
    link.braille_result.connect(window.show_braille_result)
    link.serial_connected.connect(window.set_ircamera_serial_connected)
    link.video_client_connected.connect(window.set_ircamera_video_connected)

    # 정확히 어떤 조건에서 긴급상황 음성(0x04)을 쏴야 하는지 아직 안 정해져서, 일단 기존
    # UI 트리거 지점(비상 버튼 클릭 + 키워드 "four" 인식) 둘 다에 같이 물려둔다.
    def trigger_emergency_alert():
        window.trigger_emergency()
        link.send_emergency_alert()  # "긴급상황발생 119신고요청" 재생(ircamera 스피커)

    # ---- 키워드스파팅 (ACM0: GPIO17 버튼 -> 녹음 -> 인식 -> ircamera 모드 릴레이) ----
    keyword_link = KeywordSpottingLink()
    keyword_link.serial_connected.connect(window.set_keyword_serial_connected)
    keyword_link.listening_started.connect(window.keyword_status.show_listening)
    keyword_link.listening_stopped.connect(window.keyword_status.show_processing)

    # 키워드 인식 릴레이가 이 헬퍼를 통해 길안내 모드로 들어간다 - guide_link는 아래에서
    # 만들어지지만, 이 함수는 실제 인식 시점(즉 main() 실행이 다 끝난 뒤)에나 호출되므로
    # 클로저로 참조해도 문제없다.
    def trigger_guide_mode():
        guide_link.stop_navigation()  # 이전에 진행 중이던 목적지/GPS 스트리밍이 있었다면 리셋
        window.enter_guide_mode()
        link.send_guide_voice(GUIDE_VOICE_ENTER)  # "길안내모드전환" 재생(ircamera 스피커)

    def on_keyword_recognized(mode):
        window.keyword_status.show_result(mode)
        if mode == MODE_BUS_OCR:
            link.request_bus_mode()
        elif mode == MODE_BRAILLE:
            link.request_braille_mode()
        elif mode == MODE_GUIDE:
            trigger_guide_mode()
        elif mode == MODE_EMERGENCY:
            trigger_emergency_alert()

    keyword_link.keyword_recognized.connect(on_keyword_recognized)

    def on_fall_event(state):
        if state == "detected":
            window.show_fall_event()
            link.send_fall_alert_start()  # ircamera가 반복 알림을 자체적으로 시작(ESP32 쪽 구현은 별도)
        elif state == "resolved":
            window.hide_fall_event()
            link.send_fall_alert_stop()
        elif state == "danger":
            link.send_fall_alert_stop()  # 반복 알림 멈추고
            # ESP32가 이미 kPickupIgnoreMs 유예시간 동안 pickup 동작 없음을 확인하고 보낸
            # 확정 신호라서, 기존 비상 버튼/키워드 "four"와 동일하게 처리한다.
            trigger_emergency_alert()

    keyword_link.fall_event.connect(on_fall_event)

    # 길안내 모드 중엔 GPIO17이 오디오 키워드스파팅 대신 목적지 선택용으로 동작해야 하므로,
    # "지금 GPIO17을 목적지선택용으로 가로채야 하나"를 판단해서 넘긴다. guide_status의
    # 화면 표시 여부(isVisible)는 쓰지 않는다 - "길안내를 시작합니다" 이후에도 패널은
    # 계속 보이므로 그걸 기준으로 삼으면 길안내 시작 후 키워드스파팅으로 못 돌아온다.
    keyword_link.guide_mode_check = lambda: window._guide_button_active
    keyword_link.guide_short_press.connect(lambda: on_guide_cycle())

    # ---- TOF (/dev/tof: 8x8 원본 그리드 0xB2 + 햅틱 발화 이벤트 0xB3 - RPI_COMM.md) ----
    tof_link = TofLink()
    tof_link.serial_connected.connect(window.set_tof_connected)
    tof_link.grid_received.connect(window.update_tof_grid)
    tof_link.haptic_fired.connect(window.handle_tof_haptic_fired)

    # ---- 길안내 보드 (/dev/guide: 짧게 누름=목적지 순환, 길게 누름(2초+)=목적지 확정+길안내 시작) ----
    guide_link = GuideLink()
    guide_link.serial_connected.connect(window.set_guide_connected)
    guide_link.short_press.connect(lambda: on_guide_cycle())

    GUIDE_DEST_CODES = {"학교": CMD_DEST_SCHOOL, "버스정류장": CMD_DEST_BUS_STOP}
    # ircamera 음성 트리거용 subcode. GUIDE_VOICE_CONFIRM은 목적지 상관없이 항상
    # "버스정류장으로 길안내를 시작합니다"만 재생함(학교 문구는 미구현, 의도적으로 방치).
    GUIDE_VOICE_CODES = {"학교": GUIDE_VOICE_SCHOOL, "버스정류장": GUIDE_VOICE_BUS_STOP}

    def on_guide_cycle():
        window.guide_cycle_destination()
        if window._guide_button_active:
            name = GUIDE_DESTINATIONS[window._guide_dest_index]
            link.send_guide_voice(GUIDE_VOICE_CODES[name])

    def on_guide_long_press():
        name = window.guide_start()  # UI 갱신 + 확정된 목적지 이름(없으면 None)
        if name is not None:
            guide_link.start_navigation(GUIDE_DEST_CODES[name])
            link.send_guide_voice(GUIDE_VOICE_CONFIRM)

    guide_link.long_press.connect(on_guide_long_press)
    keyword_link.guide_long_press.connect(on_guide_long_press)

    def on_guide_arrived():
        window.guide_arrived()
        link.send_guide_arrived()  # "목적지에 도착했습니다" 재생(ircamera 스피커)

    guide_link.arrived.connect(on_guide_arrived)

    def on_guide_event():
        # TODO: ircamera 스피커로 릴레이 - 실제 명령 바이트는 ircamera 펌웨어 쪽에서 정해지면 채울 것
        print("[guide] 이벤트 발생 - ircamera 릴레이 대기 중(TODO)")

    guide_link.event_occurred.connect(on_guide_event)

    # 좌회전/우회전/횡단보도/계단 - guide 보드 원본 신호(0x03~0x06)를 ircamera로 그대로
    # 릴레이하면 CMD_GUIDE_VOICE(0x03)/CMD_EMERGENCY_ALERT(0x04)/CMD_GUIDE_ARRIVED(0x05)와
    # 값이 겹치므로, ircamera 쪽 빈 값(0x06~0x09)으로 번역해서 보낸다.
    guide_link.turn_left.connect(link.send_guide_turn_left)
    guide_link.turn_right.connect(link.send_guide_turn_right)
    guide_link.crosswalk.connect(link.send_guide_crosswalk)
    guide_link.stairs.connect(link.send_guide_stairs)

    # 화면에도 3초간만 문구로 표시(GuideStatus 패널) - ircamera 릴레이와는 별개로 병행.
    guide_link.turn_left.connect(lambda: window.guide_turn_signal("↩️  좌회전"))
    guide_link.turn_right.connect(lambda: window.guide_turn_signal("↪️  우회전"))
    guide_link.crosswalk.connect(lambda: window.guide_turn_signal("🚸  횡단보도"))
    guide_link.stairs.connect(lambda: window.guide_turn_signal("🪜  계단"))

    # ---- 블루투스 이어폰 (bluetoothctl 폴링) ----
    bt_watcher = BluetoothWatcher(BT_EARPHONE_MACS)
    bt_watcher.connected_changed.connect(window.set_bt_connected)

    # ---- GPS (/dev/ttyAMA4 UART) ----
    gps_link = GPSLink()
    gps_link.fix_changed.connect(window.set_gps_fix)

    def on_gps_fix(connected, lat, lon):
        if connected:
            guide_link.update_position(lat, lon)
            link.set_gps_fix(lat, lon)
            keyword_link.set_gps_fix(lat, lon)

    gps_link.fix_changed.connect(on_gps_fix)

    link.start()
    keyword_link.start()
    tof_link.start()
    gps_link.start()
    guide_link.start()

    window.show()
    window.resize(window.sizeHint())  # setMinimumSize만으론 최초 show() 크기가 sizeHint보다 작게 잡힐 때가 있어 명시적으로 맞춘다
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
