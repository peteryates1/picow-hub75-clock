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
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Wi-Fi credentials are passed in at configure time, not committed.
# Override with: cmake -DWIFI_SSID="net" -DWIFI_PASSWORD="pw" ..
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)

# Select the target hardware. Default is the real clock board; pass
# -DTARGET_BOARD=control_panel to build for the alternative test board.
# Pin maps live in include/board_*.h, dispatched by include/config.h.
if (TARGET_BOARD STREQUAL "control_panel")
    target_compile_definitions(${PROJECT_NAME} PRIVATE BOARD_CONTROL_PANEL)
    message(STATUS "Target board: control_panel")
else ()
    message(STATUS "Target board: clock (default)")
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
