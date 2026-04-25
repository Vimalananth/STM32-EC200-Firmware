/* lora.c — Reyax RYL998 LoRa driver (USART3, PB8=TX, PB9=RX, 115200 baud) */

#include "lora.h"
#include "main.h"
#include "modem.h"          /* Relay1_Set(), Relay1_Get() */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ─── private state ─────────────────────────────────────────────────────── */
static UART_HandleTypeDef *lora_uart = NULL;

#define LORA_LINE_MAX   160
#define LORA_DEST_ADDR  1       /* ESP32 transmitter address */
#define LORA_OWN_ADDR   2       /* This STM32 receiver address */

static char     lora_line[LORA_LINE_MAX];
static uint16_t lora_pos = 0;

/* ─── forward declarations ──────────────────────────────────────────────── */
static void lora_send_cmd(const char *cmd);
static void lora_send_data(const char *payload);
static void lora_handle_rcv(const char *line);
static void lora_process_line(void);

/* ─── helpers ───────────────────────────────────────────────────────────── */

static void lora_send_cmd(const char *cmd)
{
    HAL_UART_Transmit(lora_uart, (const uint8_t *)cmd,    strlen(cmd), 300);
    HAL_UART_Transmit(lora_uart, (const uint8_t *)"\r\n", 2,           50);
}

static int lora_getc(uint8_t *out)
{
    /* Clear overrun/framing errors that permanently block polling receive.
     * ORE is set when a byte arrives before the previous one was read
     * (no FIFO on USART3). The lost byte is discarded but UART un-sticks. */
    if (__HAL_UART_GET_FLAG(lora_uart, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(lora_uart);
        lora_uart->ErrorCode = HAL_UART_ERROR_NONE;
        lora_uart->RxState   = HAL_UART_STATE_READY;
    }
    return (HAL_UART_Receive(lora_uart, out, 1, 0) == HAL_OK) ? 1 : 0;
}

/* Send a data payload back to the transmitter using AT+SEND */
static void lora_send_data(const char *payload)
{
    char cmd[LORA_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "AT+SEND=%d,%d,%s",
             LORA_DEST_ADDR, (int)strlen(payload), payload);

    char dbg[LORA_LINE_MAX + 16];
    snprintf(dbg, sizeof(dbg), "[LoRa] TX >> %s\r\n", payload);
    Debug_Print(dbg);

    lora_send_cmd(cmd);
}

/* ─── +RCV parser ───────────────────────────────────────────────────────── */
/*
 * Format: +RCV=<addr>,<len>,<data>,<rssi>,<snr>
 * Example: +RCV=1,8,RELAY_ON,-45,9
 */
static void lora_handle_rcv(const char *line)
{
    char buf[LORA_LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Skip "+RCV=" prefix */
    char *p = buf + 5;

    /* addr */
    char *tok = strtok(p, ",");
    if (!tok) return;
    int addr = atoi(tok);

    /* len (advance parser, value unused) */
    tok = strtok(NULL, ",");
    if (!tok) return;

    /* data */
    tok = strtok(NULL, ",");
    if (!tok) return;
    char data[64];
    strncpy(data, tok, sizeof(data) - 1);
    data[sizeof(data) - 1] = '\0';

    /* rssi */
    tok = strtok(NULL, ",");
    int rssi = tok ? atoi(tok) : 0;

    /* snr */
    tok = strtok(NULL, ",");
    int snr = tok ? atoi(tok) : 0;

    /* Print decoded frame */
    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "[LoRa] RX from:%d  data:%s  RSSI:%ddBm  SNR:%ddB\r\n",
             addr, data, rssi, snr);
    Debug_Print(dbg);

    /* ── Command dispatch ── */
    if (strcmp(data, "RELAY_ON") == 0)
    {
        Relay1_Set(true);
        lora_send_data("ACK_ON");
    }
    else if (strcmp(data, "RELAY_OFF") == 0)
    {
        Relay1_Set(false);
        lora_send_data("ACK_OFF");
    }
    else if (strcmp(data, "STATUS") == 0)
    {
        char status[32];
        snprintf(status, sizeof(status),
                 "ST:%s,R:%d,S:%d",
                 Relay1_Get() ? "ON" : "OFF",
                 rssi, snr);
        lora_send_data(status);
    }
    else
    {
        lora_send_data("ERR:UNKNOWN");
    }
}

/* ─── line processor ────────────────────────────────────────────────────── */

static void lora_process_line(void)
{
    /* Strip trailing \r */
    if (lora_pos > 0 && lora_line[lora_pos - 1] == '\r')
        lora_line[--lora_pos] = '\0';
    else
        lora_line[lora_pos] = '\0';

    if (lora_pos == 0) return;

    /* Print raw line */
    char out[LORA_LINE_MAX + 16];
    snprintf(out, sizeof(out), "[LoRa] %s\r\n", lora_line);
    Debug_Print(out);

    /* Handle incoming data frame */
    if (strncmp(lora_line, "+RCV=", 5) == 0)
        lora_handle_rcv(lora_line);

    lora_pos = 0;
}

static void lora_poll_response(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        uint8_t b;
        if (lora_getc(&b))   /* uses ORE-recovery lora_getc */
        {
            if (b == '\n')
                lora_process_line();
            else if (lora_pos < LORA_LINE_MAX - 1)
                lora_line[lora_pos++] = (char)b;
            else
                lora_pos = 0;
        }
    }
}

/* ─── public API ────────────────────────────────────────────────────────── */

void LoRa_Init(UART_HandleTypeDef *huart)
{
    lora_uart = huart;
    lora_pos  = 0;

    lora_send_cmd("AT+RESET");
    lora_poll_response(3500);     /* wait up to 3.5s for +READY — module can be slow after OTA */

    lora_send_cmd("AT");
    lora_poll_response(300);
    lora_send_cmd("AT+ADDRESS=2");
    lora_poll_response(300);
    lora_send_cmd("AT+NETWORKID=18");
    lora_poll_response(300);
    lora_send_cmd("AT+BAND=865000000");
    lora_poll_response(500);
    lora_send_cmd("AT+CRFOP=22");
    lora_poll_response(300);
    lora_send_cmd("AT+PARAMETER=9,7,1,12");
    lora_poll_response(300);
    lora_send_cmd("AT+MODE=0");
    lora_poll_response(300);

    Debug_Print("[LoRa] --- module settings ---\r\n");
    lora_send_cmd("AT+ADDRESS?");   lora_poll_response(300);
    lora_send_cmd("AT+NETWORKID?"); lora_poll_response(300);
    lora_send_cmd("AT+BAND?");      lora_poll_response(300);
    lora_send_cmd("AT+PARAMETER?"); lora_poll_response(300);
    Debug_Print("[LoRa] --- end settings ---\r\n");

    Debug_Print("[LoRa] RYL998 init complete (BAND=865MHz NETID=18 ADDR=2)\r\n");
}

void LoRa_Process(void)
{
    if (!lora_uart) return;

    uint8_t b;
    while (lora_getc(&b))
    {
        if (b == '\n')
            lora_process_line();
        else
        {
            if (lora_pos < LORA_LINE_MAX - 1)
                lora_line[lora_pos++] = (char)b;
            else
                lora_pos = 0;
        }
    }
}
