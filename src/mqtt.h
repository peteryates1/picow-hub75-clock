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

#endif // MQTT_H
