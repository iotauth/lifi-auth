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
#include "serial_linux.h"
#include "sst_crypto_embedded.h"  
#include "utils.h"

#define SESSION_KEY_ID_SIZE 8

// Function to print session key details nicely
void print_session_key_details(session_key_t *key) {
    if (!key) {
        printf("Error: Session Key is NULL.\n");
        return;
    }
    printf("=== Session Key Details ===\n");
//check later about exact key id size to be used here
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
        fprintf(stderr, "Usage: %s [<path/to/lifi_sender.config>]\n",
                argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    } else {
        // Default
        config_path = "lifi_sender.config";
    }

    // Resolve / chdir and pick the config filename
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);
    printf("Using config file: %s\n", config_path);

    // Initialize SST Context and get new key
    printf("Initializing SST Context & Fetching Session Key for SENDER...\n");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "SST init failed.\n");
        return 1;
    }
    
    // Get Session Key from Auth
    session_key_list_t* key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "Failed to get session key from Auth.\n");
        free_SST_ctx_t(sst);
        return 1;
    }
    session_key_t s_key = key_list->s_key[0]; // Use first key
    
    printf("Got Session Key from Auth!\n");
    print_session_key_details(&s_key);

    // --- Serial setup ---
    // Connect to Pico via UART
    // Host on Laptop typically sees Pico as /dev/ttyACM0
    const char *uart_dev = "/dev/ttyACM0"; 
    printf("Connecting to Pico via UART (%s)...\n", uart_dev);
    int fd = init_serial(uart_dev, UART_BAUDRATE_TERMIOS);
    if (fd < 0) {
        fprintf(stderr, "Failed to open serial port. Is Pico connected?\n");
        free_SST_ctx_t(sst);
        return 1;
    }
    
    // --- Provisioning: Send Key ID + Key to Pico ---
    printf("Provisioning Pico with Session Key (ID + Key)...\n");
    const uint8_t preamble[2] = {0xAB, 0xCD};
    
    // 1. Send Preamble
    if (write_all(fd, preamble, 2) < 0) perror("write preamble");
    
    // 2. Send ID (8 bytes)
    if (write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0) perror("write key id");
    
    // 3. Send Cipher Key (16 bytes)
    // Note: s_key.cipher_key_size MUST match what receiving side expects (SST_KEY_SIZE = 16)
    if (write_all(fd, s_key.cipher_key, 16) < 0) perror("write cipher key");
    
    tcdrain(fd);
    printf("Provisioning Data Sent.\n");
    printf("Pico should now have the key.\n");
    
    // --- Interactive Command Loop ---
    printf("\n=== Interactive Mode ===\n");
    printf("Type 'CMD: send key id' (or just 's') to trigger LiFi transmission.\n");
    printf("Press Ctrl+C to exit.\n");

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Raw mode for immediate keypress
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    uint8_t rx_buf[256];
    
    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(fd, &fds);

        // Wait for input
        int max_fd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;
        select(max_fd + 1, &fds, NULL, NULL, NULL);

        // Check Serial Input (from Pico)
        if (FD_ISSET(fd, &fds)) {
            int n = read(fd, rx_buf, sizeof(rx_buf));
            if (n > 0) {
                write(STDOUT_FILENO, rx_buf, n);
            } else if (n < 0 && errno != EAGAIN) {
                perror("Read Serial");
                break;
            }
        }

        // Check Keyboard Input (from User)
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 's') { // Shortcut for the main command
                    const char* cmd = "CMD: send key id\n";
                    write_all(fd, cmd, strlen(cmd));
                    printf("\n[Sent 'CMD: send key id']\n");
                } 
                else if (c == 3) { // Ctrl+C
                    break;
                }
                else {
                    // Pass other chars through (e.g. typing manual commands)
                    write(fd, &c, 1);
                    // Local echo
                    write(STDOUT_FILENO, &c, 1);
                }
            }
        }
    }
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore terminal
    close(fd);
    free_session_key_list_t(key_list);
    free_SST_ctx_t(sst);
    return 0;
}
