cmake_minimum_required(VERSION 3.13)
project(sender_host_project C)

# --- Host-side deps ---
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---- Common helpers: utils, serial, replay_window, config handler ----
add_library(sender_common
  ${CMAKE_CURRENT_SOURCE_DIR}/src/utils.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/serial_linux.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/replay_window.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../../src/config_handler.c
)

target_include_directories(sender_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# ---- sst-c-api source tree ----
set(SST_C_API_DIR "${CMAKE_SOURCE_DIR}/deps/sst-c-api")

# =========================
# Sender Host Executable
# =========================
add_executable(sender_host
  ${CMAKE_CURRENT_SOURCE_DIR}/src/sender_host_linux.c
  ${SST_C_API_DIR}/c_api.c
  ${SST_C_API_DIR}/c_common.c
  ${SST_C_API_DIR}/c_crypto.c
  ${SST_C_API_DIR}/c_secure_comm.c
  ${SST_C_API_DIR}/load_config.c
  ${SST_C_API_DIR}/ipfs.c
)

target_include_directories(sender_host PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include
  ${SST_C_API_DIR}
  ${MBEDTLS_DIR}/include
)

target_link_libraries(sender_host PRIVATE
  sender_common
  sst_embedded
  mbedcrypto
  OpenSSL::SSL
  OpenSSL::Crypto
  CURL::libcurl
  Threads::Threads
)

set_property(TARGET sender_host PROPERTY C_STANDARD 11)
target_compile_options(sender_host PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Default config for Auth
target_compile_definitions(sender_host PRIVATE
  DEFAULT_SST_CONFIG_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/../lifi_sender.config\"
)
