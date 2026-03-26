/* stm32g0xx_hal_conf.h — minimal config for bootloader (FLASH only) */
#ifndef STM32G0XX_HAL_CONF_H
#define STM32G0XX_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED

#define USE_HAL_DRIVER
#define STM32G071xx

/* HSI frequency (no PLL — bootloader runs at reset speed 16 MHz) */
#define HSI_VALUE    16000000U
#define HSE_VALUE     8000000U
#define LSE_VALUE        32768U
#define LSI_VALUE        32000U
#define EXTERNAL_CLOCK_VALUE 12288000U

#define HAL_TICK_FREQ_DEFAULT  HAL_TICK_FREQ_1KHZ

/* Include HAL drivers */
#include "stm32g0xx_hal_rcc.h"
#include "stm32g0xx_hal_gpio.h"
#include "stm32g0xx_hal_dma.h"
#include "stm32g0xx_hal_cortex.h"
#include "stm32g0xx_hal_pwr.h"
#include "stm32g0xx_hal_flash.h"
#include "stm32g0xx_hal_flash_ex.h"

#define assert_param(expr) ((void)0U)

#endif /* STM32G0XX_HAL_CONF_H */
