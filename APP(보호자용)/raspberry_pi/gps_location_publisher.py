from __future__ import annotations

import json
import os
import socket
import time
from typing import Any

from dotenv import load_dotenv
import firebase_admin
from firebase_admin import credentials, firestore


load_dotenv()


def read_gpsd_location(host: str, port: int, timeout: float) -> dict[str, Any] | None:
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(b'?WATCH={"enable":true,"json":true};\n')
            deadline = time.monotonic() + timeout
            buffer = b''
            while time.monotonic() < deadline:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                lines = buffer.split(b'\n')
                buffer = lines.pop()
                for line in lines:
                    try:
                        report = json.loads(line.decode('utf-8'))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        continue
                    if report.get('class') != 'TPV':
                        continue
                    latitude = report.get('lat')
                    longitude = report.get('lon')
                    if isinstance(latitude, (int, float)) and isinstance(
                        longitude, (int, float)
                    ):
                        return {
                            'latitude': float(latitude),
                            'longitude': float(longitude),
                            'altitude': report.get('alt'),
                            'accuracy': report.get('eph'),
                            'source': 'raspberry_pi_gpsd',
                            'gpsTime': report.get('time'),
                            'updatedAt': firestore.SERVER_TIMESTAMP,
                        }
    except OSError as error:
        print(f'GPSD read failed: {error}', flush=True)
    return None


def main() -> None:
    service_account = os.getenv('FIREBASE_SERVICE_ACCOUNT')
    device_id = os.getenv('DEVICE_ID', 'cane-001')
    gpsd_host = os.getenv('GPSD_HOST', '127.0.0.1')
    gpsd_port = int(os.getenv('GPSD_PORT', '2947'))
    gps_timeout = float(os.getenv('GPS_TIMEOUT_SECONDS', '2.0'))
    publish_interval = float(os.getenv('GPS_PUBLISH_INTERVAL_SECONDS', '10'))

    if not service_account:
        raise RuntimeError('FIREBASE_SERVICE_ACCOUNT is required.')
    if not firebase_admin._apps:
        firebase_admin.initialize_app(credentials.Certificate(service_account))
    db = firestore.client()
    device_ref = db.collection('devices').document(device_id)

    print(f'Publishing GPSD location for devices/{device_id}', flush=True)
    while True:
        location = read_gpsd_location(gpsd_host, gpsd_port, gps_timeout)
        if location is not None:
            device_ref.set(
                {
                    'latestLocation': location,
                    'latestLocationSource': 'raspberry_pi_gpsd',
                    'lastLocationAt': firestore.SERVER_TIMESTAMP,
                },
                merge=True,
            )
            print(
                f"GPS published: {location['latitude']}, {location['longitude']}",
                flush=True,
            )
        time.sleep(publish_interval)


if __name__ == '__main__':
    main()
