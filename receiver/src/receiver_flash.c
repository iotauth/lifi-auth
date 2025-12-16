#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>  // Linux serial
#include <time.h>
#include <unistd.h>

//for ui on pi4
#include <fcntl.h>  // for open()
#include <ncurses.h>
#include <stdarg.h>


// Project headers (use -I include dirs instead of ../../../)
#include "c_api.h"
#include "config_handler.h"  // change_directory_to_config_path, get_config_path
#include "key_exchange.h"
#include "../../include/protocol.h"
#include "replay_window.h"
#include "serial_linux.h"
#include "sst_crypto_embedded.h"  // brings in sst_decrypt_gcm prototype and sizes
#include "utils.h"

// ncurses for ui on pi4

static WINDOW *win_log = NULL;
static WINDOW *win_cmd = NULL;

static void log_printf(const char *fmt, ...) {
    if (!win_log) return;
    int y, x;
    getyx(win_log, y, x);
    (void)x;
    if (y == 0) wmove(win_log, 1, 1);

    va_list ap;
    va_start(ap, fmt);
    vw_printw(win_log, fmt, ap);
    va_end(ap);
    wprintw(win_log, "\n");
    wrefresh(win_log);

    redraw_frames();
}

static void cmd_printf(const char *fmt, ...) {
    if (!win_cmd) return;
    int y, x;
    getyx(win_cmd, y, x);
    (void)x;
    if (y == 0) wmove(win_cmd, 1, 1);

    va_list ap;
    va_start(ap, fmt);
    vw_printw(win_cmd, fmt, ap);
    va_end(ap);
    wprintw(win_cmd, "\n");
    wrefresh(win_cmd);

    redraw_frames();
}

static void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);     // getch nonblocking
    keypad(stdscr, TRUE);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int cmd_h = 6;
    int log_h = rows - cmd_h;

    win_log = newwin(log_h, cols, 0, 0);
    win_cmd = newwin(cmd_h, cols, log_h, 0);


    scrollok(win_log, TRUE);
    scrollok(win_cmd, TRUE);

    box(win_log, 0, 0);
    box(win_cmd, 0, 0);
    wsetscrreg(win_log, 1, log_h - 2);
    wsetscrreg(win_cmd, 1, cmd_h - 2);

    mvwprintw(win_log, 0, 2, " RX / Photodiode Log ");
    mvwprintw(win_cmd, 0, 2, " Commands / Status ");
    wrefresh(win_log);
    wrefresh(win_cmd);

    // Put cursor in cmd window
    wmove(win_cmd, 1, 1);
    wmove(win_log, 1, 1);
    wrefresh(win_log);
    wrefresh(win_cmd);
}

static void redraw_frames(void) {
    if (win_log) {
        box(win_log, 0, 0);
        mvwprintw(win_log, 0, 2, " RX / Photodiode Log ");
        wrefresh(win_log);
    }
    if (win_cmd) {
        box(win_cmd, 0, 0);
        mvwprintw(win_cmd, 0, 2, " Commands / Status ");
        wrefresh(win_cmd);
    }
}

static void cmd_hex(const char* label, const uint8_t* b, size_t n) {
    if (!win_cmd) return;
    int y, x;
    getyx(win_cmd, y, x);
    (void)x;
    if (y == 0) wmove(win_cmd, 1, 1);

    wprintw(win_cmd, "%s", label);
    for (size_t i = 0; i < n; i++) wprintw(win_cmd, "%02X ", b[i]);
    wprintw(win_cmd, "\n");
    wrefresh(win_cmd);

    redraw_frames();
}

static void ui_shutdown(void) {
    if (win_log) delwin(win_log);
    if (win_cmd) delwin(win_cmd);
    endwin();
}


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

    ui_init();
    atexit(ui_shutdown);

    cmd_printf("Using config file: %s", config_path);

    // --- Fetch session key from SST ---
    cmd_printf("Retrieving session key from SST...");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        log_printf("SST init failed.");
        return 1;
    }

    session_key_list_t* key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        log_printf("No session key.");
        return 1;
    }

    session_key_t s_key = key_list->s_key[0];
    cmd_hex("Session Key: ", s_key.cipher_key, SESSION_KEY_SIZE);

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
    int fd = -1;
    fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
    if (fd < 0) {
        log_printf("Warning: serial not open (%s). Press 'r' to retry.", UART_DEVICE);
    }
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    // Initial key push retry machinery
    struct timespec next_send = {0};
    clock_gettime(CLOCK_MONOTONIC, &next_send);  // send immediately

    const uint8_t preamble[2] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2};
    // Automatic handshake REMOVED - waitForUser to press [1]

    cmd_printf("\n");
    cmd_printf("╔════════════════════════════════════════════════════════╗\n");
    cmd_printf("║          LiFi Receiver - Interactive Mode             ║\n");
    cmd_printf("╠════════════════════════════════════════════════════════╣\n");
    cmd_printf("║  [1] Send Session Key to Pico                         ║\n");
    cmd_printf("║  [2] Send HMAC Challenge (verify key)                 ║\n");
    cmd_printf("║  [s] Show Status (Key & Connection)                   ║\n");
    cmd_printf("║  [q] Quit                                              ║\n");
    cmd_printf("╚════════════════════════════════════════════════════════╝\n");
    cmd_printf("\n");

    // UART framing state
    uint8_t byte = 0;
    int uart_state = 0;

    log_printf("Listening for encrypted message...\n");
    if (fd >= 0) tcflush(fd, TCIFLUSH);

    while (1) {
        // --- Handle Keyboard Shortcuts ---
        int key = getch();
        if (key == ERR) key = -1;

        if (key != -1) {
            switch (key) {
                case '1': {
                    cmd_printf("[Shortcut] Sending session key to Pico...");
                    if (fd < 0) { cmd_printf("Serial not open. Press 'r' to retry."); break; }
                    if (!key_valid) { cmd_printf("No valid session key loaded."); break; }

                    if (write_all(fd, preamble, sizeof preamble) < 0 ||
                        write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
                        cmd_printf("Error: Failed to send session key.");
                    } else {
                        tcdrain(fd);
                        cmd_printf("✓ Session key sent.");
                    }
                    break;
                }

                case '2': {
                    cmd_printf("[Shortcut] Initiating HMAC challenge...");
                    if (fd < 0) { cmd_printf("Serial not open. Press 'r' to retry."); break; }
                    if (!key_valid) { cmd_printf("No valid session key loaded."); break; }

                    if (rand_bytes(pending_challenge, CHALLENGE_SIZE) != 0) {
                        cmd_printf("Error: Failed to generate challenge nonce.");
                        break;
                    }

                    uint8_t msg[] = { PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, MSG_TYPE_CHALLENGE };
                    if (write_all(fd, msg, sizeof msg) < 0 ||
                        write_all(fd, pending_challenge, CHALLENGE_SIZE) < 0) {
                        cmd_printf("Error: Failed to send challenge.");
                    } else {
                        state = STATE_WAITING_FOR_HMAC_RESP;
                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                        state_deadline.tv_sec += 5;
                        challenge_active = true;
                        cmd_printf("✓ Challenge sent. Waiting for HMAC response...");
                    }
                    break;
                }

                case 's':
                case 'S': {
                    cmd_printf("--- Status Report ---");
                    cmd_printf("Serial: %s", (fd >= 0) ? "OPEN" : "CLOSED");
                    cmd_printf("UART Device: %s", UART_DEVICE);
                    cmd_printf("Key valid: %s", key_valid ? "YES" : "NO");
                    if (key_valid) {
                        cmd_hex("Key ID: ", s_key.key_id, SESSION_KEY_ID_SIZE);
                        cmd_hex("Session Key: ", s_key.cipher_key, SESSION_KEY_SIZE);
                    }
                    cmd_printf("State: %d", state);
                    cmd_printf("---------------------");
                    break;
                }

                case 'r':
                case 'R': {
                    if (fd >= 0) {
                        cmd_printf("Closing serial...");
                        close(fd);
                        fd = -1;
                    }
                    fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
                    if (fd >= 0) {
                        int flags = fcntl(fd, F_GETFL, 0);
                        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                        tcflush(fd, TCIFLUSH);
                        cmd_printf("✓ Serial opened.");
                    } else {
                        cmd_printf("Still failed to open serial.");
                    }
                    break;
                }

                case 'q':
                case 'Q': {
                    cmd_printf("Exiting...");
                    if (fd >= 0) close(fd);
                    free_session_key_list_t(key_list);
                    free_SST_ctx_t(sst);
                    return 0;
                }

                default:
                    break;
            }
        }



        // --- Handle State Timeouts ---
        if (state != STATE_IDLE && timespec_passed(&state_deadline)) {
            if (state == STATE_WAITING_FOR_YES) {
                cmd_printf(
                    "Confirmation for 'new key' timed out. Returning to "
                    "idle.\n");
                // nothing to wipe here
            } else if (state == STATE_WAITING_FOR_ACK) {
                cmd_printf(
                    "Timeout waiting for key update ACK. Discarding new "
                    "key.\n");
                explicit_bzero(pending_key, sizeof pending_key);
                // keep old key; key_valid stays true
            } else if (state == STATE_WAITING_FOR_HMAC_RESP) {
                cmd_printf("HMAC challenge timed out. Pico did not respond.\n");
                explicit_bzero(pending_challenge, sizeof(pending_challenge));
                challenge_active = false;
            }
            state = STATE_IDLE;
            state_deadline = (struct timespec){0, 0};
        }

        if (fd >= 0 && read(fd, &byte, 1) == 1) {
            switch (uart_state) {
                case 0:
                    if (byte == PREAMBLE_BYTE_1) {
                        uart_state = 1;
                    } else {
                        log_printf(
                            "Waiting: got 0x%02X, expecting PREAMBLE_BYTE_1\n",
                            byte);
                    }
                    break;
                case 1:
                    if (byte == PREAMBLE_BYTE_2) {
                        uart_state = 2;
                    } else {
                        log_printf("Bad second preamble byte: 0x%02X\n", byte);
                        uart_state = 0;
                    }
                    break;

                case 2:
                    if (byte == MSG_TYPE_ENCRYPTED) {
                        uint8_t nonce[NONCE_SIZE];
                        uint8_t len_bytes[2];

                        // Read nonce + length
                        if (read_exact(fd, nonce, NONCE_SIZE) != NONCE_SIZE ||
                            read_exact(fd, len_bytes, 2) != 2) {
                            log_printf("Failed to read nonce or length\n");
                            uart_state = 0;
                            continue;
                        }

                        // --- Nonce Replay Check ---
                        if (replay_window_seen(&rwin, nonce)) {
                            log_printf("Nonce replayed! Rejecting message.\n");
                            uart_state = 0;
                            continue;
                        }
                        replay_window_add(&rwin, nonce);

                        // Length -> host order + bounds check
                        uint16_t msg_len =
                            ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                        if (msg_len == 0 ||
                            msg_len >
                                1024) {  // or use MAX_MSG_LEN from protocol.h
                            log_printf("Message too long: %u bytes\n", msg_len);
                            uart_state = 0;
                            continue;
                        }

                        // read payload
                        uint8_t ciphertext[msg_len];
                        uint8_t tag[TAG_SIZE];
                        uint8_t decrypted[msg_len + 1];  // for null-terminator

                        ssize_t c = read_exact(fd, ciphertext, msg_len);
                        ssize_t t = read_exact(fd, tag, TAG_SIZE);

                        if (c == msg_len && t == TAG_SIZE) {
                            if (!key_valid) {  // Skip decryption if key was
                                               // cleared and not yet rotated
                                log_printf(
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
                                log_printf("%s\n", decrypted);

                                // If the decrypted message is "I have the key",
                                // stop sending the key.
                                if (strcmp((char*)decrypted,
                                           "I have the key") == 0) {
                                    log_printf(
                                        "Pico has confirmed receiving the "
                                        "key.\n");
                                }

                                // Handle "new key" commands (if the flag for
                                // sending new key is set)
                                else if (strcmp((char*)decrypted,
                                                "new key -f") == 0) {
                                    // Logic to request a new key and forcefully
                                    // overwrite current one
                                    cmd_printf(
                                        "Received 'new key -f' command. "
                                        "Requesting new key...\n");

                                    free_session_key_list_t(key_list);
                                    key_list = get_session_key(
                                        sst, init_empty_session_key_list());
                                    
                                    if (!key_list || key_list->num_key == 0) {
                                        cmd_printf("Failed to fetch new session "
                                                "key.\n");
                                    } else {
                                        memcpy(pending_key,
                                               key_list->s_key[0].cipher_key,
                                               SESSION_KEY_SIZE);
                                        cmd_hex(
                                            "New Session Key (pending ACK): ",
                                            pending_key, SESSION_KEY_SIZE);
                                        key_valid = true;

                                        uint8_t preamble[2] = {0xAB, 0xCD};
                                        write(fd, preamble, 2);
                                        write(fd, pending_key,
                                              SESSION_KEY_SIZE);
                                        usleep(5000);  // 5ms sleep to let
                                                       // transmission complete
                                        cmd_printf(
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
                                        cmd_printf(
                                            "Rate limit: another new key "
                                            "request too soon. Ignoring.\n");
                                    } else {
                                        last_key_req_time = now;
                                        cmd_printf(
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
                                    cmd_printf(
                                        "ACK received. Finalizing key "
                                        "update.\n");
                                    memcpy(s_key.cipher_key, pending_key,
                                           SESSION_KEY_SIZE);
                                    explicit_bzero(pending_key,
                                                   sizeof(pending_key));
                                    cmd_hex("New key is now active: ",
                                              s_key.cipher_key,
                                              SESSION_KEY_SIZE);
                                    state = STATE_IDLE;
                                }

                                // Handle "verify key" command - initiate HMAC challenge
                                else if (strcmp((char*)decrypted, "verify key") == 0) {
                                    cmd_printf("Initiating HMAC challenge to verify Pico has session key...\n");
                                    
                                    // Generate random challenge
                                    if (rand_bytes(pending_challenge, CHALLENGE_SIZE) != 0) {
                                        cmd_printf("Failed to generate challenge nonce.\n");
                                    } else {
                                        // Send challenge via UART
                                        uint8_t msg[] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, MSG_TYPE_CHALLENGE};
                                        write_all(fd, msg, sizeof(msg));
                                        write_all(fd, pending_challenge, CHALLENGE_SIZE);
                                        
                                        // Set state to wait for response
                                        state = STATE_WAITING_FOR_HMAC_RESP;
                                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                        state_deadline.tv_sec += 5;  // 5 second timeout
                                        challenge_active = true;
                                        
                                        cmd_printf("Challenge sent. Waiting for HMAC response...\n");
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
                                        log_printf("✅ HMAC VERIFICATION SUCCESSFUL: Pico has correct session key!\n");
                                    } else {
                                        log_printf("❌ HMAC VERIFICATION FAILED: Pico does not have correct key!\n");
                                    }
                                    
                                    // Clear challenge state
                                    explicit_bzero(pending_challenge, sizeof(pending_challenge));
                                    challenge_active = false;
                                    state = STATE_IDLE;
                                }

                            } else {
                                // AES-GCM decryption failed
                                log_printf("AES-GCM decryption failed: %d\n", ret);
                            }

                        } else {
                            log_printf("Incomplete ciphertext or tag.\n");
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
        usleep(1000); // 1ms
    }

    close(fd);
    free_session_key_list_t(key_list);
    free_SST_ctx_t(sst);
    return 0;
}