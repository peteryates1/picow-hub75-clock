#ifndef CONFIG_H
#define CONFIG_H

// ---------------------------------------------------------------------------
// Build-time configuration for the Pico W HUB75 clock.
//
// GPIO assignments are board-specific and live in the board header selected
// below. Select a board at configure time with -DTARGET_BOARD=control_panel
// (see CMakeLists.txt); the default is the real clock board. Everything in
// this file is common to all boards.
// ---------------------------------------------------------------------------

#if defined(BOARD_CONTROL_PANEL)
#include "board_control_panel.h"
#else
#include "board_clock.h"
#endif

// --- HUB75 panel geometry (common) -----------------------------------------
#define PANEL_WIDTH     64
#define PANEL_HEIGHT    32
// 64x32 panels are 1/16 scan: address lines A..D select 16 row-pairs.
#define PANEL_SCAN_ROWS (PANEL_HEIGHT / 2)   // 16

// --- Clock / time (common) -------------------------------------------------
// Local time offset from UTC in seconds (no automatic DST handling).
// Example: UK winter = 0, UK summer (BST) = 3600.
#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS  0
#endif

#define NTP_SERVER       "pool.ntp.org"
#define NTP_RESYNC_HOURS 6   // periodic re-sync interval

// --- Weather (outside temperature via wttr.in) -----------------------------
// Location passed to wttr.in (a single token; no spaces). Override at configure
// time with -DWEATHER_LOCATION=Town.
#ifndef WEATHER_LOCATION
#define WEATHER_LOCATION "London"
#endif

#define WEATHER_UPDATE_MINUTES 15   // how often to refresh the temperature

#endif // CONFIG_H
