#ifndef LORA_H
#define LORA_H

#include "stm32g0xx_hal.h"

/**
 * LoRa driver for Reyax RYL998 on USART3 (PB8=TX, PB9=RX, 115200 baud)
 *
 * Call LoRa_Init() once before the main loop (before IWDG starts).
 * Call LoRa_Process() every main loop iteration.
 *
 * Received LoRa frames (+RCV=<addr>,<len>,<data>,<RSSI>,<SNR>) are
 * printed to the serial monitor via Debug_Print.
 *
 * AT settings (must match sender):
 *   ADDRESS=1, NETWORKID=5, BAND=865000000 (865 MHz)
 *   PARAMETER=9,7,1,12  (SF9, BW125, CR4/5, Preamble=12)
 */
void LoRa_Init(UART_HandleTypeDef *huart);
void LoRa_Process(void);

#endif /* LORA_H */
