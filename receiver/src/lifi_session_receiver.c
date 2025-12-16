#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// Project headers
#include "c_api.h"
#include "config_handler.h"   // change_directory_to_config_path, get_config_path
#include "key_exchange.h"
#include "protocol.h"         // protocol constants & sizes (NONCE_SIZE, etc.)
#include "serial_linux.h"     // UART_DEVICE, UART_BAUDRATE_TERMIOS, init_serial()
#include "sst_crypto_embedded.h"
#include "utils.h"

#define PREAMBLE_BYTE_1   0xAB
#define PREAMBLE_BYTE_2   0xCD
#define MSG_TYPE_KEY_ID   0x03
#define SESSION_KEY_ID_SIZE 8

// If SST_KEY_SIZE isn't already defined, it's usually 16 for AES-128.
// But we don't actually need it in this stripped-down version.

// ---------- Helpers ----------

// Just in case you don't already have this in utils/serial_linux:
static ssize_t read_exact_local(int fd, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}


void print_session_key_details(session_key_t *key) {
    if (!key) {
        printf("Error: Session Key is NULL.\n");
        return;
    }
    printf("=== Session Key Details ===\n");
    printf("Key ID: ");
    for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
        printf("%02X", key->key_id[i]);
    }
    printf("\n");
    print_hex("Cipher Key: ", key->cipher_key, key->cipher_key_size);
    print_hex("MAC Key:    ", key->mac_key, key->mac_key_size);
    printf("===========================\n");
}

// ---------- Main ----------

int main(int argc, char *argv[]) {
    const char *config_path = NULL;

    if (argc > 2) {
        fprintf(stderr, "Error: Too many arguments.\n");
        fprintf(stderr, "Usage: %s [<path/to/lifi_receiver.config>]\n", argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    }

    // Resolve / chdir and pick the config filename
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);
    printf("Using config file: %s\n", config_path);

    // --- 1) Initialize SST and fetch a session key from Auth ---
    printf("Initializing SST Context & Fetching Session Key...\n");
    SST_ctx_t *sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "SST init failed.\n");
        return 1;
    }

    session_key_list_t *key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "Failed to get session key.\n");
        free_SST_ctx_t(sst);
        return 1;
    }

    session_key_t s_key = key_list->s_key[0];  // Use first key from Auth
    print_session_key_details(&s_key);

    // If you want to force later lookup-by-ID to hit the Auth server,
    // create an *empty* list here:
    session_key_list_t *s_key_list = init_empty_session_key_list();

    // --- 2) Open UART to LiFi receiver path ---
    printf("Opening UART device: %s @ %d\n", UART_DEVICE, UART_BAUDRATE_TERMIOS);
    int fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
    if (fd < 0) {
        fprintf(stderr, "Failed to open UART.\n");
        free_session_key_list_t(key_list);
        free_SST_ctx_t(sst);
        return 1;
    }

    tcflush(fd, TCIFLUSH);
    printf("Listening for LiFi messages (expecting key ID packets)...\n");
    printf("Frame format: 0x%02X 0x%02X 0x%02X <8-byte KeyID>\n",
           PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, MSG_TYPE_KEY_ID);

    uint8_t byte = 0;
    int uart_state = 0;

    while (1) {
        ssize_t n = read(fd, &byte, 1);
        if (n == 1) {
            switch (uart_state) {
                case 0:  // Waiting for first preamble byte
                    if (byte == PREAMBLE_BYTE_1) {
                        uart_state = 1;
                    }
                    break;

                case 1:  // Got first preamble; expect second
                    if (byte == PREAMBLE_BYTE_2) {
                        uart_state = 2;
                    } else {
                        uart_state = 0;  // reset state
                    }
                    break;

                case 2:  // Expect message type
                    if (byte == MSG_TYPE_KEY_ID) {
                        // ... (Existing Key ID logic) ...
                         printf("\n[LiFi] Detected MSG_TYPE_KEY_ID. Reading Key ID...\n");

                        uint8_t received_key_id[SESSION_KEY_ID_SIZE];
                        ssize_t got = read_exact_local(fd, received_key_id, SESSION_KEY_ID_SIZE);

                        if (got == SESSION_KEY_ID_SIZE) {
                            printf("[LiFi] Received Key ID: ");
                            for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                                printf("%02X", received_key_id[i]);
                            }
                            printf("\n");
                            // ... (Auth lookup logic can remain or be removed if not focused on this)
                        }
                        uart_state = 0;
                    } 
                    else if (byte == MSG_TYPE_RESPONSE) {
                        printf("\n[LiFi] Detected MSG_TYPE_RESPONSE. Reading HMAC...\n");
                        uint8_t received_hmac[HMAC_SIZE];
                        ssize_t got = read_exact_local(fd, received_hmac, HMAC_SIZE);
                        
                        if (got == HMAC_SIZE) {
                            printf("[LiFi] Received HMAC: ");
                            print_hex("", received_hmac, HMAC_SIZE); // Reusing/Assuming print_hex exists or using loop
                            
                            // Verify
                            if (memcmp(received_hmac, expected_hmac, HMAC_SIZE) == 0) {
                                printf("\n*** AUTHENTICATION SUCCESSFUL: PICO VERIFIED ***\n\n");
                            } else {
                                printf("\n!!! AUTHENTICATION FAILED: HMAC MISMATCH !!!\n\n");
                                printf("Expected: ");
                                for(int i=0; i<HMAC_SIZE; i++) printf("%02X ", expected_hmac[i]);
                                printf("\n");
                            }
                        } else {
                            printf("Error reading HMAC.\n");
                        }
                        uart_state = 0;
                    }
                    else {
                        // Not a known message type
                        uart_state = 0;
                    }
                    break;

                default:
                    uart_state = 0;
            }
        } else if (n < 0 && errno != EINTR) {
            perror("UART read error");
            break;  // break out and clean up if needed
        }
    }

    close(fd);
    free_session_key_list_t(key_list);
    free_session_key_list_t(s_key_list);
    free_SST_ctx_t(sst);
    return 0;
}
