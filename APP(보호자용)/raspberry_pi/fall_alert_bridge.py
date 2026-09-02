from __future__ import annotations

import argparse
import json
import os
import socket
import time
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from dotenv import load_dotenv
import firebase_admin
from firebase_admin import credentials, firestore, messaging


load_dotenv()


VALID_STATES = {'normal', 'safe_fall', 'danger_fall'}

STATE_MESSAGES = {
    'normal': {
        'title': '정상 상태',
        'body': '기기가 정상 상태를 보고했습니다.',
        'priority': 'normal',
        'status': 'resolved',
    },
    'safe_fall': {
        'title': '안전 확인',
        'body': '낙상/낙하 감지 후 사용자가 지팡이를 다시 든 것으로 판단되었습니다.',
        'priority': 'normal',
        'status': 'resolved',
    },
    'danger_fall': {
        'title': '위험 낙상 감지',
        'body': '낙상 후 지팡이를 다시 들지 못했습니다. 즉시 확인이 필요합니다.',
        'priority': 'high',
        'status': 'active',
    },
}


class BridgeConfig:
    def __init__(self) -> None:
        self.host = os.getenv('BRIDGE_HOST', '0.0.0.0')
        self.port = int(os.getenv('BRIDGE_PORT', '8080'))
        self.shared_secret = os.getenv('BRIDGE_SHARED_SECRET')
        self.service_account = os.getenv('FIREBASE_SERVICE_ACCOUNT')
        self.device_id = os.getenv('DEVICE_ID', 'cane-001')
        self.patient_id = os.getenv('PATIENT_ID', 'patient-001')
        self.guardian_uids = _csv(os.getenv('GUARDIAN_UIDS', ''))
        self.cooldown_seconds = int(os.getenv('ALERT_COOLDOWN_SECONDS', '20'))
        self.notify_normal = _truthy(os.getenv('NOTIFY_NORMAL', 'false'))
        self.notify_safe_fall = _truthy(os.getenv('NOTIFY_SAFE_FALL', 'true'))
        self.gpsd_enabled = _truthy(os.getenv('GPSD_ENABLED', 'false'))
        self.gpsd_host = os.getenv('GPSD_HOST', '127.0.0.1')
        self.gpsd_port = int(os.getenv('GPSD_PORT', '2947'))
        self.gps_timeout_seconds = float(os.getenv('GPS_TIMEOUT_SECONDS', '1.5'))


class DeviceRouting:
    def __init__(
        self,
        device_id: str,
        patient_id: str,
        guardian_uids: list[str],
    ) -> None:
        self.device_id = device_id
        self.patient_id = patient_id
        self.guardian_uids = guardian_uids


class GpsdLocationProvider:
    def __init__(self, config: BridgeConfig) -> None:
        self.config = config

    def read_location(self) -> dict[str, Any] | None:
        if not self.config.gpsd_enabled:
            return None

        try:
            with socket.create_connection(
                (self.config.gpsd_host, self.config.gpsd_port),
                timeout=self.config.gps_timeout_seconds,
            ) as sock:
                sock.settimeout(self.config.gps_timeout_seconds)
                sock.sendall(b'?WATCH={"enable":true,"json":true};\n')
                deadline = time.monotonic() + self.config.gps_timeout_seconds
                buffer = b''

                while time.monotonic() < deadline:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    buffer += chunk
                    for line in buffer.splitlines():
                        location = _location_from_gpsd_line(line)
                        if location is not None:
                            return location
        except OSError as error:
            print(f'GPSD read failed: {error}')

        return None


class FallAlertBridge:
    def __init__(self, config: BridgeConfig) -> None:
        if not config.service_account:
            raise RuntimeError('FIREBASE_SERVICE_ACCOUNT is required.')

        self.config = config
        self.gps = GpsdLocationProvider(config)
        self._last_alert_at_by_key: dict[str, float] = {}

        if not firebase_admin._apps:
            cred = credentials.Certificate(config.service_account)
            firebase_admin.initialize_app(cred)

        self.db = firestore.client()

    def process_alert(self, payload: dict[str, Any]) -> dict[str, Any]:
        state = _normalize_state(payload)
        routing = self._resolve_routing(payload)
        location = _location_from_payload(payload) or self.gps.read_location()
        score = payload.get('score')
        source = str(payload.get('source') or 'fall_detection')
        default_message = STATE_MESSAGES[state]['body']
        message = str(payload.get('message') or default_message)

        self._update_device_status(
            routing=routing,
            state=state,
            source=source,
            location=location,
            score=score,
        )

        if state == 'normal' and not self.config.notify_normal:
            return {
                'ok': True,
                'sent': False,
                'state': state,
                'reason': 'normal_status_only',
            }

        if state == 'safe_fall' and not self.config.notify_safe_fall:
            return {
                'ok': True,
                'sent': False,
                'state': state,
                'reason': 'safe_fall_notification_disabled',
            }

        cooldown_result = self._cooldown_result(routing.device_id, state)
        if cooldown_result is not None:
            return cooldown_result

        event = {
            'deviceId': routing.device_id,
            'patientId': routing.patient_id,
            'guardianUids': routing.guardian_uids,
            'state': state,
            'severity': _legacy_severity(state),
            'source': source,
            'message': message,
            'score': score,
            'location': location,
            'status': STATE_MESSAGES[state]['status'],
            'createdAt': firestore.SERVER_TIMESTAMP,
            'deviceTime': datetime.now(timezone.utc).isoformat(),
        }

        _, document_ref = self.db.collection('fall_events').add(event)
        event_id = document_ref.id
        self._write_guardian_event_copies(
            guardian_uids=routing.guardian_uids,
            event_id=event_id,
            event=event,
        )

        tokens = self._guardian_tokens(routing.guardian_uids)
        notification_results = self._send_notifications(
            tokens=tokens,
            event_id=event_id,
            routing=routing,
            state=state,
            message=message,
            source=source,
            location=location,
        )

        self._mark_alert_sent(routing.device_id, state)

        return {
            'ok': True,
            'sent': notification_results['sentCount'] > 0,
            'state': state,
            'eventId': event_id,
            **notification_results,
        }

    def _resolve_routing(self, payload: dict[str, Any]) -> DeviceRouting:
        device_id = str(payload.get('deviceId') or self.config.device_id)
        patient_id = str(payload.get('patientId') or self.config.patient_id)
        guardian_uids = _uid_list(payload.get('guardianUids'))

        user_id = payload.get('userId')
        if isinstance(user_id, str) and user_id.strip():
            guardian_uids.append(user_id.strip())

        device_doc = self.db.collection('devices').document(device_id).get()
        if device_doc.exists:
            device_data = device_doc.to_dict() or {}
            patient_id = str(device_data.get('patientId') or patient_id)
            guardian_uids.extend(_uid_list(device_data.get('guardianUids')))

        patient_doc = self.db.collection('patients').document(patient_id).get()
        if patient_doc.exists:
            patient_data = patient_doc.to_dict() or {}
            guardian_uids.extend(_uid_list(patient_data.get('guardianUids')))

        guardian_uids.extend(self.config.guardian_uids)
        guardian_uids = sorted(set(uid for uid in guardian_uids if uid))

        return DeviceRouting(
            device_id=device_id,
            patient_id=patient_id,
            guardian_uids=guardian_uids,
        )

    def _update_device_status(
        self,
        routing: DeviceRouting,
        state: str,
        source: str,
        location: dict[str, Any] | None,
        score: Any,
    ) -> None:
        self.db.collection('devices').document(routing.device_id).set(
            {
                'deviceId': routing.device_id,
                'patientId': routing.patient_id,
                'guardianUids': routing.guardian_uids,
                'latestState': state,
                'latestSeverity': _legacy_severity(state),
                'latestScore': score,
                'latestSource': source,
                'latestLocation': location,
                'lastSeenAt': firestore.SERVER_TIMESTAMP,
            },
            merge=True,
        )

    def _cooldown_result(self, device_id: str, state: str) -> dict[str, Any] | None:
        key = f'{device_id}:{state}'
        now = time.monotonic()
        last_alert_at = self._last_alert_at_by_key.get(key, 0)
        cooldown_remaining = self.config.cooldown_seconds - (now - last_alert_at)

        if cooldown_remaining <= 0:
            return None

        return {
            'ok': True,
            'sent': False,
            'state': state,
            'reason': 'cooldown',
            'retryAfterSeconds': round(cooldown_remaining, 1),
        }

    def _mark_alert_sent(self, device_id: str, state: str) -> None:
        self._last_alert_at_by_key[f'{device_id}:{state}'] = time.monotonic()

    def _write_guardian_event_copies(
        self,
        guardian_uids: list[str],
        event_id: str,
        event: dict[str, Any],
    ) -> None:
        if not guardian_uids:
            return

        batch = self.db.batch()
        event_copy = {
            **event,
            'eventId': event_id,
            'sourceEventPath': f'fall_events/{event_id}',
        }
        for uid in guardian_uids:
            event_ref = (
                self.db.collection('users')
                .document(uid)
                .collection('fall_events')
                .document(event_id)
            )
            batch.set(event_ref, event_copy, merge=True)
        batch.commit()

    def _guardian_tokens(self, guardian_uids: list[str]) -> list[str]:
        tokens: list[str] = []
        for uid in guardian_uids:
            token_docs = (
                self.db.collection('users')
                .document(uid)
                .collection('fcmTokens')
                .stream()
            )
            for token_doc in token_docs:
                token = (token_doc.to_dict() or {}).get('token')
                if isinstance(token, str) and token:
                    tokens.append(token)
        return sorted(set(tokens))

    def _send_notifications(
        self,
        tokens: list[str],
        event_id: str,
        routing: DeviceRouting,
        state: str,
        message: str,
        source: str,
        location: dict[str, Any] | None,
    ) -> dict[str, Any]:
        if not tokens:
            return {
                'sentCount': 0,
                'failedCount': 0,
                'reason': 'no_guardian_tokens',
            }

        sent_count = 0
        failed_count = 0
        title = STATE_MESSAGES[state]['title']
        priority = STATE_MESSAGES[state]['priority']
        data = {
            'eventId': event_id,
            'deviceId': routing.device_id,
            'patientId': routing.patient_id,
            'state': state,
            'severity': _legacy_severity(state),
            'source': source,
            'message': message,
        }

        if location is not None:
            latitude = location.get('latitude')
            longitude = location.get('longitude')
            if latitude is not None:
                data['latitude'] = str(latitude)
            if longitude is not None:
                data['longitude'] = str(longitude)

        for token in tokens:
            try:
                messaging.send(
                    messaging.Message(
                        token=token,
                        notification=messaging.Notification(
                            title=title,
                            body=f'{routing.patient_id}: {message}',
                        ),
                        data=data,
                        android=messaging.AndroidConfig(
                            priority=priority,
                            notification=messaging.AndroidNotification(
                                channel_id='fall_alerts',
                                priority='max' if state == 'danger_fall' else 'default',
                                default_sound=True,
                            ),
                        ),
                    )
                )
                sent_count += 1
            except Exception as error:
                failed_count += 1
                print(f'FCM send failed for token={token[:12]}...: {error}')

        return {
            'sentCount': sent_count,
            'failedCount': failed_count,
        }


class AlertRequestHandler(BaseHTTPRequestHandler):
    bridge: FallAlertBridge

    def do_GET(self) -> None:
        if self.path == '/health':
            self._send_json({'ok': True})
            return

        self._send_json({'ok': False, 'error': 'Not found'}, HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        if self.path != '/fall':
            self._send_json({'ok': False, 'error': 'Not found'}, HTTPStatus.NOT_FOUND)
            return

        if not self._authorized():
            self._send_json(
                {'ok': False, 'error': 'Unauthorized'},
                HTTPStatus.UNAUTHORIZED,
            )
            return

        try:
            payload = self._read_json_body()
            result = self.bridge.process_alert(payload)
            self._send_json(result)
        except Exception as error:
            self._send_json(
                {'ok': False, 'error': str(error)},
                HTTPStatus.INTERNAL_SERVER_ERROR,
            )

    def log_message(self, format: str, *args: Any) -> None:
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        print(f'[{timestamp}] {self.address_string()} {format % args}')

    def _authorized(self) -> bool:
        expected = self.bridge.config.shared_secret
        if not expected:
            return True

        return self.headers.get('X-Fall-Token') == expected

    def _read_json_body(self) -> dict[str, Any]:
        content_length = int(self.headers.get('Content-Length', '0'))
        raw_body = self.rfile.read(content_length)
        if not raw_body:
            return {}

        parsed = json.loads(raw_body.decode('utf-8'))
        if not isinstance(parsed, dict):
            raise ValueError('JSON body must be an object.')

        return parsed

    def _send_json(
        self,
        payload: dict[str, Any],
        status: HTTPStatus = HTTPStatus.OK,
    ) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def _normalize_state(payload: dict[str, Any]) -> str:
    raw_state = (
        payload.get('state')
        or payload.get('fallState')
        or payload.get('severity')
        or 'danger_fall'
    )
    state = str(raw_state).strip().lower()
    aliases = {
        'safe': 'safe_fall',
        'danger': 'danger_fall',
        'event_safe': 'safe_fall',
        'event_danger': 'danger_fall',
    }
    state = aliases.get(state, state)

    if state not in VALID_STATES:
        raise ValueError(
            f'Unsupported state={raw_state!r}. '
            'Expected one of normal, safe_fall, danger_fall.'
        )

    return state


def _legacy_severity(state: str) -> str:
    if state == 'danger_fall':
        return 'danger'
    if state == 'safe_fall':
        return 'safe'
    return 'normal'


def _location_from_payload(payload: dict[str, Any]) -> dict[str, Any] | None:
    raw = payload.get('location')
    if isinstance(raw, dict):
        location = _normalize_location_map(raw)
        if location is not None:
            return location

    location = _normalize_location_map(payload)
    return location


def _normalize_location_map(raw: dict[str, Any]) -> dict[str, Any] | None:
    latitude = raw.get('latitude', raw.get('lat'))
    longitude = raw.get('longitude', raw.get('lng', raw.get('lon')))
    if latitude is None or longitude is None:
        return None

    try:
        location: dict[str, Any] = {
            'latitude': float(latitude),
            'longitude': float(longitude),
        }
    except (TypeError, ValueError):
        return None

    altitude = raw.get('altitude', raw.get('alt'))
    accuracy = raw.get('accuracy')
    if altitude is not None:
        try:
            location['altitude'] = float(altitude)
        except (TypeError, ValueError):
            pass
    if accuracy is not None:
        try:
            location['accuracy'] = float(accuracy)
        except (TypeError, ValueError):
            pass

    location['source'] = str(raw.get('source') or raw.get('locationSource') or 'payload')
    return location


def _location_from_gpsd_line(line: bytes) -> dict[str, Any] | None:
    try:
        payload = json.loads(line.decode('utf-8'))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None

    if payload.get('class') != 'TPV':
        return None

    if int(payload.get('mode', 0)) < 2:
        return None

    location = _normalize_location_map(
        {
            'latitude': payload.get('lat'),
            'longitude': payload.get('lon'),
            'altitude': payload.get('alt'),
            'accuracy': payload.get('eph'),
            'source': 'gpsd',
        }
    )
    return location


def _uid_list(value: Any) -> list[str]:
    if isinstance(value, str):
        return _csv(value)
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    return []


def _csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(',') if item.strip()]


def _truthy(value: str) -> bool:
    return value.strip().lower() in {'1', 'true', 'yes', 'y', 'on'}


def run_server(bridge: FallAlertBridge) -> None:
    AlertRequestHandler.bridge = bridge
    server = ThreadingHTTPServer(
        (bridge.config.host, bridge.config.port),
        AlertRequestHandler,
    )

    print(
        f'Fall alert bridge listening on '
        f'http://{bridge.config.host}:{bridge.config.port}'
    )
    server.serve_forever()


def simulate_alert(bridge: FallAlertBridge, state: str) -> None:
    result = bridge.process_alert(
        {
            'deviceId': bridge.config.device_id,
            'patientId': bridge.config.patient_id,
            'state': state,
            'score': 0.97,
            'source': 'simulation',
        }
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))


def main() -> None:
    parser = argparse.ArgumentParser(description='Firebase fall alert bridge.')
    parser.add_argument('--simulate', action='store_true')
    parser.add_argument(
        '--state',
        choices=sorted(VALID_STATES),
        default='danger_fall',
    )
    args = parser.parse_args()

    config = BridgeConfig()
    bridge = FallAlertBridge(config)

    if args.simulate:
        simulate_alert(bridge, args.state)
    else:
        run_server(bridge)


if __name__ == '__main__':
    main()
