#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>

// Fetch the current outside temperature plus today's forecast min/max (whole
// degrees Celsius) from wttr.in's JSON feed over plain HTTP, for the location
// configured as WEATHER_LOCATION. Blocking with a timeout; call from core0 with
// cyw43 up and connected.
//
// Returns true if at least the current temperature was parsed. *out_min and
// *out_max are set when present in the response (left untouched otherwise).
bool weather_fetch(int *out_current, int *out_min, int *out_max,
                   uint32_t timeout_ms);

#endif // WEATHER_H
