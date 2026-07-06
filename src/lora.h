#pragma once
#include "stm32g0xx_hal.h"
#include <stdbool.h>

void LoRa_Init(UART_HandleTypeDef *huart);
void LoRa_Process(void);
void LoRa_SendRelay(int relay_num, bool on);  /* 1=pump03(P1), 2=pump04(P2) */
void LoRa_SendRaw(const char *msg);           /* send arbitrary msg to slave  */

/* Returns last known state from slave heartbeat/ack: 1=ON, 0=OFF, -1=unknown */
int      LoRa_GetRelay3State(void);
int      LoRa_GetRelay4State(void);
/* Signal quality of last received +RCV frame */
int      LoRa_GetLastRSSI(void);
int      LoRa_GetLastSNR(void);
/* Milliseconds since last +RCV received; 0xFFFFFFFF if never */
uint32_t LoRa_GetLastRcvAge(void);
/* Flow meter — updated on each slave heartbeat */
uint32_t LoRa_GetFlowLpmX10(void);     /* L/min × 10, e.g. 125 = 12.5 L/min */
uint32_t LoRa_GetTotalLitresInt(void); /* tv from last heartbeat (resets on slave reboot) */
uint32_t LoRa_GetTvCumulative(void);   /* true cumulative litres, never resets */
/* EM4M energy meter — valid only when LoRa_IsModbusValid() returns true */
bool     LoRa_IsModbusValid(void);
int32_t  LoRa_GetV1x10(void);   /* phase 1 voltage × 10  (e.g. 2305 = 230.5 V) */
int32_t  LoRa_GetV2x10(void);
int32_t  LoRa_GetV3x10(void);
int32_t  LoRa_GetI1x100(void);  /* phase 1 current × 100 (e.g.  125 = 1.25 A)  */
int32_t  LoRa_GetI2x100(void);
int32_t  LoRa_GetI3x100(void);
int32_t  LoRa_GetKWx10(void);   /* total kW × 10         (e.g.   45 = 4.5 kW)  */
uint32_t LoRa_GetKWh(void);     /* total kWh (integer)                           */
