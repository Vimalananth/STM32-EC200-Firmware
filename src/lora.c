/* lora.c — Reyax RYLR998 LoRa driver — MASTER (USART3, PB8=TX, PB9=RX)
 *
 * Role   : MASTER  — address 1
 * Slave  : Blue Pill  — address 2  (controls pump03 relay1 + pump04 relay2)
 *
 * Commands sent to slave:
 *   P1:ON   P1:OFF   P2:ON   P2:OFF   STATUS?
 *
 * Replies received from slave:
 *   P1:ON OK   P1:OFF OK   P2:ON OK   P2:OFF OK
 *   S:R1:ON|R2:OFF|FL:12.5|TV:1250
 *   ERR:UNKNOWN CMD
 */

#include "lora.h"
#include "lora_ota.h"
#include "ota.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ─── config ────────────────────────────────────────────────────────────── */
#define LORA_OWN_ADDR   "1"
#define LORA_SLAVE_ADDR "2"
#define LORA_NETWORK_ID "6"
#define LORA_BAND       "865000000"
#define LORA_TX_POWER   "22"

/* ─── private state ─────────────────────────────────────────────────────── */
static UART_HandleTypeDef *lora_uart = NULL;

#define LORA_LINE_MAX 160
static char     lora_line[LORA_LINE_MAX];
static uint16_t lora_pos = 0;

/* ─── pending command retry ──────────────────────────────────────────────── */
#define LORA_CMD_ACK_TIMEOUT_MS  5000U   /* wait 5 s for ACK before retry    */
#define LORA_CMD_MAX_RETRIES     5U      /* give up after 5 retries           */
#define LORA_PING_INTERVAL_MS   150000U  /* ping slave if silent for 150 s (2.5 × 60 s heartbeat) */
static char     lora_pending_msg[16] = "";  /* e.g. "P1:ON" — empty = none   */
static uint32_t lora_pending_sent_ms = 0;
static uint8_t  lora_pending_retries = 0;

/* slave relay states — updated whenever a heartbeat/ack arrives */
static int   lora_relay3_state = -1;  /* -1 = unknown, 0 = OFF, 1 = ON */
static int   lora_relay4_state = -1;

/* slave flow meter — updated on heartbeat (integer, avoids soft-float) */
static uint32_t lora_flow_lpm_x10    = 0; /* L/min × 10, e.g. 125 = 12.5 L/min */
static uint32_t lora_total_litres_int = 0; /* tv from last heartbeat (resets on slave reboot) */
static uint32_t lora_tv_prev         = 0; /* previous tv — reboot detection     */
static uint32_t lora_tv_cumulative   = 0; /* true cumulative litres, never resets */
/* slave depth sensor — 0xFFFFFFFF = no reading / fault */
static uint32_t lora_depth_mm        = 0xFFFFFFFFUL;
/* slave battery — 0xFF = no reading / failed */
static uint8_t  lora_bat_pct         = 0xFFU;

/* slave EM4M energy meter — parsed from heartbeat when Blue Pill Modbus is valid */
static bool     lora_modbus_valid = false;
static int32_t  lora_v1x10  = 0;  /* phase 1 voltage × 10  (e.g. 2305 = 230.5 V) */
static int32_t  lora_v2x10  = 0;
static int32_t  lora_v3x10  = 0;
static int32_t  lora_i1x100 = 0;  /* phase 1 current × 100 (e.g.  125 = 1.25 A)  */
static int32_t  lora_i2x100 = 0;
static int32_t  lora_i3x100 = 0;
static int32_t  lora_kwx10  = 0;  /* total kW × 10         (e.g.   45 = 4.5 kW)  */
static uint32_t lora_kwh    = 0;  /* total kWh (integer)                           */

/* auto-ping — timestamp of last STATUS? we sent */
static uint32_t lora_last_ping_ms  = 0;

/* last received +RCV signal quality */
static int      lora_last_rssi     = 0;
static int      lora_last_snr      = 0;
static uint32_t lora_last_rcv_tick = 0; /* HAL_GetTick() of last +RCV */

/* ─── helpers ───────────────────────────────────────────────────────────── */

static void lora_send_cmd(const char *cmd)
{
    HAL_UART_Transmit(lora_uart, (const uint8_t *)cmd,    strlen(cmd), 300);
    HAL_UART_Transmit(lora_uart, (const uint8_t *)"\r\n", 2,           50);
}

static int lora_getc(uint8_t *out)
{
    if (__HAL_UART_GET_FLAG(lora_uart, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(lora_uart);
        lora_uart->ErrorCode = HAL_UART_ERROR_NONE;
        lora_uart->RxState   = HAL_UART_STATE_READY;
    }
    return (HAL_UART_Receive(lora_uart, out, 1, 0) == HAL_OK) ? 1 : 0;
}

static void lora_poll(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    while (HAL_GetTick() - t0 < timeout_ms)
    {
        uint8_t b;
        if (!lora_getc(&b)) continue;
        if (b == '\n')
        {
            lora_line[lora_pos] = '\0';
            if (lora_pos > 0)
            {
                char dbg[LORA_LINE_MAX + 16];
                snprintf(dbg, sizeof(dbg), "[LoRa] %s\r\n", lora_line);
                Debug_Print(dbg);
            }
            lora_pos = 0;
        }
        else if (b != '\r' && lora_pos < LORA_LINE_MAX - 1)
        {
            lora_line[lora_pos++] = (char)b;
        }
    }
}

/* ─── RCV parser ─────────────────────────────────────────────────────────
 * Handles +RCV=<addr>,<len>,<data>,<RSSI>,<SNR>
 * Parses slave heartbeat/ack: S:R1:ON|R2:OFF  or  P1:ON OK  etc.
 * ────────────────────────────────────────────────────────────────────── */
static void lora_process_rcv(const char *line)
{
    /* skip "+RCV=" prefix */
    const char *p = line + 5;

    /* skip addr field */
    p = strchr(p, ','); if (!p) return; p++;
    /* skip len field */
    p = strchr(p, ','); if (!p) return; p++;
    /* p now points to data — no commas inside relay/OTA messages */
    char data[160] = "";
    size_t i = 0;
    while (*p && *p != ',' && i < sizeof(data) - 1)
        data[i++] = *p++;
    data[i] = '\0';

    /* extract RSSI (4th field) and SNR (5th field) */
    if (*p == ',') { p++; lora_last_rssi = (int)strtol(p, NULL, 10); }
    p = strchr(p, ',');
    if (p)          { p++; lora_last_snr  = (int)strtol(p, NULL, 10); }
    lora_last_rcv_tick = HAL_GetTick();

    char dbg[80];
    if (!OTA_IsActive()) {
        snprintf(dbg, sizeof(dbg), "[LoRa] slave: %.63s\r\n", data);
        Debug_Print(dbg);
    }

    /* Route OTA protocol responses to lora_ota state machine */
    if (strncmp(data, "OTA:", 4) == 0) {
        LoRaOta_HandleResponse(data);
        return;
    }

    /* Parse status heartbeat: S:R1:ON|R2:OFF */
    if (strncmp(data, "S:R1:", 5) == 0)
    {
        lora_relay3_state = (strncmp(data + 5, "ON", 2) == 0) ? 1 : 0;

        const char *r2 = strstr(data, "R2:");
        if (r2)
            lora_relay4_state = (strncmp(r2 + 3, "ON", 2) == 0) ? 1 : 0;

        /* Parse flow meter fields (optional — old slaves omit them)
         * Format: FL:12.5  TV:1250  (no %f needed — integer parse) */
        const char *fl = strstr(data, "FL:");
        if (fl) {
            const char *p2 = fl + 3;
            long whole = strtol(p2, (char **)&p2, 10);
            long frac  = 0;
            if (*p2 == '.') frac = strtol(p2 + 1, NULL, 10) % 10;
            lora_flow_lpm_x10 = (uint32_t)(whole * 10 + frac);
        }
        const char *tv = strstr(data, "TV:");
        if (tv) {
            uint32_t new_tv = (uint32_t)strtol(tv + 3, NULL, 10);
            if (new_tv >= lora_tv_prev)
                lora_tv_cumulative += new_tv - lora_tv_prev;   /* normal delta   */
            else
                lora_tv_cumulative += new_tv;                  /* slave rebooted */
            lora_tv_prev          = new_tv;
            lora_total_litres_int = new_tv;
        }
        const char *dp = strstr(data, "DP:");
        if (dp) {
            int32_t dpv = (int32_t)strtol(dp + 3, NULL, 10);
            lora_depth_mm = (dpv >= 0) ? (uint32_t)dpv : 0xFFFFFFFFUL;
        }
        const char *batf = strstr(data, "BAT:");
        if (batf) {
            long bv = strtol(batf + 4, NULL, 10);
            lora_bat_pct = (bv >= 0 && bv <= 100) ? (uint8_t)bv : 0xFFU;
        }

        /* Parse EM4M energy meter fields — only present when Blue Pill Modbus valid */
        const char *v1f = strstr(data, "V1:");
        lora_modbus_valid = (v1f != NULL);
        if (lora_modbus_valid) {
            lora_v1x10  = strtol(v1f + 3, NULL, 10);
            const char *v2f  = strstr(data, "V2:");  if (v2f)  lora_v2x10  = strtol(v2f  + 3, NULL, 10);
            const char *v3f  = strstr(data, "V3:");  if (v3f)  lora_v3x10  = strtol(v3f  + 3, NULL, 10);
            const char *i1f  = strstr(data, "I1:");  if (i1f)  lora_i1x100 = strtol(i1f  + 3, NULL, 10);
            const char *i2f  = strstr(data, "I2:");  if (i2f)  lora_i2x100 = strtol(i2f  + 3, NULL, 10);
            const char *i3f  = strstr(data, "I3:");  if (i3f)  lora_i3x100 = strtol(i3f  + 3, NULL, 10);
            /* Use "|KW:" prefix to distinguish from "|KWH:" */
            const char *kwf  = strstr(data, "|KW:"); if (kwf)  lora_kwx10  = strtol(kwf  + 4, NULL, 10);
            const char *kwhf = strstr(data, "KWH:"); if (kwhf) lora_kwh    = (uint32_t)strtol(kwhf + 4, NULL, 10);
        }

        if (!OTA_IsActive()) {
            snprintf(dbg, sizeof(dbg), "[LoRa] HB r3=%d r4=%d fl=%lu.%lu tv=%lu tot=%lu\r\n",
                     lora_relay3_state, lora_relay4_state,
                     (unsigned long)(lora_flow_lpm_x10 / 10),
                     (unsigned long)(lora_flow_lpm_x10 % 10),
                     (unsigned long)lora_total_litres_int,
                     (unsigned long)lora_tv_cumulative);
            Debug_Print(dbg);
        }
        return;
    }

    /* Parse ack: P1:ON OK or P1:OFF OK  — also clears pending command */
    if (strncmp(data, "P1:", 3) == 0)
    {
        lora_relay3_state = (strncmp(data + 3, "ON", 2) == 0) ? 1 : 0;
        if (lora_pending_msg[0] != '\0' &&
            strncmp(data, lora_pending_msg, strlen(lora_pending_msg)) == 0)
            lora_pending_msg[0] = '\0';   /* ACK received — cancel retry */
        return;
    }
    if (strncmp(data, "P2:", 3) == 0)
    {
        lora_relay4_state = (strncmp(data + 3, "ON", 2) == 0) ? 1 : 0;
        if (lora_pending_msg[0] != '\0' &&
            strncmp(data, lora_pending_msg, strlen(lora_pending_msg)) == 0)
            lora_pending_msg[0] = '\0';   /* ACK received — cancel retry */
        return;
    }
}

/* ─── public API ────────────────────────────────────────────────────────── */

void LoRa_Init(UART_HandleTypeDef *huart)
{
    lora_uart = huart;
    lora_pos  = 0;

    lora_send_cmd("AT+RESET");
    lora_poll(3500);

    lora_send_cmd("AT");
    lora_poll(300);
    lora_send_cmd("AT+ADDRESS=" LORA_OWN_ADDR);
    lora_poll(300);
    lora_send_cmd("AT+NETWORKID=" LORA_NETWORK_ID);
    lora_poll(300);
    lora_send_cmd("AT+BAND=" LORA_BAND);
    lora_poll(500);
    lora_send_cmd("AT+CRFOP=" LORA_TX_POWER);
    lora_poll(300);
    lora_send_cmd("AT+PARAMETER=9,7,1,12");
    lora_poll(300);
    lora_send_cmd("AT+MODE=0");
    lora_poll(300);

    Debug_Print("[LoRa] --- module settings ---\r\n");
    lora_send_cmd("AT+ADDRESS?");   lora_poll(300);
    lora_send_cmd("AT+NETWORKID?"); lora_poll(300);
    lora_send_cmd("AT+BAND?");      lora_poll(300);
    lora_send_cmd("AT+PARAMETER?"); lora_poll(300);
    Debug_Print("[LoRa] MASTER init done (addr=1 net=6 band=865MHz)\r\n");

    /* Ping slave immediately to get its state without waiting for the next
     * 60-second heartbeat.  Sets lora_last_ping_ms so LoRa_Process()
     * won't fire another ping for 150 s.                                  */
    LoRa_SendStatusRequest();
}

/*
 * LoRa_SendRelay — send relay command to slave (Blue Pill, addr=2)
 *   relay_num : 1 = pump03 (P1), 2 = pump04 (P2)
 *   on        : true = ON, false = OFF
 */
void LoRa_SendRelay(int relay_num, bool on)
{
    if (!lora_uart) return;

    char msg[16];
    snprintf(msg, sizeof(msg), "P%d:%s", relay_num, on ? "ON" : "OFF");

    /* Save as pending — LoRa_Process() will retry if no ACK within 5 s */
    strncpy(lora_pending_msg, msg, sizeof(lora_pending_msg) - 1);
    lora_pending_msg[sizeof(lora_pending_msg) - 1] = '\0';
    lora_pending_sent_ms = HAL_GetTick();
    lora_pending_retries = 0;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s",
             LORA_SLAVE_ADDR, (int)strlen(msg), msg);

    char dbg[80];
    snprintf(dbg, sizeof(dbg), "[LoRa] TX -> slave: %s\r\n", msg);
    Debug_Print(dbg);

    lora_send_cmd(cmd);
}

void LoRa_SendStatusRequest(void)
{
    if (!lora_uart) return;
    static const char msg[] = "STATUS?";
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s",
             LORA_SLAVE_ADDR, (int)(sizeof(msg) - 1), msg);
    Debug_Print("[LoRa] TX -> slave: STATUS?\r\n");
    lora_send_cmd(cmd);
    lora_last_ping_ms = HAL_GetTick();
}

void LoRa_Process(void)
{
    if (!lora_uart) return;

    /* ─── auto-ping: request status if slave has been silent for 90 s ───
     * Rate-limited: at most one ping per LORA_PING_INTERVAL_MS.          */
    if (LoRa_GetLastRcvAge() > LORA_PING_INTERVAL_MS &&
        HAL_GetTick() - lora_last_ping_ms > LORA_PING_INTERVAL_MS)
    {
        Debug_Print("[LoRa] slave silent — sending STATUS?\r\n");
        LoRa_SendStatusRequest();
    }

    /* ─── pending command retry ──────────────────────────────────────────
     * If no ACK within LORA_CMD_ACK_TIMEOUT_MS, resend up to MAX_RETRIES. */
    if (lora_pending_msg[0] != '\0' &&
        HAL_GetTick() - lora_pending_sent_ms >= LORA_CMD_ACK_TIMEOUT_MS)
    {
        if (lora_pending_retries < LORA_CMD_MAX_RETRIES)
        {
            lora_pending_retries++;
            lora_pending_sent_ms = HAL_GetTick();

            char cmd[64];
            snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s",
                     LORA_SLAVE_ADDR, (int)strlen(lora_pending_msg), lora_pending_msg);
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "[LoRa] retry %u/%u -> %s\r\n",
                     (unsigned)lora_pending_retries, (unsigned)LORA_CMD_MAX_RETRIES,
                     lora_pending_msg);
            Debug_Print(dbg);
            lora_send_cmd(cmd);
        }
        else
        {
            char dbg[80];
            snprintf(dbg, sizeof(dbg), "[LoRa] cmd FAILED after %u retries: %s\r\n",
                     (unsigned)LORA_CMD_MAX_RETRIES, lora_pending_msg);
            Debug_Print(dbg);
            lora_pending_msg[0] = '\0';
        }
    }

    uint8_t b;
    while (lora_getc(&b))
    {
        if (b == '\n')
        {
            lora_line[lora_pos] = '\0';
            if (lora_pos > 0)
            {
                /* Suppress debug print during OTA binary stream — Debug_Print
                 * blocks ~87µs/char on USART3; a 50-char LoRa line stalls the
                 * main loop ~4ms, overflowing the 8-byte modem UART FIFO and
                 * losing stream bytes → CRC mismatch.  Relay commands still
                 * execute; only the serial log line is skipped.              */
                if (!OTA_IsActive()) {
                    char dbg[LORA_LINE_MAX + 16];
                    snprintf(dbg, sizeof(dbg), "[LoRa] RX: %s\r\n", lora_line);
                    Debug_Print(dbg);
                }

                if (strncmp(lora_line, "+RCV=", 5) == 0)
                    lora_process_rcv(lora_line);
            }
            lora_pos = 0;
        }
        else if (b != '\r' && lora_pos < LORA_LINE_MAX - 1)
        {
            lora_line[lora_pos++] = (char)b;
        }
    }
}

/* Send an arbitrary message to the slave (used by lora_ota.c for OTA protocol) */
void LoRa_SendRaw(const char *msg)
{
    if (!lora_uart) return;
    char cmd[200];  /* OTA:D:nnnn:<128hex>:<crc16> = 144 chars + AT+SEND header */
    snprintf(cmd, sizeof(cmd), "AT+SEND=%s,%d,%s", LORA_SLAVE_ADDR, (int)strlen(msg), msg);
    lora_send_cmd(cmd);
}

int      LoRa_GetRelay3State(void)       { return lora_relay3_state; }
int      LoRa_GetRelay4State(void)       { return lora_relay4_state; }
int      LoRa_GetLastRSSI(void)          { return lora_last_rssi; }
int      LoRa_GetLastSNR(void)           { return lora_last_snr; }
/* Flow: returns L/min × 10 (e.g. 125 = 12.5 L/min) */
uint32_t LoRa_GetFlowLpmX10(void)        { return lora_flow_lpm_x10; }
/* tv from last heartbeat — resets when slave reboots */
uint32_t LoRa_GetTotalLitresInt(void)    { return lora_total_litres_int; }
/* True cumulative litres — survives slave reboots (resets only on master reboot) */
uint32_t LoRa_GetTvCumulative(void)      { return lora_tv_cumulative; }
/* EM4M energy meter fields from Blue Pill heartbeat */
bool     LoRa_IsModbusValid(void)        { return lora_modbus_valid; }
int32_t  LoRa_GetV1x10(void)             { return lora_v1x10;  }
int32_t  LoRa_GetV2x10(void)             { return lora_v2x10;  }
int32_t  LoRa_GetV3x10(void)             { return lora_v3x10;  }
int32_t  LoRa_GetI1x100(void)            { return lora_i1x100; }
int32_t  LoRa_GetI2x100(void)            { return lora_i2x100; }
int32_t  LoRa_GetI3x100(void)            { return lora_i3x100; }
int32_t  LoRa_GetKWx10(void)             { return lora_kwx10;  }
uint32_t LoRa_GetKWh(void)               { return lora_kwh;    }
/* Depth in mm from last heartbeat; 0xFFFFFFFF = fault or no sensor */
uint32_t LoRa_GetDepthMm(void)           { return lora_depth_mm; }
/* Slave battery percentage 0-100; 0xFF = not present or read failed */
uint8_t  LoRa_GetSlaveBatPct(void)       { return lora_bat_pct;  }
/* Returns ms since last +RCV, or 0xFFFFFFFF if never received */
uint32_t LoRa_GetLastRcvAge(void)
{
    if (lora_last_rcv_tick == 0) return 0xFFFFFFFFUL;
    return HAL_GetTick() - lora_last_rcv_tick;
}
