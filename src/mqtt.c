#include "mqtt.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"

#define SUB_BRIGHTNESS  MQTT_TOPIC_PREFIX "/brightness/set"
#define SUB_POWER       MQTT_TOPIC_PREFIX "/power/set"

static mqtt_client_t  *s_client;
static ip_addr_t       s_broker;
static volatile bool   s_connected;
static volatile bool   s_connecting;
static char            s_topic[80];   // topic of the in-flight incoming publish

// --- incoming message handling (lwIP callback context) ---------------------

static void incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) {
    (void)arg; (void)tot_len;
    strncpy(s_topic, topic, sizeof s_topic - 1);
    s_topic[sizeof s_topic - 1] = '\0';
}

static void incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) {
    (void)arg; (void)flags;
    char buf[24];
    u16_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    if (strstr(s_topic, "brightness/set")) {
        if (strncasecmp(buf, "auto", 4) == 0)
            mqtt_set_brightness(-1);
        else
            mqtt_set_brightness(atoi(buf));
        printf("MQTT: brightness <- '%s'\n", buf);
    } else if (strstr(s_topic, "power/set")) {
        bool on = strncasecmp(buf, "ON", 2) == 0 || buf[0] == '1' ||
                  strncasecmp(buf, "true", 4) == 0;
        mqtt_set_power(on);
        printf("MQTT: power <- '%s'\n", buf);
    }
}

static void sub_request_cb(void *arg, err_t err) {
    (void)arg;
    if (err != ERR_OK) printf("MQTT: subscribe failed (%d)\n", err);
}

static void connection_cb(mqtt_client_t *client, void *arg,
                          mqtt_connection_status_t status) {
    (void)arg;
    s_connecting = false;
    if (status == MQTT_CONNECT_ACCEPTED) {
        s_connected = true;
        mqtt_set_inpub_callback(client, incoming_publish_cb, incoming_data_cb, NULL);
        mqtt_subscribe(client, SUB_BRIGHTNESS, 0, sub_request_cb, NULL);
        mqtt_subscribe(client, SUB_POWER, 0, sub_request_cb, NULL);
        printf("MQTT: connected to %s, subscribed\n", MQTT_BROKER_IP);
    } else {
        s_connected = false;
        printf("MQTT: connection lost/refused (%d)\n", status);
    }
}

// --- connection management (call with the lwIP lock held) ------------------

static void do_connect(void) {
    struct mqtt_connect_client_info_t ci = {0};
    ci.client_id  = MQTT_CLIENT_ID;
    ci.keep_alive = 60;
    s_connecting = true;
    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(s_client, &s_broker, MQTT_BROKER_PORT,
                                    connection_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        s_connecting = false;
        printf("MQTT: connect call failed (%d)\n", err);
    }
}

void mqtt_start(void) {
    if (!ipaddr_aton(MQTT_BROKER_IP, &s_broker)) {
        printf("MQTT: bad broker IP '%s'\n", MQTT_BROKER_IP);
        return;
    }
    s_client = mqtt_client_new();
    if (!s_client) {
        printf("MQTT: client alloc failed\n");
        return;
    }
    do_connect();
}

void mqtt_poll(void) {
    if (!s_client || s_connected || s_connecting) return;
    do_connect();   // reconnect after a drop
}

bool mqtt_is_connected(void) {
    return s_connected;
}
