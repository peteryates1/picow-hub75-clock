#ifndef LDR_H
#define LDR_H

#include <stdint.h>

// Light-dependent-resistor auto-brightness helper.

// Configure ADC0 for the LDR divider. Call once at startup.
void ldr_init(void);

// Read the ambient light level, 0 (dark) .. 4095 (bright), as a raw average.
uint16_t ldr_read_raw(void);

// Map the current ambient light to a display brightness, 0..255, clamped
// between a night-time floor and a daytime ceiling so the panel is never
// fully off and never harshly bright. Smoothed across calls.
uint8_t ldr_brightness(void);

#endif // LDR_H
