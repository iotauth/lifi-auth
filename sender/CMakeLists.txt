cmake_minimum_required(VERSION 3.13)
project(sender_project C CXX ASM)

# The Pico SDK and MBEDTLS_DIR are set by the top-level CMakeLists.txt.
# Don't import the SDK again here.

add_executable(lifi_session_sender 
  src/lifi_session_sender.c
)

pico_generate_pio_header(lifi_session_sender ${CMAKE_CURRENT_LIST_DIR}/src/lifi_multi_tx.pio)

# Make sure the app sees mbedTLS config (not the SDK default)
target_include_directories(lifi_session_sender PRIVATE
  ${CMAKE_SOURCE_DIR}            # for "config/mbedtls_config.h"
  ${MBEDTLS_DIR}/include
  ${CMAKE_SOURCE_DIR}/include
)

target_compile_definitions(lifi_session_sender PRIVATE
  MBEDTLS_CONFIG_FILE="config/mbedtls_config.h"
)

target_link_libraries(lifi_session_sender PRIVATE
  pico_stdlib
  pico_rand
  pico_platform
  sst_embedded
  heatshrink
  hardware_pio
  hardware_flash
  hardware_watchdog
  hardware_clocks
)

# stdio + binary artifacts
pico_enable_stdio_usb(lifi_session_sender 1)
pico_enable_stdio_uart(lifi_session_sender 0)
pico_add_extra_outputs(lifi_session_sender)

# --- Speed Test Sender (Pico Firmware) ---
add_executable(pico_speed_test_sender
  src/pico_speed_test_sender.c
)

# Generate PIO header
pico_generate_pio_header(pico_speed_test_sender ${CMAKE_CURRENT_LIST_DIR}/src/lifi_multi_tx.pio)

target_include_directories(pico_speed_test_sender PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(pico_speed_test_sender PRIVATE
  pico_stdlib
  hardware_pio
  hardware_clocks
)

# Enable USB for command interaction
pico_enable_stdio_usb(pico_speed_test_sender 1)
pico_enable_stdio_uart(pico_speed_test_sender 0)
pico_add_extra_outputs(pico_speed_test_sender)