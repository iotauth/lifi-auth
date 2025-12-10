#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>  // Linux serial
#include <time.h>
#include <unistd.h>

// Project headers
#include "c_api.h"
#include "config_handler.h"  // change_directory_to_config_path, get_config_path
#include "key_exchange.h"
#include "protocol.h"  // protocol constants & sizes
#include "replay_window.h"
#include "serial_linux.h"
#include "sst_crypto_embedded.h"  // brings in sst_decrypt_gcm prototype and sizes
#include "utils.h"

#define MSG_TYPE_KEY_ID 0x03
#define SESSION_KEY_ID_SIZE 8
#define SST_KEY_SIZE 16 // Explicitly defined if not in headers, likely is.

// Function to print session key details nicely
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

static int write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;  // interrupted -> retry
            return -1;                     // real error
        }
        if (n == 0) break;  // shouldn't happen on tty, treat as error
        sent += (size_t)n;
    }
    return (sent == len) ? 0 : -1;
}

int main(int argc, char* argv[]) {
    const char* config_path = NULL;

    if (argc > 2) {
        fprintf(stderr, "Error: Too many arguments.\n");
        fprintf(stderr, "Usage: %s [<path/to/lifi_receiver.config>]\n",
                argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    }

    // Resolve / chdir and pick the config filename
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);
    printf("Using config file: %s\n", config_path);

    // Initialize SST Context and get new key
    printf("Initializing SST Context & Fetching Session Key...\n");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "SST init failed.\n");
        return 1;
    }
    
    // Get Session Key from Auth (or load from cache)
    session_key_list_t* key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "Failed to get session key.\n");
        free_SST_ctx_t(sst);
        return 1;
    }
    session_key_t s_key = key_list->s_key[0]; // Use first key
    
    print_session_key_details(&s_key);

    // Create an empty session key list for later retrieval by ID
    session_key_list_t* s_key_list = init_empty_session_key_list();

    // --- Serial setup ---
    int fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
    if (fd < 0) {
        free_SST_ctx_t(sst);
        return 1;
    }
    
    // --- Provisioning: Send Key ID + Key to Pico ---
    // User requested "Receive session key on sender (pico) now"
    // We send it via UART to Pico which should be listening (CMD: new key or on boot)
    printf("Provisioning Pico with Session Key (ID + Key)...\n");
    const uint8_t preamble[2] = {0xAB, 0xCD};
    
    write_all(fd, preamble, 2);
    // Send ID (8 bytes)
    write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE);
    // Send Cipher Key (16 bytes)
    write_all(fd, s_key.cipher_key, s_key.cipher_key_size); // Assuming size matches SST_KEY_SIZE (16)
    
    tcdrain(fd);
    printf("Provisioning Complete.\n");

    printf("Listening for LiFi messages (Expecting Session Key ID)...\n");
    tcflush(fd, TCIFLUSH);

    // UART framing state
    uint8_t byte = 0;
    int uart_state = 0;
    
    replay_window_t rwin;
    replay_window_init(&rwin, NONCE_SIZE, NONCE_HISTORY_SIZE);

    while (1) {
        if (read(fd, &byte, 1) == 1) {
            switch (uart_state) {
                case 0:
                    if (byte == PREAMBLE_BYTE_1) {
                        uart_state = 1;
                    } 
                    break;
                case 1:
                    if (byte == PREAMBLE_BYTE_2) {
                        uart_state = 2;
                    } else {
                        uart_state = 0;
                    }
                    break;

                case 2:
                    // Check Message Type
                    if (byte == MSG_TYPE_KEY_ID) {
                        printf("Detected MSG_TYPE_KEY_ID. Reading Key ID...\n");
                        
                        uint8_t received_key_id[SESSION_KEY_ID_SIZE];
                        // blocking read/timeout handled by simple read loop for now
                        // Linux serial defaults might need adjustment for strict timeouts, 
                        // but logic remains simple: read_exact calls read loop
                        if (read_exact(fd, received_key_id, SESSION_KEY_ID_SIZE) == SESSION_KEY_ID_SIZE) {
                            printf("Received Key ID: ");
                            for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", received_key_id[i]);
                            printf("\n");
                            
                            printf("Requesting Session Key from Auth using this ID...\n");
                            // IMPORTANT: We need a fresh s_key_list or clear it if we want to ensure fetch from Auth?
                            // get_session_key_by_ID checks existing_s_key_list first.
                            // If we already have it (implied by provisioning), it returns it immediately.
                            // To prove we can fetch it, checking if it works is enough.
                            // The user wants: "Receiver will then request the session key from Auth (using the given session key ID) and print it".
                            
                            // If we passed `s_key_list` which is EMPTY, it will ask Auth.
                            // If we passed `key_list` (from get_session_key), it would find it locally.
                            // We use `s_key_list` (EMPTY) to force Auth request/check.
                            
                            session_key_t *retrieved_key = get_session_key_by_ID(
                                received_key_id,
                                sst,
                                s_key_list 
                            );
                            
                            if (retrieved_key) {
                                printf("SUCCESS: Retrieved Session Key from Auth!\n");
                                print_session_key_details(retrieved_key);
                            } else {
                                printf("FAILURE: Could not retrieve session key for this ID.\n");
                            }

                        } else {
                            printf("Timeout or error reading Key ID payload.\n");
                        }
                        uart_state = 0;
                    } 
                    else if (byte == MSG_TYPE_ENCRYPTED) {
                        // Handle standard encrypted messages
                        // Just consume logic similar to receiver_flash.c if needed
                        // For this task, we can ignore or print notification
                        uart_state = 0; 
                    }
                    else {
                        // printf("Unknown Message Type: 0x%02X\n", byte);
                        uart_state = 0;
                    }
                    break;
                default:
                    uart_state = 0;
            }
        }
    }

    close(fd);
    free_session_key_list_t(key_list);
    free_session_key_list_t(s_key_list);
    free_SST_ctx_t(sst);
    return 0;
}
