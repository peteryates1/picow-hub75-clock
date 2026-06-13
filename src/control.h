#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>

// Display control state, set from MQTT or the web server and read by the
// render loop. Implemented in main.c.

void control_set_brightness(int value);  // 0-255 to force, or -1 = automatic
void control_set_power(bool on);          // false blanks the panel
void control_set_day(int value);          // 0-255 daytime level
void control_set_night(int value);        // 0-255 night level
void control_set_day_start(int hour);     // 0-23
void control_set_day_end(int hour);       // 0-23

// Getters for the web UI.
bool        control_power(void);
int         control_override(void);       // -1 when automatic
int         control_day(void);
int         control_night(void);
int         control_day_start(void);
int         control_day_end(void);
const char *control_temp(void);           // current temp string, e.g. "15~"
const char *control_minmax(void);         // "11/17" or "" if unknown

#endif // CONTROL_H
