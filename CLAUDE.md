# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a Raspberry Pi Pico W that drives a **64×32 HUB75 RGB LED matrix**
as a network clock. Time is set from NTP over Wi-Fi at boot and re-synced
periodically; a GL5516 LDR on ADC0 auto-dims the panel (bright by day, dim at
night). Pure C on the Pico SDK (2.2.0), no RTOS.

## Build & flash

```sh
export PICO_SDK_PATH=/home/peter/pico-sdk
# Clock board (default):
cmake -B build                                          # UK time: -DTZ_DST_UK=ON  (or fixed -DTZ_OFFSET_SECONDS=3600)
cmake --build build -j4
# Control-panel test board:
cmake -B build_cp -DTARGET_BOARD=control_panel
cmake --build build_cp -j4
```

- Output: `build/picow_hub75_clock.uf2`. Flash by holding BOOTSEL, plugging in,
  and copying the `.uf2` to the `RPI-RP2` drive (or via SWD — see below).
- `PICO_BOARD` is forced to `pico_w` in CMakeLists.txt **before** the SDK import
  — required or the `pico/cyw43_arch.h` headers won't be on the include path.
- **Wi-Fi credentials**: CMake reads them from an untracked `network.txt` in the
  project root (`WIFI_SSID`/`WIFI_PASSWORD`, plus optional
  `WIFI_SSID_BACKUP`/`WIFI_PASSWORD_BACKUP`), or from the matching `-D` flags
  which take precedence. The firmware prefers the primary, falls back to the
  backup (`wifi_connect()`/`wifi_try()` in `main.c`), and the loop reconnects if
  the link drops. `network.txt` is gitignored — never commit it. There is no
  test suite; this is bare-metal firmware, so "running it" means flashing hardware.
- Logs go to **USB CDC serial** (`pico_enable_stdio_usb`), not UART. The main
  loop prints a periodic `[hb] wifi_link=… time=…` heartbeat so status is
  observable without having to catch the one-shot boot logs.

## Flashing & on-hardware testing (Pico 2 W + debug probe)

The current **bench test board is a Pico 2 W (RP2350, Cortex-M33)** driven over
SWD by a Raspberry Pi Debug Probe — *not* the RP2040 Pico W that the product
clock PCB uses. The two are different architectures, so firmware must be built
for the matching chip:

```sh
# Build for the RP2350 bench board (separate build dir). The Adafruit P2.5
# 64x32 panel has green/blue swapped, so add -DPANEL_SWAP_GB=ON:
cmake -B build_pico2 -DPICO_BOARD=pico2_w -DTARGET_BOARD=control_panel \
      -DTZ_DST_UK=ON -DPANEL_SWAP_GB=ON
cmake --build build_pico2 -j4
```

Panel mounting holes are **M3**.

`pico_aon_timer` (not `hardware_rtc`) is used precisely so the same time code
works on both: **RP2350 has no RTC peripheral**, so on that target the SDK
transparently backs the AON timer with `hardware_powman`. Don't switch to
`hardware/rtc.h` — it would break the RP2350 build.

Flash and reset over SWD (the probe enumerates as USB `2e8a:000c`):

```sh
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 1000" \
  -c "program build_pico2/picow_hub75_clock.elf verify reset exit"
```

- Use `target/rp2350.cfg` for the Pico 2 W, `target/rp2040.cfg` for a real Pico
  W. Wrong target ⇒ `Too long SWD WAIT` / `Failed to connect multidrop`.
- `sudo` is currently required: the host user is not in the `plugdev` group, so
  openocd can't open the probe's USB node otherwise. (Fix: add the user to
  `plugdev` and re-login.)
- Keep SWD at ~1000 kHz; 5000 kHz was unreliable on this rig.

Read serial logs from the **target's** USB CDC (vendor `2e8a`, *not* the `000c`
probe); the node moves around (`/dev/ttyACM*`) across re-enumerations, so pick it
by vendor id. Opening it over USB CDC re-enumerates on reset, which makes the
one-shot boot lines racy to catch — rely on the heartbeat instead, e.g.:

```sh
stty -F /dev/ttyACM1 raw -echo 115200
timeout 20 cat /dev/ttyACM1
# [hb] wifi_link=1  time=15:47:19   <- link=1 means joined; time matching host UTC means NTP worked
```

## Architecture — the dual-core split is the key idea

The two cores have strict, separate jobs and communicate only through the shared
framebuffer + a brightness byte. Keep this boundary intact.

- **core1 = display only.** `hub75.c` runs an infinite bit-banged **Binary Code
  Modulation (BCM)** refresh loop (`hub75_refresh_loop`), launched via
  `multicore_launch_core1`. It owns every HUB75 GPIO and all the timing. core0
  must never touch those pins.
- **core0 = everything else.** `main.c` does Wi-Fi, NTP, reads the LDR, formats
  the time, and writes pixels. It only ever calls `hub75_set_pixel/clear` and
  `hub75_set_brightness`.
- **Shared state, deliberately lock-free.** The `fb[][][]` framebuffer and
  `g_brightness` in `hub75.c` are plain `volatile` with no mutex. A pixel can
  tear for a single frame mid-update, which is invisible on a clock. Don't add
  locking unless a real artifact appears — it would stall the refresh loop.

### RP2350 core1 launch order — do not reorder (hard-won)

On the Pico 2 W (RP2350) two things are mandatory or core1's display dies:

1. **Launch core1 *after* `cyw43_arch_init()`.** `cyw43_arch_init()` performs
   flash-touching work; a core1 already running the refresh loop out of flash
   gets knocked back into the bootrom by it (symptom: core1 runs ~16 frames,
   then PC returns to ROM `~0x000000da` with a garbage SP, panel flashes once
   then goes dark). So `main()` brings up Wi-Fi first, *then* calls
   `hub75_init()`. The cost is a brief blank panel during the boot Wi-Fi connect.
2. **`hub75_init()` does `multicore_reset_core1()` before `multicore_launch_core1()`**
   (core1 otherwise intermittently stays parked in the bootrom after a debugger
   reset and never starts), and the refresh loop calls
   **`multicore_lockout_victim_init()`** so later core0 flash operations park
   core1 safely instead of corrupting it.

The serial heartbeat prints `core1=<alive> frames=<n>`; if `frames` is frozen,
core1 has died — check this ordering first. Note SWD/debugger resets don't
reliably reset core1, so after flashing, a **cold power-cycle of the Pico** is
sometimes needed for core1 to launch; a normal power-on boot is always fine.

### Display driver details (`hub75.c`)

- 64×32 panel is **1/16 scan**: address lines A–D select one of 16 row-pairs;
  each scan drives an upper pixel (R1/G1/B1) and a lower pixel (R2/G2/B2).
- The six RGB GPIOs are **contiguous on GP10–GP15** specifically so a column is
  written with one `gpio_put_masked` instead of six `gpio_put`s. The bit order
  within that block is scrambled by PCB routing — `pack_column()` maps colours
  to the right bit using the `RGB_BIT_*` offsets in `config.h`.
- Colour depth = `HUB75_BCM_DEPTH` (6 bits). Bit-plane *p* is lit for
  `base << p` (binary-weighted intensity), and brightness scales that on-time,
  so it dims the whole panel without redrawing. The on-time is timed in **CPU
  cycles** (`base_cyc`, = `BCM_BASE_US` µs in cycles) via
  `busy_wait_at_least_cycles`, not `busy_wait_us_32` — the µs granularity used to
  truncate dim levels to 0, putting a brightness floor at ~4; cycles let it go
  sub-µs so night levels like 1–2 work.
- `OE` is **active-low** (low = lit). The loop blanks (OE high), latches, sets
  the row, then enables.

### Pin assignments come from the PCB, not preference — and are per-board

GPIO maps are **transcribed from EasyEDA netlists** and are physical traces:
never "tidy" or reassign them. Each board has its own header:

- `include/board_clock.h` — the real clock PCB (`Netlist_clock-picow-hub75-ldr_*.enet`),
  has the LDR and the button.
- `include/board_control_panel.h` — alternative bring-up board
  (`Netlist_Control_Panel_8_*.enet`); same HUB75 IDC connector, **different
  GPIOs**, and **no LDR / no button** (those Pico pins drive I²C expanders,
  servos, motors there).

`include/config.h` `#include`s the right header based on the `BOARD_CONTROL_PANEL`
define and holds only board-common settings (panel geometry, timezone, NTP).
Select the board at configure time: `-DTARGET_BOARD=control_panel` (default is
the clock board). Build both side by side in separate dirs (`build/`, `build_cp/`).

Each board header **must** define the HUB75 pins, `RGB_BASE_PIN` + `RGB_BIT_*`
(the six RGB pins must stay contiguous for the masked write), `HAS_LDR`,
`HAS_BUTTON`, and `DEFAULT_BRIGHTNESS`. `HAS_LDR`/`HAS_BUTTON` gate the matching
code in `main.c` (and the whole body of `ldr.c`); when `HAS_LDR` is 0 the panel
runs at the fixed `DEFAULT_BRIGHTNESS` with no light sensing.

### Time (`main.c` + `ntp.c`)

- `ntp.c` is a minimal SNTP client on **lwIP raw UDP** (no sockets). It resolves
  `NTP_SERVER` via DNS, sends one request, parses the transmit timestamp, and
  converts NTP→Unix epoch (subtract `NTP_UNIX_DELTA`). Runs on core0 with
  `cyw43_arch_poll()`; all lwIP calls are wrapped in
  `cyw43_arch_lwip_begin/end`.
- The on-chip **AON timer / RTC** (`pico/aon_timer.h`) holds wall-clock time
  (UTC). The local offset is applied only at render time by `local_offset()`:
  either a fixed `TZ_OFFSET_SECONDS`, or — with `-DTZ_DST_UK=ON` — automatic UK
  BST/GMT (BST = UTC+1 from 01:00 UTC last Sunday of March to 01:00 UTC last
  Sunday of October). Re-sync runs every `NTP_RESYNC_HOURS`, or on demand when **SW1
  (GP16)** is pressed — the button's only job is to force an NTP re-sync.
  It's active-low with the internal pull-up; `button_pressed()` does falling-edge
  detection debounced against the 200 ms loop tick.
- Wi-Fi or NTP failure is non-fatal: the display keeps running, the time just
  stays unset until the next successful sync.

### Rendering

Fonts:
- `font.h` — 5×7 font (digits, A–Z, `:`, space, `-`, `~`=degree) via
  `font_glyph()`, plus a tiny 3×5 digit/dash/slash font for the forecast line.
- `font_large.h` — **antialiased grayscale** large digits + colon, *generated*
  by `tools/genfont.py` (rasterises DejaVu Sans Condensed at a fixed size with
  Pillow; one byte/pixel = brightness). Regenerate with
  `python3 tools/genfont.py > src/font_large.h`. `draw_glyph_aa()` scales the
  draw colour by each pixel's value, so the 6-bit BCM gives smooth edges.
- `icons.h` — six 10×10 palette-indexed colour weather icons (sun, partly,
  cloud, rain, snow, storm) + `wx_from_code()` mapping a WMO code to one.

`draw_clock_face()` lays out: large antialiased `HH:MM` top-left (1 Hz blinking
colon); top-right the current temperature with the forecast (`19/9`, tiny font)
under it and a current-conditions weather icon below that; the **day name
left-aligned** on the upper bottom line with the icon tucked in the gap at its
right, and `DD MON YYYY` centred on the lower line. `draw_char`/`draw_text` do
scalable 5×7 text with per-character advances (narrow `-`/`:`/space/`~`); the
forecast's `-` and `/` are also narrowed so 2-digit negatives like `-3/-12`
still fit. Cleared and redrawn each 200 ms on core0; before the clock is set it
shows the bring-up test pattern. `-DICON_DEMO=ON` cycles all icons (and shows
WEDNESDAY + negative-temp examples) for layout checks.

### Weather (outside temperature + time-aware forecast)

`weather.c` fetches from **Open-Meteo over plain HTTP** (lwIP raw TCP, no TLS):
`GET /v1/forecast?latitude=..&longitude=..&current=temperature_2m&daily=temperature_2m_max,temperature_2m_min`.
The ~500-byte JSON gives current temp and today's max/min in one small response
(wttr.in's j1 was rejected — its min/max sit ~14 KB into a 40 KB reply). It
requests `forecast_days=2` (today + tomorrow) and the parser anchors on the
`"current"`/`"daily"` objects (not the `*_units` strings), reading the daily
`[today, tomorrow]` arrays. `build_minmax()` in `main.c` then picks which
pair/order to show by time of day (today-high/tonight-low by day, etc.). Location is `WEATHER_LAT`/`WEATHER_LON`
build defines (default Fleckney). The loop refreshes every
`WEATHER_UPDATE_MINUTES`, retrying sooner on failure; results go to `g_temp`
(`~` = degree) and `g_minmax` (`"14-22"`). Blocks core0 briefly; core1 keeps
refreshing.

### MQTT (brightness / power control)

`mqtt.c` uses the lwIP MQTT app (`pico_lwip_mqtt`) to connect anonymously to a
broker at `MQTT_BROKER_IP` (default `192.168.0.2`, override with `-D`). It makes
**one wildcard subscription** `<prefix>/+/set` (subscribing to each topic
separately can exhaust lwIP's in-flight request pool and silently drop the later
ones); the incoming handler routes by topic name:
- `brightness/set` — `0`–`255`, or `auto` to clear the override (transient)
- `power/set` — `ON`/`OFF` (transient)
- `day/set`, `night/set` — automatic day/night levels (publish **retained**)
- `day_start/set`, `day_end/set` — day-window hours 0–23 (publish **retained**)

Incoming callbacks (lwIP context) call the `mqtt_set_*()` functions in `main.c`,
which set `g_power` / `g_bright_override` / `g_bright_day` / `g_bright_night` /
`g_day_start` / `g_day_end`. The schedule values are published retained, so the
broker re-delivers them on every connect — runtime config that survives reboots
without flash writes. `mqtt_poll()` (10 s timer) reconnects after a drop; MQTT
runs concurrently with NTP/weather (lwIP serviced from an IRQ).

`tools/clock_control.py` is a dependency-free Tkinter GUI (built-in minimal MQTT
publisher) exposing all of the above, publishing the schedule settings retained.

### Web control page + static IP

`webserver.c` is a tiny HTTP server on port 80 (lwIP raw TCP, one connection at
a time, HTTP/1.0-style close). `GET /` serves an embedded HTML+JS page; `GET
/state` returns the control state as JSON; `GET /set?...` applies query params
(`bri`, `auto=1`, `power`, `day`, `night`, `start`, `end`). It and MQTT both go
through the shared **`control.h`** API (`control_set_*` / getters) implemented in
`main.c` — that's the single source of truth for the control state, not MQTT.

Static IP is optional, from `network.txt` (`STATIC_IP`/`GATEWAY`/`NETMASK`,
parsed by CMake into `STATIC_IP`/`STATIC_GW`/`STATIC_MASK`). `apply_static_ip()`
runs after Wi-Fi connect: `dhcp_stop()` + `netif_set_addr()`, and points DNS at
the gateway (+ `1.1.1.1` fallback) so NTP/weather still resolve. Empty
`STATIC_IP` ⇒ DHCP. The boot log prints the resulting IP.

### Brightness model

`apply_brightness()` (called every loop tick) resolves brightness in priority
order: **MQTT power-off → MQTT override → automatic**. Automatic is the LDR on
boards with `HAS_LDR`, otherwise time-of-day (`BRIGHT_DAY`/`BRIGHT_NIGHT` between
`BRIGHT_DAY_START_HOUR`/`END_HOUR`, local time). `hub75_set_brightness()` scales
the BCM on-time.

## lwIP configuration

`include/lwipopts.h` configures the threadsafe-background cyw43 stack (DHCP +
DNS + UDP). Note `MEMP_NUM_TCP_SEG` must stay ≥ the computed `TCP_SND_QUEUELEN`
(currently 32) or the lwIP sanity check fails the build, even though this
project doesn't use TCP.
