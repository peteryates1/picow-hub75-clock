#include <stdio.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/aon_timer.h"
#include "hardware/gpio.h"

#include "config.h"
#include "hub75.h"
#include "ldr.h"
#include "ntp.h"
#include "font.h"

// Digit colour (full-scale; the LDR brightness control dims the whole panel).
#define CLK_R 0
#define CLK_G 180
#define CLK_B 255

// Draw one 5x7 glyph at (x,y) in the given colour. `glyph` indexes FONT[].
static void draw_glyph(int x, int y, int glyph, uint8_t r, uint8_t g, uint8_t b) {
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = FONT[glyph][row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (1u << (FONT_W - 1 - col)))
                hub75_set_pixel(x + col, y + row, r, g, b);
        }
    }
}

// Render HH:MM centred on the panel. `colon_on` blinks the separator.
static void draw_time(int hh, int mm, bool colon_on) {
    hub75_clear();

    // Layout: D D : D D. Digits 5px + 1px gap; colon 3px wide.
    // Total = 5+1+5+1+3+1+5+1+5 = 27px, centred on a 64px panel.
    const int y = (PANEL_HEIGHT - FONT_H) / 2;
    int x = (PANEL_WIDTH - 27) / 2;

    draw_glyph(x, y, hh / 10, CLK_R, CLK_G, CLK_B); x += 6;
    draw_glyph(x, y, hh % 10, CLK_R, CLK_G, CLK_B); x += 6;
    if (colon_on)
        draw_glyph(x, y, FONT_COLON, CLK_R, CLK_G, CLK_B);
    x += 4;
    draw_glyph(x, y, mm / 10, CLK_R, CLK_G, CLK_B); x += 6;
    draw_glyph(x, y, mm % 10, CLK_R, CLK_G, CLK_B);
}

// Set the on-chip always-on timer (RTC) from a UTC epoch.
static void set_clock(time_t utc_epoch) {
    struct timespec ts = { .tv_sec = utc_epoch, .tv_nsec = 0 };
    if (aon_timer_is_running())
        aon_timer_set_time(&ts);
    else
        aon_timer_start(&ts);
}

#if HAS_BUTTON
// SW1 is active-low to GND with the internal pull-up enabled, so the pin reads
// 0 when pressed.
static void button_init(void) {
    gpio_init(PIN_BUTTON);
    gpio_set_dir(PIN_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_BUTTON);
}

// Return true once per press (falling edge), debounced against the main loop's
// tick rate. `prev_pressed` holds state between calls.
static bool button_pressed(bool *prev_pressed) {
    bool pressed = !gpio_get(PIN_BUTTON);   // active-low
    bool edge = pressed && !*prev_pressed;
    *prev_pressed = pressed;
    return edge;
}
#endif // HAS_BUTTON

// Connect Wi-Fi, fetch NTP time, and set the RTC. Returns true on success.
static bool sync_time_from_ntp(void) {
    time_t epoch;
    if (!ntp_sync(&epoch, 10000)) {
        printf("NTP sync failed\n");
        return false;
    }
    set_clock(epoch);
    printf("NTP sync ok: epoch=%lld\n", (long long)epoch);
    return true;
}

int main(void) {
    stdio_init_all();

    hub75_init();   // launches the refresh loop on core1
    hub75_set_brightness(DEFAULT_BRIGHTNESS);
#if HAS_LDR
    ldr_init();
#endif
#if HAS_BUTTON
    button_init();
#endif

    if (cyw43_arch_init()) {
        printf("cyw43 init failed\n");
        return 1;
    }
    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi '%s'...\n", WIFI_SSID);
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD,
                                           CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Wi-Fi connect failed\n");
        // Keep running: the display works, the time is just unset.
    } else {
        printf("Wi-Fi connected\n");
        sync_time_from_ntp();
    }

    absolute_time_t next_resync =
        make_timeout_time_ms((uint32_t)NTP_RESYNC_HOURS * 3600 * 1000);
#if HAS_BUTTON
    bool button_prev = false;
#endif
    unsigned ticks = 0;

    for (;;) {
#if HAS_LDR
        // Auto-brightness from ambient light.
        hub75_set_brightness(ldr_brightness());
#endif

#if HAS_BUTTON
        // Button forces an immediate NTP re-sync.
        if (button_pressed(&button_prev)) {
            printf("Button: manual NTP re-sync\n");
            if (sync_time_from_ntp())
                next_resync = make_timeout_time_ms(
                    (uint32_t)NTP_RESYNC_HOURS * 3600 * 1000);
        }
#endif

        // Read local time and draw it.
        struct timespec ts;
        if (aon_timer_is_running()) {
            aon_timer_get_time(&ts);
            time_t local = ts.tv_sec + TZ_OFFSET_SECONDS;
            struct tm t;
            gmtime_r(&local, &t);
            bool colon_on = (t.tm_sec & 1) == 0;  // 1 Hz blink
            draw_time(t.tm_hour, t.tm_min, colon_on);
        }

        // Periodic re-sync to correct RTC drift.
        if (absolute_time_diff_us(get_absolute_time(), next_resync) <= 0) {
            if (sync_time_from_ntp())
                next_resync = make_timeout_time_ms(
                    (uint32_t)NTP_RESYNC_HOURS * 3600 * 1000);
            else
                next_resync = make_timeout_time_ms(60 * 1000);  // retry soon
        }

        // Heartbeat: periodic status on the serial console (~3s).
        if (++ticks % 15 == 0) {
            int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            if (aon_timer_is_running()) {
                struct timespec hts;
                aon_timer_get_time(&hts);
                time_t local = hts.tv_sec + TZ_OFFSET_SECONDS;
                struct tm t;
                gmtime_r(&local, &t);
                printf("[hb] wifi_link=%d  time=%02d:%02d:%02d\n",
                       link, t.tm_hour, t.tm_min, t.tm_sec);
            } else {
                printf("[hb] wifi_link=%d  time=unset\n", link);
            }
        }

        sleep_ms(200);
    }
}
