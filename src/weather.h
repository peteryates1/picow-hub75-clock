#ifndef WEATHER_H
#define WEATHER_H

#include <stdbool.h>
#include <stdint.h>

// Fetch the current outside temperature in whole degrees Celsius from wttr.in
// over plain HTTP, for the location configured as WEATHER_LOCATION. Blocking
// with a timeout; call from core0 with cyw43 up and connected. Returns true and
// writes *out_celsius on success.
bool weather_fetch_temp(int *out_celsius, uint32_t timeout_ms);

#endif // WEATHER_H
