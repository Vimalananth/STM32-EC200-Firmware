#ifndef LORA_H
#define LORA_H

#include "stm32g0xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * LoRa driver for Reyax RYLR998 on USART3 (PB8=TX, PB9=RX, 115200 baud)
 *
 * Call LoRa_Init() once before the main loop (before IWDG starts).
 * Call LoRa_Process() every main loop iteration.
 *
 * Received LoRa frames (+RCV=<addr>,<len>,<data>,<RSSI>,<SNR>) are
 * printed to the serial monitor via Debug_Print.
 *
 * AT settings (must match sender):
 *   ADDRESS=1 (master), NETWORKID=6, BAND=865000000 (865 MHz), CRFOP=22
 *   PARAMETER=9,7,1,12  (SF9, BW125, CR4/5, Preamble=12)
 *
 * Slave heartbeat format: S:R1:ON|R2:OFF|FL:12.5|TV:1250
 */
void LoRa_Init(UART_HandleTypeDef *huart);
void LoRa_Process(void);

/* Send relay command to slave — relay_num: 1=pump03, 2=pump04 */
void LoRa_SendRelay(int relay_num, bool on);

/* Send arbitrary message to slave (used by lora_ota.c) */
void LoRa_SendRaw(const char *msg);

/* Slave relay states: -1=unknown, 0=OFF, 1=ON */
int      LoRa_GetRelay3State(void);
int      LoRa_GetRelay4State(void);

/* Last received signal quality */
int      LoRa_GetLastRSSI(void);
int      LoRa_GetLastSNR(void);

/* Flow meter values from slave heartbeat (integer — avoids soft-float) */
uint32_t LoRa_GetFlowLpmX10(void);     /* L/min × 10, e.g. 125 = 12.5 L/min */
uint32_t LoRa_GetTotalLitresInt(void); /* tv from last heartbeat (resets on slave reboot) */
uint32_t LoRa_GetTvCumulative(void);   /* true cumulative litres — survives slave reboots */

/* Returns ms since last +RCV, or 0xFFFFFFFF if never received */
uint32_t LoRa_GetLastRcvAge(void);

/* Number of +ERR responses during LoRa_Init (0 = all AT commands succeeded) */
int LoRa_GetInitErrs(void);

/* Energy meter from slave Modbus — valid only when LoRa_EnergyValid() == true */
bool    LoRa_EnergyValid(void);
int32_t LoRa_GetV1x10(void);    /* Phase 1 voltage  × 10   V      */
int32_t LoRa_GetV2x10(void);    /* Phase 2 voltage  × 10   V      */
int32_t LoRa_GetV3x10(void);    /* Phase 3 voltage  × 10   V      */
int32_t LoRa_GetI1x100(void);   /* Phase 1 current  × 100  A      */
int32_t LoRa_GetI2x100(void);   /* Phase 2 current  × 100  A      */
int32_t LoRa_GetI3x100(void);   /* Phase 3 current  × 100  A      */
int32_t LoRa_GetKWx10(void);    /* Total active power × 10 kW     */
int32_t LoRa_GetKWh(void);      /* Import energy integer   kWh    */

#endif /* LORA_H */
