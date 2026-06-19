// battery.h — battery voltage sensing (GPIO4 / ADC1_CH3 via resistor divider)
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Configure the ADC oneshot unit + calibration. Safe to call once at boot.
void battery_init(void);

// Battery pack voltage in millivolts (already corrected for the divider), or 0
// if the ADC is unavailable.
int battery_read_mv(void);

// Charge estimate 0..100 (linear over BAT_EMPTY_MV..BAT_FULL_MV), or -1 if the
// reading is unavailable.
int battery_percent(void);

#ifdef __cplusplus
}
#endif
