/* sensors.c
 * Real sensor readings via Modbus RTU from Selec EM4M-3P-C-100A energy meter.
 * Replaces the stub that returned fixed 415 V / 5 A values.
 *
 * Modbus driver (modbus.c) polls the meter every 2 seconds.
 * These functions return the last successfully read value.
 * Safe defaults (415 V / 5 A) are returned until the first read completes.
 */

#include "modem.h"
#include "modbus.h"

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
