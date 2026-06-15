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

// Some panels (e.g. the Adafruit P2.5 64x32) have green and blue swapped
// relative to the standard HUB75 pinout. PANEL_SWAP_GB compensates in software,
// so the board pin maps can stay faithful to the schematic. Used by both
// backends' packing.
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

// Drive the row-address lines A..D to select one of the 16 row-pairs.
static inline void hub75_set_row(int row) {
    gpio_put(PIN_ADDR_A, row & 0x1);
    gpio_put(PIN_ADDR_B, row & 0x2);
    gpio_put(PIN_ADDR_C, row & 0x4);
    gpio_put(PIN_ADDR_D, row & 0x8);
}

#ifdef HUB75_PIO
// ===========================================================================
// PIO + DMA backend
//
// A PIO state machine clocks the RGB pixels out (CLK on its side-set pin) while
// DMA streams pre-packed bit-planes into it, so core1 does no per-pixel work.
// core1 only sequences the bit-planes/rows and drives blank/latch/address/OE.
// The SM runs at a fixed rate (HUB75_PIO_SM_HZ) independent of clk_sys, so the
// refresh rate no longer depends on the system clock -- the clock can drop back
// to default without reintroducing flicker.
// ===========================================================================
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hub75.pio.h"

// State-machine clock. CLK = SM/2 (two instructions per pixel). 30 MHz SM ->
// 15 MHz pixel clock, comfortably within the panel's limit and far faster than
// the bit-banged version. Lower this if a panel shows shifted/garbled columns.
#ifndef HUB75_PIO_SM_HZ
#define HUB75_PIO_SM_HZ 30000000u
#endif

// Packed bit-planes for the shifter: 4 pixels (6 RGB bits each) per 32-bit word,
// 16 words per row, one set per (plane, row). Double-buffered: core0 re-packs
// into the off-screen buffer in hub75_flip() and swaps g_front, so core1's
// refresh never has to stop to re-pack (a mid-frame pause shows as a periodic
// ~5 Hz blink). core1 reads g_front once per frame.
#define PIO_ROW_WORDS (PANEL_WIDTH / 4)
static uint32_t pio_fb[2][HUB75_BCM_DEPTH][PANEL_SCAN_ROWS][PIO_ROW_WORDS];
static volatile uint8_t g_front = 0;

static PIO  pio_inst = pio0;
static uint pio_sm;
static int  dma_chan;

// Re-pack the whole framebuffer into one PIO plane buffer. Each 6-bit pixel
// value sits at the RGB_BIT_* offsets within the 6-pin OUT block (the SM's OUT
// base adds the GPIO offset); four pixels are packed LSB-first per word to match
// the OSR's right-shift + autopull-at-24.
static void hub75_repack(int buf) {
    for (int plane = 0; plane < HUB75_BCM_DEPTH; plane++) {
        const uint8_t m = 1u << (uint8_t)(8 - HUB75_BCM_DEPTH + plane);
        for (int row = 0; row < PANEL_SCAN_ROWS; row++) {
            const int bot = row + PANEL_SCAN_ROWS;
            for (int wx = 0; wx < PIO_ROW_WORDS; wx++) {
                uint32_t w = 0;
                for (int k = 0; k < 4; k++) {
                    const int x = wx * 4 + k;
                    uint32_t v = 0;
                    if (fb[row][x][0] & m) v |= 1u << RGB_BIT_R1;
                    if (fb[row][x][1] & m) v |= 1u << BIT_G1;
                    if (fb[row][x][2] & m) v |= 1u << BIT_B1;
                    if (fb[bot][x][0] & m) v |= 1u << RGB_BIT_R2;
                    if (fb[bot][x][1] & m) v |= 1u << BIT_G2;
                    if (fb[bot][x][2] & m) v |= 1u << BIT_B2;
                    w |= v << (6 * k);
                }
                pio_fb[buf][plane][row][wx] = w;
            }
        }
    }
}

// Called by core0 after drawing a frame: re-pack into the off-screen buffer and
// swap it in. core1 picks up the new buffer on its next frame.
void hub75_flip(void) {
    uint8_t back = g_front ^ 1u;
    hub75_repack(back);
    g_front = back;
}

static void hub75_pio_init(void) {
    // Blank/latch/address are driven directly by core1.
    const uint ctrl[] = {
        PIN_ADDR_A, PIN_ADDR_B, PIN_ADDR_C, PIN_ADDR_D, PIN_ADDR_E,
        PIN_LAT, PIN_OE,
    };
    for (size_t i = 0; i < count_of(ctrl); i++) {
        gpio_init(ctrl[i]);
        gpio_set_dir(ctrl[i], GPIO_OUT);
        gpio_put(ctrl[i], 0);
    }
    gpio_put(PIN_OE, 1);  // active-low: start blanked

    // Data shifter SM: OUT = the 6 RGB pins, side-set = CLK.
    uint offset = pio_add_program(pio_inst, &hub75_data_program);
    pio_sm = pio_claim_unused_sm(pio_inst, true);

    pio_sm_config c = hub75_data_program_get_default_config(offset);
    sm_config_set_out_pins(&c, RGB_BASE_PIN, 6);
    sm_config_set_sideset_pins(&c, PIN_CLK);
    // OUT shifts right (LSB first) so pixel 0 clocks out first; autopull at 24
    // bits = 4 pixels per word. Join the FIFOs for a deeper (8-word) TX buffer.
    sm_config_set_out_shift(&c, true, true, 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)HUB75_PIO_SM_HZ);

    for (uint i = 0; i < 6; i++) pio_gpio_init(pio_inst, RGB_BASE_PIN + i);
    pio_gpio_init(pio_inst, PIN_CLK);
    pio_sm_set_consecutive_pindirs(pio_inst, pio_sm, RGB_BASE_PIN, 6, true);
    pio_sm_set_consecutive_pindirs(pio_inst, pio_sm, PIN_CLK, 1, true);

    pio_sm_init(pio_inst, pio_sm, offset, &c);
    pio_sm_set_enabled(pio_inst, pio_sm, true);

    // DMA: 32-bit words from a plane-row buffer into the SM's TX FIFO, paced by
    // the SM's TX DREQ. Read address + count are set per row before triggering.
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(pio_inst, pio_sm, true));
    dma_channel_configure(dma_chan, &dc, &pio_inst->txf[pio_sm], NULL,
                          PIO_ROW_WORDS, false);
}

// Run from RAM (see the bit-bang note below): keeps the OE timing off the
// shared flash path on the RP2040.
static void __not_in_flash_func(hub75_refresh_loop)(void) {
    hub75_pio_init();
    multicore_lockout_victim_init();
    g_core1_alive = true;

    const uint32_t base_cyc = (clock_get_hz(clk_sys) / 1000000u) * BCM_BASE_US;
    const uint32_t stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + pio_sm);

    for (;;) {
        g_frame_count++;
        // Display whichever buffer core0 last committed (read once per frame so
        // a swap mid-frame just takes effect next frame -- no tearing).
        const uint8_t f = g_front;

        for (int plane = 0; plane < HUB75_BCM_DEPTH; plane++) {
            uint32_t on_cyc = ((uint32_t)base_cyc << plane) * g_brightness / 255;

            for (int row = 0; row < PANEL_SCAN_ROWS; row++) {
                // Stream this (plane,row)'s 64 pixels out through the PIO, then
                // wait until they've all been clocked before latching.
                dma_channel_set_read_addr(dma_chan, pio_fb[f][plane][row], false);
                dma_channel_set_trans_count(dma_chan, PIO_ROW_WORDS, true);
                // 1) wait for the DMA to push every word into the FIFO. 2) THEN
                // clear TXSTALL and wait for the SM to stall on the next (empty)
                // autopull -- i.e. it has clocked out the last pixel. Clearing
                // only after the DMA is done is essential: at row start the SM is
                // still stalled from the previous row, so clearing earlier sees
                // TXSTALL re-assert within a cycle and latches before any pixel
                // has shifted (garbled, speckled output).
                while (dma_channel_is_busy(dma_chan)) tight_loop_contents();
                pio_inst->fdebug = stall_mask;
                while (!(pio_inst->fdebug & stall_mask)) tight_loop_contents();

                // Blank, latch the shifted row, select it, then light it. (Small
                // waits give the panel valid latch setup/pulse widths.)
                gpio_put(PIN_OE, 1);
                hub75_set_row(row);
                busy_wait_at_least_cycles(8);
                gpio_put(PIN_LAT, 1);
                busy_wait_at_least_cycles(8);
                gpio_put(PIN_LAT, 0);
                busy_wait_at_least_cycles(8);
                gpio_put(PIN_OE, 0);
                if (on_cyc) busy_wait_at_least_cycles(on_cyc);
                gpio_put(PIN_OE, 1);
            }
        }
    }
}

#else
// ===========================================================================
// Bit-banged backend (default)
// ===========================================================================

// The bit-bang refresh reads the framebuffer directly, so committing a frame is
// a no-op.
void hub75_flip(void) {}

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

// Pack the six RGB bits for a column (top + bottom pixel) into the GPIO
// positions of the GP10..GP15 block, ready for a single masked write.
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

#endif // HUB75_PIO

void hub75_init(void) {
    // Reset core1 to a clean state before launching. On RP2350 core1 can be
    // left parked in the bootrom after a reset such that multicore_launch_core1
    // silently fails to start it (PC stuck in bootrom, garbage SP) and the
    // refresh loop never runs. An explicit reset first makes the launch
    // reliable.
    multicore_reset_core1();
    multicore_launch_core1(hub75_refresh_loop);
}
