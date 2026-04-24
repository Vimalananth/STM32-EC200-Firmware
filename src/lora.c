/* lora.c — Reyax RYL998 LoRa driver (USART3, PB8=TX, PB9=RX, 115200 baud)
 *
 * Init:    blocking AT command sequence (called before IWDG starts — safe)
 * Process: non-blocking byte-by-byte RX, prints +RCV frames to serial monitor
 *
 * IMPORTANT — both devices must use the same settings:
 *   NETWORKID, BAND, PARAMETER (SF/BW/CR/Preamble)
 */

#include "lora.h"
#include "main.h"
#include <string.h>
#include <stdio.h>

/* ─── private state ────────────────────────────────────────────────────── */
static UART_HandleTypeDef *lora_uart = NULL;

#define LORA_LINE_MAX 160
static char     lora_line[LORA_LINE_MAX];
static uint16_t lora_pos = 0;

/* ─── helpers ───────────────────────────────────────────────────────────── */

/* Send an AT command followed by \r\n */
static void lora_send_cmd(const char *cmd)
{
    HAL_UART_Transmit(lora_uart, (const uint8_t *)cmd,   strlen(cmd), 300);
    HAL_UART_Transmit(lora_uart, (const uint8_t *)"\r\n", 2,          50);
}

/* Read one character from LoRa UART (non-blocking, timeout=0) */
static int lora_getc(uint8_t *out)
{
    return (HAL_UART_Receive(lora_uart, out, 1, 0) == HAL_OK) ? 1 : 0;
}

/* Flush the RX line buffer */
static void lora_process_line(void)
{
    /* strip trailing \r */
    if (lora_pos > 0 && lora_line[lora_pos - 1] == '\r')
        lora_line[--lora_pos] = '\0';
    else
        lora_line[lora_pos] = '\0';

    if (lora_pos == 0) return;  /* skip blank lines */

    /* Print every line to serial monitor so the user sees AT responses
     * during startup and +RCV frames during normal operation.        */
    char out[LORA_LINE_MAX + 16];
    snprintf(out, sizeof(out), "[LoRa] %s\r\n", lora_line);
    Debug_Print(out);

    lora_pos = 0;
}

/* ─── public API ────────────────────────────────────────────────────────── */

/**
 * @brief  Configure the RYL998 with sensible defaults.
 *         Called once before the main loop (IWDG not yet started — safe to delay).
 * @param  huart  Pointer to USART3 handle (initialised by MX_USART3_UART_Init)
 */
void LoRa_Init(UART_HandleTypeDef *huart)
{
    lora_uart = huart;
    lora_pos  = 0;

    HAL_Delay(500);                           /* let RYL998 finish booting */

    lora_send_cmd("AT");                      /* basic connectivity test   */
    HAL_Delay(100);
    lora_send_cmd("AT+ADDRESS=1");            /* this device address       */
    HAL_Delay(100);
    lora_send_cmd("AT+NETWORKID=5");          /* network group (0-16)      */
    HAL_Delay(100);
    lora_send_cmd("AT+BAND=865000000");       /* 865 MHz (India ISM band)  */
    HAL_Delay(100);
    lora_send_cmd("AT+PARAMETER=9,7,1,12");   /* SF9, BW125, CR4/5, P=12  */
    HAL_Delay(100);
    lora_send_cmd("AT+MODE=0");               /* transceiver (RX+TX) mode  */
    HAL_Delay(100);

    Debug_Print("[LoRa] RYL998 init complete (BAND=865MHz SF9 BW125)\r\n");
}

/**
 * @brief  Non-blocking LoRa RX processing — call every main loop iteration.
 *         Assembles incoming bytes into lines; prints each completed line
 *         to the debug serial monitor via Debug_Print.
 */
void LoRa_Process(void)
{
    if (!lora_uart) return;

    uint8_t b;
    while (lora_getc(&b))
    {
        if (b == '\n')
        {
            lora_process_line();
        }
        else
        {
            if (lora_pos < LORA_LINE_MAX - 1)
                lora_line[lora_pos++] = (char)b;
            else
                lora_pos = 0;  /* overflow — discard and re-sync */
        }
    }
}
