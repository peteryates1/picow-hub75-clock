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

// Optional static IP (from network.txt: STATIC_IP/GATEWAY/NETMASK). Empty
// STATIC_IP => DHCP. When set, DNS is pointed at the gateway (+ a public
// fallback) so NTP/weather still resolve.
#ifndef STATIC_IP
#define STATIC_IP ""
#endif
#ifndef STATIC_GW
#define STATIC_GW ""
#endif
#ifndef STATIC_MASK
#define STATIC_MASK "255.255.255.0"
#endif

// --- Clock / time (common) -------------------------------------------------
// Local time offset from UTC in seconds (no automatic DST handling).
// Example: UK winter = 0, UK summer (BST) = 3600.
#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS  0
#endif

#define NTP_SERVER       "pool.ntp.org"
#define NTP_RESYNC_HOURS 6   // periodic re-sync interval

// --- Time-of-day dimming (used on boards without a light sensor) -----------
// Panel runs at BRIGHT_DAY during the day window and BRIGHT_NIGHT outside it.
// Bright from START_HOUR (inclusive) to END_HOUR (exclusive), local time.
#define BRIGHT_DAY            160
#define BRIGHT_NIGHT          40
#define BRIGHT_DAY_START_HOUR 8
#define BRIGHT_DAY_END_HOUR   21

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

// --- MQTT (brightness/power control) ---------------------------------------
// Anonymous connection to a broker on the LAN. Override the IP with
// -DMQTT_BROKER_IP=... at configure time.
#ifndef MQTT_BROKER_IP
#define MQTT_BROKER_IP "192.168.0.2"
#endif
#define MQTT_BROKER_PORT  1883
#define MQTT_CLIENT_ID    "picow-clock"
#define MQTT_TOPIC_PREFIX "picow-clock"
// Subscribed topics: <prefix>/brightness/set  (0-255 or "auto")
//                    <prefix>/power/set        ("ON"/"OFF")

#endif // CONFIG_H
