#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>

// MQTT control client. Connects (anonymously) to the broker at MQTT_BROKER_IP
// and subscribes to brightness/power control topics. Call mqtt_start() once
// after Wi-Fi is up, then mqtt_poll() periodically from the main loop to
// reconnect if the link drops.
void mqtt_start(void);
void mqtt_poll(void);
bool mqtt_is_connected(void);

// Implemented by the application; invoked when a control message arrives.
//   value: 0-255 to force a brightness, or -1 to return to automatic.
void mqtt_set_brightness(int value);
//   on: true = panel on, false = blank.
void mqtt_set_power(bool on);
//   day/night levels (0-255) used by the automatic time-of-day dimming.
void mqtt_set_day_brightness(int value);
void mqtt_set_night_brightness(int value);
//   day-window start/end hour (0-23, local time).
void mqtt_set_day_start(int hour);
void mqtt_set_day_end(int hour);

#endif // MQTT_H
