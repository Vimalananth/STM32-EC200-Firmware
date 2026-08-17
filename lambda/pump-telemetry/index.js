/**
 * pump-telemetry — AWS Lambda
 *
 * Triggered by AWS IoT Core Rules Engine on topic: pump/#
 * IoT Rule SQL:
 *   SELECT *, topic() as topic_name FROM 'pump/#'
 *
 * Writes device telemetry MQTT → Firebase Realtime Database.
 * Sends FCM push notifications for alerts and relay events.
 *
 * Environment variables (set in Lambda console):
 *   FIREBASE_SERVICE_ACCOUNT  — Firebase service account JSON string
 *   FIREBASE_DB_URL           — Firebase Realtime DB URL
 */

const admin = require('firebase-admin');

// ── Firebase init (runs once per Lambda container) ────────────────────────────
let firebaseReady = false;
function initFirebase() {
  if (firebaseReady) return;
  const serviceAccount = JSON.parse(process.env.FIREBASE_SERVICE_ACCOUNT);
  admin.initializeApp({
    credential:  admin.credential.cert(serviceAccount),
    databaseURL: process.env.FIREBASE_DB_URL ||
                 'https://pump-controller-4398d-default-rtdb.firebaseio.com'
  });
  firebaseReady = true;
}

// ── Topic → Firebase routing map ──────────────────────────────────────────────
const TOPIC_MAP = {
  // ── site01 ──
  'pump/01/status':       { fbPath: 'sites/site01/line01/pump01/status',     method: 'set',  label: 'Site01 Pump01' },
  'pump/01/alerts':       { fbPath: 'sites/site01/line01/pump01/alerts',     method: 'push', label: 'Site01 Pump01' },
  'pump/01/log':          { fbPath: 'sites/site01/line01/pump01/alerts',     method: 'push', label: 'Site01 Pump01' },
  'pump/01/vlog':         { fbPath: 'sites/site01/line01/pump01/logs',       method: 'push' },
  'pump/01/ota/status':   { fbPath: 'sites/site01/line01/pump01/ota_status', method: 'set'  },
  'pump/01/slave_status': { fbPath: 'sites/site01/line02/pump01/status',     method: 'set'  },
  'pump/01/slave_vlog':   { fbPath: 'sites/site01/line02/pump01/logs',       method: 'push' },
  'pump/01/slave_alerts': { fbPath: 'sites/site01/line02/pump01/alerts',     method: 'push', label: 'Site01 Slave' },

  'pump/02/status':       { fbPath: 'sites/site01/line01/pump02/status',     method: 'set',  label: 'Site01 Pump02' },
  'pump/02/alerts':       { fbPath: 'sites/site01/line01/pump02/alerts',     method: 'push', label: 'Site01 Pump02' },
  'pump/02/log':          { fbPath: 'sites/site01/line01/pump02/alerts',     method: 'push', label: 'Site01 Pump02' },
  'pump/02/vlog':         { fbPath: 'sites/site01/line01/pump02/logs',       method: 'push' },
  'pump/02/ota/status':   { fbPath: 'sites/site01/line01/pump02/ota_status', method: 'set'  },

  // ── site02 ──
  'pump/03/status':       { fbPath: 'sites/site02/line01/pump01/status',     method: 'set',  label: 'Site02 Pump01' },
  'pump/03/alerts':       { fbPath: 'sites/site02/line01/pump01/alerts',     method: 'push', label: 'Site02 Pump01' },
  'pump/03/log':          { fbPath: 'sites/site02/line01/pump01/alerts',     method: 'push', label: 'Site02 Pump01' },
  'pump/03/vlog':         { fbPath: 'sites/site02/line01/pump01/logs',       method: 'push' },
  'pump/03/ota/status':   { fbPath: 'sites/site02/line01/pump01/ota_status', method: 'set'  },
  'pump/03/slave_status': { fbPath: 'sites/site02/line02/pump01/status',     method: 'set'  },
  'pump/03/slave_vlog':   { fbPath: 'sites/site02/line02/pump01/logs',       method: 'push' },
  'pump/03/slave_alerts': { fbPath: 'sites/site02/line02/pump01/alerts',     method: 'push', label: 'Site02 Slave' },

  'pump/04/status':       { fbPath: 'sites/site02/line01/pump02/status',     method: 'set',  label: 'Site02 Pump02' },
  'pump/04/alerts':       { fbPath: 'sites/site02/line01/pump02/alerts',     method: 'push', label: 'Site02 Pump02' },
  'pump/04/log':          { fbPath: 'sites/site02/line01/pump02/alerts',     method: 'push', label: 'Site02 Pump02' },
  'pump/04/vlog':         { fbPath: 'sites/site02/line01/pump02/logs',       method: 'push' },
  'pump/04/ota/status':   { fbPath: 'sites/site02/line01/pump02/ota_status', method: 'set'  },
};

// ── Timestamp normalisation ───────────────────────────────────────────────────
function normaliseTs(payload) {
  if (payload.ts && payload.ts > 0 && payload.ts < 1e12) payload.ts = payload.ts * 1000;
  if (!payload.ts) payload.ts = Date.now();
  return payload;
}

// ── Run time formatter ────────────────────────────────────────────────────────
function fmtRunTime(s) {
  if (s < 60)   return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${s % 60}s`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

// ── FCM topic from Firebase path ──────────────────────────────────────────────
// e.g. sites/site02/line01/pump01/alerts → site02_line01_pump01
function fcmTopic(fbPath) {
  return fbPath.replace('sites/', '').replace('/alerts', '').replace(/\//g, '_');
}

// ── Send FCM push notification ────────────────────────────────────────────────
async function sendFCM(topic, title, body) {
  try {
    await admin.messaging().send({
      topic,
      notification: { title, body },
      android: { priority: 'high', notification: { channelId: 'pump_alerts' } },
    });
    console.log(`[FCM] -> ${topic}: ${title}`);
  } catch (e) {
    console.error('[FCM] Error:', e.message);
  }
}

// ── FCM alert logic per topic type ────────────────────────────────────────────
async function handleAlerts(mqttTopic, fbPath, payload, label) {
  if (!label) return; // vlog / ota_status — no FCM needed

  const ft = fcmTopic(fbPath);

  // Relay on/off events (from /log topic)
  if (mqttTopic.endsWith('/log')) {
    if (payload.event === 'on') {
      await sendFCM(ft, `${label} Started`, `Turned ON (${payload.reason || 'manual'})`);
    } else if (payload.event === 'off') {
      const runStr = payload.run_s ? ` — ran ${fmtRunTime(payload.run_s)}` : '';
      await sendFCM(ft, `${label} Stopped`, `Turned OFF (${payload.reason || 'manual'})${runStr}`);
    }
    return;
  }

  // Protection trips (from /alerts topic)
  if (mqttTopic.endsWith('/alerts')) {
    if (payload.dry_run_trip) await sendFCM(ft, `${label} — Dry Run Trip`,  'Pump tripped: no water detected');
    if (payload.overvoltage)  await sendFCM(ft, `${label} — Overvoltage`,   'Pump tripped: voltage too high');
    if (payload.undervoltage) await sendFCM(ft, `${label} — Undervoltage`,  'Pump tripped: voltage too low');
    if (payload.phase_loss)   await sendFCM(ft, `${label} — Phase Loss`,    'Pump tripped: phase loss detected');
    return;
  }

  // Slave online/offline (from /slave_alerts topic)
  if (mqttTopic.endsWith('/slave_alerts')) {
    if (payload.event === 'slave_offline') await sendFCM(ft, `${label} Offline`, 'Blue Pill not responding via LoRa');
    if (payload.event === 'slave_online')  await sendFCM(ft, `${label} Online`,  'Blue Pill reconnected via LoRa');
  }
}

// ── Lambda handler ─────────────────────────────────────────────────────────────
exports.handler = async (event) => {
  initFirebase();
  const db = admin.database();

  const topic = event.topic_name;
  if (!topic) {
    console.error('[ERROR] No topic_name in event:', JSON.stringify(event));
    return { statusCode: 400, body: 'Missing topic_name' };
  }

  const route = TOPIC_MAP[topic];
  if (!route) {
    console.log(`[SKIP] No route for topic: ${topic}`);
    return { statusCode: 200, body: 'No route' };
  }

  const { topic_name, ...payload } = event;
  normaliseTs(payload);

  console.log(`[ROUTE] ${topic} -> ${route.fbPath} (${route.method})`);

  try {
    const ref = db.ref(route.fbPath);
    if (route.method === 'set') {
      await ref.set(payload);
    } else {
      await ref.push(payload);
    }
    console.log(`[OK] Written to ${route.fbPath}`);

    // Send FCM alerts (non-blocking — don't fail if FCM fails)
    await handleAlerts(topic, route.fbPath, payload, route.label);

    return { statusCode: 200, body: 'OK' };
  } catch (err) {
    console.error(`[ERROR] Firebase write failed: ${err.message}`);
    throw err;
  }
};
