cmake_minimum_required(VERSION 3.13)
project(receiver_uart_project C)

# Host build needs OpenSSL
find_package(OpenSSL REQUIRED)

# ---- Common helpers (utils, serial, replay, config handler) ----
add_library(receiver_common
  ${CMAKE_CURRENT_SOURCE_DIR}/src/utils.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/serial_linux.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/replay_window.c
  # ${CMAKE_CURRENT_SOURCE_DIR}/src/key_exchange.c  # enable when needed
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/config_handler.c
)

target_include_directories(receiver_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include       # receiver/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../include    # project include (protocol.h, etc.)
)

# ---- sst-c-api (submodule under deps/) ----
set(SST_C_API_DIR "${CMAKE_SOURCE_DIR}/deps/sst-c-api")

add_executable(lifi_session_receiver
  ${CMAKE_CURRENT_SOURCE_DIR}/src/lifi_session_receiver.c
  ${SST_C_API_DIR}/c_api.c
  ${SST_C_API_DIR}/c_common.c
  ${SST_C_API_DIR}/c_crypto.c
  ${SST_C_API_DIR}/c_secure_comm.c
  ${SST_C_API_DIR}/load_config.c
  ${SST_C_API_DIR}/ipfs.c
)

target_include_directories(lifi_session_receiver PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/include
  ${SST_C_API_DIR}
  ${MBEDTLS_DIR}/include
)
target_link_libraries(lifi_session_receiver PRIVATE
  receiver_common
  sst_embedded         # <-- provides sst_decrypt_gcm
  mbedcrypto           # <-- mbedTLS crypto objs
  OpenSSL::Crypto
)

# C standard & warnings
set_property(TARGET receiver_common lifi_session_receiver PROPERTY C_STANDARD 11)
target_compile_options(lifi_session_receiver PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Use an absolute default config path
target_compile_definitions(lifi_session_receiver PRIVATE
  DEFAULT_SST_CONFIG_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/lifi_receiver.config\"
)
