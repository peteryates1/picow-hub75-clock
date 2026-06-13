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

// --- Wi-Fi (credentials come from CMake/network.txt) -----------------------
// Backup network is optional; default to empty so it's simply skipped.
#ifndef WIFI_SSID_BACKUP
#define WIFI_SSID_BACKUP ""
#endif
#ifndef WIFI_PASSWORD_BACKUP
#define WIFI_PASSWORD_BACKUP ""
#endif

// --- Clock / time (common) -------------------------------------------------
// Local time offset from UTC in seconds (no automatic DST handling).
// Example: UK winter = 0, UK summer (BST) = 3600.
#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS  0
#endif

#define NTP_SERVER       "pool.ntp.org"
#define NTP_RESYNC_HOURS 6   // periodic re-sync interval

// --- Weather (outside temperature via Open-Meteo) --------------------------
// Location as latitude/longitude strings (Open-Meteo takes coordinates).
// Override at configure time with -DWEATHER_LAT=.. -DWEATHER_LON=..
// Default: Fleckney, Leicestershire.
#ifndef WEATHER_LAT
#define WEATHER_LAT "52.52"
#endif
#ifndef WEATHER_LON
#define WEATHER_LON "-1.00"
#endif

#define WEATHER_UPDATE_MINUTES 15   // how often to refresh the temperature

#endif // CONFIG_H
