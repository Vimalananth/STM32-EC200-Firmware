/* modbus.c
 * Non-blocking Modbus RTU master — Selec EM4M-3P-C-100A energy meter.
 *
 * Hardware:
 *   USART2  PA2=TX → DI, PA3=RX ← RO
 *   PA8     GPIO output → DE + RE (tied together)
 *           HIGH = transmit, LOW = receive
 *
 * Request:  FC04, slave 0x01, start 0x0000, count 22 (0x16)
 *           Reads V1N, V2N, V3N, AvgVLN, V12, V23, V31, AvgVLL, I1, I2, I3
 * Response: 49 bytes — 3 header + 44 data + 2 CRC
 *           V1N  @ buf+3,   V2N  @ buf+7,   V3N  @ buf+11
 *           I1   @ buf+35,  I2   @ buf+39,  I3   @ buf+43
 *
 * Note: Selec EM4M limits FC04 to max 32 registers per request.
 *       PF registers (0x0030+) need a separate second request — not yet implemented.
 *
 * All measurement values are IEEE 754 float, big-endian (ABCD Modbus standard),
 * in engineering units (V, A, PF) — no scaling required.
 *
 * Poll cycle: every 2 seconds.
 * The RX phase is non-blocking (1 ms HAL timeout per call) so USART1 (modem)
 * remains serviced every main-loop iteration.
 */

#include "modbus.h"
#include "modem.h"
#include "main.h"
#include <string.h>

/* ── Hardware ────────────────────────────────────────────────────────────── */
#define DE485_PORT   GPIOA
#define DE485_PIN    GPIO_PIN_8

#define DE_TX()      HAL_GPIO_WritePin(DE485_PORT, DE485_PIN, GPIO_PIN_SET)
#define DE_RX()      HAL_GPIO_WritePin(DE485_PORT, DE485_PIN, GPIO_PIN_RESET)

/* ── Modbus frame parameters ─────────────────────────────────────────────── */
#define MB_SLAVE      0x01
#define MB_FC         0x04   /* Read Input Registers — EM4M live V/I measurements */
#define MB_START_HI   0x00
#define MB_START_LO   0x00   /* first register: V1N at 0x0000 */
#define MB_COUNT_HI   0x00
#define MB_COUNT_LO   0x16   /* 22 registers = 44 bytes data (V1-3, I1-3)  */

#define TX_LEN        8      /* request frame length            */
#define RX_LEN        49     /* 3 + 44 + 2                      */

/* ── State machine ───────────────────────────────────────────────────────── */
typedef enum {
    MB_IDLE,
    MB_TX,
    MB_RX_WAIT,
    MB_PARSE,
} MbState;

static UART_HandleTypeDef *mb_uart;
static MbState  state       = MB_IDLE;
static uint32_t last_poll   = 0;
static uint32_t rx_start    = 0;
static uint8_t  tx_buf[TX_LEN];
static uint8_t  rx_buf[RX_LEN];
static uint8_t  rx_idx      = 0;

/* Safe defaults until first successful read (avoids false protection trips) */
static float v1 = 415.0f, v2 = 415.0f, v3 = 415.0f;
static float i1 = 5.0f,   i2 = 5.0f,   i3 = 5.0f;
static float pf1 = 1.0f,  pf2 = 1.0f,  pf3 = 1.0f;

/* Diagnostic counters — visible in Firebase as mb_ok / mb_rx */
static uint8_t mb_last_rx = 0;   /* bytes received in last transaction (0 = no response) */
static bool    mb_data_ok = false; /* true if last CRC passed */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint16_t crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

static float parse_float_be(const uint8_t *b)
{
    /* Selec EM4M uses CDAB byte order (low 16-bit word transmitted first).
     * e.g. 230 V = 0x43660000 → wire: [0x00,0x00,0x43,0x66]
     *   b[0]=lo_hi  b[1]=lo_lo  b[2]=hi_hi  b[3]=hi_lo
     * Reassemble: (b[2]<<24)|(b[3]<<16)|(b[0]<<8)|b[1]            */
    uint32_t u = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16)
               | ((uint32_t)b[0] <<  8) |  (uint32_t)b[1];
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void build_request(void)
{
    tx_buf[0] = MB_SLAVE;
    tx_buf[1] = MB_FC;
    tx_buf[2] = MB_START_HI;
    tx_buf[3] = MB_START_LO;
    tx_buf[4] = MB_COUNT_HI;
    tx_buf[5] = MB_COUNT_LO;
    uint16_t crc = crc16(tx_buf, 6);
    tx_buf[6] = (uint8_t)(crc & 0xFF);       /* CRC low  */
    tx_buf[7] = (uint8_t)((crc >> 8) & 0xFF);/* CRC high */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void Modbus_Init(UART_HandleTypeDef *huart)
{
    mb_uart   = huart;
    state     = MB_IDLE;
    last_poll = 0;
    rx_idx    = 0;
    DE_RX();  /* default: receive mode (/RE LOW = receiver enabled on genuine MAX485) */
}

void Modbus_Process(void)
{
    if (!mb_uart) return;
    uint32_t now = HAL_GetTick();

    switch (state) {

    /* ── IDLE: only poll when MQTT is connected ───────────────────────── */
    case MB_IDLE:
        /* Block Modbus TX entirely during MQTT connection setup.
         * The 8.3 ms HAL_UART_Transmit block starves USART1 and corrupts
         * critical modem URCs (+QMTCONN, +QMTSUB) that fit in < 10 ms. */
        if (Modem_IsConnected() && (now - last_poll >= 2000U)) {
            state = MB_TX;
        }
        break;

    /* ── TX: send Modbus request ──────────────────────────────────────── */
    case MB_TX:
        build_request();
        rx_idx    = 0;
        mb_last_rx = 0;
        DE_TX();
        /* ~8.3 ms at 9600 baud — blocking but well within IWDG 4s window */
        HAL_UART_Transmit(mb_uart, tx_buf, TX_LEN, 20);
        DE_RX();
        /* Clear any UART error flags before RX phase.
         * With genuine MAX485 (/RE active-LOW), receiver is OFF during TX so
         * no echo occurs.  This clear is a precaution only.                  */
        mb_uart->ErrorCode = HAL_UART_ERROR_NONE;
        __HAL_UART_CLEAR_FLAG(mb_uart, UART_CLEAR_OREF | UART_CLEAR_FEF | UART_CLEAR_NEF);
        rx_start = HAL_GetTick();
        state    = MB_RX_WAIT;
        break;

    /* ── RX_WAIT: accumulate response bytes, fully non-blocking ──────── */
    case MB_RX_WAIT: {
        /* Read directly from hardware registers — bypasses HAL error
         * checking so a stale ORE/FE/NE flag can never block reception.
         * Clears error flags on each iteration to handle ORE mid-burst. */
        uint8_t byte;
        while (1) {
            uint32_t isr = mb_uart->Instance->ISR;
            if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
                mb_uart->Instance->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
                mb_uart->ErrorCode = HAL_UART_ERROR_NONE;
            }
            if (!(isr & USART_ISR_RXNE_RXFNE))   /* STM32G0: RXNE bit is RXNE_RXFNE */
                break;                             /* no byte available   */
            byte = (uint8_t)(mb_uart->Instance->RDR & 0xFFU);
            mb_last_rx++;
            if (rx_idx < RX_LEN) {
                rx_buf[rx_idx++] = byte;
            }
            if (rx_idx >= RX_LEN) {
                state = MB_PARSE;
                return;
            }
        }
        /* Timeout: meter did not complete response within 500 ms */
        if (now - rx_start > 500U) {
            last_poll = now;
            state     = MB_IDLE;
        }
        break;
    }

    /* ── PARSE: validate CRC and extract floats ───────────────────────── */
    case MB_PARSE: {
        /* Sanity: check slave addr, FC, and byte count */
        mb_data_ok = false;
        if (rx_buf[0] == MB_SLAVE && rx_buf[1] == MB_FC && rx_buf[2] == 0x2C) {
            /* CRC is over everything except the trailing 2 CRC bytes */
            uint16_t crc = crc16(rx_buf, RX_LEN - 2);
            uint16_t rx_crc = (uint16_t)rx_buf[RX_LEN - 2]
                            | ((uint16_t)rx_buf[RX_LEN - 1] << 8);
            if (crc == rx_crc) {
                mb_data_ok = true;
                /* Response layout — each float = 2 registers = 4 bytes
                 * Offset = 3 + (modbus_addr * 2)
                 * V1N(0x0000)→buf+3   V2N(0x0002)→buf+7   V3N(0x0004)→buf+11
                 * I1 (0x0010)→buf+35  I2 (0x0012)→buf+39  I3 (0x0014)→buf+43 */
                v1 = parse_float_be(rx_buf + 3);
                v2 = parse_float_be(rx_buf + 7);
                v3 = parse_float_be(rx_buf + 11);
                i1 = parse_float_be(rx_buf + 35);
                i2 = parse_float_be(rx_buf + 39);
                i3 = parse_float_be(rx_buf + 43);
            }
        }
        last_poll = now;
        state     = MB_IDLE;
        break;
    }
    }
}

/* ── Getters ─────────────────────────────────────────────────────────────── */
float    Modbus_GetV1(void)       { return v1; }
float    Modbus_GetV2(void)       { return v2; }
float    Modbus_GetV3(void)       { return v3; }
float    Modbus_GetI1(void)       { return i1; }
float    Modbus_GetI2(void)       { return i2; }
float    Modbus_GetI3(void)       { return i3; }
float    Modbus_GetPF1(void)      { return pf1; }
float    Modbus_GetPF2(void)      { return pf2; }
float    Modbus_GetPF3(void)      { return pf3; }
bool     Modbus_IsDataValid(void) { return mb_data_ok; }
uint8_t  Modbus_GetLastRx(void)   { return mb_last_rx; } /* bytes in last transaction */
