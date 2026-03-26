#ifndef MODEM_H
#define MODEM_H

#include "stm32g0xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Public API ──────────────────────────────────────────────────────────── */
void  Modem_Init(UART_HandleTypeDef *huart);
void  Modem_Process(void);              /* call every loop iteration        */
void  Modem_Send(const char *cmd);      /* send raw AT command              */
bool  Modem_IsConnected(void);          /* true only when MQTT CONNECTED    */

/* ── Relay setters called from MQTT command handler ─────────────────────── */
void  Relay1_Set(bool on);
void  Relay2_Set(bool on);
bool  Relay1_Get(void);
bool  Relay2_Get(void);

/* ── Sensor readings — implement in sensors.c ───────────────────────────── */
float Sensor_ReadVoltagePhase1(void);
float Sensor_ReadVoltagePhase2(void);
float Sensor_ReadVoltagePhase3(void);
float Sensor_ReadCurrentACS712(void);

#endif /* MODEM_H */
