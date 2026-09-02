const firebaseToolsLib =
  process.env.FIREBASE_TOOLS_LIB ||
  'C:/nvm4w/nodejs/node_modules/firebase-tools/lib';
const auth = require(`${firebaseToolsLib}/auth`);
const { requireAuth } = require(`${firebaseToolsLib}/requireAuth`);
const { Client } = require(`${firebaseToolsLib}/apiv2`);

const project = process.env.FIREBASE_PROJECT || 'tiny-vision-cane-6e80d';
const deviceId = process.env.DEVICE_ID || 'cane-001';
const state = process.env.STATE || 'danger_fall';
const patientFallback = process.env.PATIENT_ID || 'user';
const repairDeviceOnly = process.argv.includes('--repair-device-only');

const defaultLocation = {
  latitude: 37.5638209021699,
  longitude: 126.96237696604,
  source: 'simulation',
};

function stringValue(value) {
  return { stringValue: String(value) };
}

function arrayValue(items) {
  return { arrayValue: { values: items.map(stringValue) } };
}

function timestampValue(value) {
  return { timestampValue: value };
}

function doubleValue(value) {
  return { doubleValue: value };
}

function mapValue(fields) {
  return { mapValue: { fields } };
}

function fieldString(fields, key) {
  return fields?.[key]?.stringValue || '';
}

function fieldStringArray(fields, key) {
  return (fields?.[key]?.arrayValue?.values || [])
    .map((value) => value.stringValue)
    .filter(Boolean);
}

async function main() {
  const account = auth.getGlobalDefaultAccount();
  if (!account) {
    throw new Error('Firebase CLI login account not found.');
  }

  const options = { project, nonInteractive: true };
  auth.setActiveAccount(options, account);
  await requireAuth(options);

  const client = new Client({
    urlPrefix: 'https://firestore.googleapis.com/v1',
    auth: true,
  });
  const base = `/projects/${project}/databases/(default)/documents`;

  async function getDocument(path) {
    const response = await client.get(path, {
      resolveOnHTTPError: true,
      skipLog: { resBody: true },
    });
    return response.status === 200 ? response.body : null;
  }

  const device = await getDocument(`${base}/devices/${deviceId}`);
  if (!device) {
    throw new Error(`devices/${deviceId} not found.`);
  }

  const deviceFields = device.fields || {};
  const patientId = fieldString(deviceFields, 'patientId') || patientFallback;
  let guardianUids = fieldStringArray(deviceFields, 'guardianUids');

  const patient = await getDocument(`${base}/patients/${patientId}`);
  if (patient) {
    guardianUids = [
      ...guardianUids,
      ...fieldStringArray(patient.fields || {}, 'guardianUids'),
    ];
  }
  guardianUids = [...new Set(guardianUids.filter(Boolean))];
  if (guardianUids.length === 0) {
    throw new Error(`No guardianUids found for ${deviceId}/${patientId}.`);
  }

  const now = new Date().toISOString();
  const eventId = `test-${state}-${Date.now()}`;
  const severity = state === 'danger_fall'
    ? 'danger'
    : state === 'safe_fall'
      ? 'safe'
      : 'normal';
  const status = state === 'danger_fall' ? 'active' : 'resolved';
  const location = mapValue({
    latitude: doubleValue(defaultLocation.latitude),
    longitude: doubleValue(defaultLocation.longitude),
    source: stringValue(defaultLocation.source),
  });
  const fields = {
    eventId: stringValue(eventId),
    deviceId: stringValue(deviceId),
    patientId: stringValue(patientId),
    guardianUids: arrayValue(guardianUids),
    state: stringValue(state),
    severity: stringValue(severity),
    source: stringValue('simulation'),
    score: doubleValue(0.97),
    location,
    status: stringValue(status),
    createdAt: timestampValue(now),
    deviceTime: stringValue(now),
  };

  const writes = [
    {
      update: {
        name: `projects/${project}/databases/(default)/documents/devices/${deviceId}`,
        fields: {
          deviceId: stringValue(deviceId),
          patientId: stringValue(patientId),
          guardianUids: arrayValue(guardianUids),
          latestState: stringValue(state),
          latestSeverity: stringValue(severity),
          latestSource: stringValue('simulation'),
          latestScore: doubleValue(0.97),
          latestLocation: location,
          lastSeenAt: timestampValue(now),
        },
      },
      updateMask: {
        fieldPaths: [
          'deviceId',
          'patientId',
          'guardianUids',
          'latestState',
          'latestSeverity',
          'latestSource',
          'latestScore',
          'latestLocation',
          'lastSeenAt',
        ],
      },
    },
  ];

  if (!repairDeviceOnly) {
    writes.unshift({
      update: {
        name: `projects/${project}/databases/(default)/documents/fall_events/${eventId}`,
        fields,
      },
    });

    for (const uid of guardianUids) {
      writes.push({
        update: {
          name: `projects/${project}/databases/(default)/documents/users/${uid}/fall_events/${eventId}`,
          fields: {
            ...fields,
            sourceEventPath: stringValue(`fall_events/${eventId}`),
          },
        },
      });
    }
  }

  await client.post(
    `/projects/${project}/databases/(default)/documents:commit`,
    { writes },
    { skipLog: { body: true, resBody: true } },
  );

  console.log(JSON.stringify({
    ok: true,
    repairDeviceOnly,
    eventId: repairDeviceOnly ? null : eventId,
    state,
    deviceId,
    patientId,
    guardianCount: guardianUids.length,
  }, null, 2));
}

main().catch((error) => {
  console.error(error.message || error);
  process.exit(1);
});
