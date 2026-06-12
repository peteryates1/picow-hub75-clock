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
#include "font_large.h"

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
#define FC_R 150
#define FC_G 80
#define FC_B 0       // dimmer amber for the min/max forecast

static const char *const WDAY_FULL[7] = {"SUNDAY","MONDAY","TUESDAY","WEDNESDAY",
                                         "THURSDAY","FRIDAY","SATURDAY"};
static const char *const MONTH[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                      "JUL","AUG","SEP","OCT","NOV","DEC"};

// Latest outside temperature as a printable string; '~' renders as the degree
// glyph. "--~" until the first weather fetch lands.
static char g_temp[12] = "--~";
// Today's forecast min/max, e.g. "12-19"; empty until fetched.
static char g_minmax[12] = "";

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

// Horizontal advance for a character. Narrow glyphs ('-', ':', space) take less
// room so dense lines like "12 JUN 2026" fit the 64px width.
static int char_advance(char c, int scale) {
    return (c == '-' || c == ':' || c == ' ' || c == '~') ? 4 * scale
                                                          : (FONT_W + 1) * scale;
}

// Draw an antialiased grayscale glyph: each cell value (0-255) scales the
// colour, giving smooth edges via the panel's BCM brightness levels.
static void draw_glyph_aa(int x, int y, const uint8_t *glyph, int gw, int gh,
                          uint8_t cr, uint8_t cg, uint8_t cb) {
    for (int py = 0; py < gh; py++)
        for (int px = 0; px < gw; px++) {
            uint8_t v = glyph[py * gw + px];
            if (!v) continue;
            hub75_set_pixel(x + px, y + py,
                            (uint16_t)cr * v / 255,
                            (uint16_t)cg * v / 255,
                            (uint16_t)cb * v / 255);
        }
}

// Tiny 3x5 font, for the min/max forecast line. Digits advance 4px, dash 3px.
static void draw_tiny_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t *rows;
    if (c >= '0' && c <= '9') rows = TINY_DIGITS[c - '0'];
    else if (c == '-')        rows = TINY_DASH;
    else if (c == '/')        rows = TINY_SLASH;
    else return;
    for (int ry = 0; ry < TINY_H; ry++)
        for (int rx = 0; rx < TINY_W; rx++)
            if (rows[ry] & (1u << (TINY_W - 1 - rx)))
                hub75_set_pixel(x + rx, y + ry, r, g, b);
}

static int tiny_width(const char *s) {
    int w = 0;
    for (; *s; s++) w += (*s == '-') ? 3 : (TINY_W + 1);
    return w ? w - 1 : 0;
}

static void draw_tiny(int x, int y, const char *s, uint8_t r, uint8_t g, uint8_t b) {
    for (; *s; s++) {
        draw_tiny_char(x, y, *s, r, g, b);
        x += (*s == '-') ? 3 : (TINY_W + 1);
    }
}

// Draw a string left-to-right using per-character advances.
static void draw_text(int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (; *s; s++) {
        draw_char(x, y, *s, scale, r, g, b);
        x += char_advance(*s, scale);
    }
}

// Pixel width of a string at the given scale (used for centring/right-align).
static int text_width(const char *s, int scale) {
    int w = 0;
    for (; *s; s++) w += char_advance(*s, scale);
    return w;
}

// Full clock face: large HH:MM top-left, temperature top-right, day+date below.
static void draw_clock_face(const struct tm *t, bool colon_on) {
    hub75_clear();

    // Time: large antialiased font, top-left. HH<colon>MM, colon blinks.
    // 1px gaps between digits; tight colon — sized to leave the top-right for
    // the temperature.
    const int gw = FONT_LARGE_W, gh = FONT_LARGE_H;
    const int ty = (PANEL_SCAN_ROWS - gh) / 2;   // centre in the top half
    const int d[4] = { t->tm_hour / 10, t->tm_hour % 10,
                       t->tm_min / 10,  t->tm_min % 10 };
    int x = 0;
    draw_glyph_aa(x, ty, &FONT_LARGE_DIGITS[d[0]][0][0], gw, gh, TIME_R, TIME_G, TIME_B); x += gw + 1;
    draw_glyph_aa(x, ty, &FONT_LARGE_DIGITS[d[1]][0][0], gw, gh, TIME_R, TIME_G, TIME_B); x += gw + 1;
    if (colon_on)
        draw_glyph_aa(x, ty, &FONT_LARGE_COLON[0][0], FONT_LARGE_COLON_W, gh,
                      TIME_R, TIME_G, TIME_B);
    x += FONT_LARGE_COLON_W - 1;                  // colon advance (~5px)
    draw_glyph_aa(x, ty, &FONT_LARGE_DIGITS[d[2]][0][0], gw, gh, TIME_R, TIME_G, TIME_B); x += gw + 1;
    draw_glyph_aa(x, ty, &FONT_LARGE_DIGITS[d[3]][0][0], gw, gh, TIME_R, TIME_G, TIME_B);

    // Temperature: scale 1, top-right, right-aligned but kept clear of the time.
    int tw = text_width(g_temp, 1);
    int tx = PANEL_WIDTH - tw;
    if (tx < 48) tx = 48;
    draw_text(tx, 0, g_temp, 1, TEMP_R, TEMP_G, TEMP_B);

    // Today's min/max forecast, tiny font, right-aligned under the temperature.
    if (g_minmax[0]) {
        int mw = tiny_width(g_minmax);
        int mmx = PANEL_WIDTH - mw;
        if (mmx < 44) mmx = 44;
        draw_tiny(mmx, 9, g_minmax, FC_R, FC_G, FC_B);
    }

    // Bottom half: two centred lines — numeric date, then the full day name.
    char date[16];
    snprintf(date, sizeof date, "%02d %s %04d",
             t->tm_mday, MONTH[t->tm_mon], t->tm_year + 1900);
    int dw1 = text_width(date, 1);
    int dx1 = (PANEL_WIDTH - dw1) / 2;
    if (dx1 < 0) dx1 = 0;
    draw_text(dx1, PANEL_SCAN_ROWS + 1, date, 1, DATE_R, DATE_G, DATE_B);

    const char *day = WDAY_FULL[t->tm_wday];
    int dw2 = text_width(day, 1);
    int dx2 = (PANEL_WIDTH - dw2) / 2;
    if (dx2 < 0) dx2 = 0;
    draw_text(dx2, PANEL_SCAN_ROWS + 9, day, 1, DATE_R, DATE_G, DATE_B);
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
            int cur, mn = -999, mx = -999;
            if (weather_fetch(&cur, &mn, &mx, 8000)) {
                snprintf(g_temp, sizeof g_temp, "%d~", cur);
                if (mn > -999 && mx > -999)
                    snprintf(g_minmax, sizeof g_minmax, "%d/%d", mn, mx);
                printf("Weather: %d C (min %d max %d)\n", cur, mn, mx);
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
                printf("[hb] core1=%d frames=%u wifi_link=%d time=%02d:%02d:%02d temp=%s mm=%s\n",
                       core1, frames, link, t.tm_hour, t.tm_min, t.tm_sec,
                       g_temp, g_minmax[0] ? g_minmax : "-");
            } else {
                printf("[hb] core1=%d frames=%u wifi_link=%d time=unset temp=%s mm=%s\n",
                       core1, frames, link, g_temp, g_minmax[0] ? g_minmax : "-");
            }
        }

        sleep_ms(200);
    }
}
