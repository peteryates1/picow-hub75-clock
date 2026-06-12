# picow-hub75-clock

A network clock for the Raspberry Pi Pico W that drives a 64×32 HUB75 RGB LED
matrix. Time is set over Wi-Fi via NTP at boot and re-synced periodically; a
GL5516 light-dependent resistor (LDR) on ADC0 dims the display automatically so
it's bright by day and dim at night.

## Hardware

Two boards are supported, selected at build time. GPIO maps come straight from
each board's EasyEDA netlist and live in `include/board_*.h`:

- **`clock`** (default) — the real clock PCB `clock-picow-hub75-ldr`, with LDR
  auto-brightness and the NTP-resync button. Pin map below.
- **`control_panel`** — an alternative board used for initial HUB75 bring-up.
  Same HUB75 IDC connector, **different Pico GPIOs**, and **no LDR or button**
  (it runs at a fixed brightness). Build with `-DTARGET_BOARD=control_panel`.

GPIO assignments for the **clock** board:

| Function | GPIO | Notes |
|----------|------|-------|
| HUB75 R1/G1/B1 | GP13 / GP14 / GP12 | upper half RGB |
| HUB75 R2/G2/B2 | GP11 / GP15 / GP10 | lower half RGB |
| HUB75 A/B/C/D/E | GP9 / GP3 / GP8 / GP4 / GP2 | row address (E unused at 1/16 scan) |
| HUB75 CLK / LAT / OE | GP7 / GP5 / GP6 | OE active-low |
| LDR | GP26 / ADC0 | GL5516 divider, brighter ⇒ higher reading |
| Button (SW1) | GP16 | active-low, internal pull-up; press forces NTP re-sync |

Power: 5 V via a DSN-MINI-360 buck from the DC jack, OR-ed with USB VBUS
through a DMG2305 P-MOSFET.

## Building

Requires the Pico SDK (2.2.0) and the ARM GNU toolchain.

Put your Wi-Fi credentials in an untracked `network.txt` in the project root:

```
WIFI_SSID=yourssid
WIFI_PASSWORD=yourpassword
```

```sh
export PICO_SDK_PATH=/home/peter/pico-sdk
cmake -B build
cmake --build build
# For the control-panel test board, add: -DTARGET_BOARD=control_panel
```

`network.txt` is gitignored. Credentials can also be passed with `-DWIFI_SSID=`
/ `-DWIFI_PASSWORD=`, which override the file.

Flash `build/picow_hub75_clock.uf2` by holding BOOTSEL while plugging in the
Pico W, then copying the file to the `RPI-RP2` mass-storage device. Serial logs
appear on USB CDC.

Timezone: build with `-DTZ_DST_UK=ON` for automatic UK BST/GMT switching, or
`-DTZ_OFFSET_SECONDS=3600` for a fixed offset (no DST).

### Bench testing on a Pico 2 W via debug probe

The original Pico W (RP2040) and the Pico 2 W (RP2350) are **different
architectures**; build for whichever chip is connected. To test on a Pico 2 W
over a Raspberry Pi Debug Probe:

```sh
cmake -B build_pico2 -DPICO_BOARD=pico2_w
cmake --build build_pico2
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 1000" \
  -c "program build_pico2/picow_hub75_clock.elf verify reset exit"
```

Then watch the target's USB-CDC serial (`/dev/ttyACM*`, vendor `2e8a`) for the
`[hb] wifi_link=… time=…` heartbeat. See [CLAUDE.md](CLAUDE.md) for details
(SWD target configs, permissions, why `pico_aon_timer` is used instead of the
RP2040 RTC).

## Architecture

See [CLAUDE.md](CLAUDE.md) for the dual-core design and module layout.
