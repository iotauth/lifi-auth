cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_DISABLE_MBEDTLS 1)

if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
  set(PICO_SDK_PATH $ENV{PICO_SDK_PATH} CACHE PATH "")
endif()
if(NOT DEFINED PICO_SDK_PATH)
  set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../../lib/pico-sdk" CACHE PATH "")
endif()
set(ENV{PICO_SDK_PATH} "${PICO_SDK_PATH}")

set(PICO_COMPILER pico_arm_cortex_m33_gcc CACHE STRING "")
set(PICO_PLATFORM rp2350                  CACHE STRING "")
set(PICO_BOARD    pico2                   CACHE STRING "")

include("${PICO_SDK_PATH}/pico_sdk_init.cmake")

project(lifi_pico2_receiver C CXX ASM)
pico_sdk_init()

add_executable(lifi_pico2_rx
    src/main.c
)

# Move this AFTER add_executable
pico_generate_pio_header(lifi_pico2_rx
    ${CMAKE_CURRENT_LIST_DIR}/src/lifi_rx.pio
)

target_include_directories(lifi_pico2_rx PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include
)

target_link_libraries(lifi_pico2_rx
    pico_stdlib
    hardware_pio
    hardware_clocks
)

pico_enable_stdio_usb(lifi_pico2_rx 1)
pico_enable_stdio_uart(lifi_pico2_rx 0)
pico_add_extra_outputs(lifi_pico2_rx)

# ── Raw bit monitor (hardware bring-up / TLV pin test) ──────────────────────
add_executable(raw_bit_monitor
    src/raw_bit_monitor.c
)
target_link_libraries(raw_bit_monitor
    pico_stdlib
    hardware_i2c
)
pico_enable_stdio_usb(raw_bit_monitor 1)
pico_enable_stdio_uart(raw_bit_monitor 0)
pico_add_extra_outputs(raw_bit_monitor)