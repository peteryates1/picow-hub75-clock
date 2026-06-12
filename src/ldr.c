#include "ldr.h"

#include "config.h"

#if HAS_LDR

#include "hardware/adc.h"

// Brightness limits. The night floor keeps the time faintly readable in the
// dark; the day ceiling avoids an uncomfortably bright panel in a lit room.
#define BRIGHT_MIN   12
#define BRIGHT_MAX   255

// Raw ADC readings (0..4095) mapped to the brightness range. Below DARK the
// output sits at BRIGHT_MIN; above LIGHT it sits at BRIGHT_MAX; linear between.
#define ADC_DARK     300
#define ADC_LIGHT    3000

// Exponential smoothing factor (0..255); higher = slower to react.
#define SMOOTH_ALPHA 230

static uint16_t smoothed = 0;

void ldr_init(void) {
    adc_init();
    adc_gpio_init(PIN_LDR_ADC);
    adc_select_input(LDR_ADC_INPUT);
    smoothed = ldr_read_raw();
}

uint16_t ldr_read_raw(void) {
    adc_select_input(LDR_ADC_INPUT);
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) acc += adc_read();
    return (uint16_t)(acc / 8);
}

uint8_t ldr_brightness(void) {
    uint16_t raw = ldr_read_raw();
    // Smooth: new = (alpha*old + (256-alpha)*raw) / 256.
    smoothed = (uint16_t)(((uint32_t)SMOOTH_ALPHA * smoothed +
                           (256 - SMOOTH_ALPHA) * raw) >> 8);

    if (smoothed <= ADC_DARK)  return BRIGHT_MIN;
    if (smoothed >= ADC_LIGHT) return BRIGHT_MAX;

    uint32_t span = ADC_LIGHT - ADC_DARK;
    uint32_t pos  = smoothed - ADC_DARK;
    return (uint8_t)(BRIGHT_MIN + pos * (BRIGHT_MAX - BRIGHT_MIN) / span);
}

#endif // HAS_LDR
