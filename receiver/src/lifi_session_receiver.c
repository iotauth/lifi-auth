#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>  // Linux serial
#include <time.h>
#include <unistd.h>

// Project headers (use -I include dirs instead of ../../../)
#include "c_api.h"
#include "config_handler.h"  // change_directory_to_config_path, get_config_path
#include "key_exchange.h"
#include "../../include/protocol.h"
#include "replay_window.h"
#include "serial_linux.h"
#include "sst_crypto_embedded.h"  // brings in sst_decrypt_gcm prototype and sizes
#include "utils.h"

static inline int timespec_passed(const struct timespec* dl) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec > dl->tv_sec) ||
           (now.tv_sec == dl->tv_sec && now.tv_nsec >= dl->tv_nsec);
}

// write_exact: loop until all bytes are written (or error)
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

// Set stdin to non-blocking mode for keyboard shortcuts
static void set_nonblocking_stdin(void) {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    tty.c_cc[VMIN] = 0;   // Non-blocking read
    tty.c_cc[VTIME] = 0;  // No timeout
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

// Restore stdin to normal mode on exit
static void restore_stdin(void) {
    struct termios tty;
    tcgetattr(STDIN_FILENO, &tty);
    tty.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &tty);
}

// Check if a key was pressed (non-blocking)
static int get_keypress(void) {
    char ch;
    if (read(STDIN_FILENO, &ch, 1) == 1) {
        return ch;
    }
    return -1;  // No key pressed
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

    // Resolve / chdir and pick the config filename (host-only; Pico stub is
    // no-op)
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);
    printf("Using config file: %s\n", config_path);

    // --- Fetch session key from SST ---
    printf("Retrieving session key from SST...\n");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "SST init failed.\n");
        return 1;
    }

    session_key_list_t* key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "No session key.\n");
        return 1;
    }

    session_key_t s_key = key_list->s_key[0];
    print_hex("Session Key: ", s_key.cipher_key, SESSION_KEY_SIZE);

    bool key_valid = true;                        // track if current key usable
    uint8_t pending_key[SESSION_KEY_SIZE] = {0};  // for rotations

    // --- Receiver state + replay window ---
    receiver_state_t state = STATE_IDLE;
    struct timespec state_deadline = (struct timespec){0, 0};
    time_t last_key_req_time = 0;

    // nonce setup
    replay_window_t rwin;
    replay_window_init(&rwin, NONCE_SIZE, NONCE_HISTORY_SIZE);

    // Challenge tracking for HMAC verification
    uint8_t pending_challenge[CHALLENGE_SIZE] = {0};
    bool challenge_active = false;

    // --- Serial setup ---
    int fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);  // termios for
                                                               // pi4
    if (fd < 0) return 1;

    // Initial key push retry machinery
    struct timespec next_send = {0};
    clock_gettime(CLOCK_MONOTONIC, &next_send);  // send immediately

    // Build key provisioning frame: [PREAMBLE:4][TYPE:1][LEN:2][KEY_ID:8][KEY:32]
    // Length = KEY_ID_SIZE + SESSION_KEY_SIZE
    uint16_t key_payload_len = SST_KEY_ID_SIZE + SESSION_KEY_SIZE;
    uint8_t key_header[] = {
        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
        MSG_TYPE_KEY,
        (key_payload_len >> 8) & 0xFF,
        key_payload_len & 0xFF
    };
    
    // Send key provisioning frame
    if (write_all(fd, key_header, sizeof(key_header)) < 0) {
        perror("write key header");
    }
    if (write_all(fd, s_key.key_id, SST_KEY_ID_SIZE) < 0) {
        perror("write key id");
    }
    if (write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
        perror("write session key");
    }
    tcdrain(fd);  // ensure bytes actually leave the UART
    printf("Sent session key over UART (4-byte preamble + KEY_ID + KEY).\n");

    // Setup non-blocking keyboard input for shortcuts
    set_nonblocking_stdin();
    atexit(restore_stdin);  // Restore terminal on exit

    printf("\n");
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║          LiFi Receiver - Keyboard Shortcuts           ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║  [1] Send Session Key to Pico                         ║\n");
    printf("║  [2] Send HMAC Challenge (verify key)                 ║\n");
    printf("║  [q] Quit                                              ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // UART framing state
    uint8_t byte = 0;
    int uart_state = 0;

    printf("Listening for encrypted message...\n");
    tcflush(fd, TCIFLUSH);

    while (1) {
        // --- Handle Keyboard Shortcuts ---
        int key = get_keypress();
        if (key != -1) {
            switch (key) {
                case '1': {
                    printf("\n[Shortcut] Sending session key to Pico...\n");
                    // Build key provisioning frame with 4-byte preamble
                    uint16_t klen = SST_KEY_ID_SIZE + SESSION_KEY_SIZE;
                    uint8_t hdr[] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_KEY,
                        (klen >> 8) & 0xFF,
                        klen & 0xFF
                    };
                    if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                        write_all(fd, s_key.key_id, SST_KEY_ID_SIZE) < 0 ||
                        write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
                        printf("Error: Failed to send session key.\n");
                    } else {
                        tcdrain(fd);
                        printf("✓ Session key sent.\n");
                    }
                    break;
                }
                
                case '2':
                    printf("\n[Shortcut] Initiating HMAC challenge...\n");
                    if (rand_bytes(pending_challenge, CHALLENGE_SIZE) != 0) {
                        printf("Error: Failed to generate challenge nonce.\n");
                    } else {
                        // Use 4-byte preamble + type + length as expected by sender
                        uint8_t header[] = {
                            PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                            MSG_TYPE_CHALLENGE,
                            (CHALLENGE_SIZE >> 8) & 0xFF,  // Length high byte
                            CHALLENGE_SIZE & 0xFF          // Length low byte
                        };
                        if (write_all(fd, header, sizeof(header)) < 0 ||
                            write_all(fd, pending_challenge, CHALLENGE_SIZE) < 0) {
                            printf("Error: Failed to send challenge.\n");
                        } else {
                            tcdrain(fd);
                            state = STATE_WAITING_FOR_HMAC_RESP;
                            clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                            state_deadline.tv_sec += 5;
                            challenge_active = true;
                            printf("✓ Challenge sent. Waiting for HMAC response...\n");
                        }
                    }
                    break;
                
                case 'q':
                case 'Q':
                    printf("\nExiting...\n");
                    close(fd);
                    free_session_key_list_t(key_list);
                    free_SST_ctx_t(sst);
                    return 0;
                
                default:
                    // Ignore other keys
                    break;
            }
        }

        // --- Handle State Timeouts ---
        if (state != STATE_IDLE && timespec_passed(&state_deadline)) {
            if (state == STATE_WAITING_FOR_YES) {
                printf(
                    "Confirmation for 'new key' timed out. Returning to "
                    "idle.\n");
                // nothing to wipe here
            } else if (state == STATE_WAITING_FOR_ACK) {
                printf(
                    "Timeout waiting for key update ACK. Discarding new "
                    "key.\n");
                explicit_bzero(pending_key, sizeof pending_key);
                // keep old key; key_valid stays true
            } else if (state == STATE_WAITING_FOR_HMAC_RESP) {
                printf("⚠️  HMAC challenge timed out. Pico did not respond.\n");
                explicit_bzero(pending_challenge, sizeof(pending_challenge));
                challenge_active = false;
            }
            state = STATE_IDLE;
            state_deadline = (struct timespec){0, 0};
        }

        if (read(fd, &byte, 1) == 1) {
            switch (uart_state) {
                case 0:
                    if (byte == PREAMBLE_BYTE_1) {
                        uart_state = 1;
                    }
                    // Don't spam - only print if we're getting noise
                    break;
                case 1:
                    if (byte == PREAMBLE_BYTE_2) {
                        uart_state = 2;
                    } else {
                        uart_state = 0;
                    }
                    break;
                case 2:
                    if (byte == PREAMBLE_BYTE_3) {
                        uart_state = 3;
                    } else {
                        uart_state = 0;
                    }
                    break;
                case 3:
                    if (byte == PREAMBLE_BYTE_4) {
                        uart_state = 4;
                    } else {
                        uart_state = 0;
                    }
                    break;

                case 4:
                    if (byte == MSG_TYPE_ENCRYPTED) {
                        uint8_t len_bytes[2];
                        uint8_t nonce[NONCE_SIZE];

                        // Read length first, then nonce (matches sender format)
                        if (read_exact(fd, len_bytes, 2) != 2) {
                            printf("Failed to read length\n");
                            uart_state = 0;
                            continue;
                        }

                        // Length includes NONCE + CIPHERTEXT + TAG
                        uint16_t payload_len =
                            ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                        
                        // Sanity check payload
                        if (payload_len < NONCE_SIZE + TAG_SIZE || payload_len > MAX_MSG_LEN) {
                            printf("Invalid payload length: %u bytes\n", payload_len);
                            uart_state = 0;
                            continue;
                        }

                        // Calculate actual message length
                        uint16_t msg_len = payload_len - NONCE_SIZE - TAG_SIZE;

                        // Read nonce
                        if (read_exact(fd, nonce, NONCE_SIZE) != NONCE_SIZE) {
                            printf("Failed to read nonce\n");
                            uart_state = 0;
                            continue;
                        }

                        // --- Nonce Replay Check ---
                        if (replay_window_seen(&rwin, nonce)) {
                            printf("Nonce replayed! Rejecting message.\n");
                            uart_state = 0;
                            continue;
                        }
                        replay_window_add(&rwin, nonce);

                        // read ciphertext and tag
                        uint8_t ciphertext[msg_len];
                        uint8_t tag[TAG_SIZE];
                        uint8_t crc_bytes[CRC16_SIZE];
                        uint8_t decrypted[msg_len + 1];  // for null-terminator

                        ssize_t c = read_exact(fd, ciphertext, msg_len);
                        ssize_t t = read_exact(fd, tag, TAG_SIZE);
                        ssize_t crc_read = read_exact(fd, crc_bytes, CRC16_SIZE);  // Read CRC
                        
                        // TODO: validate CRC if needed (optional for now)

                        if (c == msg_len && t == TAG_SIZE) {
                            if (!key_valid) {  // Skip decryption if key was
                                               // cleared and not yet rotated
                                printf(
                                    "No valid session key. Rejecting encrypted "
                                    "message.\n");
                                uart_state = 0;
                                continue;
                            }

                            int ret = sst_decrypt_gcm(s_key.cipher_key, nonce,
                                                      ciphertext, msg_len, tag,
                                                      decrypted);

                            if (ret == 0) {  // Successful decryption
                                decrypted[msg_len] =
                                    '\0';  // Null-terminate the decrypted
                                           // message
                                printf("%s\n", decrypted);

                                // If the decrypted message is "I have the key",
                                // stop sending the key.
                                if (strcmp((char*)decrypted,
                                           "I have the key") == 0) {
                                    printf(
                                        "Pico has confirmed receiving the "
                                        "key.\n");
                                }

                                // Handle "new key" commands (if the flag for
                                // sending new key is set)
                                else if (strcmp((char*)decrypted,
                                                "new key -f") == 0) {
                                    // Logic to request a new key and forcefully
                                    // overwrite current one
                                    printf(
                                        "Received 'new key -f' command. "
                                        "Requesting new key...\n");

                                    free_session_key_list_t(key_list);
                                    key_list = get_session_key(
                                        sst, init_empty_session_key_list());

                                    if (!key_list || key_list->num_key == 0) {
                                        fprintf(stderr,
                                                "Failed to fetch new session "
                                                "key.\n");
                                    } else {
                                        memcpy(pending_key,
                                               key_list->s_key[0].cipher_key,
                                               SESSION_KEY_SIZE);
                                        print_hex(
                                            "New Session Key (pending ACK): ",
                                            pending_key, SESSION_KEY_SIZE);
                                        key_valid = true;

                                        uint8_t preamble[2] = {0xAB, 0xCD};
                                        write(fd, preamble, 2);
                                        write(fd, pending_key,
                                              SESSION_KEY_SIZE);
                                        usleep(5000);  // 5ms sleep to let
                                                       // transmission complete
                                        printf(
                                            "Sent new session key to Pico. "
                                            "Waiting 5s for ACK...\n");
                                        state = STATE_WAITING_FOR_ACK;
                                        clock_gettime(CLOCK_MONOTONIC,
                                                      &state_deadline);
                                        state_deadline.tv_sec += 5;
                                    }
                                }

                                // Handle other "new key" commands
                                else if (strcmp((char*)decrypted, "new key") ==
                                         0) {
                                    // Logic to check key cooldown and request a
                                    // new key
                                    time_t now = time(NULL);
                                    if (now - last_key_req_time <
                                        KEY_UPDATE_COOLDOWN_S) {
                                        printf(
                                            "Rate limit: another new key "
                                            "request too soon. Ignoring.\n");
                                    } else {
                                        last_key_req_time = now;
                                        printf(
                                            "Received 'new key' command. "
                                            "Waiting 5s for 'yes' "
                                            "confirmation...\n");
                                        state = STATE_WAITING_FOR_YES;
                                        clock_gettime(CLOCK_MONOTONIC,
                                                      &state_deadline);
                                        state_deadline.tv_sec += 5;
                                    }
                                }

                                // Handle key confirmation ACK (after new key
                                // sent)
                                else if (state == STATE_WAITING_FOR_ACK &&
                                         strcmp((char*)decrypted, "ACK") == 0) {
                                    printf(
                                        "ACK received. Finalizing key "
                                        "update.\n");
                                    memcpy(s_key.cipher_key, pending_key,
                                           SESSION_KEY_SIZE);
                                    explicit_bzero(pending_key,
                                                   sizeof(pending_key));
                                    print_hex("New key is now active: ",
                                              s_key.cipher_key,
                                              SESSION_KEY_SIZE);
                                    state = STATE_IDLE;
                                }

                                // Handle "verify key" command - initiate HMAC challenge
                                else if (strcmp((char*)decrypted, "verify key") == 0) {
                                    printf("Initiating HMAC challenge to verify Pico has session key...\n");
                                    
                                    // Generate random challenge
                                    if (rand_bytes(pending_challenge, CHALLENGE_SIZE) != 0) {
                                        printf("Failed to generate challenge nonce.\n");
                                    } else {
                                        // Send challenge via UART with 4-byte preamble + type + length
                                        uint8_t header[] = {
                                            PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                            MSG_TYPE_CHALLENGE,
                                            (CHALLENGE_SIZE >> 8) & 0xFF,  // Length high byte
                                            CHALLENGE_SIZE & 0xFF          // Length low byte
                                        };
                                        write_all(fd, header, sizeof(header));
                                        write_all(fd, pending_challenge, CHALLENGE_SIZE);
                                        tcdrain(fd);
                                        
                                        // Set state to wait for response
                                        state = STATE_WAITING_FOR_HMAC_RESP;
                                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                        state_deadline.tv_sec += 5;  // 5 second timeout
                                        challenge_active = true;
                                        
                                        printf("Challenge sent. Waiting for HMAC response...\n");
                                    }
                                }

                                // Handle HMAC response verification
                                else if (challenge_active && 
                                         strncmp((char*)decrypted, HMAC_RESPONSE_PREFIX, 5) == 0) {
                                    // Extract HMAC from message (format: "HMAC:HEXSTRING")
                                    const char* hmac_hex = (char*)decrypted + 5;
                                    
                                    // Convert hex string to bytes
                                    uint8_t received_hmac[HMAC_SIZE];
                                    for (int i = 0; i < HMAC_SIZE; i++) {
                                        sscanf(hmac_hex + (i * 2), "%2hhx", &received_hmac[i]);
                                    }
                                    
                                    // Compute expected HMAC
                                    uint8_t expected_hmac[HMAC_SIZE];
                                    int ret = sst_hmac_sha256(s_key.cipher_key, pending_challenge, 
                                                              CHALLENGE_SIZE, expected_hmac);
                                    
                                    if (ret == 0 && memcmp(received_hmac, expected_hmac, HMAC_SIZE) == 0) {
                                        printf("✅ HMAC VERIFICATION SUCCESSFUL: Pico has correct session key!\n");
                                    } else {
                                        printf("❌ HMAC VERIFICATION FAILED: Pico does not have correct key!\n");
                                    }
                                    
                                    // Clear challenge state
                                    explicit_bzero(pending_challenge, sizeof(pending_challenge));
                                    challenge_active = false;
                                    state = STATE_IDLE;
                                }

                            } else {
                                // AES-GCM decryption failed
                                printf("AES-GCM decryption failed: %d\n", ret);
                            }

                        } else {
                            printf("Incomplete ciphertext or tag.\n");
                        }
                        uart_state = 0;  // Reset uart_state machine
                    } else {
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
    free_SST_ctx_t(sst);
    return 0;
}