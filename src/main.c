#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/aon_timer.h"
#include "hardware/gpio.h"

#include "config.h"
#include "hub75.h"
#include "ldr.h"
#include "ntp.h"
#include "weather.h"
#include "font.h"

// Colours (full-scale; the LDR/brightness control dims the whole panel).
#define TIME_R 0
#define TIME_G 180
#define TIME_B 255   // cyan time
#define DATE_R 170
#define DATE_G 170
#define DATE_B 170   // soft white day/date
#define TEMP_R 255
#define TEMP_G 140
#define TEMP_B 0     // amber temperature

static const char *const WDAY[7]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char *const MONTH[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                      "JUL","AUG","SEP","OCT","NOV","DEC"};

// Latest outside temperature as a printable string; '~' renders as the degree
// glyph. "--~" until the first weather fetch lands (wired up separately).
static char g_temp[12] = "--~";

// Draw one 5x7 character at (x,y) with each pixel expanded to scale x scale.
static void draw_char(int x, int y, char c, int scale,
                      uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t *rows = font_glyph(c);
    for (int ry = 0; ry < FONT_H; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < FONT_W; rx++) {
            if (!(bits & (1u << (FONT_W - 1 - rx)))) continue;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    hub75_set_pixel(x + rx * scale + dx, y + ry * scale + dy, r, g, b);
        }
    }
}

// Draw a string left-to-right; each cell is (FONT_W+1)*scale wide.
static void draw_text(int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (; *s; s++) {
        draw_char(x, y, *s, scale, r, g, b);
        x += (FONT_W + 1) * scale;
    }
}

// Pixel width of a string at the given scale (no trailing inter-char gap).
static int text_width(const char *s, int scale) {
    int n = (int)strlen(s);
    return n > 0 ? n * (FONT_W + 1) * scale - scale : 0;
}

// Full clock face: large HH:MM top-left, temperature top-right, day+date below.
static void draw_clock_face(const struct tm *t, bool colon_on) {
    hub75_clear();

    // Time: scale 2 (10x14 digits), top-left.
    const int s = 2, dw = FONT_W * s, ty = 1;
    int x = 0;
    draw_char(x, ty, '0' + t->tm_hour / 10, s, TIME_R, TIME_G, TIME_B); x += dw + 1;
    draw_char(x, ty, '0' + t->tm_hour % 10, s, TIME_R, TIME_G, TIME_B); x += dw + 1;
    if (colon_on) {                       // narrow colon: two scale x scale dots
        for (int dy = 0; dy < s; dy++)
            for (int dx = 0; dx < s; dx++) {
                hub75_set_pixel(x + dx, ty + 4 + dy, TIME_R, TIME_G, TIME_B);
                hub75_set_pixel(x + dx, ty + 9 + dy, TIME_R, TIME_G, TIME_B);
            }
    }
    x += s + 2;                           // colon advance (~4px)
    draw_char(x, ty, '0' + t->tm_min / 10, s, TIME_R, TIME_G, TIME_B); x += dw + 1;
    draw_char(x, ty, '0' + t->tm_min % 10, s, TIME_R, TIME_G, TIME_B);

    // Temperature: scale 1, top-right, right-aligned but kept clear of the time.
    int tw = text_width(g_temp, 1);
    int tx = PANEL_WIDTH - tw;
    if (tx < 48) tx = 48;
    draw_text(tx, 0, g_temp, 1, TEMP_R, TEMP_G, TEMP_B);

    // Day + date: scale 1, centred on the bottom half. e.g. "MON 12 JUN".
    char date[16];
    snprintf(date, sizeof date, "%s %d %s",
             WDAY[t->tm_wday], t->tm_mday, MONTH[t->tm_mon]);
    int dwid = text_width(date, 1);
    int dx = (PANEL_WIDTH - dwid) / 2;
    if (dx < 0) dx = 0;
    int dy = PANEL_SCAN_ROWS + (PANEL_SCAN_ROWS - FONT_H) / 2;
    draw_text(dx, dy, date, 1, DATE_R, DATE_G, DATE_B);
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
    absolute_time_t next_weather = make_timeout_time_ms(3000);  // shortly after boot
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
            draw_clock_face(&t, colon_on);
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

        // Outside temperature from wttr.in: refresh periodically, retry sooner
        // on failure. core1 keeps refreshing the panel while this blocks.
        if (absolute_time_diff_us(get_absolute_time(), next_weather) <= 0) {
            int celsius;
            if (weather_fetch_temp(&celsius, 8000)) {
                snprintf(g_temp, sizeof g_temp, "%d~", celsius);
                printf("Weather: %d C\n", celsius);
                next_weather = make_timeout_time_ms(
                    (uint32_t)WEATHER_UPDATE_MINUTES * 60 * 1000);
            } else {
                printf("Weather fetch failed\n");
                next_weather = make_timeout_time_ms(60 * 1000);
            }
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
