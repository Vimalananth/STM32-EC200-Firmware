/* sensors_adc.c
 * Real 3-phase voltage + ACS712 current sensing via STM32G0 ADC.
 *
 * ── STEP 4 — HOW TO ACTIVATE ────────────────────────────────────────────────
 * When ADC hardware is wired up and you are ready to use real readings:
 *   1. Delete  src/sensors.c            (the stub that returns 415V / 5A)
 *   2. Rename  src/sensors_adc.c  →  src/sensors.c
 *   Build will now use real ADC readings automatically (weak symbols overridden).
 *
 * ── PIN ASSIGNMENTS ─────────────────────────────────────────────────────────
 *   PA0 (ADC_IN0)  — Phase 1 voltage (via resistor divider + rectifier)
 *   PA4 (ADC_IN4)  — Phase 2 voltage
 *   PA6 (ADC_IN6)  — Phase 3 voltage
 *   PA7 (ADC_IN7)  — ACS712-20A current sensor output
 *
 *   NOTE: PA1=Relay2, PA2=USART2_TX, PA3=USART2_RX, PA5=Relay1 are all in use.
 *   PA0/PA4/PA6/PA7 are the only available analog-capable pins on GPIOA.
 *
 * ── VOLTAGE DIVIDER ─────────────────────────────────────────────────────────
 *   For 3-phase 415 V (line-to-neutral = 240 Vrms = 339 Vpeak):
 *     R1 = 1 MΩ,  R2 = 3.3 kΩ  →  ratio = 304
 *     ADC reads peak after half-wave rectifier + 100 nF cap.
 *     V_phase = adc_V * 304 * V_CORRECTION
 *   Calibrate V_CORRECTION with a multimeter on first install.
 *
 * ── CURRENT SENSOR ─────────────────────────────────────────────────────────
 *   ACS712-20A powered from 3.3 V:
 *     Midpoint = 1.65 V (VREF/2) at 0 A
 *     Sensitivity = 100 mV/A
 *     I = (adc_V - 1.65) / 0.1
 *   If powered from 5 V through a divider, adjust ACS712_MIDV accordingly.
 *
 * ── REQUIRED CHANGES IN OTHER FILES (already done if you ran step 4) ────────
 *   stm32g0xx_hal_conf.h  : uncomment  #define HAL_ADC_MODULE_ENABLED
 *   stm32g0xx_hal_msp.c   : HAL_ADC_MspInit added (clocks + analog GPIO)
 *   src/main.c            : hadc1 declared, MX_ADC1_Init() called before Modem_Init
 */

#include "modem.h"
#include "main.h"
#include "stm32g0xx_hal.h"

/* ── Scaling constants — calibrate V_CORRECTION on site ─────────────────── */
#define VREF          3.3f
#define ADC_MAX       4096.0f
#define R1            1000000.0f   /* 1 MΩ                                   */
#define R2            3300.0f      /* 3.3 kΩ                                 */
#define V_DIVIDER     ((R1 + R2) / R2)   /* ≈ 304                           */
#define V_CORRECTION  1.05f        /* tweak until ADC reading matches meter  */

#define ACS712_MIDV   (VREF / 2.0f) /* 1.65 V = midpoint at 0 A (3.3 V pwr) */
#define ACS712_SENS   0.1f          /* 100 mV/A  (ACS712-20A)               */

/* hadc1 is declared in main.c — MX_ADC1_Init() must be called there first   */
extern ADC_HandleTypeDef hadc1;

/* ── Read raw ADC voltage for a given channel ──────────────────────────── */
static float read_adc_voltage(uint32_t channel)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel      = channel;
    cfg.Rank         = ADC_RANK_CHANNEL_NUMBER;   /* STM32G0 field           */
    cfg.SamplingTime = ADC_SAMPLINGTIME_COMMON_1; /* set in MX_ADC1_Init     */
    HAL_ADC_ConfigChannel(&hadc1, &cfg);

    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) != HAL_OK)
        return VREF / 2.0f;           /* return midscale on timeout          */
    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    return (raw / ADC_MAX) * VREF;
}

/* ── Public sensor API ─────────────────────────────────────────────────── */

float Sensor_ReadVoltagePhase1(void)
{
    float v = read_adc_voltage(ADC_CHANNEL_0);   /* PA0 — Phase 1           */
    return v * V_DIVIDER * V_CORRECTION;
}

float Sensor_ReadVoltagePhase2(void)
{
    float v = read_adc_voltage(ADC_CHANNEL_4);   /* PA4 — Phase 2           */
    return v * V_DIVIDER * V_CORRECTION;
}

float Sensor_ReadVoltagePhase3(void)
{
    float v = read_adc_voltage(ADC_CHANNEL_6);   /* PA6 — Phase 3           */
    return v * V_DIVIDER * V_CORRECTION;
}

float Sensor_ReadCurrentACS712(void)
{
    /* Average 10 samples to reduce noise (~10 ms total with 1 ms HAL_Delay) */
    float sum = 0.0f;
    for (int i = 0; i < 10; i++)
    {
        sum += read_adc_voltage(ADC_CHANNEL_7);  /* PA7 — ACS712            */
        HAL_Delay(1);
    }
    float v_avg  = sum / 10.0f;
    float amps   = (v_avg - ACS712_MIDV) / ACS712_SENS;
    if (amps < 0.0f) amps = 0.0f;
    return amps;
}
