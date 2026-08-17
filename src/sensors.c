/* sensors.c
 * Real sensor readings via Modbus RTU from Selec EM4M-3P-C-100A energy meter.
 * Replaces the stub that returned fixed 415 V / 5 A values.
 *
 * Modbus driver (modbus.c) polls the meter every 2 seconds.
 * These functions return the last successfully read value.
 * Safe defaults (415 V / 5 A) are returned until the first read completes.
 *
 * Battery sensing: LiPo 3.7V on PA0 via 100kΩ+100kΩ voltage divider.
 *   Full = 4.2V → PA0 = 2.10V,  Empty = 3.0V → PA0 = 1.50V
 *   Percentage is linear: 0% at 3000mV, 100% at 4200mV.
 */

#include "modem.h"
#include "modbus.h"
#include "main.h"
#include "stm32g0xx_hal.h"

float Sensor_ReadVoltagePhase1(void) { return Modbus_GetV1(); }
float Sensor_ReadVoltagePhase2(void) { return Modbus_GetV2(); }
float Sensor_ReadVoltagePhase3(void) { return Modbus_GetV3(); }

/* Return max(I1, I2, I3) for dry-run detection (current < 1.5 A for 8 s).
 * Max matches per-phase current on a balanced 3-phase load and avoids false
 * dry-run trips if one phase carries slightly less current than the others. */
float Sensor_ReadCurrentACS712(void)
{
    float i1 = Modbus_GetI1();
    float i2 = Modbus_GetI2();
    float i3 = Modbus_GetI3();
    float m  = i1 > i2 ? i1 : i2;
    return m > i3 ? m : i3;
}

/* ── Battery voltage + percentage — LiPo 3.7V on PA0 (ADC_IN0) ──────────────
 *
 * Hardware: 10kΩ + 10kΩ voltage divider → PA0 sees half the battery voltage.
 *   Full  4.20V → PA0 = 2.10V   Empty  3.40V → PA0 = 1.70V
 *
 * Calibration trim (adjust if multimeter reading differs from reported value):
 *   Example: multimeter = 3.82V, firmware reports 3.76V
 *   → set BAT_TRIM_NUM = 382, BAT_TRIM_DEN = 376
 *   Default 100/100 = no trim.
 */
#define BAT_TRIM_NUM  100U
#define BAT_TRIM_DEN  100U

/* Samples to average — mirrors ESP32 reference sketch (16 samples) */
#define BAT_SAMPLES   16U

#ifdef HAL_ADC_MODULE_ENABLED
extern ADC_HandleTypeDef hadc1;

/* ── Shared ADC read: 16-sample average → battery mV ─────────────────────── */
static uint32_t bat_read_mv(void)
{
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Channel      = ADC_CHANNEL_0;            /* PA0                        */
    cfg.Rank         = ADC_REGULAR_RANK_1;
    cfg.SamplingTime = ADC_SAMPLETIME_160CYCLES_5;
    if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK) return 0U;

    uint32_t sum = 0U;
    for (uint8_t i = 0U; i < BAT_SAMPLES; i++) {
        HAL_ADC_Start(&hadc1);
        if (HAL_ADC_PollForConversion(&hadc1, 10U) != HAL_OK) {
            HAL_ADC_Stop(&hadc1);
            return 0U;
        }
        sum += HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
    }

    uint32_t raw    = sum / BAT_SAMPLES;          /* 12-bit average (0–4095)    */
    uint32_t bat_mv = raw * 6600U / 4095U;        /* ×2 divider, 3300 mV ref    */
    return bat_mv * BAT_TRIM_NUM / BAT_TRIM_DEN;  /* calibration trim           */
}

/* ── LiPo discharge curve (ported from ESP32 reference sketch) ────────────
 * Segments (mV → %):
 *   4200 – 4100  →  90–100 %   (0.1 %/mV)
 *   4100 – 4000  →  80–90  %   (0.1 %/mV)
 *   4000 – 3900  →  65–80  %   (0.15%/mV)
 *   3900 – 3800  →  45–65  %   (0.2 %/mV)
 *   3800 – 3700  →  25–45  %   (0.2 %/mV)
 *   3700 – 3600  →  10–25  %   (0.15%/mV)
 *   3600 – 3400  →   2–10  %   (0.04%/mV)
 *         < 3400  →   1    %
 *
 * Integer math only — no float printf.  All intermediates fit uint32_t.     */
static uint8_t lipo_curve(uint32_t mv)
{
    if (mv >= 4200U) return 100U;
    if (mv >= 4100U) return (uint8_t)(90U + (mv - 4100U) / 10U);
    if (mv >= 4000U) return (uint8_t)(80U + (mv - 4000U) / 10U);
    if (mv >= 3900U) return (uint8_t)(65U + (mv - 3900U) * 15U / 100U);
    if (mv >= 3800U) return (uint8_t)(45U + (mv - 3800U) * 20U / 100U);
    if (mv >= 3700U) return (uint8_t)(25U + (mv - 3700U) * 20U / 100U);
    if (mv >= 3600U) return (uint8_t)(10U + (mv - 3600U) * 15U / 100U);
    if (mv >= 3400U) return (uint8_t)( 2U + (mv - 3400U) *  4U / 100U);
    return 1U;
}

/* Returns 0–100 %; 0xFF if ADC read fails */
uint8_t Battery_ReadPercent(void)
{
    uint32_t mv = bat_read_mv();
    if (mv == 0U) return 0xFF;
    return lipo_curve(mv);
}

/* Returns battery voltage in mV (e.g. 3820 = 3.82 V); 0 if ADC read fails */
uint32_t Battery_ReadMv(void)
{
    return bat_read_mv();
}

#else
uint8_t  Battery_ReadPercent(void) { return 0xFF; }
uint32_t Battery_ReadMv(void)      { return 0U;   }
#endif
