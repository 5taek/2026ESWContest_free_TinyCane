# ESP32 direct Firebase setup

The production paths are:

`ESP32 -> HTTPS Cloud Function -> Firestore + FCM -> Care app`

`Raspberry Pi GPSD -> Firestore device location -> Care app`

The ESP never contains a Firebase service-account key. A per-device token
authenticates the HTTPS request, while the Cloud Function performs privileged
Firestore writes and sends push notifications.

## 1. Register the function secret

Choose a long random token for each device. The same token is configured on the
ESP in step 3.

```powershell
cd Care_app
firebase functions:secrets:set DEVICE_TOKENS
```

Enter JSON when prompted:

```json
{"cane-001":"replace-with-at-least-32-random-characters"}
```

## 2. Deploy

```powershell
cd Care_app\functions
npm install
cd ..
firebase deploy --only functions:reportFall
```

Copy the deployed `reportFall` URL. The function is deployed in
`asia-northeast3`.

The Firestore document `devices/cane-001` must already contain a `patientId`
and a non-empty `guardianUids` array. The Care app registration flow creates
that link.

## 3. Configure and build the ESP

```powershell
cd Fall_detection
idf.py menuconfig
```

Under **Fall detection Firebase connection**, set:

- Wi-Fi SSID and password
- deployed `reportFall` URL
- device ID, for example `cane-001`
- the matching device token

Then build the inference firmware:

```powershell
idf.py reconfigure build flash monitor
```

To build the inference firmware, explicitly configure
`FALL_DETECTION_EDGE_IMPULSE_DATA_FORWARDER=OFF`. The repository default
remains the Edge Impulse data forwarder mode (`ON`).

## Event contract

- `safe_fall`: an event was detected, but pickup motion occurred within 10 s
- `danger_fall`: no pickup motion occurred within 10 s

Only `danger_fall` sends FCM. Both states are written to the global event,
guardian event copies, and the device's latest status.

## Raspberry Pi GPS

The Raspberry Pi remains responsible for GPS only. It no longer receives the
fall signal from the ESP.

```powershell
cd Care_app\raspberry_pi
pip install -r requirements.txt
python gps_location_publisher.py
```

Configure `FIREBASE_SERVICE_ACCOUNT`, `DEVICE_ID`, and optionally
`GPSD_HOST`, `GPSD_PORT`, and `GPS_PUBLISH_INTERVAL_SECONDS`. The publisher
updates `devices/{deviceId}.latestLocation`. When a fall report arrives, the
Cloud Function copies that latest Raspberry Pi location into the fall event.
