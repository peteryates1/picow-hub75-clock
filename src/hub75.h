#ifndef HUB75_H
#define HUB75_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// ---------------------------------------------------------------------------
// HUB75 LED-matrix driver.
//
// The refresh loop is a bit-banged Binary Code Modulation (BCM) driver that
// runs continuously on core1 so the display never flickers while core0 does
// Wi-Fi / NTP / rendering work. core0 only ever touches the framebuffer
// (hub75_set_pixel / hub75_clear) and the brightness control; core1 owns the
// GPIO and the timing.
// ---------------------------------------------------------------------------

// Initialise GPIOs and launch the refresh loop on core1.
// Call once from core0 before drawing.
void hub75_init(void);

// Set a single pixel (0,0 = top-left). Channels are 8-bit; only the high
// HUB75_BCM_DEPTH bits are displayed. Out-of-range coordinates are ignored.
void hub75_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);

// Clear the whole framebuffer to black.
void hub75_clear(void);

// Commit the current framebuffer to the display. Call once after finishing a
// frame's drawing. For the PIO backend this re-packs the bit-planes into the
// off-screen buffer and swaps it in, so the refresh never pauses mid-frame; for
// the bit-bang backend (which reads the framebuffer directly) it is a no-op.
void hub75_flip(void);

// Global brightness, 0 (off) .. 255 (full). Applied by the refresh loop, so
// it affects everything already on screen without redrawing.
void hub75_set_brightness(uint8_t brightness);

// Liveness instrumentation: true once core1 has entered the refresh loop, and a
// counter that increments every full frame. Lets core0 confirm core1 is alive.
bool hub75_core1_alive(void);
uint32_t hub75_frame_count(void);

#endif // HUB75_H
