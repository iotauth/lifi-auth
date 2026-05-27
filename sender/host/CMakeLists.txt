cmake_minimum_required(VERSION 3.13)
project(host_tools C)

find_package(OpenSSL REQUIRED)

set(SST_C_API_DIR "${CMAKE_SOURCE_DIR}/deps/sst-c-api")

add_executable(pico_provisioner
    ${CMAKE_CURRENT_SOURCE_DIR}/pico_provisioner.c
    ${SST_C_API_DIR}/c_api.c
    ${SST_C_API_DIR}/c_common.c
    ${SST_C_API_DIR}/c_crypto.c
    ${SST_C_API_DIR}/c_secure_comm.c
    ${SST_C_API_DIR}/load_config.c
    ${SST_C_API_DIR}/ipfs.c
)

target_include_directories(pico_provisioner PRIVATE
    ${SST_C_API_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(pico_provisioner PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
    pthread
)

set_property(TARGET pico_provisioner PROPERTY C_STANDARD 11)
target_compile_options(pico_provisioner PRIVATE -Wall -Wextra -Wno-unused-parameter)
