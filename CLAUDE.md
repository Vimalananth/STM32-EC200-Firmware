# Pump Controller — Project Rules & Architecture

## IMPORTANT — AI Assistant Rules

1. **Do NOT make any code changes without explicit user approval.**
   Always explain what you plan to change and why, then wait for the user to say yes
   before editing any file. This includes platformio.ini, source files, headers,
   scripts, and any other project file.

2. **Do NOT change build flags without understanding hardware consequences.**
   Removing or adding flags like `-DFW_MIN_LOGS` can have side effects that are not
   obvious from the code alone (e.g., feedback loops through shared UART pins).

3. **After every code change session: bump FW_VER and deploy both firmwares.**
   - Update `#define FW_VER` in `src/main.c` to today's date (format `YYYY-MM-DDa`).
   - Build with PUMP_ID="03" → copy `.pio/build/disco_g071rb/firmware.bin` → `_pumpstarter_sync/firmware_test.bin`
   - Switch modem.h to PUMP_ID="01"/PUMP_ID2="02" → build → copy → `_pumpstarter_sync/firmware.bin`
   - Restore modem.h to PUMP_ID="03"/PUMP_ID2="04"
   - `git add firmware.bin firmware_test.bin && git commit && git push` inside `_pumpstarter_sync/`
   - Railway OTA will pick up both binaries automatically after the push.

---

## System Overview

Remote pump controller using STM32G0 (master) + Quectel EC200U modem for cloud connectivity,
and STM32F103 Blue Pill (slave) for relay control + flow meter via LoRa.

---

## Hardware

### Master — 7semi STM32G0 board
- MCU: STM32G070KBT6
- Modem: Quectel EC200U (MQTT over LTE)
- LoRa: RYLR998 on USART3 (PB8=TX, PB9=RX) — address 1
- **CRITICAL: PB8 is shared between USART3 TX (→ RYLR998 RX) and Debug_Print output.
  NEVER remove `-DFW_MIN_LOGS` from platformio.ini. If Debug_Print is enabled, every
  log line feeds back into the RYLR998 RX as garbage AT commands, permanently scrambling
  the module until AT+RESET clears it. FW_MIN_LOGS must always be ON on master.**
- Project: `c:\Users\admin\Documents\PlatformIO\Projects\STM32_EC200`
- Linker: `STM32G070KBTX_APP.ld`
- Uses `--specs=nano.specs` → **NO float printf**, use integer arithmetic only

### Slave — Blue Pill STM32F103
- MCU: STM32F103C6T6 (genuine ST chip, IDCODE = 0x1ba01477)
- LoRa: RYLR998 on USART2 (PA2=TX, PA3=RX) — address 2
- Relay: **Single latching relay** — PB14 = latch (SET) coil, PB13 = unlatch (RESET) coil
  (P1 and P2 commands both control this one relay; R1 and R2 in heartbeat mirror each other)
- Flow meter: YF-DN50 on PB0 (EXTI0, rising edge, pull-up)
- Modbus RS485: USART1 PA9=TX/DI, PA10=RX/RO at 9600 baud (STM32F103C6 has no USART3)
- **Debug_Print is disabled** — USART1 repurposed for Modbus; no debug output on slave
- LED: PC13 (active LOW)
- Project: `c:\Users\admin\Documents\PlatformIO\Projects\STM32_BluePill_LoRa`
- Linker: `STM32F103C6TX_FLASH.ld` — ORIGIN=0x08000000, LENGTH=32K
- Upload: ST-Link (upload_protocol = stlink), NO fix_cputapid.py
- Uses `--specs=nano.specs` → **NO float printf**, use integer arithmetic only
- **IWDG** — 4 s timeout (LSI 40 kHz, prescaler 64, reload 2500); started after `LoRa_Init()` to avoid tripping during ~8 s RYLR998 boot; kicked at top of every main loop

---

## Pump IDs & Firmware

| Pump ID | Relay | Device | Firmware file |
|---------|-------|--------|---------------|
| pump01  | relay1 | 7semi board site01 | firmware.bin (PUMP_ID="01") |
| pump02  | relay2 | 7semi board site01 | firmware.bin (PUMP_ID="01") |
| pump03  | relay1 | 7semi board site02 | firmware_test.bin (PUMP_ID="03") |
| pump04  | relay2 | 7semi board site02 | firmware_test.bin (PUMP_ID="03") |

- PUMP_ID defined in `include/modem.h`
- Default (repo) PUMP_ID = "03"
- Build pump01/02: change to "01", build → firmware.bin, restore to "03"
- Push to `_pumpstarter_sync` repo → Railway OTA picks up automatically

---

## Firebase Structure

```
sites/{siteId}/
  config/
    slave_fb_path: "sites/site02/line02/pump01"  ← dynamic slave activation (app reads this)
  line01/
    pump01/
      status/        ← live master telemetry (firmware writes via MQTT)
      cmd/           ← relay commands (app writes)
      alerts/        ← protection trips, relay events (push log)
      logs/          ← voltage log (push log)
      settings/      ← protection thresholds (app writes, firmware reads)
      ota_status/    ← OTA progress
    pump02/          ← same structure as pump01 (relay2)
    rotation_schedule/
  line02/
    pump01/
      status/        ← slave relay + online state (master writes via MQTT line2_status)
      logs/          ← slave periodic log (master writes via MQTT line2_log)
      alerts/        ← slave events: online/offline (master writes via MQTT line2_alerts)
```

### slave line2/status fields (written by master from LoRa heartbeat)
```json
{
  "r3": 0,        // relay3 state (-1=unknown, 0=OFF, 1=ON)
  "r4": 0,        // relay4 state
  "rssi": -21,    // LoRa RSSI (0 = no signal)
  "snr": 9,       // LoRa SNR
  "age_s": 5,     // seconds since last heartbeat received
  "fl": 43.6,     // flow rate L/min (formatted as integer: %lu.%lu)
  "tv": 150,      // total volume litres since last Blue Pill boot
  "online": true,
  "ts": 1234567890000
}
```

---

## Flow Meter

- Model: YF-DN50
- Formula: `Hz = 0.2 × Q` (Q in L/min)
- K-factor: **12 pulses per litre**
- 1 pulse = 83.33 mL

### Blue Pill calculation (integer only, no floats)
```c
flow_lpm_x10   = pulses * 50U;           // pulses/sec × 5 L/min × 10
flow_total_ml += pulses * 1000U / 12U;   // 83.33 mL per pulse
```

### Heartbeat format sent by Blue Pill
```
S:R1:ON|R2:OFF|FL:12.5|TV:150
```
FL = L/min with 1 decimal, TV = total litres (integer)

### tv resets on Blue Pill power cycle (RAM variable, not persistent)

---

## LoRa Configuration

- Network ID: **6** (`LORA_NETWORK_ID "6"` in both master and slave lora.c)
- Master address: 1
- Slave address: 2
- Band: 865000000 (865 MHz)
- CRFOP: **22** (`AT+CRFOP=22` on both boards)
- Spreading factor / BW / CR / preamble: `AT+PARAMETER=9,7,1,12` (both boards)

### Commands (master → slave)
- `P1:ON` / `P1:OFF` — latch/unlatch relay (slave relay1)
- `P2:ON` / `P2:OFF` — same physical relay as P1 (both coils of one latching relay)
- `STATUS?` — request immediate status reply

### Heartbeat (slave → master every **60 seconds**)
- `S:R1:ON|R2:OFF|FL:0.0|TV:0`
- R2 always mirrors R1 (single relay — both fields reflect same state)
- Optional Modbus extension: `|V1:2305|V2:2298|V3:2301|I1:125|I2:130|I3:128|KW:45|KWH:1250`

---

## COM Ports (Dev Machine)

| Port | Device | Use |
|------|--------|-----|
| COM43 | STM32G0 debug UART | Watch firmware debug prints |
| COM46 | Quectel Modem Port | — |
| COM47 | Quectel AP Log Port | Internal modem debug (MBEDTLS logs) |
| COM50 | Quectel AT Port | Send AT commands manually |
| COM51 | Quectel USB Serial | — |

### Test LoRa via AT commands (COM50 at 115200):
```
AT+QMTPUBEX=0,1,0,0,"pump/03/cmd",13
{"relay1":1}
```

---

## MQTT Topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `pump/{pumpId}/cmd` | App → device | Relay commands (relay1, relay2) |
| `pump/{pumpId}/status` | Device → Firebase | Live status (voltage, current, relay states) |
| `pump/{pumpId}/alerts` | Device → Firebase | Protection trips (OV, UV, PL, dry-run) |
| `pump/{pumpId}/log` | Device → Firebase | Relay on/off events with run time |
| `pump/{pumpId}/vlog` | Device → Firebase | Periodic voltage log (every 5 min) |
| `pump/{pumpId}/settings` | Bridge → device | Push settings to firmware |
| `pump/{pumpId}/ota` | Bridge → device | OTA trigger URL |
| `pump/{pumpId}/ota/status` | Device → Firebase | OTA progress |
| `pump/{pumpId}/line2_status` | Device → Firebase | Slave relay + online state |
| `pump/{pumpId}/line2_log` | Device → Firebase | Slave periodic log (flow, voltage) |
| `pump/{pumpId}/line2_alerts` | Device → Firebase | Slave online/offline events |

---

## Firmware Watchdogs & Recovery

- **Progressive MQTT offline watchdog** — uses `.noinit` RAM (survives `NVIC_SystemReset()`):
  - Reset #1: after **2 minutes** offline
  - Reset #2: after **30 minutes** offline (on next boot)
  - Reset #3: after **1 hour** offline (on next boot)
  - After 3 resets: stop and log "manual power cycle required"
  - Reset counter cleared on any successful MQTT connection
  - Watchdog suppressed while OTA is active
- **CFUN nuclear reset** — `AT+CFUN=1,1` hard modem reset after 5 consecutive MQTT reconnect failures (1 if post-OTA)
- **IWDG** — 32-second hardware watchdog; kicked throughout all blocking operations
- **Offline relay state persistence** — relay states saved to `.noinit` RAM on disconnect; restored with magic-byte validation on reconnect
- **SMS reception** — works during MQTT offline periods
- `mqtt_offline_since_ms` reset on CFUN reinit to avoid watchdog race

---

## Master UART Connections (STM32G0)

| UART | Pins | Connected to |
|------|------|-------------|
| USART1 | PA9=TX, PA10=RX | Quectel EC200U modem |
| USART3 | PB8=TX, PB9=RX | RYLR998 LoRa module |
| Debug  | — | Via Quectel AP log (COM47) or STM32 UART (COM43) |

---

## Settings Fields (sites/{siteId}/line01/pump{NN}/settings)

| Field | Type | Description |
|-------|------|-------------|
| `ov` | float | Over-voltage threshold (V) — relay1 only |
| `uv` | float | Under-voltage threshold (V) — relay1 only |
| `pl` | float | Phase loss threshold (V) — relay1 only |
| `uv_rst` | int | UV/PL restart delay (seconds) |
| `dry_i` | float | Dry-run current threshold (A) |
| `dry_t` | int | Dry-run trip time (seconds) |
| `dry_en` | int | Dry-run protection enabled (0/1) |
| `start_t` | int | Startup delay (seconds) |
| `hp` | int | HP rating (5 or 75 for 7.5HP) |

---

## Critical Coding Rules

1. **NO float printf** — both STM32G0 and Blue Pill use `--specs=nano.specs`.
   Using `%.1f` or `%.0f` causes `.rodata` overflow (~4KB penalty).
   Always use integer formatting: `%lu.%lu` for decimals.

2. **Blue Pill linker at 0x08000000** — no bootloader needed. Do NOT set
   `SCB->VTOR` in main.c. Do NOT use fix_cputapid.py.

3. **PUMP_ID in modem.h** — always restore to "03" after building pump01/02.

4. **Integer-only flow math** — K=12 pulses/litre for YF-DN50.

5. **Bridge forwards full JSON** — no need to modify bridge when adding new
   fields to slave_log; it passes through the complete payload.

6. **LORA_OTA_DISABLED** — LoRa OTA is disabled in current firmware.

---

## Bridge (Railway)

- Node.js service subscribed to MQTT
- Forwards all MQTT messages to Firebase in real-time
- Source: `bridge/` directory
- No changes needed when adding new telemetry fields

---

## Flutter App

- Source: `flutter_app/lib/main.dart`
- Sites defined in `kSites` (compile-time list); slave activation is **dynamic** via Firebase
- `_slavePaths` map loaded from `sites/{siteId}/config/slave_fb_path` at runtime
- Slave (Line 2) tab appears/disappears without app rebuild; cached in SharedPreferences for offline/startup
- Settings page uses real-time Firebase listener (`onValue`) for multi-device sync
- HP presets: 5HP (dry_i=3.0), 7.5HP (dry_i=4.5)

### To activate a slave for a site (no rebuild needed):
Write to Firebase: `sites/{siteId}/config/slave_fb_path = "sites/{siteId}/line02/pump01"`
Delete that key to remove Line 2 from the app.

---

## OTA Firmware Update

- Repo: `_pumpstarter_sync` (separate git repo)
- firmware.bin → pump01/02 (PUMP_ID=01)
- firmware_test.bin → pump03/04 (PUMP_ID=03)
- Push to GitHub → Railway service picks up and serves via HTTP
- **Blue Pill LoRa OTA**: slave firmware has OTA receiver (`handle_ota_cmd` in lora.c);
  master has it **disabled** (`LORA_OTA_DISABLED`) in current build — Blue Pill must be flashed via ST-Link for now
