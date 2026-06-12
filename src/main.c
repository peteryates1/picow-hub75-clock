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

static void draw_block(int x0, int y0, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = y0; y < y0 + 6; y++)
        for (int x = x0; x < x0 + 10; x++)
            hub75_set_pixel(x, y, r, g, b);
}

// HUB75 colour-mapping test: six small blocks (low current). Each drives one
// channel; the colour shown reveals the real wiring.
//   top row    blocks drive R / G / B  -> exercises R1 / G1 / B1
//   bottom row blocks drive R / G / B  -> exercises R2 / G2 / B2
// Read the six colours: top L/M/R, then bottom L/M/R.
static void draw_test_pattern(void) {
    hub75_clear();
    // Top half (rows < 16): R1, G1, B1
    draw_block(8,  5, 255, 0,   0);    // drive RED   -> R1
    draw_block(27, 5, 0,   255, 0);    // drive GREEN -> G1
    draw_block(46, 5, 0,   0,   255);  // drive BLUE  -> B1
    // Bottom half (rows >= 16): R2, G2, B2
    draw_block(8,  21, 255, 0,   0);   // drive RED   -> R2
    draw_block(27, 21, 0,   255, 0);   // drive GREEN -> G2
    draw_block(46, 21, 0,   0,   255); // drive BLUE  -> B2
}

// Local-time offset (seconds) from UTC at the given instant.
//
// With TZ_DST_UK defined, computes UK time automatically: BST (UTC+1) from
// 01:00 UTC on the last Sunday of March to 01:00 UTC on the last Sunday of
// October, GMT (UTC+0) otherwise. Without it, a fixed TZ_OFFSET_SECONDS.
static int local_offset(time_t utc) {
#ifdef TZ_DST_UK
    struct tm t;
    gmtime_r(&utc, &t);
    int month = t.tm_mon + 1;   // 1..12
    bool bst;
    if (month < 3 || month > 10) {
        bst = false;                       // Jan, Feb, Nov, Dec -> GMT
    } else if (month > 3 && month < 10) {
        bst = true;                        // Apr..Sep -> BST
    } else {
        // March or October (both 31 days): find the date of the last Sunday.
        int last_day = 31;
        int wday_last = (t.tm_wday + (last_day - t.tm_mday)) % 7;  // 0 = Sunday
        int last_sunday = last_day - wday_last;
        if (month == 3)   // BST begins 01:00 UTC on the last Sunday
            bst = t.tm_mday > last_sunday ||
                  (t.tm_mday == last_sunday && t.tm_hour >= 1);
        else              // October: BST ends 01:00 UTC on the last Sunday
            bst = t.tm_mday < last_sunday ||
                  (t.tm_mday == last_sunday && t.tm_hour < 1);
    }
    return bst ? 3600 : 0;
#else
    (void)utc;
    return TZ_OFFSET_SECONDS;
#endif
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
    // Short timeout: pool.ntp.org rotates servers and some don't answer, so
    // it's better to give up quickly and let the caller retry (which re-resolves
    // to a different server) than to stall 10s on a dead one.
    if (!ntp_sync(&epoch, 4000)) {
        printf("NTP sync failed\n");
        return false;
    }
    set_clock(epoch);
    printf("NTP sync ok: epoch=%lld\n", (long long)epoch);
    return true;
}

int main(void) {
    stdio_init_all();

#ifdef DISPLAY_TEST_PATTERN
    // Bring-up mode: no Wi-Fi at all. Launch core1 and hold the test pattern.
    hub75_init();
    hub75_set_brightness(255);
    for (;;) {
        draw_test_pattern();
        sleep_ms(200);
    }
#endif

    // Bring up wireless BEFORE launching core1. cyw43_arch_init() touches flash;
    // a core1 already running the refresh loop out of flash gets knocked back
    // into the bootrom by that (observed: core1 dies after ~16 frames, PC back
    // in ROM). Initialising wireless first avoids the race; core1 also registers
    // as a flash lockout victim (see hub75_init) to stay safe afterwards.
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
    }

    // Now safe to launch the display refresh on core1.
    hub75_init();
    hub75_set_brightness(DEFAULT_BRIGHTNESS);
#if HAS_LDR
    ldr_init();
#endif
#if HAS_BUTTON
    button_init();
#endif

    // Fire the first NTP sync on the first loop iteration. The loop handles
    // success/retry uniformly, so a failed boot sync (common right after
    // associating, before DHCP/DNS settle) is retried quickly rather than
    // waiting a full re-sync interval.
    absolute_time_t next_resync = get_absolute_time();
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

        // Draw the time once the clock is set; until then show the bring-up
        // test pattern so the panel is never blank.
        struct timespec ts;
        if (aon_timer_is_running()) {
            aon_timer_get_time(&ts);
            time_t local = ts.tv_sec + local_offset(ts.tv_sec);
            struct tm t;
            gmtime_r(&local, &t);
            bool colon_on = (t.tm_sec & 1) == 0;  // 1 Hz blink
            draw_time(t.tm_hour, t.tm_min, colon_on);
        } else {
            draw_test_pattern();
        }

        // NTP sync: full interval once the clock is set, quick retry until it
        // is (and to recover from a dropped network).
        if (absolute_time_diff_us(get_absolute_time(), next_resync) <= 0) {
            bool ok = sync_time_from_ntp();
            next_resync = make_timeout_time_ms(
                ok ? (uint32_t)NTP_RESYNC_HOURS * 3600 * 1000 : 5 * 1000);
        }

        // Heartbeat: periodic status on the serial console (~3s).
        if (++ticks % 15 == 0) {
            int link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
            int core1 = hub75_core1_alive();
            unsigned frames = hub75_frame_count();
            if (aon_timer_is_running()) {
                struct timespec hts;
                aon_timer_get_time(&hts);
                time_t local = hts.tv_sec + local_offset(hts.tv_sec);
                struct tm t;
                gmtime_r(&local, &t);
                printf("[hb] core1=%d frames=%u wifi_link=%d time=%02d:%02d:%02d\n",
                       core1, frames, link, t.tm_hour, t.tm_min, t.tm_sec);
            } else {
                printf("[hb] core1=%d frames=%u wifi_link=%d time=unset\n",
                       core1, frames, link);
            }
        }

        sleep_ms(200);
    }
}
