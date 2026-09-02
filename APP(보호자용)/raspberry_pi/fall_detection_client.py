import argparse
import json
import os
import urllib.error
import urllib.request
from typing import Any


def send_fall_alert(
    url: str,
    state: str,
    device_id: str = 'cane-001',
    patient_id: str = 'patient-001',
    user_id: str | None = None,
    score: float | None = None,
    source: str = 'fall_detection',
    message: str | None = None,
    latitude: float | None = None,
    longitude: float | None = None,
    secret: str | None = None,
) -> int:
    payload: dict[str, Any] = {
        'deviceId': device_id,
        'patientId': patient_id,
        'state': state,
        'source': source,
    }

    if user_id:
        payload['userId'] = user_id
    if score is not None:
        payload['score'] = score
    if message:
        payload['message'] = message
    if latitude is not None and longitude is not None:
        payload['location'] = {
            'latitude': latitude,
            'longitude': longitude,
            'source': 'payload',
        }

    headers = {'Content-Type': 'application/json'}
    if secret:
        headers['X-Fall-Token'] = secret

    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode('utf-8'),
        headers=headers,
        method='POST',
    )

    with urllib.request.urlopen(request, timeout=5) as response:
        response.read()
        return response.status


def main() -> None:
    parser = argparse.ArgumentParser(description='Send a test fall state.')
    parser.add_argument('--url', default='http://127.0.0.1:8080/fall')
    parser.add_argument(
        '--state',
        choices=['normal', 'safe_fall', 'danger_fall'],
        default='danger_fall',
    )
    parser.add_argument('--secret', default=os.getenv('BRIDGE_SHARED_SECRET'))
    parser.add_argument('--device-id', default='cane-001')
    parser.add_argument('--patient-id', default='patient-001')
    parser.add_argument('--user-id')
    parser.add_argument('--score', type=float, default=0.97)
    parser.add_argument('--latitude', type=float)
    parser.add_argument('--longitude', type=float)
    args = parser.parse_args()

    try:
        status = send_fall_alert(
            url=args.url,
            state=args.state,
            device_id=args.device_id,
            patient_id=args.patient_id,
            user_id=args.user_id,
            score=args.score,
            latitude=args.latitude,
            longitude=args.longitude,
            secret=args.secret,
        )
        print(f'Alert sent. HTTP {status}')
    except urllib.error.URLError as error:
        raise SystemExit(f'Failed to send alert: {error}') from error


if __name__ == '__main__':
    main()
