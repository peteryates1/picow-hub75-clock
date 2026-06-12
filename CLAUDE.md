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
cmake -B build -DWIFI_SSID="ssid" -DWIFI_PASSWORD="pw"   # optionally -DTZ_OFFSET_SECONDS=3600
cmake --build build -j4
# Control-panel test board:
cmake -B build_cp -DWIFI_SSID="ssid" -DWIFI_PASSWORD="pw" -DTARGET_BOARD=control_panel
cmake --build build_cp -j4
```

- Output: `build/picow_hub75_clock.uf2`. Flash by holding BOOTSEL, plugging in,
  and copying the `.uf2` to the `RPI-RP2` drive.
- `PICO_BOARD` is forced to `pico_w` in CMakeLists.txt **before** the SDK import
  — required or the `pico/cyw43_arch.h` headers won't be on the include path.
- Wi-Fi credentials are compile-time `-D` defines, never committed. There is no
  test suite; this is bare-metal firmware — "running it" means flashing hardware.
- Logs go to **USB CDC serial** (`pico_enable_stdio_usb`), not UART.

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

### Display driver details (`hub75.c`)

- 64×32 panel is **1/16 scan**: address lines A–D select one of 16 row-pairs;
  each scan drives an upper pixel (R1/G1/B1) and a lower pixel (R2/G2/B2).
- The six RGB GPIOs are **contiguous on GP10–GP15** specifically so a column is
  written with one `gpio_put_masked` instead of six `gpio_put`s. The bit order
  within that block is scrambled by PCB routing — `pack_column()` maps colours
  to the right bit using the `RGB_BIT_*` offsets in `config.h`.
- Colour depth = `HUB75_BCM_DEPTH` (4 bits). Bit-plane *p* is lit for
  `BCM_BASE_US << p` µs, giving binary-weighted intensity. Brightness scales
  that on-time, so it dims the whole panel without redrawing.
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
  (UTC). `TZ_OFFSET_SECONDS` is added only at render time — there is **no DST
  handling**. Re-sync runs every `NTP_RESYNC_HOURS`, or on demand when **SW1
  (GP16)** is pressed — the button's only job is to force an NTP re-sync.
  It's active-low with the internal pull-up; `button_pressed()` does falling-edge
  detection debounced against the 200 ms loop tick.
- Wi-Fi or NTP failure is non-fatal: the display keeps running, the time just
  stays unset until the next successful sync.

### Rendering

`font.h` is a 5×7 bitmap font containing only digits 0–9 and `:`. `main.c`
draws `HH:MM` centred, blinking the colon at 1 Hz. The whole frame is cleared
and redrawn each 200 ms tick on core0.

## lwIP configuration

`include/lwipopts.h` configures the threadsafe-background cyw43 stack (DHCP +
DNS + UDP). Note `MEMP_NUM_TCP_SEG` must stay ≥ the computed `TCP_SND_QUEUELEN`
(currently 32) or the lwIP sanity check fails the build, even though this
project doesn't use TCP.
