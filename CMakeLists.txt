cmake_minimum_required(VERSION 3.13)

# This project targets the Pico W (RP2040 + CYW43 wireless). Must be set before
# the SDK is imported so the cyw43_arch headers are made available.
if (NOT PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Target board")
endif ()

# Pull in the Pico SDK (must be before project()).
include(pico_sdk_import.cmake)

project(picow_hub75_clock C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(${PROJECT_NAME}
    src/main.c
    src/hub75.c
    src/ldr.c
    src/ntp.c
    src/weather.c
    src/mqtt.c
    src/webserver.c
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Generate the HUB75 PIO program header (hub75.pio.h, in the build dir) used by
# the -DHUB75_PIO refresh backend.
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/src/hub75.pio)

# Wi-Fi credentials: a primary network and an optional backup. From -D flags,
# else read from an untracked network.txt in the project root:
#     WIFI_SSID=PrimaryNet
#     WIFI_PASSWORD=primarypass
#     WIFI_SSID_BACKUP=BackupNet
#     WIFI_PASSWORD_BACKUP=backuppass
# (-D values take precedence; the backup pair may be omitted.)
if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/network.txt)
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/network.txt _net_lines)
    foreach (_line IN LISTS _net_lines)
        if (_line MATCHES "^WIFI_SSID=(.*)" AND NOT DEFINED WIFI_SSID)
            set(WIFI_SSID "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^WIFI_PASSWORD=(.*)" AND NOT DEFINED WIFI_PASSWORD)
            set(WIFI_PASSWORD "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^WIFI_SSID_BACKUP=(.*)" AND NOT DEFINED WIFI_SSID_BACKUP)
            set(WIFI_SSID_BACKUP "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^WIFI_PASSWORD_BACKUP=(.*)" AND NOT DEFINED WIFI_PASSWORD_BACKUP)
            set(WIFI_PASSWORD_BACKUP "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^STATIC_IP=(.*)" AND NOT DEFINED STATIC_IP)
            set(STATIC_IP "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^GATEWAY=(.*)" AND NOT DEFINED GATEWAY)
            set(GATEWAY "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^NETMASK=(.*)" AND NOT DEFINED NETMASK)
            set(NETMASK "${CMAKE_MATCH_1}")
        endif ()
    endforeach ()
    message(STATUS "Wi-Fi: primary '${WIFI_SSID}', backup '${WIFI_SSID_BACKUP}'")
endif ()

# Optional static IP (else DHCP). NETMASK defaults to a /24.
if (STATIC_IP)
    if (NOT NETMASK)
        set(NETMASK "255.255.255.0")
    endif ()
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        STATIC_IP=\"${STATIC_IP}\"
        STATIC_GW=\"${GATEWAY}\"
        STATIC_MASK=\"${NETMASK}\")
    message(STATUS "Static IP: ${STATIC_IP} gw ${GATEWAY} mask ${NETMASK}")
endif ()

if (NOT WIFI_SSID AND NOT WIFI_SSID_BACKUP)
    message(WARNING "No Wi-Fi SSID set: create network.txt or pass -DWIFI_SSID/-DWIFI_PASSWORD")
endif ()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    WIFI_SSID_BACKUP=\"${WIFI_SSID_BACKUP}\"
    WIFI_PASSWORD_BACKUP=\"${WIFI_PASSWORD_BACKUP}\"
)

# Timezone. Either a fixed offset (-DTZ_OFFSET_SECONDS=3600), or automatic UK
# BST/GMT handling (-DTZ_DST_UK=ON), which derives the offset from UTC and needs
# no fixed offset.
if (TZ_DST_UK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TZ_DST_UK)
    message(STATUS "Timezone: UK auto BST/GMT")
elseif (DEFINED TZ_OFFSET_SECONDS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TZ_OFFSET_SECONDS=${TZ_OFFSET_SECONDS})
    message(STATUS "TZ_OFFSET_SECONDS: ${TZ_OFFSET_SECONDS}")
endif ()

# Outside-temperature location for Open-Meteo (coordinates).
# e.g. -DWEATHER_LAT=52.52 -DWEATHER_LON=-1.00
if (DEFINED WEATHER_LAT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEATHER_LAT=\"${WEATHER_LAT}\")
    message(STATUS "Weather lat: ${WEATHER_LAT}")
endif ()
if (DEFINED WEATHER_LON)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEATHER_LON=\"${WEATHER_LON}\")
    message(STATUS "Weather lon: ${WEATHER_LON}")
endif ()

# Select the target hardware. Default is the real clock board; pass
# -DTARGET_BOARD=control_panel to build for the alternative test board.
# Pin maps live in include/board_*.h, dispatched by include/config.h.
if (TARGET_BOARD STREQUAL "control_panel")
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOARD_CONTROL_PANEL)
    message(STATUS "Target board: control_panel")
else ()
    message(STATUS "Target board: clock (default)")
endif ()

# Panels with green/blue swapped vs standard HUB75 (e.g. Adafruit P2.5 64x32):
# -DPANEL_SWAP_GB=ON compensates in software.
if (PANEL_SWAP_GB)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PANEL_SWAP_GB)
    message(STATUS "PANEL_SWAP_GB: ON (green/blue swapped)")
endif ()

# HUB75 refresh backend. Default is the bit-banged core1 loop. -DHUB75_PIO=ON
# uses the PIO+DMA RGB shifter: core1 streams pre-packed bit-planes to a PIO
# state machine that clocks the pixels out, so the per-pixel clocking no longer
# burns CPU and the refresh rate is decoupled from the system clock.
if (HUB75_PIO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HUB75_PIO)
    message(STATUS "HUB75_PIO: ON (PIO+DMA shifter)")
endif ()

# Icon demo: -DICON_DEMO=ON cycles all weather icons (2s each) and shows the day
# as WEDNESDAY, to check the icons and the longest-day gap.
if (ICON_DEMO)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ICON_DEMO)
    message(STATUS "ICON_DEMO: ON")
endif ()

# Bring-up diagnostic: -DDISPLAY_TEST_PATTERN=ON forces a full-screen HUB75
# test pattern at full brightness and ignores Wi-Fi/NTP/clock.
if (DISPLAY_TEST_PATTERN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DISPLAY_TEST_PATTERN)
    message(STATUS "DISPLAY_TEST_PATTERN: ON")
endif ()

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
    hardware_adc
    hardware_gpio
    hardware_pio
    hardware_dma
    pico_aon_timer
)

# USB serial for logs; disable UART.
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

# Generate .uf2, .hex, .bin, .map.
pico_add_extra_outputs(${PROJECT_NAME})
