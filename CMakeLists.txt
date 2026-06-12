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
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Wi-Fi credentials. Provided either on the command line
# (-DWIFI_SSID=... -DWIFI_PASSWORD=...) or, if not given, read from an
# untracked network.txt in the project root with the form:
#     WIFI_SSID=MyNetwork
#     WIFI_PASSWORD=mypassword
if ((NOT WIFI_SSID) AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/network.txt)
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/network.txt _net_lines)
    foreach (_line IN LISTS _net_lines)
        if (_line MATCHES "^WIFI_SSID=(.*)")
            set(WIFI_SSID "${CMAKE_MATCH_1}")
        elseif (_line MATCHES "^WIFI_PASSWORD=(.*)")
            set(WIFI_PASSWORD "${CMAKE_MATCH_1}")
        endif ()
    endforeach ()
    message(STATUS "Wi-Fi credentials loaded from network.txt (SSID: ${WIFI_SSID})")
endif ()

if (NOT WIFI_SSID)
    message(WARNING "No WIFI_SSID set: create network.txt or pass -DWIFI_SSID/-DWIFI_PASSWORD")
endif ()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
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

# Outside-temperature location for wttr.in. e.g. -DWEATHER_LOCATION=Fleckney
if (DEFINED WEATHER_LOCATION)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WEATHER_LOCATION=\"${WEATHER_LOCATION}\")
    message(STATUS "Weather location: ${WEATHER_LOCATION}")
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
    hardware_adc
    hardware_gpio
    pico_aon_timer
)

# USB serial for logs; disable UART.
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)

# Generate .uf2, .hex, .bin, .map.
pico_add_extra_outputs(${PROJECT_NAME})
