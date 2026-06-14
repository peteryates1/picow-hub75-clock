#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>

// Outside temperature plus today's and tomorrow's forecast min/max (whole
// degrees Celsius), from Open-Meteo over plain HTTP for WEATHER_LAT/WEATHER_LON.
typedef struct {
    int current;
    int condition;   // WMO weather code for the current conditions (-1 if absent)
    int today_min, today_max;
    int tomorrow_min, tomorrow_max;
} weather_t;

// Blocking with a timeout; call from core0 with cyw43 up. Returns true if the
// current temperature and today's forecast were parsed.
bool weather_fetch(weather_t *out, uint32_t timeout_ms);

#endif // WEATHER_H
