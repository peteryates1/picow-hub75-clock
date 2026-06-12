#ifndef BOARD_CLOCK_H
#define BOARD_CLOCK_H

// ---------------------------------------------------------------------------
// Board: clock-picow-hub75-ldr  (the real clock PCB)
//
// GPIO assignments transcribed from the EasyEDA netlist
// Netlist_clock-picow-hub75-ldr_*.enet. These are physical traces -- do not
// change them unless the PCB changes.
// ---------------------------------------------------------------------------

// --- HUB75 RGB data pins ---------------------------------------------------
// The six RGB pins sit on contiguous GPIOs GP10..GP15 so a column can be
// written with one masked store. The order within the block is scrambled by
// PCB routing; RGB_BIT_* gives each colour's bit offset from RGB_BASE_PIN.
#define PIN_B2   10
#define PIN_R2   11
#define PIN_B1   12
#define PIN_R1   13
#define PIN_G1   14
#define PIN_G2   15

#define RGB_BASE_PIN  10
#define RGB_PIN_MASK  (0x3Fu << RGB_BASE_PIN)

#define RGB_BIT_B2  (PIN_B2 - RGB_BASE_PIN)
#define RGB_BIT_R2  (PIN_R2 - RGB_BASE_PIN)
#define RGB_BIT_B1  (PIN_B1 - RGB_BASE_PIN)
#define RGB_BIT_R1  (PIN_R1 - RGB_BASE_PIN)
#define RGB_BIT_G1  (PIN_G1 - RGB_BASE_PIN)
#define RGB_BIT_G2  (PIN_G2 - RGB_BASE_PIN)

// --- HUB75 control pins ----------------------------------------------------
#define PIN_ADDR_A   9
#define PIN_ADDR_B   3
#define PIN_ADDR_C   8
#define PIN_ADDR_D   4
#define PIN_ADDR_E   2   // unused at 1/16 scan
#define PIN_CLK   7
#define PIN_LAT   5
#define PIN_OE    6   // active-low

// --- LDR auto-brightness (present on this board) ---------------------------
#define HAS_LDR        1
#define PIN_LDR_ADC    26   // GP26 / ADC0
#define LDR_ADC_INPUT  0

// --- User button (present on this board) -----------------------------------
#define HAS_BUTTON     1
#define PIN_BUTTON     16   // SW1, active-low; press forces NTP re-sync

// Used at startup before the LDR takes over.
#define DEFAULT_BRIGHTNESS  40

#endif // BOARD_CLOCK_H
