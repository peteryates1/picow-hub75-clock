#ifndef BOARD_CONTROL_PANEL_H
#define BOARD_CONTROL_PANEL_H

// ---------------------------------------------------------------------------
// Board: Control_Panel_8 - controller - picow  (alternative board for initial
// HUB75 bring-up / testing)
//
// GPIO assignments transcribed from the EasyEDA netlist
// Netlist_Control_Panel_8_-_controller_-_picow_*.enet. The HUB75 IDC connector
// (J6) is wired the same physically, but to DIFFERENT Pico GPIOs than the
// clock board, so only this header changes.
//
// This board has NO light sensor and NO clock button -- those Pico pins drive
// I2C port-expanders, servos and motor control instead. So HAS_LDR / HAS_BUTTON
// are 0 and the firmware runs at a fixed brightness with periodic NTP re-sync
// only.
// ---------------------------------------------------------------------------

// --- HUB75 RGB data pins ---------------------------------------------------
// Still contiguous: GP8..GP13. Order within the block differs from the clock
// board, captured by RGB_BIT_* below.
#define PIN_B2    8
#define PIN_G2    9
#define PIN_R2   10
#define PIN_G1   11
#define PIN_B1   12
#define PIN_R1   13

#define RGB_BASE_PIN  8
#define RGB_PIN_MASK  (0x3Fu << RGB_BASE_PIN)

#define RGB_BIT_B2  (PIN_B2 - RGB_BASE_PIN)
#define RGB_BIT_G2  (PIN_G2 - RGB_BASE_PIN)
#define RGB_BIT_R2  (PIN_R2 - RGB_BASE_PIN)
#define RGB_BIT_G1  (PIN_G1 - RGB_BASE_PIN)
#define RGB_BIT_B1  (PIN_B1 - RGB_BASE_PIN)
#define RGB_BIT_R1  (PIN_R1 - RGB_BASE_PIN)

// --- HUB75 control pins ----------------------------------------------------
#define PIN_ADDR_A   6
#define PIN_ADDR_B   5
#define PIN_ADDR_C   4
#define PIN_ADDR_D   3
#define PIN_ADDR_E   7   // unused at 1/16 scan
#define PIN_CLK   2
#define PIN_LAT   1
#define PIN_OE    0   // active-low

// --- No LDR / no button on this board --------------------------------------
#define HAS_LDR     0
#define HAS_BUTTON  0

// Fixed brightness used when there is no light sensor.
#define DEFAULT_BRIGHTNESS  60

#endif // BOARD_CONTROL_PANEL_H
