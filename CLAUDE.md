# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Firmware for a Raspberry Pi Pico W (or Pico 2 W) that drives a **64×32 HUB75 RGB
LED matrix** as a network clock. Pure C on the Pico SDK (2.2.0), no RTOS. It
shows a large antialiased time, the current outside temperature + a time-aware
forecast + a conditions icon, and the day/date — over Wi-Fi via NTP (UK DST) and
Open-Meteo. Brightness is automatic (LDR on the real clock board, time-of-day
schedule otherwise) and controllable over **MQTT** and an on-device **web page**.

### Current setup (live deployment)

- **Board/chip:** a bare **Pico W (RP2040)** is the live target; the same
  firmware also runs on a **Pico 2 W (RP2350)** bench board on a debug probe.
- **PCB/panel:** the "Control_Panel_8" carrier driving an **Adafruit
  RGB-Matrix-P2.5 64×32** panel — 1/16 scan, **green/blue swapped** vs standard
  HUB75 (`-DPANEL_SWAP_GB=ON`), **M3** mounting holes, powered from a separate
  5 V supply with a common ground.
- **Clock:** overclocked to **200 MHz** (`SYS_CLOCK_KHZ`) so the bit-banged
  refresh is flicker-free; the refresh loop runs from RAM. (A PIO+DMA driver is
  planned to let the clock drop back to default.)
- **Network:** static IP `192.168.0.4` (from `network.txt`), MQTT broker at
  `192.168.0.2`, control page at `http://192.168.0.4/`.
- **Brightness:** day 40 / night 2, 09:00–21:00, sub-µs dimming.

Canonical build for the two live targets (the Pico W is the active one):

```sh
export PICO_SDK_PATH=/home/peter/pico-sdk
# Pico W (RP2040), control-panel + P2.5 panel:
cmake -B build_picow -DPICO_BOARD=pico_w  -DTARGET_BOARD=control_panel -DTZ_DST_UK=ON -DPANEL_SWAP_GB=ON
cmake --build build_picow -j4
# Pico 2 W (RP2350) bench board:
cmake -B build_cp2   -DPICO_BOARD=pico2_w -DTARGET_BOARD=control_panel -DTZ_DST_UK=ON -DPANEL_SWAP_GB=ON
cmake --build build_cp2 -j4
```

## Build & flash

Canonical build commands are under **Current setup** above; full flash details
(SWD vs BOOTSEL) are under **Flashing & on-hardware testing** below. Build
options: `-DTARGET_BOARD=control_panel` (pin map; default is the clock PCB),
`-DPANEL_SWAP_GB=ON` (Adafruit P2.5 G/B swap), `-DTZ_DST_UK=ON` (UK DST) or
`-DTZ_OFFSET_SECONDS=`, `-DWEATHER_LAT=/-DWEATHER_LON=`, `-DMQTT_BROKER_IP=`,
`-DSYS_CLOCK_KHZ=`, and the diagnostics `-DDISPLAY_TEST_PATTERN=ON` /
`-DICON_DEMO=ON`. Notes:

- `PICO_BOARD` defaults to `pico_w` in CMakeLists.txt **before** the SDK import
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

## Flashing & on-hardware testing

Two boards, two flash methods. They're different architectures, so build for the
matching chip (`-DPICO_BOARD=pico_w` vs `pico2_w`). `pico_aon_timer` (not
`hardware_rtc`) keeps the same time code working on both: **RP2350 has no RTC
peripheral**, so the SDK backs the AON timer with `hardware_powman` there — don't
switch to `hardware/rtc.h` or the RP2350 build breaks. Panel mounting holes M3.

### Pico 2 W (RP2350) — debug probe, SWD

The bench Pico 2 W is driven over SWD by a Raspberry Pi Debug Probe (USB
`2e8a:000c`). Fast dev loop (no manual button), so prefer it for iterating.

```sh
cmake -B build_cp2 -DPICO_BOARD=pico2_w -DTARGET_BOARD=control_panel -DTZ_DST_UK=ON -DPANEL_SWAP_GB=ON
cmake --build build_cp2 -j4
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 1000" \
  -c "program build_cp2/picow_hub75_clock.elf verify reset exit"
```

- Use `target/rp2350.cfg` for the Pico 2 W, `target/rp2040.cfg` for a real Pico W.
  Wrong target ⇒ `Too long SWD WAIT` / `Failed to connect multidrop`.
- `sudo` is required: the host user isn't in `plugdev`, so openocd/picotool can't
  open the USB node otherwise. Keep SWD at ~1000 kHz (5000 was unreliable here).
- RP2350 gotcha: a debugger reset doesn't reliably restart core1 — sometimes a
  cold power-cycle is needed (see the core1 launch-order note above).

### Pico W (RP2040) — BOOTSEL, no probe

The live Pico W has no probe, so flash over USB:

```sh
cmake -B build_picow -DPICO_BOARD=pico_w -DTARGET_BOARD=control_panel -DTZ_DST_UK=ON -DPANEL_SWAP_GB=ON
cmake --build build_picow -j4
# Put it in BOOTSEL: HOLD the BOOTSEL button while plugging in USB (this board
# has no software reset-to-bootloader, so the button is required). It enumerates
# as 2e8a:0003 / the RPI-RP2 drive. Then:
sudo picotool load build_picow/picow_hub75_clock.uf2 && sudo picotool reboot
# ...or drag build_picow/picow_hub75_clock.uf2 onto the RPI-RP2 drive.
```

Once running, the Pico W is only reachable over the network unless USB is cabled
to the host (`http://192.168.0.4/`, or the serial heartbeat if connected). Note
both boards default to `STATIC_IP=192.168.0.4` from `network.txt`, so don't power
both at once without changing one.

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
