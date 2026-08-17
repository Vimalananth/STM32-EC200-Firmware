# PUMP CONTROLLER — FULL SYSTEM ARCHITECTURE, LOGIC & VALIDATION
**FW: 2026-08-07b | Audited: 2026-08-08 | MCU: STM32G070KBT6**

---

## 1. HARDWARE TOPOLOGY

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                    MASTER — STM32G070 (7semi board)                         ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  ┌─────────────────────┐  USART1   ┌──────────────────────────────────────┐ ║
║  │   Firmware          │◄─────────►│  Quectel EC200U LTE Modem            │─╫─► LTE
║  │   2026-08-07b       │  PA9/PA10  │  MQTT over TLS :8883                 │ ║
║  └──────┬──────────────┘           └──────────────────────────────────────┘ ║
║         │                                                                    ║
║         │ USART3   ┌──────────────────┐  ⚠ PB8 SHARED with Debug_Print     ║
║         ├─────────►│ RYLR998 LoRa     │    FW_MIN_LOGS must ALWAYS be ON   ║
║         │  PB8/PB9 │ Addr:1 Net:6     │    or LoRa module gets corrupted   ║
║         │  115200  │ 865 MHz SF9      │                                     ║
║         │          │ CRFOP=22         │                                     ║
║         │          └────────┬─────────┘                                     ║
║         │                   │ RF 865 MHz                                    ║
║         │ USART2   ┌────────┴─────────────────────────────────────────────┐ ║
║         ├─────────►│ Selec EM4M-3P-C-100A (Modbus RTU 9600 baud)         │ ║
║         │  PA2/PA3 │ Req1: FC04 0x0000 × 22  → V12, V23, V31, I1, I2, I3 │ ║
║         │  PA8=DE  │ Req2: FC04 0x002A × 2   → Total kW (IEEE754 float)   │ ║
║         │          │ Req3: FC04 0x0034 × 2   → Total kWh                  │ ║
║         │          └─────────────────────────────────────────────────────-─┘ ║
║         │                                                                    ║
║         │ PA0      LiPo battery ADC  (10k+10k divider → 2.1V max)          ║
║         │                                                                    ║
║         │ Relay1   PA1=SET coil (+200ms pulse)    PB3=RESET coil           ║
║         │          → Pump01 (line01/pump01)                                 ║
║         │                                                                    ║
║         │ Relay2   PB4=SET coil (+200ms pulse)    PB5=RESET coil           ║
║         │          → Pump02 (line01/pump02)                                 ║
║         │                                                                    ║
║         │ IWDG     Prescaler=256, Reload=4095 → ~32.8s timeout             ║
║         │          Started AFTER Modem_Init (10s boot would trip it)        ║
╚═════════╪════════════════════════════════════════════════════════════════════╝
          │ LoRa RF 865 MHz (Network ID 6, SF9, BW 125kHz, CR 4/5)
          ▼
╔══════════════════════════════════════════════════════════════════════════════╗
║                    SLAVE — STM32F103 Blue Pill                              ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  ┌─────────────────────┐  USART2   ┌──────────────────────────────────────┐ ║
║  │   Slave Firmware    │◄─────────►│  RYLR998 LoRa                        │ ║
║  │                     │  PA2/PA3   │  Addr:2 Net:6 865 MHz                │ ║
║  └──────┬──────────────┘           └──────────────────────────────────────┘ ║
║         │                                                                    ║
║         │ Latching Relay  PB14=SET(latch) / PB13=RESET(unlatch)            ║
║         │                 P1 and P2 commands both control this one relay    ║
║         │                                                                    ║
║         │ YF-DN50 flow    PB0 (EXTI0 rising edge, pull-up)                 ║
║         │                 12 pulses/litre, FL = pulses×50/10 L/min         ║
║         │                                                                    ║
║         │ USART1  PA9/PA10 → Modbus RS485 (no debug output — repurposed)   ║
║         │                                                                    ║
║         │ IWDG   4s timeout (LSI 40kHz, PSC=64, Reload=2500)              ║
║         │        Started after LoRa_Init (~8s RYLR998 boot)                ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

---

## 2. PUMP ID MAPPING

```
┌──────────┬────────┬──────────────┬──────────────────┬───────────────────────┐
│ PUMP_ID  │ Relay  │ Board        │ Firmware file     │ Firebase path         │
├──────────┼────────┼──────────────┼──────────────────┼───────────────────────┤
│ 01       │ relay1 │ site01 board │ firmware.bin      │ site01/line01/pump01  │
│ 02       │ relay2 │ site01 board │ firmware.bin      │ site01/line01/pump02  │
│ 03       │ relay1 │ site02 board │ firmware_test.bin │ site02/line01/pump01  │
│ 04       │ relay2 │ site02 board │ firmware_test.bin │ site02/line01/pump02  │
└──────────┴────────┴──────────────┴──────────────────┴───────────────────────┘
Default in repo: PUMP_ID="03" / PUMP_ID2="04"
Build for site01: change to PUMP_ID="01" / PUMP_ID2="02", restore after
```

---

## 3. MAIN LOOP — FIRMWARE EXECUTION FLOW

```
main.c:184
┌─────────────────────────────────────────────────────────────────────────────┐
│  while (1)                                                                  │
│  {                                                                          │
│    Modem_Process()     ← UART1: handles modem AT line-by-line               │
│    │                     MQTT state machine, publish, settings, OTA trigger │
│    LoRa_Process()      ← UART3: auto-ping, command retry, heartbeat parse  │
│    │                                                                        │
│    if (!OTA_IsActive())                                                     │
│      Modbus_Process()  ← USART2: non-blocking RS485 Modbus poll cycle      │
│    │                     Skipped during OTA to avoid UART2 interference     │
│    OTA_Process()       ← State machine: QHTTPGET → flash erase → write     │
│    LoRaOta_Process()   ← Blue Pill OTA state machine (disabled in master)  │
│    │                                                                        │
│    if ((HAL_GetTick() - g_hb_last_ms) >= 500)                              │
│      toggle PA8 (DE485)  ← 500ms heartbeat blink confirms MCU alive        │
│    │                                                                        │
│    HAL_IWDG_Refresh()  ← Feed 32.8s hardware watchdog                      │
│  }                                                                          │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. MQTT STATE MACHINE (modem.c)

```
Power ON
    │
    ▼
┌──────────────────┐
│  BOOT / MODEM    │  AT+CFUN=1,1 (hard reset if post-OTA)
│  INIT            │  AT+QSSLCFG (SSL config), AT+CGREG=0
└────────┬─────────┘
         │
         ▼
┌──────────────────┐   retry every 5s
│  NET_WAIT        │──────────────────────────────────────────────────────────┐
│                  │  AT+CGREG? / AT+CEREG?                                   │
│  Wait for:       │  Timeout: 36 × 5s = 3 min → DISCONNECTED                │
│  +CEREG: x,1     │                                                          │
│  +CEREG: x,5     │◄─────────────────────────────────────────────────────────┘
└────────┬─────────┘
         │  CGREG/CEREG 0,1 or 0,5 (home/roaming)
         ▼
┌──────────────────┐
│  PDP_OPEN        │  AT+QICSGP=1,1,"APN","","",0
│                  │  OK → PDP_ACTIVATE
└────────┬─────────┘  ERROR → NET_WAIT
         │
         ▼
┌──────────────────┐
│  PDP_ACTIVATE    │  AT+QIACT=1  (30s timeout)
│                  │  ERROR: force AT+QIDEACT=1 then retry once
│                  │  OK → BROKER_OPEN
└────────┬─────────┘  Timeout → NET_WAIT
         │  + AT+QIDNSCFG=1,"8.8.8.8","8.8.4.4"
         ▼
┌──────────────────┐
│  BROKER_OPEN     │  AT+QMTOPEN=0,"broker.emqx.io","8883"
│                  │
│  Two-phase TLS:  │  +QMTOPEN: 0,"host",port  → TLS starting
│                  │  OK                        → TLS intermediate (wait)
│                  │  +QMTOPEN: 0,0             → TLS complete ✓ → CONNECTING
│                  │  +QMTOPEN: 0,N (N≠0)      → FAILED → DISCONNECTED
│                  │  +QMTCLOSE / +QMTSTAT     → closed → DISCONNECTED
└────────┬─────────┘  ERROR → DISCONNECTED  (blink 1)
         │  + AT+QMTCFG="version",0,4  (force MQTT 3.1.1)
         ▼
┌──────────────────┐
│  CONNECTING      │  AT+QMTCONN=0,"clientId","user","pass"
│                  │
│  +QMTCONN: 0,0,0 → success → SUBSCRIBING
│  +QMTCONN: 0,0,4 → bad credentials (blink 4)
│  +QMTCONN: 0,0,5 → not authorised (blink 5)
│  +QMTCONN: 0,2   → transport error (blink 2)
│  +QMTCONN: 0,1   → no CONNACK (blink 7)
└────────┬─────────┘  ERROR → DISCONNECTED (blink 6)
         │
         ▼
┌──────────────────┐
│  SUBSCRIBING     │  Sequential topic subscriptions (QoS=1):
│                  │  #1 AT+QMTSUB=0,1,"pump/03/cmd",1
│                  │  #2 AT+QMTSUB=0,2,"pump/03/ota",1
│                  │  #3 AT+QMTSUB=0,3,"pump/03/settings",1
│                  │  #4 AT+QMTSUB=0,4,"pump/04/cmd",1
│                  │  #5 AT+QMTSUB=0,5,"pump/04/settings",1
│                  │  #6 AT+QMTSUB=0,6,"pump/03/lora_ota",1
│                  │  All +QMTSUB: 0,N,0 received → CONNECTED
└────────┬─────────┘
         │
         ▼
┌──────────────────┐   ┌─────────────────────────────────────────────────┐
│  CONNECTED       │   │ Every 10s: publish_status() → pump/03/status    │
│                  │   │ Every 5min: publish_vlog() → pump/03/vlog       │
│  Receives:       │   │ Every 60s: LoRa slave status → line2_status     │
│  +QMTRECV        │   │ Relay events: pump/03/log → alerts push         │
│                  │   │ Protection alerts: pump/03/alerts → alerts push │
│  Commands on     │   └─────────────────────────────────────────────────┘
│  relay1/relay2/  │
│  OTA/settings    │   +QMTCLOSE / +QMTSTAT → DISCONNECTED
└──────────────────┘

DISCONNECTED:
  consecutive_failures++
  if consecutive_failures >= 5 (or 1 if post-OTA):
    AT+CFUN=1,1  ← Nuclear modem reset
    consecutive_failures = 0
  else:
    AT+QMTCLOSE=0 + AT+QIDEACT=1 → NET_WAIT
```

---

## 5. RELAY CONTROL LOGIC

```
modem.c:439-477

Relay1_Set(on):                         Relay2_Set(on):
  relay1 = on                             relay2 = on
  if (mains_is_off):                      if (mains_is_off):
    record intent in .noinit RAM            record intent in .noinit RAM
    return  (no 12V supply)                 return  (no 12V supply)
  if (on):                                if (on):
    PA1 HIGH → 200ms → PA1 LOW (SET)       PB4 HIGH → 200ms → PB4 LOW (SET)
  else:                                   else:
    PB3 HIGH → 200ms → PB3 LOW (RESET)     PB5 HIGH → 200ms → PB5 LOW (RESET)

MQTT Command Flow (modem.c:2007-2028):
  Receive {"relay1":1} on pump/03/cmd
    if relay2 active → BLOCK (interlock)
    if lockout_until not expired → BLOCK (dry-run lockout)
    if is_volt_fault() → BLOCK (voltage trip)
    else:
      Relay1_Set(true)
      relay1_on_tick = HAL_GetTick()  ← starts startup grace window
      log_relay_event(1, true, "manual")
      publish_status()

  Receive {"relay1":0} on pump/03/cmd
    Relay1_Set(false)
    log_relay_event(1, false, "manual")
    publish_status()

  Receive {"relay3":1} → LoRa_SendRelay(1, true)  → slave P1:ON
  Receive {"relay4":1} → LoRa_SendRelay(2, true)  → slave P2:ON
```

---

## 6. PROTECTION STATE MACHINE (per relay, modem.c:1005-1241)

```
Called every 1 second from Modem_Process when CONNECTED

Input signals:
  v1, v2, v3  — L-L voltages from Modbus (L1-L2, L2-L3, L3-L1)
  i           — max(I1, I2, I3) from Modbus
  relay1/2    — current relay state
  cfg_*       — protection thresholds from Firebase settings

━━━━━━━━━━━━━━━━━━━━━━━━━━━ VOLTAGE PROTECTION ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  ov = any(v1,v2,v3) > cfg_ov (480V default)
  uv = any(v1,v2,v3) < cfg_uv (340V default)
  pl = any(v1,v2,v3) < cfg_pl (200V default)   ← phase loss

  Startup grace window:
    relay_in_startup = relay_on_tick != 0
                    && (HAL_GetTick() - relay_on_tick) < cfg_start_t * 1000
    During grace: UV and PL ignored (motor inrush can dip voltage)
                  OV still active (overvoltage during startup is dangerous)

  volt_trip = ov OR (!relay_in_startup AND (uv OR pl))

  Hold counter (prevents nuisance trips from momentary spikes):
    if volt_trip: volt_trip_count++ (max VOLT_TRIP_HOLD_S = 300)
    else:         volt_trip_count = 0

  if volt_trip_count >= VOLT_TRIP_HOLD_S AND relay ON:
    Relay_Set(false)
    if !ov: uv_pl_tripped = true   ← eligible for auto-restart
    log_relay_event(reason)
    publish_alert(ov, uv, pl, false)

  UV/PL AUTO-RESTART (if cfg_uv_restart_t > 0):
    Conditions: uv_pl_tripped AND !relay AND voltage normal AND !dry_run_tripped
    Interlock:  cancel if OTHER relay is ON (mutual exclusion)
    Timer:      wait cfg_uv_restart_t seconds after voltage clears
    Then:       Relay_Set(true), relay_on_tick = now, log("uv_restart")

  OV RESTART: NEVER (safety-critical — requires manual intervention)

━━━━━━━━━━━━━━━━━━━━━━━━━━━ DRY-RUN PROTECTION ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Conditions to count:
    relay ON  AND  !startup grace  AND  !meter_stale  AND  cfg_dry_en
    AND  i >= DRY_RUN_MIN_I_A (0.3A)   ← motor IS spinning
    AND  i < cfg_dry_i                  ← but drawing too little

  dry_run_count++ each second
  dry_run_count >= cfg_dry_t (8s default):
    dry_run_tripped = true
    lockout_until = HAL_GetTick() + LOCKOUT_MS (5 min)
    Relay_Set(false)
    log_relay_event("dry_run")
    publish_alert(dry_run=true)

  After lockout expires:
    dry_run_tripped = false
    dry_run_count = 0
    publish_alert(all_clear)

  NOTE: i < DRY_RUN_MIN_I_A (0.3A) = motor de-energised externally → NOT a
        dry-run condition, counter stays at 0 (external preventer/timer)

━━━━━━━━━━━━━━━━━━━━━━━━━━━ OVERLOAD PROTECTION ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  HP → current limit:  5HP → 11A,  7.5HP → 16A,  0 → disabled

  if !meter_stale AND relay ON AND !startup grace AND limit > 0:
    if i > limit: overload_count++
    else:         overload_count = 0
    if overload_count >= 3:   ← 3 consecutive 1-second samples
      Relay_Set(false)
      log_relay_event("overload")
  if relay OFF: overload_tripped = false (allows manual restart)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━ COMMAND BLOCKING ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Relay1 ON blocked if:
    relay2 is ON     (mutual exclusion — only one pump at a time)
    HAL_GetTick() < lockout_until   (dry-run 5-min lockout)
    is_volt_fault()  (active OV/UV/PL)

  Relay2 ON blocked if:
    relay1 is ON
    HAL_GetTick() < lockout_until2
    is_volt_fault()

━━━━━━━━━━━━━━━━━━━━━━━━━━━━ STATUS FIELDS ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  relay1_running = relay1 AND Modbus_IsDataValid() AND (i > cfg_dry_i)
  relay2_running = relay2 AND Modbus_IsDataValid() AND (i > cfg_dry_i2)
  → Used by bridge for rotation current verification
```

---

## 7. MAINS POWER TRACKING (modem.c:885-981)

```
check_mains_state() — called every 1 second

  all_dead = v1 < 50V AND v2 < 50V AND v3 < 50V   (MAINS_OFF_V = 50.0f)

MAINS OFF TRANSITION (prev_dead=false → all_dead=true):
  ┌────────────────────────────────────────────────────────────────────────┐
  │ 1. Read relay state to restore later (.noinit preferred if valid)      │
  │    mains_restore_relay1 = relay1 (or noinit if magic valid)            │
  │    mains_restore_relay2 = relay2 (or noinit if magic valid)            │
  │    mains_restore_slave  = LoRa_GetRelay3State()==1                     │
  │                                                                        │
  │ 2. Save to .noinit RAM + set magic  (survives soft reset)              │
  │    noinit_mains_relay1, noinit_mains_relay2, noinit_mains_slave        │
  │    noinit_mains_magic = MAINS_BKUP_MAGIC                               │
  │                                                                        │
  │ 3. Mark relays OFF in software (12V coil supply absent)               │
  │    relay1=false, relay2=false                                          │
  │    RelayState_Save() → .noinit                                         │
  │                                                                        │
  │ 4. Tell slave to record OFF state via LoRa                             │
  │    LoRa_SendRelay(1, false)                                            │
  │                                                                        │
  │ 5. Record timestamps                                                   │
  │    mains_off_tick    = HAL_GetTick()    ← for duration calc            │
  │    mains_off_unix_ms = Modem_GetUnixMs()← for log timestamp            │
  │    mains_is_off = true                                                 │
  └────────────────────────────────────────────────────────────────────────┘

MAINS ON TRANSITION (prev_dead=true → !all_dead):
  ┌────────────────────────────────────────────────────────────────────────┐
  │ 1. mains_is_off = false                                               │
  │    Clear UV/PL trip flags (fresh voltage → don't block restore)        │
  │    noinit_mains_magic = 0 (invalidate backup)                          │
  │                                                                        │
  │ 2. RESET both relays to known-OFF (coils may still be latched ON)      │
  │    Relay1_Set(false)  ← PB3 pulse                                      │
  │    Relay2_Set(false)  ← PB5 pulse                                      │
  │                                                                        │
  │ 3. Restore correct relay — INTERLOCK (if/else, one at a time)          │
  │    if mains_restore_relay1: Relay1_Set(true) + relay1_on_tick=now      │
  │    else if mains_restore_relay2: Relay2_Set(true) + relay2_on_tick=now │
  │    log_relay_event("mains_restore")                                    │
  │    RelayState_Save()                                                   │
  │                                                                        │
  │ 4. Restore slave relay via LoRa                                        │
  │    LoRa_SendRelay(1, mains_restore_slave)                              │
  │                                                                        │
  │ 5. Publish outage log to TOPIC_LOG                                     │
  │    duration_s = (HAL_GetTick() - mains_off_tick) / 1000               │
  │    {"event":"mains_restore","duration_s":123,                          │
  │     "off_ts":1754500000,"ts":1754500123}                               │
  │    Bridge pushes to {fbBase}/alerts/                                   │
  │                                                                        │
  │ 6. Clear tracking variables                                            │
  │    mains_off_tick = 0, mains_off_unix_ms = 0                          │
  └────────────────────────────────────────────────────────────────────────┘

STATUS PAYLOAD (published every 10s):
  "pwr_off"    : mains_is_off ? (mains_off_unix_ms / 1000) : 0   ← Unix s
  "mains_dur_s": mains_is_off ? (HAL_GetTick()-mains_off_tick)/1000 : 0
  Both use %lu — no float printf
```

---

## 8. WATCHDOG & RESET LOGIC

```
━━━━━━━━━━━━━━━━━━━━━━━━━ HARDWARE IWDG (main.c:428-440) ━━━━━━━━━━━━━━━━━━━━

  Prescaler=256, Reload=4095, LSI=32kHz → timeout = 4096 × 256 / 32000 = 32.8s
  Started after Modem_Init() (boot sequence takes ~10s)
  Kicked in: main loop every iteration, Modem_Process (54+ sites),
             OTA_Process every 50ms, LoRa_Process

━━━━━━━━━━━━━━━━━━━━━━━ PROGRESSIVE MQTT WATCHDOG (modem.c:3910-3945) ━━━━━━━

  .noinit RAM: mqtt_reset_count (uint8_t, survives NVIC_SystemReset)
               Valid only if bkup magic pair matches

  When CONNECTED:
    mqtt_offline_since_ms = 0
    if mqtt_reset_count > 0: bkup_clear_reset_count() → mqtt_reset_count = 0

  When NOT connected:
    if mqtt_offline_since_ms == 0: record start time
    else if reset_count < MQTT_WD_MAX_RESETS (3):
      if (HAL_GetTick() - mqtt_offline_since_ms) > MQTT_WD_DELAY_MS[count]:
        bkup_set_reset_count(count + 1)
        NVIC_SystemReset()

  Thresholds: [0]=2min  [1]=30min  [2]=60min

  ┌────────────────────────────────────────────────────────────────────────┐
  │ Boot 1 → offline 2 min  → reset #1  (count=1 in .noinit)             │
  │ Boot 2 → offline 30 min → reset #2  (count=2 in .noinit)             │
  │ Boot 3 → offline 60 min → reset #3  (count=3 in .noinit)             │
  │ Boot 4 → count=3 >= MAX → stop, log "manual power cycle required"    │
  │                                                                        │
  │ Any successful MQTT connect → count cleared, timer reset              │
  └────────────────────────────────────────────────────────────────────────┘

━━━━━━━━━━━━━━━━━━━━━━━━━━ CFUN NUCLEAR RESET ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  consecutive_failures = 0 at start
  Each BROKER_OPEN fail or CONN fail: consecutive_failures++
  Threshold: 5 failures normally, 1 failure if ota_first_reconnect=true
  AT+CFUN=1,1  (full modem reset — clears TCP/TLS state)
  consecutive_failures = 0 after nuclear

━━━━━━━━━━━━━━━━━━━━━━━━━━ .noinit RAM LAYOUT ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Reset counter    (3 vars, ~8 bytes):
    noinit_reset_count, bkup_reset_magic_a, bkup_reset_magic_b

  Relay state      (magic + relay1 + relay2, ~5 bytes):
    offline_relay_magic, noinit_relay1_state, noinit_relay2_state

  Mains backup     (magic + r1 + r2 + slave, ~5 bytes):
    noinit_mains_magic, noinit_mains_relay1, noinit_mains_relay2, noinit_mains_slave

  OTA sentinel     (1 var, 4 bytes):
    ota_reboot_sentinel
  ──────────────────────────────
  Total: ~22 bytes (out of 36KB RAM)
```

---

## 9. MODBUS COMMUNICATION (modbus.c)

```
Hardware: USART2, PA2=TX(DI), PA3=RX(RO), PA8=DE+RE (HIGH=TX, LOW=RX)
Meter:    Selec EM4M-3P-C-100A, slave ID=0x01, 9600 baud

REGISTER MAP (FC04 Read Input Registers):

  Request 1: start=0x0000, count=22 (49-byte response)
  ┌──────────┬────────────────┬────────────┬─────────────────────────────┐
  │ Register │ Description    │ buf offset │ Type                        │
  ├──────────┼────────────────┼────────────┼─────────────────────────────┤
  │ 0x0000   │ V1N            │ buf+3      │ IEEE754 float, BE, V        │
  │ 0x0002   │ V2N            │ buf+7      │ IEEE754 float, BE, V        │
  │ 0x0004   │ V3N            │ buf+11     │ IEEE754 float, BE, V        │
  │ 0x0008   │ V12 (L1-L2)   │ buf+19     │ used as "v1" in firmware    │
  │ 0x000A   │ V23 (L2-L3)   │ buf+23     │ used as "v2" in firmware    │
  │ 0x000C   │ V31 (L3-L1)   │ buf+27     │ used as "v3" in firmware    │
  │ 0x0010   │ I1             │ buf+35     │ IEEE754 float, BE, A        │
  │ 0x0012   │ I2             │ buf+39     │ IEEE754 float, BE, A        │
  │ 0x0014   │ I3             │ buf+43     │ IEEE754 float, BE, A        │
  └──────────┴────────────────┴────────────┴─────────────────────────────┘

  Request 2: start=0x002A, count=2 (9-byte response)
  ┌──────────┬────────────────┬────────────┬─────────────────────────────┐
  │ 0x002A   │ Total kW       │ buf2+3     │ IEEE754 float, BE, kW       │
  └──────────┴────────────────┴────────────┴─────────────────────────────┘

  Request 3: start=0x0034, count=2 (9-byte response)
  ┌──────────┬────────────────┬────────────┬─────────────────────────────┐
  │ 0x0034   │ Import kWh     │ buf3+3     │ IEEE754 float, BE, kWh      │
  └──────────┴────────────────┴────────────┴─────────────────────────────┘

POLL STATE MACHINE (every 2s, fully non-blocking):

  MB_IDLE → MB_TX_WAIT: send Request1 frame after 2s interval
    (DE HIGH → TX 8 bytes → DE LOW)

  MB_RX_WAIT: read USART2 hardware FIFO directly (bypasses HAL)
    Clear ORE/FE/NE flags on each iteration
    Accumulate bytes until rx_idx >= RX_LEN (49) → MB_PARSE
    500ms timeout → MB_IDLE

  MB_PARSE:
    Check: rx_buf[0]==0x01, rx_buf[1]==0x04, rx_buf[2]==0x2C
    CRC16 check on rx_buf[0..46]
    If valid: extract floats, mb_data_ok=true, mb_last_ok_tick=now
    If invalid: mb_data_ok=false
    → MB_TX2_WAIT: send Request2

  MB_TX2_WAIT / MB_RX2_WAIT / MB_PARSE2: same pattern for kW
  MB_TX3_WAIT / MB_RX3_WAIT / MB_PARSE3: same pattern for kWh

STALE DATA DETECTION:
  Modbus_IsStale() = (HAL_GetTick() - mb_last_ok_tick) > 10000
  Used by protection to freeze dry-run counter when meter offline
```

---

## 10. LORA COMMUNICATION (lora.c)

```
Config: Network ID=6, Master Addr=1, Slave Addr=2
        Band=865MHz, CRFOP=22, PARAMETER=9,7,1,12
        USART3: PB8=TX, PB9=RX, 115200 baud

COMMAND SENDING (LoRa_SendRelay):
  Build msg: "P1:ON" or "P1:OFF" or "P2:ON" or "P2:OFF"
  AT+SEND=2,5,P1:ON  (addr, len, payload)

  Pending retry: saved in lora_pending_msg
  If no ACK in 5s: retry (up to 5 times)
  After 5 retries: give up, log error

HEARTBEAT PARSE (every 60s from slave):
  Receive +RCV=2,LEN,DATA,RSSI,SNR

  Format: "S:R1:ON|R2:OFF|FL:12.5|TV:1250"
  ┌──────────────────────────────────────────────────────────────────────┐
  │ R1:ON/OFF  → lora_relay3_state (1/0)                                │
  │ R2:ON/OFF  → lora_relay4_state (1/0)  (mirrors R1 — 1 relay)        │
  │ FL:12.5    → lora_flow_lpm_x10 = 125  (12.5 L/min)                  │
  │ TV:1250    → lora_total_litres_int = 1250 (cumulative, RAM, resets) │
  │ DP:mmm     → lora_depth_mm (water depth if sensor fitted)           │
  │ BAT:pct    → lora_bat_pct                                           │
  │ V1/V2/V3/I1/I2/I3/KW → slave Modbus readings (optional)            │
  └──────────────────────────────────────────────────────────────────────┘
  Extract RSSI (field 4), SNR (field 5)

AUTO-PING:
  If slave silent for 150s (2.5× heartbeat): send STATUS?
  Rate-limited: at most one ping per 150s

SLAVE ONLINE DETECTION (modem.c:653):
  age_s = LoRa_GetLastRcvAge() / 1000
  if age_s > 120: slave offline → publish online:false in line2_status

FLOW RATE NOTES:
  YF-DN50: 12 pulses/litre
  Blue Pill: flow_lpm_x10 = pulses_per_sec × 50
             Integer parse from FL field: whole×10 + frac
  TV resets on Blue Pill power cycle (RAM variable)
```

---

## 11. PUBLISH PAYLOADS

```
━━━━━━━━━━━━━━━━━━━━━━━━━ pub_payload BUFFER ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  static char pub_payload[824]  (modem.c:196)
  Single-slot: queue_publish() drops new publish if pub_pending=true
  Deferred flags: pub_status2_needed, pub_line2_needed, pub_vlog2_needed

━━━━━━━━━━━━━━━━━━━━━━━ pump/03/status (every 10s) ━━━━━━━━━━━━━━━━━━━━━━━━━━

{
  "relay1_state":1,      // 0 or 1
  "relay2_state":0,      // 0 or 1
  "relay1_running":1,    // relay ON AND Modbus valid AND i > cfg_dry_i
  "relay2_running":0,    // relay ON AND Modbus valid AND i > cfg_dry_i2
  "relay3_state":1,      // LoRa slave relay (-1=unknown)
  "relay4_state":1,      // LoRa slave relay (-1=unknown)
  "v1":"415.2",          // L1-L2 voltage (fmt_f1, 1 decimal)
  "v2":"414.8",          // L2-L3 voltage
  "v3":"415.0",          // L3-L1 voltage
  "current":"12.50",     // max(I1,I2,I3) (fmt_f2, 2 decimal)
  "kw":"5.60",           // Total kW (fmt_f2)
  "dry_run":false,       // relay1 dry-run tripped
  "dry_run2":false,      // relay2 dry-run tripped
  "online":true,
  "mb_ok":1,             // Modbus data valid
  "mb_rx":12345,         // Modbus rx byte counter
  "rssi":-65,            // last MQTT RSSI
  "boot_phase":12,
  "fw":"2026-08-07b",
  "bat":85,              // battery % (0xFF if ADC fail)
  "bat_mv":3820,         // raw LiPo mV
  "cfg_ov":"480.0",      // protection thresholds (echoed for UI display)
  "cfg_uv":"340.0",
  "cfg_pl":"200.0",
  "cfg_dry_i":"1.50",    "cfg_dry_t":8,  "cfg_start_t":300,
  "cfg_hp":75,           "cfg_dry_en":1,
  "cfg_dry_i2":"1.50",   "cfg_dry_t2":8, "cfg_start_t2":300,
  "cfg_hp2":50,          "cfg_dry_en2":1,
  "cfg_uv_rst_t":300,
  "pwr_off":1754500000,  // Unix s when mains went off (0 if on)  [%lu]
  "mains_dur_s":3600     // seconds on battery so far              [%lu]
}
Estimated: ~700 chars | Buffer: 824 bytes | Margin: ~124 bytes

━━━━━━━━━━━━━━━━━━━━━━━ pump/04/status (pump02, minimal) ━━━━━━━━━━━━━━━━━━━━

  {"relay1_state":0,"online":true}   ← relay2 state only (no current data)

━━━━━━━━━━━━━━━━━━━━━━━ pump/03/log / pump/03/alerts ━━━━━━━━━━━━━━━━━━━━━━━━

  Relay event:  {"event":"on","reason":"manual","ts":1754500000}
                {"event":"off","reason":"dry_run","run_s":7200,"ts":...}
  Alert:        {"overvoltage":true,"ts":...}
                {"dry_run_trip":true,"ts":...}
  Mains event:  {"event":"mains_restore","duration_s":3600,
                 "off_ts":1754496400,"ts":1754500000}

━━━━━━━━━━━━━━━━━━━━━━━━ pump/03/vlog (every 5 min) ━━━━━━━━━━━━━━━━━━━━━━━━━

  {"v1":"415.2","v2":"414.8","v3":"415.0",
   "i":"12.50","kw":"5.60","kwh":14523,"ts":1754500000}
  Note: kwh uses %lu (integer) — fixed in 2026-08-07b to avoid fmt_f2 9999 cap

━━━━━━━━━━━━━━━━━━━━━━━━ pump/03/slave_status (every 60s) ━━━━━━━━━━━━━━━━━━━

  {"r3":1,"r4":1,"rssi":-65,"snr":9,"age_s":5,
   "fl":"12.5","tv":1250,"online":true,"ts":1754500000}

FORMAT RULES (--specs=nano.specs — NO float printf):
  All floats → pre-formatted via fmt_f1() (1dp) or fmt_f2() (2dp)
  fmt_f1 range: -9999 to 99999   clamps to "0.0" if OOB or NaN
  fmt_f2 range: -999  to 9999    clamps to "0.00" if OOB or NaN
  Timestamps:   %lu (unsigned long, Unix seconds)
  Counters:     %d, %u, %lu
  NO %f, %lf, %llu anywhere in publish paths
```

---

## 12. OTA UPDATE FLOW (ota.c)

```
Trigger: MQTT pump/03/ota → {"url":"http://railway.app/firmware_test.bin","crc32":"A1B2C3D4"}

OTA STATE MACHINE:
┌─────────────────────────────────────────────────────────────────────────────┐
│  IDLE                                                                       │
│  → SSL_ENABLE/SECLEVEL/CACERT/VERSION/CIPHER/CTXID                        │
│     (AT+QSSLCFG commands, each with 15s timeout)                           │
│  → HTTP_URL_CMD: AT+QHTTPURL=0,LEN,5000                                   │
│  → HTTP_URL_BODY: send URL string                                          │
│  → HTTP_GET: AT+QHTTPGET=0,120   (120s timeout, 3 retries)                │
│     Wait for +QHTTPGET: 0,0,200                                            │
│  → PREERASE: erase Slot B flash pages one at a time                       │
│     Each OTA_Process call: one 2KB page (ota_erase_page_safe())            │
│     IWDG kicked before AND after each erase (~30ms per page)               │
│  → HTTP_READFILE: AT+QHTTPREAD=0,45  (45s timeout, 2 retries)             │
│     Stream 256-byte chunks, CRC32 computed on-the-fly                     │
│     flash_write_dword() — 8-byte aligned writes                           │
│  → FLAG_WRITE: write boot flag to flash                                    │
│  → REBOOT: NVIC_SystemReset()                                             │
│                                                                             │
│  ERROR: publish {"error":"crc_mismatch"/"http_fail"} to ota/status        │
│         NO reboot — device stays in app slot                               │
└─────────────────────────────────────────────────────────────────────────────┘

FLASH LAYOUT (STM32G070, 128KB):
  0x08000000 — Bootloader (8KB, 4 pages)
  0x08002000 — Application Slot A (active, SCB->VTOR=0x08002000)
  0x08010000 — Application Slot B (OTA target)

CRC32 VALIDATION:
  Streaming CRC computed during download
  Compared to crc32 field from MQTT JSON
  Mismatch → ota_state = OTA_ST_ERROR, no flash flag written, no reboot

POST-OTA RECONNECT:
  ota_first_reconnect = true → nuclear threshold drops to 1 failure
  (accelerated recovery after reboot)
  OTA retry block: 5 min cooldown to ignore retained ota topic on reconnect
```

---

## 13. BRIDGE LOGIC (bridge/bridge.js)

```
━━━━━━━━━━━━━━━━━━━━━━━━━ MQTT → FIREBASE ROUTING ━━━━━━━━━━━━━━━━━━━━━━━━━━━

  pump/01/status    ──set──►  site01/line01/pump01/status
  pump/01/log       ──push─►  site01/line01/pump01/alerts   (relay events)
  pump/01/alerts    ──push─►  site01/line01/pump01/alerts   (protection)
  pump/01/vlog      ──push─►  site01/line01/pump01/logs     (5-min vlog)
  pump/01/ota/status──set──►  site01/line01/pump01/ota_status
                              + clear retained pump/01/ota topic
  pump/01/slave_*   ──set/push► site01/line02/pump01/*
  (same for 02/03/04)

  Timestamp normalization (applied on every message):
    if (payload.ts > 0 && payload.ts < 1e12) payload.ts *= 1000  ← s → ms
    if (!payload.ts) payload.ts = Date.now()

  Log retention: push entries with ts < (now - 5 days) are purged

━━━━━━━━━━━━━━━━━━━━━━━━━ FIREBASE → MQTT ROUTING ━━━━━━━━━━━━━━━━━━━━━━━━━━━

  {fbBase}/cmd → pump/NN/cmd    (relay1/relay2 commands, QoS1)
  {fbBase}/settings → pump/NN/settings  (retained QoS1)
  {fbBase}/ota → pump/NN/ota    (retained QoS1)

  cmdRelayMap:
    pump01/cmd {relay1:1} → MQTT {relay1:1} to pump/01/cmd
    pump02/cmd {relay1:1} → MQTT {relay1:1} to pump/02/cmd
    line02/pump01/cmd {relay1:1} → MQTT {relay3:1} to pump/01/cmd

━━━━━━━━━━━━━━━━━━━━━━━━━ ROTATION EXECUTOR (every 60s) ━━━━━━━━━━━━━━━━━━━━━

  Firebase: {site}/line01/rotation_schedule/{enabled, interval_minutes,
                                             current_pump, started_at}

  Phase A: pendingVerify check (in-memory, not in Firebase)
    If pendingVerify && now >= verifyAt:
      Read {masterStatusPath}/{runningField} from Firebase
        pump01 → relay1_running at line01/pump01/status
        pump02 → relay2_running at line01/pump01/status  (same board!)
      If running: log success, clear pendingVerify
      If !running && retryCount < 2:
        Resend relay1:1 to nextPump/cmd
        FCM: "Rotation Retry N/2: pump did not respond — ON resent"
        Set verifyAt = now + 5min, retryCount++
      If retryCount >= 2:
        FCM: "Rotation Failed: pump did not start after 2 retries"
        Clear pendingVerify
    continue  (skip Phase B)

  Phase B: interval check
    if elapsed >= interval_minutes:
      Step 1: write {currentPump}/cmd = {relay1:0}
      Step 2: poll {currentPump}/status/relay1_state every 3s (max 30s)
              If not 0 after 30s: ABORT, leave Firebase unchanged, retry next minute
      Step 3: write {nextPump}/cmd = {relay1:1}
      Step 4: update rotation_schedule: current_pump=nextPump, started_at=now
      Step 5: rotationState[fbPath].pendingVerify = {nextPump, verifyAt:now+5min, retryCount:0}

  pendingVerify preserved across Firebase listener updates (explicit merge)

━━━━━━━━━━━━━━━━━━━━━━━━━ SCHEDULE EXECUTOR (every 60s) ━━━━━━━━━━━━━━━━━━━━━

  Per-pump: {fbBase}/schedule/{enabled, on_hour, on_min, off_hour, off_min}
  At exact minute match: publish {relay1:1/0, src:'sched'} to MQTT cmd topic

━━━━━━━━━━━━━━━━━━━━━━━━━ OFFLINE DETECTION (every 60s) ━━━━━━━━━━━━━━━━━━━━━

  lastSeen[statusPath] updated on every /status or /slave_status message
  If (now - lastSeen) > 15 min:
    Set {statusPath}/online = false
    Send FCM: "Device Offline"
  When status received again:
    offlineNotified cleared
    Send FCM: "Back Online"

━━━━━━━━━━━━━━━━━━━━━━━━━━━ FCM NOTIFICATIONS ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  relay on/off events, protection alerts (OV/UV/PL/dry-run),
  slave online/offline, device offline/online,
  rotation retry/failed
```

---

## 14. FIREBASE DATA STRUCTURE

```
sites/
├── site01/
│   ├── config/
│   │   └── slave_fb_path: "sites/site01/line02/pump01"  ← dynamic slave
│   └── line01/
│       ├── pump01/           (relay1 — PUMP_ID 01)
│       │   ├── status/       SET  — live telemetry (10s)
│       │   ├── alerts/       PUSH — relay events + protection trips
│       │   ├── logs/         PUSH — 5-min vlog snapshots
│       │   ├── cmd/          SET  — app writes relay commands
│       │   ├── settings/     SET  — protection thresholds
│       │   └── ota_status/   SET  — OTA progress
│       ├── pump02/           (relay2 — PUMP_ID 02, same board)
│       │   ├── status/       SET  — {relay1_state, online} only
│       │   └── alerts/       PUSH — relay2 events
│       ├── rotation_schedule/
│       │   ├── enabled: true
│       │   ├── interval_minutes: 120
│       │   ├── current_pump: "pump01"
│       │   └── started_at: 1754500000000
│       └── (per pump schedule/)
│
│   └── line02/pump01/        (Blue Pill slave via LoRa)
│       ├── status/           SET  — {r3,r4,rssi,snr,age_s,fl,tv,online,ts}
│       ├── logs/             PUSH — 5-min slave vlog
│       └── alerts/           PUSH — slave online/offline events
│
└── site02/                   (same structure, PUMP_ID 03/04)
```

---

## 15. FLUTTER APP DATA FLOW

```
PowerMeterCard
  _listenStatus (site01/line01/pump01/status)
    v1, v2, v3, kw, current, bat, online
    pwr_off → compute live outage duration
    relay2_running → for pump02 display (same board)

  _listenLogs (site01/line01/pump01/alerts) ← mains_restore events
    filter event=="mains_restore", ts from today midnight
    show today's outage list + total

PumpCard (pump01)
  _listenStatus (pump01/status)
    relay1_state, relay1_running, online, dry_run, pwr_off

  _listenAlerts (pump01/alerts)
    find latest entry with overvoltage/undervoltage/phase_loss/dry_run_trip

  _listenTodayRun (pump01/alerts)
    filter event=="on"/"off" for today
    pair on/off events → run time per session

PumpCard (pump02)
  _listenStatus (pump01/status)  ← reads relay2_state/relay2_running!
  _listenAlerts (pump02/alerts)

RotationScheduleCard
  Reads rotation_schedule/
  On save: writes {enabled, interval_minutes, current_pump, started_at:0}
  Bridge executor does actual pump switching

LogsPage / SlaveLogsPage
  pump01/logs → voltage/current/kW/kWh chart
  line02/pump01/logs → slave flow/voltage chart
  pump01/alerts → relay event history
```

---

## 16. MQTT TOPICS REFERENCE

```
Direction        Topic                       Method   Purpose
──────────────   ─────────────────────────   ──────   ──────────────────────────
Device → Cloud   pump/03/status              MQTT     Live telemetry (10s)
Device → Cloud   pump/03/log                 MQTT     Relay on/off events
Device → Cloud   pump/03/alerts              MQTT     Protection trips
Device → Cloud   pump/03/vlog                MQTT     5-min voltage log
Device → Cloud   pump/03/ota/status          MQTT     OTA progress
Device → Cloud   pump/03/slave_status        MQTT     Blue Pill relay/flow
Device → Cloud   pump/03/slave_vlog          MQTT     Blue Pill 5-min log
Device → Cloud   pump/03/slave_alerts        MQTT     Slave online/offline
Cloud → Device   pump/03/cmd                 MQTT     Relay commands
Cloud → Device   pump/03/settings            MQTT     Protection thresholds (retained)
Cloud → Device   pump/03/ota                 MQTT     OTA trigger URL (retained)
Cloud → Device   pump/03/lora_ota            MQTT     Blue Pill OTA trigger
(same for 01/02/04)
```

---

## 17. VALIDATION SUMMARY

```
┌─────────────────────────────────┬────────┬────────────────────────────────────┐
│ Component                       │ Result │ Key Validation Points              │
├─────────────────────────────────┼────────┼────────────────────────────────────┤
│ Hardware watchdog (IWDG)        │ ✓ PASS │ 32.8s, 54+ kick sites, OTA covered │
│ Progressive MQTT watchdog       │ ✓ PASS │ 2m/30m/60m stages, .noinit magic   │
│ CFUN nuclear reset              │ ✓ PASS │ 5 failures → modem reset correct   │
│ Mains OFF tracking              │ ✓ PASS │ tick+unix timestamps, .noinit save │
│ Mains ON restore                │ ✓ PASS │ RESET both coils first, then ONE   │
│ mains_dur_s / pwr_off fields    │ ✓ PASS │ %lu format, correct overflow math  │
│ Relay interlock (mutual excl.)  │ ✓ PASS │ if/else in mains restore + command │
│ Voltage protection (OV/UV/PL)   │ ✓ PASS │ 300s hold, startup grace, OV never │
│ UV/PL auto-restart              │ ✓ PASS │ configurable delay, interlock check│
│ Dry-run protection              │ ✓ PASS │ DRY_RUN_MIN_I guard, 5-min lockout │
│ Overload protection             │ ✓ PASS │ HP-based limit, 3 consecutive secs │
│ Independent relay2 thresholds   │ ✓ PASS │ cfg_dry_i2/t2/en2/hp2/start_t2    │
│ Modbus register map             │ ✓ PASS │ L-L voltages buf+19/23/27 correct  │
│ Modbus non-blocking RX          │ ✓ PASS │ direct ISR read, clears ORE/FE/NE  │
│ Modbus CRC16 validation         │ ✓ PASS │ checked every response             │
│ Modbus stale detection          │ ✓ PASS │ 10s window, freezes dry-run counter│
│ LoRa heartbeat parsing          │ ✓ PASS │ integer-only, no float parse       │
│ LoRa command retry              │ ✓ PASS │ 5s ACK timeout, up to 5 retries    │
│ LoRa auto-ping                  │ ✓ PASS │ 150s silence → STATUS?             │
│ MQTT state machine              │ ✓ PASS │ full TLS handshake, sub sequence   │
│ MQTT subscription sequence      │ ✓ PASS │ 6 topics in order, QoS=1          │
│ pub_payload buffer sizing       │ ✓ PASS │ 824 bytes, ~700 used, 124 margin   │
│ No float printf                 │ ✓ PASS │ all via fmt_f1/fmt_f2, no %f/%lf   │
│ No %llu                         │ ✓ PASS │ timestamps divided to seconds %lu  │
│ HAL_GetTick overflow            │ ✓ PASS │ subtraction pattern throughout     │
│ OTA CRC32 streaming             │ ✓ PASS │ on-the-fly, mismatch = no reboot   │
│ OTA flash erase (non-blocking)  │ ✓ PASS │ one page per call, IWDG before+after│
│ OTA 8-byte write alignment      │ ✓ PASS │ doubleword buffer for remainders   │
│ kWh integer format              │ ✓ PASS │ %lu after 2026-08-07b fix          │
│ .noinit RAM usage               │ ✓ PASS │ ~22 bytes total, magic validation  │
│ PB8 LoRa/Debug shared           │ ✓ PASS │ FW_MIN_LOGS ON in platformio.ini   │
│ Bridge ts normalization         │ ✓ PASS │ s→ms if ts < 1e12                  │
│ Bridge log purge (cutoff)       │ ✓ PASS │ purgeOldLogs uses ms cutoff now    │
│ Rotation relay verification     │ ✓ PASS │ poll relay1_state 30s before update│
│ Rotation current verification   │ ✓ PASS │ relay1/2_running 5-min after switch│
│ Rotation pendingVerify persist  │ ✓ PASS │ preserved across Firebase listener │
├─────────────────────────────────┼────────┼────────────────────────────────────┤
│ ACS712 stub                     │ ⚠ WARN │ Returns max Modbus current (OK)    │
│                                 │        │ Unused stub returns 5.0f (harmless)│
│ queue_publish single-slot       │ ⚠ WARN │ Rapid publishes drop 2nd message   │
│                                 │        │ Mitigated by deferred *_needed flags│
│ pub_payload margin              │ ⚠ WARN │ 124 bytes — any new field must fit  │
│ Some inline magic numbers       │ ⚠ WARN │ "120"(slave offline), "36"(NET_WAIT)│
└─────────────────────────────────┴────────┴────────────────────────────────────┘

CRITICAL RULES (from hardware analysis):
  1. FW_MIN_LOGS MUST always be ON — PB8 shared with LoRa module RX
  2. --specs=nano.specs — NEVER use %f, %lf, %llu in any snprintf
  3. PUMP_ID="03" default — always restore after building for site01
  4. kWh must use %lu integer — fmt_f2 caps at 9999 (exceeded in ~2 months)
  5. HAL_GetTick subtractions only — never direct comparison for timeout
  6. Relay coil pulse: 200ms then LOW — latching relay never self-holds
  7. Only ONE relay can be ON at a time — enforced by interlock in all paths
```
