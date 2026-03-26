/* stm32g0xx_hal_msp.c — minimal MSP for bootloader (no peripherals to init) */
#include "stm32g0xx_hal.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}
