/**
 * pump Firebase Cloud Functions
 *
 * Forwards Firebase RTDB changes → AWS IoT Core MQTT:
 *   cmd      — relay ON/OFF commands          (qos=1, no retain)
 *   settings — protection thresholds          (qos=1, retain=true)
 *   ota      — OTA firmware trigger           (qos=1, retain=true)
 *   lora_ota — Blue Pill OTA via master LoRa  (qos=1, no retain)
 *
 * Credentials loaded from .env:
 *   AWS_KEY_ID   — IAM Access Key (iot:Publish on pump/*)
 *   AWS_SECRET   — IAM Secret Access Key
 */

const { onValueWritten } = require('firebase-functions/v2/database');
const { IoTDataPlaneClient, PublishCommand } = require('@aws-sdk/client-iot-data-plane');

const IOT_ENDPOINT = 'a1dlr34wqh7zt3-ats.iot.us-east-1.amazonaws.com';
const AWS_REGION   = 'us-east-1';

function makeClient() {
  return new IoTDataPlaneClient({
    region:   AWS_REGION,
    endpoint: `https://${IOT_ENDPOINT}`,
    credentials: {
      accessKeyId:     process.env.AWS_KEY_ID,
      secretAccessKey: process.env.AWS_SECRET,
    },
  });
}

// ── Publish helper (retain=true for settings/OTA so device gets on reconnect) ──
async function publish(mqttTopic, payload, retain = false) {
  const body = typeof payload === 'string' ? payload : JSON.stringify(payload);
  await makeClient().send(new PublishCommand({
    topic:   mqttTopic,
    qos:     1,
    retain,
    payload: Buffer.from(body),
  }));
  console.log(`[${retain ? 'RETAIN' : 'PUB'}] -> ${mqttTopic}: ${body}`);
}

// ── CMD — relay ON/OFF ────────────────────────────────────────────────────────
async function forwardCmd(event, mqttTopic) {
  const payload = event.data.after.val();
  if (!payload) return;
  await publish(mqttTopic, payload);
}

// ── SETTINGS — protection thresholds ─────────────────────────────────────────
async function forwardSettings(event, mqttTopic) {
  const s = event.data.after.val();
  if (!s) return;

  // Validate: ov > uv > pl required
  if (s.ov !== undefined && s.uv !== undefined && s.pl !== undefined) {
    if (s.ov <= s.uv || s.uv <= s.pl) {
      console.error(`[SETTINGS] Rejected — invalid: ov(${s.ov}) > uv(${s.uv}) > pl(${s.pl}) required`);
      return;
    }
  }

  const out = {
    ov:     s.ov     ?? 480,
    uv:     s.uv     ?? 340,
    pl:     s.pl     ?? 200,
    dry_i:  s.dry_i  ?? 1.5,
    dry_t:  s.dry_t  ?? 8,
    dry_en: s.dry_en ?? 1,
    uv_rst: s.uv_rst ?? 300,
  };
  if (s.hp      != null) out.hp      = s.hp;
  if (s.start_t != null) out.start_t = s.start_t;

  await publish(mqttTopic, out, true); // retain=true — device gets on reconnect
}

// ── OTA — firmware update trigger ─────────────────────────────────────────────
async function forwardOta(event, mqttTopic) {
  const cmd = event.data.after.val();

  if (!cmd || !cmd.url) {
    // Clear retained OTA so device doesn't re-trigger on reconnect
    await publish(mqttTopic, '', true);
    console.log(`[OTA] Cleared retain on ${mqttTopic}`);
    return;
  }

  await publish(mqttTopic, { url: cmd.url }, true); // retain=true
}

// ── Trigger options ───────────────────────────────────────────────────────────
const OPTS = {
  region:   'us-central1',
  instance: 'pump-controller-4398d-default-rtdb',
};

// ════════════════════════════════════════════════════════════════════════════════
// site01 — pump01 & pump02 on same device (PUMP_ID=01)
// ════════════════════════════════════════════════════════════════════════════════

// CMD
exports.cmdSite01Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump01/cmd' },
  (e) => forwardCmd(e, 'pump/01/cmd')
);
exports.cmdSite01Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump02/cmd' },
  (e) => forwardCmd(e, 'pump/01/cmd')
);

// SETTINGS
exports.settingsSite01Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump01/settings' },
  (e) => forwardSettings(e, 'pump/01/settings')
);
exports.settingsSite01Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump02/settings' },
  (e) => forwardSettings(e, 'pump/02/settings')
);

// OTA
exports.otaSite01Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump01/ota' },
  (e) => forwardOta(e, 'pump/01/ota')
);
exports.otaSite01Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site01/line01/pump02/ota' },
  (e) => forwardOta(e, 'pump/02/ota')
);

// ════════════════════════════════════════════════════════════════════════════════
// site02 — pump01 & pump02 on same device (PUMP_ID=03)
// ════════════════════════════════════════════════════════════════════════════════

// CMD
exports.cmdSite02Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump01/cmd' },
  (e) => forwardCmd(e, 'pump/03/cmd')
);
exports.cmdSite02Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump02/cmd' },
  (e) => forwardCmd(e, 'pump/03/cmd')
);

// SETTINGS
exports.settingsSite02Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump01/settings' },
  (e) => forwardSettings(e, 'pump/03/settings')
);
exports.settingsSite02Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump02/settings' },
  (e) => forwardSettings(e, 'pump/04/settings')
);

// OTA
exports.otaSite02Pump01 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump01/ota' },
  (e) => forwardOta(e, 'pump/03/ota')
);
exports.otaSite02Pump02 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump02/ota' },
  (e) => forwardOta(e, 'pump/04/ota')
);

// LoRa OTA — triggers Blue Pill firmware update via master LoRa (site02 only)
exports.loraOtaSite02 = onValueWritten(
  { ...OPTS, ref: 'sites/site02/line01/pump01/lora_ota_cmd' },
  async (e) => {
    const cmd = e.data.after.val();
    if (!cmd || !cmd.url) return;
    await publish('pump/03/lora_ota', { url: cmd.url });
  }
);
