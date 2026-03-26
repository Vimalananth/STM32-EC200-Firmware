/*
 * modem.c  —  EC200U MQTT driver for STM32G071 (PlatformIO / STM32Cube HAL)
 *
 * Replaces the SMS-based relay control with MQTT over 4G LTE.
 *
 * UART wiring (from your stm32g0xx_hal_msp.c):
 *   USART1  PB6=TX  PB7=RX  →  EC200U
 *   USART2  PA2=TX  PA3=RX  →  debug serial monitor (115200)
 *
 * HiveMQ Cloud broker  :  44ad82d486654d68b4ac738e12fb1236.s1.eu.hivemq.cloud:8883
 * Firebase project     :  pump-controller-4398d
 *
 * MQTT topics  (PUMP_ID = "01" for pump 1, "02" for pump 2):
 *   pump/01/status   ← STM32 publishes on state change / 60 s heartbeat
 *   pump/01/alerts   ← STM32 publishes on protection trip
 *   pump/01/cmd      → STM32 subscribes (relay on/off commands)
 *
 * Payload examples:
 *   status : {"relay1_state":1,"relay2_state":0,"v1":415.2,"v2":414.8,
 *              "v3":416.1,"current":12.4,"dry_run":false,"online":true}
 *   cmd    : {"relay1":1,"relay2":0}
 *   alerts : {"overvoltage":false,"undervoltage":false,
 *              "phase_loss":false,"dry_run_trip":true}
 */

#include "modem.h"
#include "modbus.h"
#include "main.h"
#include "ota.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* IWDG handle lives in main.c — refresh it inside long blocking delays */
extern IWDG_HandleTypeDef hiwdg;

/* ═══════════════════════════════════════════════════════════════════════════
 * USER CONFIG  —  change these two lines per device before flashing
 * ═══════════════════════════════════════════════════════════════════════════ */
#define PUMP_ID "01"                 /* "01" for pump 1, "02" for pump 2  */
#define MQTT_USERNAME ""             /* anonymous — broker.emqx.io         */
#define MQTT_PASSWORD ""

/* ── APN — change to your SIM card ─────────────────────────────────────── */
/* Airtel: "airtelgprs.com"   Jio: "jionet"   BSNL: "bsnlnet"             */
#define SIM_APN "airtelgprs.com"

/* ── Broker ─────────────────────────────────────────────────────────────── */
#define BROKER_HOST "broker.emqx.io"
#define BROKER_PORT "8883"
#define CLIENT_ID "pump" PUMP_ID

/* ── Topics ─────────────────────────────────────────────────────────────── */
#define TOPIC_STATUS "pump/" PUMP_ID "/status"
#define TOPIC_CMD    "pump/" PUMP_ID "/cmd"
#define TOPIC_ALERTS "pump/" PUMP_ID "/alerts"
#define TOPIC_OTA    "pump/" PUMP_ID "/ota"

/* ── Protection thresholds ──────────────────────────────────────────────── */
#define V_OVERVOLTAGE 460.0f  /* V  — any phase above this trips relay  */
#define V_UNDERVOLTAGE 360.0f /* V  — any phase below this trips relay  */
#define V_PHASE_LOSS 50.0f    /* V  — phase considered lost             */
#define DRY_RUN_AMPS 1.5f     /* A  — below this = dry running          */
#define DRY_RUN_SECS 8        /* consecutive seconds before trip        */
#define LOCKOUT_MS 300000UL   /* 5 min lockout after dry-run trip       */

/* ── Heartbeat interval (event-driven: also publish on every state change) ── */
#define HEARTBEAT_INTERVAL_MS 60000UL

/* ── RX buffer ──────────────────────────────────────────────────────────── */
#define RX_BUF_SIZE 512

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum
{
    MQTT_STATE_BOOT,         /* waiting for EC200U power-on            */
    MQTT_STATE_NET_WAIT,     /* waiting for network registration       */
    MQTT_STATE_PDP_OPEN,     /* send AT+QICSGP, wait OK               */
    MQTT_STATE_PDP_ACTIVATE, /* send AT+QIACT=1, wait OK              */
    MQTT_STATE_SSL_CFG,      /* configuring SSL for port 8883          */
    MQTT_STATE_BROKER_OPEN,  /* AT+QMTOPEN — TCP to broker             */
    MQTT_STATE_CONNECTING,   /* AT+QMTCONN — MQTT handshake            */
    MQTT_STATE_SUBSCRIBING,  /* AT+QMTSUB  — subscribe to cmd topic    */
    MQTT_STATE_CONNECTED,    /* fully operational                      */
    MQTT_STATE_PUBLISHING,   /* waiting for '>' prompt after PUBEX     */
    MQTT_STATE_PUB_WAIT_OK,  /* waiting for +QMTPUBEX: 0,0,0          */
    MQTT_STATE_DISCONNECTED, /* lost connection — will reconnect       */
} MqttState;

static UART_HandleTypeDef *modem_uart;
static MqttState mqtt_state = MQTT_STATE_BOOT;
static char rxbuf[RX_BUF_SIZE];
static size_t rxpos = 0;
static uint32_t state_entered_ms = 0;

/* relay + protection */
static bool relay1 = false;
static bool relay2 = false;
static bool recv_payload_pending = false; /* true when +QMTRECV payload on next line */
static uint8_t dry_run_count = 0;
static bool dry_run_tripped = false;
static uint32_t lockout_until = 0;

/* publish queue — one pending payload at a time */
static char pub_topic[48];
static char pub_payload[300];
static bool pub_pending = false;

/* event-driven publish: track previous state to detect changes */
static uint32_t last_heartbeat_ms  = 0;
static bool     prev_relay1        = false;
static bool     prev_relay2        = false;
static bool     prev_dry_run_trip  = false;

/* signal strength: updated by +CSQ response; 99 = unknown */
static int8_t   last_rssi          = 99;

/* set false when PDP_OPEN is entered; set true when AT+QICSGP is sent from
 * Modem_Process (after the RX buffer is drained) so we never mistake the
 * trailing OK of AT+CGREG? for QICSGP's OK.                                */
static bool qicsgp_sent = false;

/* set false when PDP_ACTIVATE is entered; set true when AT+QIACT=1 is sent
 * from Modem_Process (after the RX buffer is drained) so we never mistake
 * stale OK responses from AT+QMTCLOSE/AT+QIDEACT for QIACT's OK.          */
static bool qiact_sent = false;

/* set true when +QMTOPEN: 0,"hostname",port URC arrives (TLS handshake
 * started).  Success fires only on the subsequent OK (TLS complete).     */
static bool qmtopen_tls_seen = false;


/* ═══════════════════════════════════════════════════════════════════════════
 * Low-level UART helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Send(const char *cmd)
{
    if (!modem_uart || !cmd)
        return;
    HAL_UART_Transmit(modem_uart, (const uint8_t *)cmd, strlen(cmd), 2000);
}

static void modem_cmd(const char *cmd)
{
    Modem_Send(cmd);
    Modem_Send("\r\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Diagnostic blink  — N × 80 ms pulses on Relay1 (PA5), saves & restores.
 * Used to indicate MQTT progress without needing a UART adapter.
 *   blink_n(1) = QMTOPEN TCP connected (sent AT+QMTCONN)
 *   blink_n(2) = publish confirmed (broker ACK / OK received)
 *   blink_n(3) = MQTT fully connected (subscribed)  ← SUCCESS
 *   blink_n(4) = AT+QMTPUBEX command sent (publish attempted)
 *   blink_n(5) = QMTCONN REFUSED  ← wrong credentials / broker rejected
 *   blink_n(6) = QMTCONN ERROR    ← AT command layer error
 *   blink_n(7) = server dropped connection (+QMTSTAT received)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════════
 * Relay control
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Latching relay helpers — pulse a coil for 50 ms then release */
static void relay_pulse(GPIO_TypeDef *port, uint16_t pin)
{
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    HAL_Delay(50);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
}

void Relay1_Set(bool on)
{
    relay1 = on;
    if (on)
        relay_pulse(Relay_Pin_GPIO_Port,   Relay_Pin_Pin);   /* SET coil  PA5 */
    else
        relay_pulse(Relay1_RST_GPIO_Port,  Relay1_RST_Pin);  /* RST coil  PA4 */
}

void Relay2_Set(bool on)
{
    relay2 = on;
    if (on)
        relay_pulse(Relay2_Pin_GPIO_Port,  Relay2_Pin_Pin);  /* SET coil  PA1 */
    else
        relay_pulse(Relay2_RST_GPIO_Port,  Relay2_RST_Pin);  /* RST coil  PA0 */
}

static void blink_n(int n)
{
    /* Use Relay1 latching coils for status blinks.
     * Save logical state and restore after blinking. */
    bool saved = relay1;
    for (int i = 0; i < n; i++) {
        relay_pulse(Relay_Pin_GPIO_Port,  Relay_Pin_Pin);   /* SET — latch ON  */
        HAL_Delay(150);
        relay_pulse(Relay1_RST_GPIO_Port, Relay1_RST_Pin);  /* RST — latch OFF */
        if (i < n - 1) HAL_Delay(350);
        HAL_IWDG_Refresh(&hiwdg);
    }
    /* Restore previous relay state */
    Relay1_Set(saved);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Connection status
 * ═══════════════════════════════════════════════════════════════════════════ */

bool Modem_IsConnected(void)
{
    return mqtt_state == MQTT_STATE_CONNECTED;
}

bool Relay1_Get(void) { return relay1; }
bool Relay2_Get(void) { return relay2; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Sensor stubs
 * Override these in a separate sensors.c once you wire up ADC channels.
 * Until then they return safe dummy values so the MQTT loop works.
 * ═══════════════════════════════════════════════════════════════════════════ */

__attribute__((weak)) float Sensor_ReadVoltagePhase1(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadVoltagePhase2(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadVoltagePhase3(void) { return 415.0f; }
__attribute__((weak)) float Sensor_ReadCurrentACS712(void) { return 5.0f; }

/* ═══════════════════════════════════════════════════════════════════════════
 * MQTT publish helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void queue_publish(const char *topic, const char *payload)
{
    if (pub_pending)
        return; /* drop if previous not sent yet      */
    strncpy(pub_topic, topic, sizeof(pub_topic) - 1);
    strncpy(pub_payload, payload, sizeof(pub_payload) - 1);
    pub_topic[sizeof(pub_topic) - 1] = '\0';
    pub_payload[sizeof(pub_payload) - 1] = '\0';
    pub_pending = true;
}

/* Format float to 1 decimal place without requiring -u_printf_float.
 * Handles negative values and clamps out-of-range to "0.0".           */
static void fmt_f1(char *out, int sz, float v)
{
    if (v != v || v > 99999.0f || v < -9999.0f) { snprintf(out, sz, "0.0"); return; }
    int neg = (v < 0.0f);
    float a = neg ? -v : v;
    int   w = (int)a;
    int   d = (int)((a - (float)w) * 10.0f);
    if (neg) snprintf(out, sz, "-%d.%d",  w, d);
    else     snprintf(out, sz,  "%d.%d",  w, d);
}

static void fmt_f2(char *out, int sz, float v)
{
    if (v != v || v > 9999.0f || v < -999.0f) { snprintf(out, sz, "0.00"); return; }
    int neg = (v < 0.0f);
    float a = neg ? -v : v;
    int   w = (int)a;
    int   d = (int)((a - (float)w) * 100.0f);
    if (neg) snprintf(out, sz, "-%d.%02d", w, d);
    else     snprintf(out, sz,  "%d.%02d", w, d);
}

static void publish_status(void)
{
    float v1 = Sensor_ReadVoltagePhase1();
    float v2 = Sensor_ReadVoltagePhase2();
    float v3 = Sensor_ReadVoltagePhase3();
    float i  = Sensor_ReadCurrentACS712();

    char sv1[12], sv2[12], sv3[12], sci[12];
    char spf1[8], spf2[8], spf3[8];
    fmt_f1(sv1,  sizeof(sv1),  v1);
    fmt_f1(sv2,  sizeof(sv2),  v2);
    fmt_f1(sv3,  sizeof(sv3),  v3);
    fmt_f2(sci,  sizeof(sci),  i);
    fmt_f2(spf1, sizeof(spf1), Modbus_GetPF1());
    fmt_f2(spf2, sizeof(spf2), Modbus_GetPF2());
    fmt_f2(spf3, sizeof(spf3), Modbus_GetPF3());

    char payload[384];
    snprintf(payload, sizeof(payload),
             "{\"relay1_state\":%d,\"relay2_state\":%d,"
             "\"v1\":%s,\"v2\":%s,\"v3\":%s,"
             "\"current\":%s,\"pf1\":%s,\"pf2\":%s,\"pf3\":%s,"
             "\"dry_run\":%s,\"online\":true,"
             "\"mb_ok\":%d,\"mb_rx\":%d,\"rssi\":%d}",
             relay1 ? 1 : 0,
             relay2 ? 1 : 0,
             sv1, sv2, sv3, sci,
             spf1, spf2, spf3,
             dry_run_tripped ? "true" : "false",
             Modbus_IsDataValid() ? 1 : 0,
             (int)Modbus_GetLastRx(),
             (int)last_rssi);

    queue_publish(TOPIC_STATUS, payload);
}

static void publish_alert(bool ov, bool uv, bool pl, bool dr)
{
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"overvoltage\":%s,\"undervoltage\":%s,"
             "\"phase_loss\":%s,\"dry_run_trip\":%s}",
             ov ? "true" : "false",
             uv ? "true" : "false",
             pl ? "true" : "false",
             dr ? "true" : "false");

    queue_publish(TOPIC_ALERTS, payload);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Protection logic  — called every 1 s when connected
 * ═══════════════════════════════════════════════════════════════════════════ */

static void run_protection(void)
{
    float v1 = Sensor_ReadVoltagePhase1();
    float v2 = Sensor_ReadVoltagePhase2();
    float v3 = Sensor_ReadVoltagePhase3();
    float i = Sensor_ReadCurrentACS712();

    bool ov = (v1 > V_OVERVOLTAGE || v2 > V_OVERVOLTAGE || v3 > V_OVERVOLTAGE);
    bool uv = (v1 < V_UNDERVOLTAGE || v2 < V_UNDERVOLTAGE || v3 < V_UNDERVOLTAGE);
    bool pl = (v1 < V_PHASE_LOSS || v2 < V_PHASE_LOSS || v3 < V_PHASE_LOSS);

    /* immediate trip on voltage fault */
    if ((ov || uv || pl) && relay1)
    {
        Relay1_Set(false);
        Relay2_Set(true); /* relay2 = alarm */
        publish_alert(ov, uv, pl, false);
        Debug_Print("[PROT] Voltage fault — pump OFF\r\n");
        return;
    }

    /* dry run detection */
    if (relay1 && !dry_run_tripped)
    {
        if (i < DRY_RUN_AMPS)
        {
            dry_run_count++;
            if (dry_run_count >= DRY_RUN_SECS)
            {
                dry_run_tripped = true;
                lockout_until = HAL_GetTick() + LOCKOUT_MS;
                Relay1_Set(false);
                Relay2_Set(true);
                publish_alert(false, false, false, true);
                Debug_Print("[PROT] Dry run — pump OFF + lockout\r\n");
            }
        }
        else
        {
            dry_run_count = 0;
        }
    }

    /* clear lockout after timeout */
    if (dry_run_tripped && HAL_GetTick() >= lockout_until)
    {
        dry_run_tripped = false;
        dry_run_count = 0;
        Relay2_Set(false);
        publish_alert(false, false, false, false);
        Debug_Print("[PROT] Lockout cleared\r\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON field extractor  — extracts integer value for a key
 * e.g. extract_int("{\"relay1\":1,\"relay2\":0}", "relay1") → 1
 * ═══════════════════════════════════════════════════════════════════════════ */

static int extract_int(const char *json, const char *key)
{
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p)
        return -1;
    p += strlen(search);
    while (*p == ' ')
        p++;
    return atoi(p);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * JSON string field extractor — copies value of "key":"<value>" into out
 * Returns true if key was found and value is non-empty.
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool extract_str(const char *json, const char *key, char *out, size_t max)
{
    char search[40];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    size_t i = 0;
    while (*p && *p != '"' && i < max - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return (i > 0);
}

static void modem_ota_start(const char *url); /* forward declaration */

/* ═══════════════════════════════════════════════════════════════════════════
 * Line processor — called for every complete line received from EC200U
 * ═══════════════════════════════════════════════════════════════════════════ */

static void process_line(const char *line)
{
    char dbg[80];

    /* ── always log non-empty lines to debug UART ── */
    if (line[0])
    {
        snprintf(dbg, sizeof(dbg), "[EC200U] %s\r\n", line);
        Debug_Print(dbg);
    }

    /* Forward ALL lines to OTA state machine during an active OTA download.
     * OTA_HandleLine() handles HTTP/file URCs (+QHTTPGET, +QFOPEN, CONNECT).
     * Return early so MQTT logic doesn't misinterpret AT responses as MQTT. */
    if (OTA_IsActive())
    {
        OTA_HandleLine(line);
        return;
    }

    /* +QMTRECV: incoming command — handle in ANY state that has an active
     * MQTT session so relay commands work even during PUBLISHING/PUB_WAIT_OK.
     * Some EC200U firmware versions put the JSON payload on the SAME line:
     *   +QMTRECV: 0,0,"pump/01/cmd","{"relay1":1,"relay2":0}"
     * Others put it on the NEXT line (split format):
     *   +QMTRECV: 0,0,"pump/01/cmd",23
     *   {"relay1":1,"relay2":0}
     * The recv_payload_pending flag handles the split-line case.             */

    /* Case: payload arrived on line following +QMTRECV header */
    if (recv_payload_pending)
    {
        const char *json = strchr(line, '{');
        if (json)
        {
            recv_payload_pending = false;
            /* OTA command: {"url":"https://..."} */
            char ota_url[200];
            if (extract_str(json, "url", ota_url, sizeof(ota_url)))
            {
                modem_ota_start(ota_url);
                return;
            }
            int r1 = extract_int(json, "relay1");
            int r2 = extract_int(json, "relay2");
            if (r1 >= 0)
            {
                if (r1 == 1 && HAL_GetTick() < lockout_until)
                    Debug_Print("[CMD] Relay1 ON blocked — lockout active\r\n");
                else
                {
                    Relay1_Set(r1 == 1);
                    Debug_Print(r1 ? "[CMD] Relay1 ON\r\n" : "[CMD] Relay1 OFF\r\n");
                }
            }
            if (r2 >= 0)
            {
                Relay2_Set(r2 == 1);
                Debug_Print(r2 ? "[CMD] Relay2 ON\r\n" : "[CMD] Relay2 OFF\r\n");
            }
            return;
        }
        /* "OK" ends an AT+QMTRECV exchange with no usable payload — clear flag */
        if (strcmp(line, "OK") == 0)
            recv_payload_pending = false;
        /* Other lines (e.g. +QMTRECV header) — keep waiting, fall through */
    }

    if (strstr(line, "+QMTRECV:"))
    {
        char *json = strchr(line, '{');
        if (json)
        {
            /* OTA command: {"url":"https://..."} */
            char ota_url[200];
            if (extract_str(json, "url", ota_url, sizeof(ota_url)))
            {
                modem_ota_start(ota_url);
                return;
            }
            /* Inline payload (single-line format) */
            int r1 = extract_int(json, "relay1");
            int r2 = extract_int(json, "relay2");
            if (r1 >= 0)
            {
                if (r1 == 1 && HAL_GetTick() < lockout_until)
                    Debug_Print("[CMD] Relay1 ON blocked — lockout active\r\n");
                else
                {
                    Relay1_Set(r1 == 1);
                    Debug_Print(r1 ? "[CMD] Relay1 ON\r\n" : "[CMD] Relay1 OFF\r\n");
                }
            }
            if (r2 >= 0)
            {
                Relay2_Set(r2 == 1);
                Debug_Print(r2 ? "[CMD] Relay2 ON\r\n" : "[CMD] Relay2 OFF\r\n");
            }
        }
        else if (strchr(line, '"'))
        {
            /* Has quoted topic but no '{': response header from AT+QMTRECV.
             * Payload arrives on the next line.                             */
            recv_payload_pending = true;
        }
        else
        {
            /* Buffer-mode notification: "+QMTRECV: 0,<id>" — no topic, no
             * payload.  EC200U has buffered the message; request it now.   */
            const char *p = strchr(line, ',');
            if (p)
            {
                int msgid = atoi(p + 1);
                char recv_cmd[32];
                snprintf(recv_cmd, sizeof(recv_cmd), "AT+QMTRECV=0,%d", msgid);
                modem_cmd(recv_cmd);
                recv_payload_pending = true;
            }
        }
        return;
    }

    /* +QMTSTAT: server closed the connection.
     * IGNORE in early states (NET_WAIT→SUBSCRIBING) — it is a stale URC from
     * a previous AT+QMTCLOSE that arrived late.  Only act when we have a live
     * MQTT session (CONNECTED / PUBLISHING / PUB_WAIT_OK).                   */
    if (strstr(line, "+QMTSTAT:"))
    {
        if (mqtt_state == MQTT_STATE_CONNECTED ||
            mqtt_state == MQTT_STATE_PUBLISHING ||
            mqtt_state == MQTT_STATE_PUB_WAIT_OK)
        {
            Debug_Print("[MQTT] Disconnected (QMTSTAT)\r\n");
            blink_n(7); /* 7 blinks = server dropped connection unexpectedly */
            pub_pending = false;
            recv_payload_pending = false;
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        /* else: ignore — stale URC from previous QMTCLOSE */
        return;
    }

    /* +CSQ: <rssi>,<ber>  — signal quality response */
    if (strstr(line, "+CSQ:"))
    {
        int rssi = 99, ber = 0;
        sscanf(line, "+CSQ: %d,%d", &rssi, &ber);
        if (rssi < 0 || rssi > 31) rssi = 99; /* sanitize */
        last_rssi = (int8_t)rssi;
        return;
    }

    switch (mqtt_state)
    {

    /* ── waiting for EC200U ready after boot ──────────────────────────── */
    case MQTT_STATE_BOOT:
        if (strstr(line, "RDY") || strstr(line, "OK"))
        {
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
            modem_cmd("ATE0"); /* echo off                  */
            HAL_Delay(200);
            modem_cmd("AT+CMEE=2"); /* verbose errors            */
            HAL_Delay(200);
            modem_cmd("AT+CGREG?"); /* GPRS / 3G registration    */
            HAL_Delay(200);
            modem_cmd("AT+CEREG?"); /* LTE / 4G registration     */
        }
        break;

    /* ── wait for network registration (GPRS or LTE) ─────────────────── */
    case MQTT_STATE_NET_WAIT:
        if (strstr(line, "+CGREG: 0,1") || strstr(line, "+CGREG: 0,5") ||
            strstr(line, "+CEREG: 0,1") || strstr(line, "+CEREG: 0,5"))
        {
            Debug_Print("[NET] Registered\r\n");
            mqtt_state = MQTT_STATE_PDP_OPEN;
            state_entered_ms = HAL_GetTick();
            qicsgp_sent = false; /* Modem_Process sends QICSGP after buffer drains */
        }
        /* retry registration every 5s */
        if (HAL_GetTick() - state_entered_ms > 5000)
        {
            state_entered_ms = HAL_GetTick();
            modem_cmd("AT+CGREG?");
        }
        break;

    /* ── AT+QICSGP sent — wait for OK then trigger AT+QIACT=1 ────────── */
    case MQTT_STATE_PDP_OPEN:
        if (strstr(line, "OK") && qicsgp_sent)
        {
            Debug_Print("[NET] APN set — activating PDP...\r\n");
            mqtt_state = MQTT_STATE_PDP_ACTIVATE;
            state_entered_ms = HAL_GetTick();
            qiact_sent = false; /* Modem_Process sends QIACT after buffer drains */
        }
        if (strstr(line, "ERROR"))
        {
            Debug_Print("[NET] APN config error — retrying\r\n");
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
            qicsgp_sent = false;
        }
        break;

    /* ── AT+QIACT=1 sent — wait for OK (up to 30 s) ──────────────────── */
    case MQTT_STATE_PDP_ACTIVATE:
        /* Guard with qiact_sent: stale "OK" from AT+QMTCLOSE / AT+QIDEACT
         * (sent during reconnect with blocking delays) must not be mistaken
         * for QIACT's OK.  SSL is already configured in Modem_Init.        */
        if (strstr(line, "OK") && qiact_sent)
        {
            Debug_Print("[NET] PDP active — opening broker\r\n");
            mqtt_state = MQTT_STATE_BROKER_OPEN;
            state_entered_ms = HAL_GetTick();

            char open_cmd[128];
            snprintf(open_cmd, sizeof(open_cmd),
                     "AT+QMTOPEN=0,\"%s\",%s", BROKER_HOST, BROKER_PORT);
            modem_cmd(open_cmd);
        }
        if (strstr(line, "ERROR"))
        {
            Debug_Print("[NET] PDP activate error — retrying\r\n");
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
        }
        /* 30 s timeout — QIACT sometimes silently fails */
        if (HAL_GetTick() - state_entered_ms > 30000)
        {
            Debug_Print("[NET] PDP activate timeout — retrying\r\n");
            mqtt_state = MQTT_STATE_NET_WAIT;
            state_entered_ms = HAL_GetTick();
        }
        break;

    /* ── waiting for +QMTOPEN: 0,0 ───────────────────────────────────── */
    case MQTT_STATE_BROKER_OPEN:
        /* This EC200U firmware sends two lines for a successful open:
         *   +QMTOPEN: 0,"hostname",port  ← TLS handshake started (set flag)
         *   OK                            ← TLS complete, session ready → CONNECT
         * Older FW sends only: +QMTOPEN: 0,0  (also handled below).
         * Do NOT send AT+QMTCONN until the OK arrives — TLS is not done yet. */
        if (strstr(line, "+QMTOPEN: 0,\""))
        {
            /* TCP connected, TLS handshake in progress — NOT ready yet.
             * Stay in BROKER_OPEN and wait for +QMTOPEN: 0,0            */
            Debug_Print("[MQTT] QMTOPEN TLS in progress...\r\n");
            qmtopen_tls_seen = true;
        }
        else if (strcmp(line, "OK") == 0 && qmtopen_tls_seen)
        {
            /* Intermediate OK — TLS still completing. Stay and wait for
             * +QMTOPEN: 0,0 which is the definitive success URC.        */
            Debug_Print("[MQTT] QMTOPEN intermediate OK — awaiting 0,0\r\n");
        }
        else if (strstr(line, "+QMTOPEN: 0,0"))
        {
            /* Definitive success — TLS complete, session ready. */
            qmtopen_tls_seen = false;
            mqtt_state = MQTT_STATE_CONNECTING;
            state_entered_ms = HAL_GetTick();

            /* Force MQTT 3.1.1 right before CONNECT — version may revert to 3
             * (MQTT 3.1) between init and here, which strips credentials.    */
            modem_cmd("AT+QMTCFG=\"version\",0,4");
            HAL_Delay(500); /* wait for OK before sending CONNECT             */

            char conn_cmd[128];
            if (MQTT_USERNAME[0] != '\0')
                snprintf(conn_cmd, sizeof(conn_cmd),
                         "AT+QMTCONN=0,\"%s\",\"%s\",\"%s\"",
                         CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
            else
                snprintf(conn_cmd, sizeof(conn_cmd),
                         "AT+QMTCONN=0,\"%s\"", CLIENT_ID);
            /* DEBUG: print exact CONN command so credentials are visible in serial monitor */
            { char dbg2[160]; snprintf(dbg2, sizeof(dbg2), "[MQTT] >> %s\r\n", conn_cmd); Debug_Print(dbg2); }
            modem_cmd(conn_cmd);
            Debug_Print("[MQTT] Broker open (0,0) — QMTCONN sent\r\n");
        }
        else if (strstr(line, "+QMTOPEN: 0,"))
        {
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Broker open FAILED — reconnecting\r\n");
            blink_n(1); /* 1 blink = BROKER_OPEN failed (TLS/TCP error) */
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        if (strstr(line, "+QMTCLOSE:") || strstr(line, "+QMTSTAT:"))
        {
            /* Connection closed while waiting — restart */
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Connection closed in BROKER_OPEN — reconnecting\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        if (strstr(line, "ERROR"))
        {
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Broker open ERROR — reconnecting\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTCONN response ───────────────────────────────── */
    case MQTT_STATE_CONNECTING:
        if (strstr(line, "+QMTCONN:"))
        {
            /* Response formats seen in the wild:
             *   +QMTCONN: 0,0      → success (older FW, no CONNACK code)
             *   +QMTCONN: 0,0,0    → success (CONNACK=0)
             *   +QMTCONN: 0,0,5    → refused (CONNACK=5, bad credentials)
             *   +QMTCONN: 0,1,X    → packet not sent (network issue)  */
            bool conn_ok =
                (strstr(line, "+QMTCONN: 0,0,0") != NULL) ||
                /* "+QMTCONN: 0,0" with no third param (older FW) */
                (strstr(line, "+QMTCONN: 0,0") != NULL &&
                 strstr(line, "+QMTCONN: 0,0,") == NULL);

            if (conn_ok)
            {
                Debug_Print("[MQTT] Connected to HiveMQ Cloud\r\n");
                mqtt_state = MQTT_STATE_SUBSCRIBING;
                state_entered_ms = HAL_GetTick();

                char sub_cmd[64];
                snprintf(sub_cmd, sizeof(sub_cmd),
                         "AT+QMTSUB=0,1,\"%s\",1", TOPIC_CMD);
                modem_cmd(sub_cmd);
            }
            else
            {
                /* Distinguish failure codes by blink count:
                 *  2 blinks = 0,2  transport error (TCP dropped)
                 *  4 blinks = 0,0,4 bad username/password
                 *  5 blinks = 0,0,5 not authorised
                 *  6 blinks = 0,0,1/2/3 protocol/ID/server error
                 *  7 blinks = 0,1  no CONNACK / retransmission      */
                if      (strstr(line, "+QMTCONN: 0,2"))   { Debug_Print("[MQTT] CONN fail: transport error\r\n");   blink_n(2); }
                else if (strstr(line, "+QMTCONN: 0,0,4")) { Debug_Print("[MQTT] CONN fail: bad credentials\r\n");  blink_n(4); }
                else if (strstr(line, "+QMTCONN: 0,0,5")) { Debug_Print("[MQTT] CONN fail: not authorised\r\n");   blink_n(5); }
                else if (strstr(line, "+QMTCONN: 0,1"))   { Debug_Print("[MQTT] CONN fail: no CONNACK\r\n");       blink_n(7); }
                else                                       { Debug_Print("[MQTT] CONN fail: other\r\n");            blink_n(6); }
                mqtt_state = MQTT_STATE_DISCONNECTED;
            }
        }
        if (strstr(line, "ERROR"))
        {
            /* AT layer rejected the command (syntax / state error).
             * 6 fast blinks = CONN ERROR. */
            Debug_Print("[MQTT] CONN command ERROR\r\n");
            blink_n(6);
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTSUB: 0,1,0 then 0,2,0 ──────────────────────── */
    case MQTT_STATE_SUBSCRIBING:
        if (strstr(line, "+QMTSUB: 0,1,0"))
        {
            /* cmd topic subscribed — now subscribe to OTA topic */
            Debug_Print("[MQTT] Subscribed to " TOPIC_CMD " — subscribing OTA...\r\n");
            char sub_cmd[64];
            snprintf(sub_cmd, sizeof(sub_cmd), "AT+QMTSUB=0,2,\"%s\",1", TOPIC_OTA);
            modem_cmd(sub_cmd);
        }
        if (strstr(line, "+QMTSUB: 0,2,0"))
        {
            /* OTA topic subscribed — fully connected now */
            /* DIAGNOSTIC: relay ON for 2 s then OFF */
            Relay1_Set(true);
            HAL_IWDG_Refresh(&hiwdg);
            HAL_Delay(2000);
            HAL_IWDG_Refresh(&hiwdg);
            Relay1_Set(false);
            HAL_Delay(300); HAL_IWDG_Refresh(&hiwdg);

            Debug_Print("[MQTT] Subscribed to " TOPIC_OTA "\r\n");
            blink_n(3); /* 3 blinks = MQTT fully connected! */
            HAL_IWDG_Refresh(&hiwdg);

            /* flush stale URCs — IWDG refresh every 500 ms inside */
            {
                uint8_t _c;
                uint32_t _t = HAL_GetTick();
                while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {
                    if (HAL_GetTick() - _t >= 500) {
                        HAL_IWDG_Refresh(&hiwdg);
                        _t = HAL_GetTick();
                    }
                }
            }
            rxpos = 0;
            HAL_IWDG_Refresh(&hiwdg);

            mqtt_state = MQTT_STATE_CONNECTED;
            /* publish current state immediately so Firebase is up to date on connect */
            prev_relay1       = relay1;
            prev_relay2       = relay2;
            prev_dry_run_trip = dry_run_tripped;
            last_heartbeat_ms = HAL_GetTick();
            publish_status();
        }
        break;

    /* ── fully connected — handle incoming messages ───────────────────── */
    case MQTT_STATE_CONNECTED:
        /* +QMTRECV is handled globally at the top of process_line so it
         * works in any state (PUBLISHING / PUB_WAIT_OK too).  No duplicate
         * handling needed here.                                            */

        /* unsolicited disconnect */
        if (strstr(line, "+QMTSTAT:"))
        {
            Debug_Print("[MQTT] Disconnected (QMTSTAT)\r\n");
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for '>' publish prompt ──────────────────────────────── */
    case MQTT_STATE_PUBLISHING:
        /* handled byte-by-byte in Modem_Process for '>' character */
        if (strstr(line, "ERROR"))
        {
            /* ERROR here means QMTPUBEX was rejected — connection is dead */
            Debug_Print("[MQTT] Publish prompt error — reconnecting\r\n");
            pub_pending = false;
            mqtt_state = MQTT_STATE_DISCONNECTED;
        }
        break;

    /* ── waiting for +QMTPUBEX: 0,0,0 ───────────────────────────────── */
    case MQTT_STATE_PUB_WAIT_OK:
        if (strstr(line, "+QMTPUBEX: 0,0,"))  /* any result code */
        {
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        /* Some FW only sends "OK" for QoS=0 without a +QMTPUBEX URC */
        else if (strcmp(line, "OK") == 0)
        {
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        if (strstr(line, "ERROR"))
        {
            Debug_Print("[MQTT] Publish failed\r\n");
            pub_pending = false;
            mqtt_state = MQTT_STATE_CONNECTED;
        }
        break;

    /* ── disconnected — will reconnect in Modem_Process ─────────────── */
    /* ── disconnected — will reconnect in Modem_Process ─────────────── */
    case MQTT_STATE_DISCONNECTED:
        break;

    /* ── SSL config — responses handled inline in Modem_Init sequence ── */
    case MQTT_STATE_SSL_CFG:
        break;
    }
}

/* ── OTA trigger helper — re-applies HTTP config before each download ────── */
static void modem_ota_start(const char *url)
{
    /* Re-apply SSL context 1 — AT+QIDEACT resets QSSLCFG RAM settings.
     * Without this, subsequent OTAs fail with SSL handshake timeout on
     * GitHub CDN (objects.githubusercontent.com).                        */
    modem_cmd("AT+QSSLCFG=\"sslversion\",1,4");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QSSLCFG=\"seclevel\",1,0");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    /* AT+QHTTPCFG settings are also lost after PDP deactivation/activation. */
    modem_cmd("AT+QHTTPCFG=\"sslctxid\",1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    modem_cmd("AT+QHTTPCFG=\"redirect\",1");
    HAL_Delay(300);
    HAL_IWDG_Refresh(&hiwdg);
    OTA_Start(url);
}

/* ── OTA publish callback — lets ota.c publish via MQTT ─────────────────── */
static void modem_ota_publish(const char *topic, const char *payload)
{
    /* OTA status is higher priority than any queued heartbeat/alert.
     * Clear any pending publish so OTA messages are never silently dropped. */
    pub_pending = false;
    queue_publish(topic, payload);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Modem_Init  — called once from main.c after UART init
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Init(UART_HandleTypeDef *huart)
{
    modem_uart = huart;
    rxpos = 0;
    mqtt_state = MQTT_STATE_BOOT;
    state_entered_ms = HAL_GetTick();

    Debug_Print("\r\n=== EC200U MQTT Pump Controller ===\r\n");
    Debug_Print("[MODEM] Pump ID : " PUMP_ID "\r\n");
    Debug_Print("[MODEM] Broker  : " BROKER_HOST "\r\n");
    Debug_Print("[MODEM] Waiting for EC200U ready...\r\n");

    /* Give EC200U 5 s to boot, then send AT to wake it */
    HAL_Delay(5000);
    modem_cmd("AT");
    HAL_Delay(300);
    modem_cmd("ATE0"); /* echo off */
    HAL_Delay(300);

    /* SSL config first — not affected by QMTCLOSE */
    modem_cmd("AT+QSSLCFG=\"sslversion\",0,4"); /* 4 = ALL TLS versions (ref code) */
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",0,0xFFFF"); /* all cipher suites        */
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"seclevel\",0,0");   /* 0=no cert verify (EMQX free tier uses self-signed) */
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"sni\",0,\"" BROKER_HOST "\""); /* SNI routing (lowercase!) */
    HAL_Delay(300);

    /* SSL context 1 — used by HTTP client for HTTPS OTA downloads.
     * Must be separate from context 0 (MQTT) so the broker SNI doesn't
     * interfere with GitHub/CDN TLS handshake.                             */
    modem_cmd("AT+QSSLCFG=\"sslversion\",1,4");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"ciphersuite\",1,0xFFFF");
    HAL_Delay(300);
    modem_cmd("AT+QSSLCFG=\"seclevel\",1,0");   /* no cert verify for testing */
    HAL_Delay(300);
    /* HTTP client: use SSL context 1, follow GitHub → CDN redirects */
    modem_cmd("AT+QHTTPCFG=\"sslctxid\",1");
    HAL_Delay(300);
    modem_cmd("AT+QHTTPCFG=\"redirect\",1");
    HAL_Delay(300);

    /* Close any lingering MQTT session and PDP context FIRST.
     * AT+QMTCLOSE reloads NVM MQTT config — so ALL AT+QMTCFG commands must
     * come AFTER this to avoid being overwritten by the NVM reload.          */
    modem_cmd("AT+QMTCLOSE=0");
    HAL_Delay(1500);
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {} }
    modem_cmd("AT+QIDEACT=1");
    HAL_Delay(2000);
    { uint8_t _c; while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {} }

    /* Configure MQTT client AFTER QMTCLOSE so NVM reload can't undo these.  */
    modem_cmd("AT+QMTCFG=\"ssl\",0,1,0");       /* MQTT client 0 → SSL context 0  */
    HAL_Delay(300);
    /* EC200U NVM default is MQTT 3.1 (version=3) which omits credentials from
     * the CONNECT packet — broker sees anonymous → CONNACK=5.
     * Must explicitly set version=4 (MQTT 3.1.1) AFTER every QMTCLOSE.     */
    modem_cmd("AT+QMTCFG=\"version\",0,4");     /* MQTT 3.1.1 — sends credentials */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"pdpcid\",0,1");      /* bind to PDP context 1          */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"keepalive\",0,300"); /* keepalive 300 s — survives OTA */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"session\",0,1");     /* clean session                  */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"will\",0,0");        /* disable Will — no stale topic  */
    HAL_Delay(300);
    modem_cmd("AT+QMTCFG=\"recv/mode\",0,1,1"); /* direct push + full payload URC */
    HAL_Delay(300);

    /* Wire up OTA send/publish callbacks so ota.c can issue AT commands
     * and publish MQTT status without a circular header dependency.         */
    OTA_Init();
    OTA_SetSendFn(Modem_Send);
    OTA_SetPublishFn(modem_ota_publish);

    /* Skip BOOT state — go directly to NET_WAIT.
     * Send AT+CEREG? now; its response arrives after Modem_Process starts
     * polling, avoiding any UART overflow / ORE timing issues.             */
    mqtt_state        = MQTT_STATE_NET_WAIT;
    state_entered_ms  = HAL_GetTick();
    modem_cmd("AT+CEREG?");   /* LTE registration query                     */
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Modem_Process  — called every loop iteration from main.c
 * ═══════════════════════════════════════════════════════════════════════════ */

void Modem_Process(void)
{
    if (!modem_uart)
        return;

    /* ── read bytes from EC200U into line buffer ── */
    /* Direct hardware register read — same approach as Modbus RX.
     * Bypasses HAL lock/unlock overhead (~6 µs per call) so the main loop
     * runs at <1 µs/iteration, fast enough to read USART2 Modbus bytes
     * (one every 1040 µs at 9600 baud) without overrun.                  */
    uint8_t c;
    for (;;) {
        uint32_t isr1 = modem_uart->Instance->ISR;
        if (isr1 & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
            modem_uart->Instance->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
            modem_uart->ErrorCode = HAL_UART_ERROR_NONE;
        }
        if (!(isr1 & USART_ISR_RXNE_RXFNE)) break;
        c = (uint8_t)(modem_uart->Instance->RDR & 0xFF);

        /* OTA binary passthrough — raw bytes from AT+QFREAD, before line logic */
        if (OTA_BinaryPending()) {
            OTA_FeedByte(c);
            continue;
        }

        if (c == '\r')
            continue;

        /* '>' is the publish prompt — not followed by '\n', catch it here */
        if (c == '>')
        {
            if (mqtt_state == MQTT_STATE_PUBLISHING && pub_pending)
            {
                /* Send payload immediately — any blocking delay here risks
                 * the modem aborting the input window.                     */
                HAL_UART_Transmit(modem_uart,
                                  (uint8_t *)pub_payload, strlen(pub_payload), 3000);
                /* Ctrl-Z commits the message */
                uint8_t ctrlz = 0x1A;
                HAL_UART_Transmit(modem_uart, &ctrlz, 1, 1000);
                mqtt_state = MQTT_STATE_PUB_WAIT_OK;
                state_entered_ms = HAL_GetTick();
            }
            rxpos = 0;
            continue;
        }

        if (c == '\n')
        {
            rxbuf[rxpos] = '\0';
            if (rxpos > 0)
                process_line(rxbuf);
            rxpos = 0;
        }
        else
        {
            if (rxpos < RX_BUF_SIZE - 1)
                rxbuf[rxpos++] = (char)c;
            else
                rxpos = 0; /* overflow guard */
        }
    }

    /* ── send AT+QIACT=1 after buffer is drained (avoids stale QMTCLOSE/QIDEACT OK) ── */
    if (mqtt_state == MQTT_STATE_PDP_ACTIVATE && !qiact_sent)
    {
        qiact_sent = true;
        modem_cmd("AT+QIACT=1");
    }

    /* ── send AT+QICSGP after buffer is drained (avoids spurious CGREG OK) ── */
    if (mqtt_state == MQTT_STATE_PDP_OPEN && !qicsgp_sent)
    {
        qicsgp_sent = true;
        char apn_cmd[80];
        snprintf(apn_cmd, sizeof(apn_cmd),
                 "AT+QICSGP=1,1,\"%s\",\"\",\"\",0", SIM_APN);
        modem_cmd(apn_cmd);
    }

    /* ── BROKER_OPEN / CONNECTING / SUBSCRIBING timeouts → reconnect ── */
    if ((mqtt_state == MQTT_STATE_BROKER_OPEN ||
         mqtt_state == MQTT_STATE_CONNECTING  ||
         mqtt_state == MQTT_STATE_SUBSCRIBING) &&
        HAL_GetTick() - state_entered_ms > 30000)
    {
        Debug_Print("[MQTT] Connection timeout — reconnecting\r\n");
        if (mqtt_state == MQTT_STATE_BROKER_OPEN) blink_n(1); /* 1 blink = BROKER_OPEN timed out */
        mqtt_state = MQTT_STATE_DISCONNECTED;
    }

    /* ── PUBLISHING timeout — no '>' within 10s means connection is dead ── */
    if (mqtt_state == MQTT_STATE_PUBLISHING &&
        HAL_GetTick() - state_entered_ms > 10000)
    {
        Debug_Print("[MQTT] Publish timeout — reconnecting\r\n");
        pub_pending = false;
        mqtt_state = MQTT_STATE_DISCONNECTED;
    }

    /* ── PUB_WAIT_OK timeout — if +QMTPUBEX never arrives, back to CONNECTED ── */
    if (mqtt_state == MQTT_STATE_PUB_WAIT_OK &&
        HAL_GetTick() - state_entered_ms > 5000)
    {
        Debug_Print("[MQTT] PubWaitOK timeout — continuing\r\n");
        pub_pending = false;
        mqtt_state = MQTT_STATE_CONNECTED;
    }

    /* ── periodic tasks (only when fully connected) ── */
    if (mqtt_state == MQTT_STATE_CONNECTED)
    {

        /* publish on state change — relay or dry-run trip changed */
        if (!pub_pending &&
            (relay1 != prev_relay1 || relay2 != prev_relay2 ||
             dry_run_tripped != prev_dry_run_trip))
        {
            prev_relay1       = relay1;
            prev_relay2       = relay2;
            prev_dry_run_trip = dry_run_tripped;
            publish_status();
        }

        /* heartbeat every 60 s — keeps Firebase online:true fresh */
        if (HAL_GetTick() - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS)
        {
            last_heartbeat_ms = HAL_GetTick();
            modem_cmd("AT+CSQ"); /* refresh signal strength; +CSQ updates last_rssi */
            publish_status();
        }

        /* run protection every 1 s */
        static uint32_t last_prot_ms = 0;
        if (HAL_GetTick() - last_prot_ms >= 1000)
        {
            last_prot_ms = HAL_GetTick();
            run_protection();
        }
    }

    /* ── send queued publish when connected and idle (not during OTA) ── */
    if (pub_pending && mqtt_state == MQTT_STATE_CONNECTED && !OTA_IsActive())
    {
        char pub_cmd[128];
        snprintf(pub_cmd, sizeof(pub_cmd),
                 "AT+QMTPUBEX=0,0,0,0,\"%s\",%d",  /* QoS=0: msgID must be 0 */
                 pub_topic, (int)strlen(pub_payload));
        modem_cmd(pub_cmd);
        mqtt_state = MQTT_STATE_PUBLISHING;
        state_entered_ms = HAL_GetTick();
    }

    /* ── auto-reconnect ── */
    if (mqtt_state == MQTT_STATE_DISCONNECTED)
    {
        static uint32_t last_reconnect = 0;
        if (HAL_GetTick() - last_reconnect > 10000)
        {
            last_reconnect = HAL_GetTick();
            state_entered_ms = HAL_GetTick();
            /* Refresh IWDG immediately — blink_n(5/6/7) may have consumed up to
             * 3800ms before setting DISCONNECTED, leaving < 200ms until timeout. */
            HAL_IWDG_Refresh(&hiwdg);
            qmtopen_tls_seen = false;
            Debug_Print("[MQTT] Reconnecting...\r\n");
            modem_cmd("AT+QMTCLOSE=0");
            HAL_Delay(1500);
            HAL_IWDG_Refresh(&hiwdg);
            /* Flush URCs from QMTCLOSE (TCP teardown can take several seconds).
             * Refresh IWDG every 500 ms so a chatty modem can't trigger a reset. */
            {
                uint8_t _c; uint32_t _t = HAL_GetTick();
                while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {
                    if (HAL_GetTick() - _t >= 500) { HAL_IWDG_Refresh(&hiwdg); _t = HAL_GetTick(); }
                }
                rxpos = 0;
            }
            /* Re-apply after QMTCLOSE — QMTCLOSE reloads NVM which resets version
             * to default=3 (MQTT 3.1) which strips credentials from CONNECT.  */
            modem_cmd("AT+QMTCFG=\"version\",0,4");  /* keep MQTT 3.1.1        */
            HAL_Delay(300);
            modem_cmd("AT+QMTCFG=\"will\",0,0");
            HAL_Delay(300);
            modem_cmd("AT+QIDEACT=1");
            HAL_Delay(1500);
            HAL_IWDG_Refresh(&hiwdg);
            /* Flush URCs from QIDEACT (PDP deactivation URCs may arrive late). */
            {
                uint8_t _c; uint32_t _t = HAL_GetTick();
                while (HAL_UART_Receive(modem_uart, &_c, 1, 1) == HAL_OK) {
                    if (HAL_GetTick() - _t >= 500) { HAL_IWDG_Refresh(&hiwdg); _t = HAL_GetTick(); }
                }
                rxpos = 0;
            }
            recv_payload_pending = false; /* clear stale recv state           */
            pub_pending          = false; /* drop unsent publish              */
            /* APN already configured — skip QICSGP, go straight to QIACT.
             * Modem_Process sends AT+QIACT=1 after the buffer is drained
             * so stale OK from QMTCLOSE/QIDEACT above is not misread.      */
            mqtt_state = MQTT_STATE_PDP_ACTIVATE;
            qiact_sent = false;
        }
    }

    /* ── net registration timeout — retry both GPRS and LTE ── */
    if (mqtt_state == MQTT_STATE_NET_WAIT &&
        HAL_GetTick() - state_entered_ms > 5000)
    {
        state_entered_ms = HAL_GetTick();
        modem_cmd("AT+CGREG?");
        HAL_Delay(200);
        modem_cmd("AT+CEREG?");
    }
}
