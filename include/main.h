/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
void Debug_Print(const char *msg);  /* send string to USART2 serial monitor */

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* Latching relay coil pins — pulse SET to latch ON, pulse RESET to latch OFF */
#define Relay_Pin_Pin        GPIO_PIN_5   /* Pump 1 SET  coil */
#define Relay_Pin_GPIO_Port  GPIOA
#define Relay1_RST_Pin       GPIO_PIN_4   /* Pump 1 RESET coil */
#define Relay1_RST_GPIO_Port GPIOA
#define Relay2_Pin_Pin       GPIO_PIN_1   /* Pump 2 SET  coil */
#define Relay2_Pin_GPIO_Port GPIOA
#define Relay2_RST_Pin       GPIO_PIN_0   /* Pump 2 RESET coil */
#define Relay2_RST_GPIO_Port GPIOA
#define DE485_Pin_Pin        GPIO_PIN_8   /* RS485 DE/RE direction control */
#define DE485_Pin_GPIO_Port  GPIOA

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
