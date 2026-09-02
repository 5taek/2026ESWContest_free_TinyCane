"use strict";

const {onRequest} = require("firebase-functions/v2/https");
const {defineSecret} = require("firebase-functions/params");
const {initializeApp} = require("firebase-admin/app");
const {getFirestore, FieldValue} = require("firebase-admin/firestore");
const {getMessaging} = require("firebase-admin/messaging");
const crypto = require("crypto");

initializeApp();
const deviceTokens = defineSecret("DEVICE_TOKENS");
const validStates = new Set(["normal", "safe_fall", "danger_fall"]);
const maxMulticastTokens = 500;
const maxFcmAttempts = 3;
const retryableMessagingCodes = new Set([
  "messaging/internal-error",
  "messaging/server-unavailable",
  "messaging/unknown-error",
  "messaging/quota-exceeded",
]);

function equalToken(actual, expected) {
  const left = Buffer.from(actual || "");
  const right = Buffer.from(expected || "");
  return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function metadata(state) {
  if (state === "danger_fall") {
    return {severity: "danger", status: "active", title: "위험 낙상 감지",
      body: "낙상 후 지팡이를 다시 들지 못했습니다. 즉시 확인이 필요합니다."};
  }
  if (state === "safe_fall") {
    return {severity: "safe", status: "resolved", title: "안전 확인",
      body: "낙상 후보 감지 후 지팡이를 다시 든 것으로 판단했습니다."};
  }
  return {severity: "normal", status: "resolved", title: "정상 상태",
    body: "기기가 정상 상태를 보고했습니다."};
}

async function loadFcmTokens(db, uids) {
  const tokens = new Set();
  for (const uid of uids) {
    const snapshot = await db.collection("users").doc(uid)
        .collection("fcmTokens").get();
    snapshot.forEach((doc) => {
      const token = doc.get("token");
      if (typeof token === "string" && token) tokens.add(token);
    });
  }
  return [...tokens];
}

function wait(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function chunks(values, size) {
  const result = [];
  for (let index = 0; index < values.length; index += size) {
    result.push(values.slice(index, index + size));
  }
  return result;
}

function isRetryableMessagingError(error) {
  return retryableMessagingCodes.has(error && error.code);
}

async function sendDangerNotificationWithRetry(message, tokens) {
  let pendingTokens = [...tokens];
  let sentCount = 0;
  let permanentFailureCount = 0;
  let attempts = 0;

  while (pendingTokens.length > 0 && attempts < maxFcmAttempts) {
    attempts++;
    const retryTokens = [];

    for (const tokenChunk of chunks(pendingTokens, maxMulticastTokens)) {
      try {
        const result = await getMessaging().sendEachForMulticast({
          ...message,
          tokens: tokenChunk,
        });
        sentCount += result.successCount;
        result.responses.forEach((sendResponse, index) => {
          if (sendResponse.success) return;
          if (isRetryableMessagingError(sendResponse.error) &&
              attempts < maxFcmAttempts) {
            retryTokens.push(tokenChunk[index]);
          } else {
            permanentFailureCount++;
          }
        });
      } catch (error) {
        console.error(`reportFall: FCM attempt ${attempts} failed`, error);
        if (attempts < maxFcmAttempts) {
          retryTokens.push(...tokenChunk);
        } else {
          permanentFailureCount += tokenChunk.length;
        }
      }
    }

    pendingTokens = retryTokens;
    if (pendingTokens.length > 0 && attempts < maxFcmAttempts) {
      await wait(500 * (2 ** (attempts - 1)));
    }
  }

  return {sentCount, failedCount: permanentFailureCount, attempts};
}

exports.reportFall = onRequest(
    {region: "asia-northeast3", secrets: [deviceTokens]},
    async (request, response) => {
      if (request.method !== "POST") {
        response.set("Allow", "POST").status(405).json({ok: false});
        return;
      }
      const body = request.body || {};
      const deviceId = String(body.deviceId || "").trim();
      const state = String(body.state || "").trim();
      let configuredTokens;
      try {
        configuredTokens = JSON.parse(deviceTokens.value());
      } catch {
        response.status(500).json({ok: false, error: "invalid server config"});
        return;
      }
      // deviceId becomes a Firestore document ID below (db.collection(...)
      // .doc(deviceId)); an oversized value would surface as an uncaught
      // "invalid document ID" exception -- generic 400 here instead.
      if (!deviceId || deviceId.length > 128 || !validStates.has(state)) {
        response.status(400).json({ok: false, error: "invalid payload"});
        return;
      }
      if (!equalToken(request.get("X-Device-Token"), configuredTokens[deviceId])) {
        response.status(401).json({ok: false, error: "unauthorized"});
        return;
      }

      const db = getFirestore();
      const deviceRef = db.collection("devices").doc(deviceId);
      const deviceSnapshot = await deviceRef.get();
      if (!deviceSnapshot.exists) {
        response.status(404).json({ok: false, error: "unknown device"});
        return;
      }
      const device = deviceSnapshot.data() || {};
      const patientId = String(device.patientId || "").trim();
      const guardianUids = Array.isArray(device.guardianUids) ?
        [...new Set(device.guardianUids.filter((uid) => typeof uid === "string"))] : [];
      if (!patientId || guardianUids.length === 0) {
        response.status(409).json({ok: false, error: "device is not linked"});
        return;
      }

      // typeof body.reason === "string" alone let an unbounded string reach
      // Firestore; a misbehaving/compromised device could bloat every
      // event document indefinitely.
      const rawReason = typeof body.reason === "string" ? body.reason.trim() : null;
      const reason = rawReason ? rawReason.slice(0, 200) : null;
      // The firmware only ever sends 0.0 or 1.0 today, but the handler
      // itself enforced no range at all -- clamp to a sane confidence
      // range instead of trusting the payload verbatim.
      const rawScore = typeof body.score === "number" && Number.isFinite(body.score) ?
        body.score : null;
      const score = rawScore === null ? null : Math.min(1, Math.max(0, rawScore));

      const info = metadata(state);
      const eventRef = db.collection("fall_events").doc();
      const event = {
        eventId: eventRef.id, deviceId, patientId, guardianUids, state,
        severity: info.severity, status: info.status, source: "esp32",
        reason, score,
        location: device.latestLocation || null,
        message: info.body, createdAt: FieldValue.serverTimestamp(),
        deviceTimeMs: Number.isFinite(body.deviceTimeMs) ? body.deviceTimeMs : null,
        notificationStatus: state === "danger_fall" ? "pending" : "not_applicable",
      };
      const batch = db.batch();
      batch.set(eventRef, event);
      for (const uid of guardianUids) {
        batch.set(db.collection("users").doc(uid).collection("fall_events")
            .doc(eventRef.id), {...event, sourceEventPath: eventRef.path});
      }
      batch.set(deviceRef, {
        latestState: state, latestSeverity: info.severity,
        latestScore: event.score, latestSource: "esp32",
        lastSeenAt: FieldValue.serverTimestamp(),
      }, {merge: true});

      try {
        await batch.commit();
      } catch (error) {
        console.error("reportFall: Firestore write failed", error);
        response.status(500).json({ok: false, error: "storage write failed"});
        return;
      }

      let sentCount = 0;
      if (state === "danger_fall") {
        let notificationStatus = "no_tokens";
        let failedCount = 0;
        let attempts = 0;
        try {
          const tokens = await loadFcmTokens(db, guardianUids);
          if (tokens.length) {
            const result = await sendDangerNotificationWithRetry({
              notification: {title: info.title, body: `${patientId}: ${info.body}`},
              data: {eventId: eventRef.id, deviceId, patientId, state},
              android: {priority: "high", notification: {
                channelId: "fall_alerts", priority: "max", sound: "default"}},
            }, tokens);
            sentCount = result.sentCount;
            failedCount = result.failedCount;
            attempts = result.attempts;
            notificationStatus = failedCount === 0 ? "sent" :
              sentCount > 0 ? "partially_failed" : "failed";
          }
        } catch (error) {
          // The event is already durably written at this point (Firestore
          // commit above succeeded) -- a push-notification failure must not
          // be reported back as a general request failure, or the device
          // would treat an already-recorded fall as unreported and retry.
          console.error("reportFall: FCM send failed", error);
          notificationStatus = "failed";
          failedCount = 1;
        }

        try {
          await eventRef.set({
            notificationStatus,
            notificationSentCount: sentCount,
            notificationFailedCount: failedCount,
            notificationAttempts: attempts,
            notificationCompletedAt: FieldValue.serverTimestamp(),
          }, {merge: true});
        } catch (error) {
          console.error("reportFall: notification result write failed", error);
        }
      }
      response.status(201).json({ok: true, eventId: eventRef.id, state, sentCount});
    },
);
