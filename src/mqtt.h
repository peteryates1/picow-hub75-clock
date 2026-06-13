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

#endif // MQTT_H
