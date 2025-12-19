#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

// Project headers
#include "c_api.h"
#include "config_handler.h"
#include "key_exchange.h"
#include "../../include/protocol.h" 
#include "serial_linux.h"
#include "utils.h"

// Hardcoded device similar to receiver_flash.c
#ifndef UART_DEVICE
#define UART_DEVICE "/dev/ttyAMA0"
#endif

#ifndef UART_BAUDRATE_TERMIOS
#define UART_BAUDRATE_TERMIOS B1000000
#endif

// write_exact helper
static int write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    return (sent == len) ? 0 : -1;
}

int main(int argc, char* argv[]) {
    const char* config_path = NULL;

    printf("--- passKeyToPico Tool ---\n");

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [<path/to/lifi_receiver.config>]\n", argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    }

    // Resolve config path
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);
    printf("Config: %s\n", config_path);

    // --- Init SST ---
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "Error: SST init failed.\n");
        return 1;
    }

    // --- Get Session Key ---
    printf("Fetching session key from SST...\n");
    session_key_list_t* key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "Error: No session keys found in SST.\n");
        free_SST_ctx_t(sst);
        return 1;
    }

    session_key_t s_key = key_list->s_key[0];
    printf("Target Key ID: ");
    for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", s_key.key_id[i]);
    printf("\n");

    printf("Target Session Key: ");
    for(int i=0; i<SESSION_KEY_SIZE; i++) printf("%02X", s_key.cipher_key[i]);
    printf("\n");

    // --- Open Serial ---
    printf("Opening serial %s @ 1Mbps...\n", UART_DEVICE);
    int fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
    if (fd < 0) {
        fprintf(stderr, "Error: Failed to open serial port.\n");
        free_session_key_list_t(key_list);
        free_SST_ctx_t(sst);
        return 1;
    }

    // --- Construct Packet ---
    // Frame: [PREAMBLE:4][TYPE:1][LEN:2][KEY_ID:8][KEY:16]
    uint16_t key_payload_len = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE;
    uint8_t header[] = {
        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
        MSG_TYPE_KEY,
        (key_payload_len >> 8) & 0xFF,
        key_payload_len & 0xFF
    };

    printf("Sending key packet to Pico...\n");
    
    if (write_all(fd, header, sizeof(header)) < 0 ||
        write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
        write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
        fprintf(stderr, "Error: Write failed.\n");
        close(fd);
        free_session_key_list_t(key_list);
        free_SST_ctx_t(sst);
        return 1;
    }

    // Flush/Drain
    tcdrain(fd);
    printf("✓ Success! Key sent to Pico.\n");
    printf("The Pico should now be ready to communicate with this key.\n");

    // Cleanup
    close(fd);
    free_session_key_list_t(key_list);
    free_SST_ctx_t(sst);

    return 0;
}
