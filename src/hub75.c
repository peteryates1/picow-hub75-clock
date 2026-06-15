#include "hub75.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

// Colour depth used for Binary Code Modulation. The top HUB75_BCM_DEPTH bits
// of each 8-bit channel are displayed. 6 bits (64 levels/channel) gives smooth
// antialiased font edges; the LSB base time is shortened to match so overall
// brightness and refresh rate stay close to the 4-bit version.
#define HUB75_BCM_DEPTH  6

// Base on-time for the least-significant bit plane, in microseconds (converted
// to CPU cycles at runtime — see base_cyc). Plane p is lit for (base << p),
// giving the binary weighting. Total per-row on-time is base*(2^DEPTH-1).
#define BCM_BASE_US      2

// Short delay (~tens of ns) inserted around the CLK/LAT edges so the panel's
// shift registers see valid data-setup and clock-high times. Without it the
// bit-banged pulses can be too short for the panel to register. Tunable.
#ifndef HUB75_DELAY_CYCLES
#define HUB75_DELAY_CYCLES 8
#endif
#define HUB75_DELAY() busy_wait_at_least_cycles(HUB75_DELAY_CYCLES)

// Framebuffer: full 8-bit RGB per pixel. Written by core0, read by core1.
// Plain memory with no lock: a pixel may tear for one frame during an update,
// which is invisible for a clock. Marked volatile so the compiler reloads it.
static volatile uint8_t fb[PANEL_HEIGHT][PANEL_WIDTH][3];

// Global brightness scales the BCM on-time. Single byte, atomic to update.
static volatile uint8_t g_brightness = 255;

// Liveness instrumentation, written by core1, read by core0.
static volatile bool     g_core1_alive = false;
static volatile uint32_t g_frame_count = 0;

bool hub75_core1_alive(void) { return g_core1_alive; }
uint32_t hub75_frame_count(void) { return g_frame_count; }

void hub75_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if ((unsigned)x >= PANEL_WIDTH || (unsigned)y >= PANEL_HEIGHT) return;
    fb[y][x][0] = r;
    fb[y][x][1] = g;
    fb[y][x][2] = b;
}

void hub75_clear(void) {
    for (int y = 0; y < PANEL_HEIGHT; y++)
        for (int x = 0; x < PANEL_WIDTH; x++)
            fb[y][x][0] = fb[y][x][1] = fb[y][x][2] = 0;
}

void hub75_set_brightness(uint8_t brightness) {
    g_brightness = brightness;
}

// --- core1: GPIO setup + refresh loop --------------------------------------

static void hub75_gpio_init(void) {
    const uint pins[] = {
        PIN_B2, PIN_R2, PIN_B1, PIN_R1, PIN_G1, PIN_G2,
        PIN_ADDR_A, PIN_ADDR_B, PIN_ADDR_C, PIN_ADDR_D, PIN_ADDR_E,
        PIN_CLK, PIN_LAT, PIN_OE,
    };
    for (size_t i = 0; i < count_of(pins); i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }
    gpio_put(PIN_OE, 1);  // OE active-low: start with the panel blanked.
}

// Drive the row-address lines A..D to select one of the 16 row-pairs.
static inline void hub75_set_row(int row) {
    gpio_put(PIN_ADDR_A, row & 0x1);
    gpio_put(PIN_ADDR_B, row & 0x2);
    gpio_put(PIN_ADDR_C, row & 0x4);
    gpio_put(PIN_ADDR_D, row & 0x8);
}

// Pack the six RGB bits for a column (top + bottom pixel) into the GPIO
// positions of the GP10..GP15 block, ready for a single masked write.
// Some panels (e.g. the Adafruit P2.5 64x32) have green and blue swapped
// relative to the standard HUB75 pinout. PANEL_SWAP_GB compensates in software,
// so the board pin maps can stay faithful to the schematic.
#ifdef PANEL_SWAP_GB
#define BIT_G1 RGB_BIT_B1
#define BIT_B1 RGB_BIT_G1
#define BIT_G2 RGB_BIT_B2
#define BIT_B2 RGB_BIT_G2
#else
#define BIT_G1 RGB_BIT_G1
#define BIT_B1 RGB_BIT_B1
#define BIT_G2 RGB_BIT_G2
#define BIT_B2 RGB_BIT_B2
#endif

static inline uint32_t pack_column(int x, int top_y, int bot_y, int plane) {
    const uint8_t bit = (uint8_t)(8 - HUB75_BCM_DEPTH + plane);  // which source bit
    const uint8_t m = 1u << bit;
    uint32_t v = 0;
    if (fb[top_y][x][0] & m) v |= 1u << RGB_BIT_R1;
    if (fb[top_y][x][1] & m) v |= 1u << BIT_G1;
    if (fb[top_y][x][2] & m) v |= 1u << BIT_B1;
    if (fb[bot_y][x][0] & m) v |= 1u << RGB_BIT_R2;
    if (fb[bot_y][x][1] & m) v |= 1u << BIT_G2;
    if (fb[bot_y][x][2] & m) v |= 1u << BIT_B2;
    return v << RGB_BASE_PIN;
}

// Run the refresh loop from RAM (not XIP flash). On the RP2040 both cores share
// one flash cache, so core1 fetching the loop from flash contends with core0's
// flash execution and jitters the OE timing -> visible flicker. Executing from
// RAM removes the contention. (Harmless on RP2350.)
static void __not_in_flash_func(hub75_refresh_loop)(void) {
    hub75_gpio_init();
    // Register core1 so that any flash-unsafe operation on core0 (e.g. inside
    // the cyw43 driver) parks core1 safely instead of corrupting its execution.
    multicore_lockout_victim_init();
    g_core1_alive = true;

    // OE on-time is measured in CPU cycles rather than microseconds so dim
    // levels can drop below 1us. busy_wait_us_32's 1us granularity was the old
    // brightness floor: with integer division only brightness >= 4 kept the top
    // plane's on-time non-zero. base_cyc is the same physical base time
    // (BCM_BASE_US) expressed in cycles, so day/normal brightness is unchanged.
    const uint32_t base_cyc = (clock_get_hz(clk_sys) / 1000000u) * BCM_BASE_US;

    for (;;) {
        g_frame_count++;
        for (int plane = 0; plane < HUB75_BCM_DEPTH; plane++) {
            // On-time (cycles) for this plane, scaled by global brightness.
            uint32_t on_cyc = ((uint32_t)base_cyc << plane) * g_brightness / 255;

            for (int row = 0; row < PANEL_SCAN_ROWS; row++) {
                // Shift out all 64 columns for this row-pair.
                for (int x = 0; x < PANEL_WIDTH; x++) {
                    uint32_t bits = pack_column(x, row, row + PANEL_SCAN_ROWS, plane);
                    gpio_put_masked(RGB_PIN_MASK, bits);
                    HUB75_DELAY();                 // data setup before clock edge
                    gpio_put(PIN_CLK, 1);
                    HUB75_DELAY();                 // hold clock high
                    gpio_put(PIN_CLK, 0);
                    HUB75_DELAY();
                }

                // Blank, latch the shifted row, select it, then light it.
                gpio_put(PIN_OE, 1);
                hub75_set_row(row);
                HUB75_DELAY();
                gpio_put(PIN_LAT, 1);
                HUB75_DELAY();
                gpio_put(PIN_LAT, 0);
                HUB75_DELAY();
                gpio_put(PIN_OE, 0);
                if (on_cyc) busy_wait_at_least_cycles(on_cyc);
                gpio_put(PIN_OE, 1);
            }
        }
    }
}

void hub75_init(void) {
    // Reset core1 to a clean state before launching. On RP2350 core1 can be
    // left parked in the bootrom after a reset such that multicore_launch_core1
    // silently fails to start it (PC stuck in bootrom, garbage SP) and the
    // refresh loop never runs. An explicit reset first makes the launch
    // reliable.
    multicore_reset_core1();
    multicore_launch_core1(hub75_refresh_loop);
}
