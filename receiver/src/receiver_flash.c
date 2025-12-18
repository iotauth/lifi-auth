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
static WINDOW *win_mid = NULL;
static WINDOW *win_cmd = NULL;

// Helper to print with color highlights based on keywords
static void wprint_styled_core(WINDOW *win, bool newline, const char *fmt, va_list ap) {
    if (!win) return;
    
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    // Simple keyword matching for styling
    int color = 0;
    int attr = 0;

    if (strstr(buf, "Error") || strstr(buf, "Failed") || strstr(buf, "Closed") || 
        strstr(buf, "NO") || strstr(buf, "Warning")) {
        color = 2; // Red
        attr = A_BOLD;
    } else if (strstr(buf, "Success") || strstr(buf, "OPEN") || strstr(buf, "YES") || 
               strstr(buf, "✓") || strstr(buf, "ACK")) {
        color = 1; // Green
        attr = A_BOLD;
    } else if (strstr(buf, "Challenge")) {
        color = 3; // Cyan
    } else if (strstr(buf, "timed out")) {
        color = 4; // Yellow (Orange)
        attr = A_BOLD;
    }

    if (color != 0) wattron(win, COLOR_PAIR(color) | attr);
    wprintw(win, "%s", buf);
    if (color != 0) wattroff(win, COLOR_PAIR(color) | attr);
    
    // If newline is requested, print it. Otherwise, rely on format string.
    if (newline) wprintw(win, "\n");
    wrefresh(win);
}

static void log_printf(const char *fmt, ...) {
    if (!win_log) return;
    va_list ap;
    va_start(ap, fmt);
    wprint_styled_core(win_log, false, fmt, ap);
    va_end(ap);
}

static void cmd_printf(const char *fmt, ...) {
    if (!win_cmd) return;
    va_list ap;
    va_start(ap, fmt);
    wprint_styled_core(win_cmd, true, fmt, ap);
    va_end(ap);
}

static void cmd_print_partial(const char *fmt, ...) {
    if (!win_cmd) return;
    va_list ap;
    va_start(ap, fmt);
    wprint_styled_core(win_cmd, false, fmt, ap); // No newline
    va_end(ap);
}

static WINDOW *win_log_border = NULL;
static WINDOW *win_cmd_border = NULL;

static void ui_init(void) {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0); // Hide cursor

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_RED, -1);
        init_pair(3, COLOR_CYAN, -1);
        init_pair(4, COLOR_YELLOW, -1);
        init_pair(5, COLOR_MAGENTA, -1);
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    int mid_h = 9;                 // Increased height for better spacing
    int top_h = (rows - mid_h) / 2;
    int bot_h = rows - mid_h - top_h;

    // Minimum check
    if (top_h < 4) top_h = 4;
    if (bot_h < 4) bot_h = 4;

    int top_y = 0;
    int mid_y = top_y + top_h;
    int bot_y = mid_y + mid_h;

    // Create Border Windows
    win_log_border = newwin(top_h, cols, top_y, 0);
    win_mid        = newwin(mid_h, cols, mid_y, 0);
    win_cmd_border = newwin(bot_h, cols, bot_y, 0);

    // Create Inner Content Windows (derived from borders)
    // 1 char offset from top/left, height-2, width-2 to stay inside box
    win_log = derwin(win_log_border, top_h - 2, cols - 2, 1, 1);
    win_cmd = derwin(win_cmd_border, bot_h - 2, cols - 2, 1, 1);

    scrollok(win_log, TRUE);
    scrollok(win_cmd, TRUE);

    // Draw parameters on borders
    box(win_log_border, 0, 0);
    box(win_mid, 0, 0);
    box(win_cmd_border, 0, 0);

    // Titles with bold on borders
    wattron(win_log_border, A_BOLD);
    mvwprintw(win_log_border, 0, 2, " RX / Photodiode Log ");
    wattroff(win_log_border, A_BOLD);

    wattron(win_mid, A_BOLD | COLOR_PAIR(4));
    mvwprintw(win_mid, 0, 2, " Key / Security ");
    wattroff(win_mid, A_BOLD | COLOR_PAIR(4));

    wattron(win_cmd_border, A_BOLD);
    mvwprintw(win_cmd_border, 0, 2, " Commands / Status ");
    wattroff(win_cmd_border, A_BOLD);

    refresh(); // Refresh stdscr
    wrefresh(win_log_border);
    wrefresh(win_mid);
    wrefresh(win_cmd_border);
    wrefresh(win_log); // Inner
    wrefresh(win_cmd); // Inner
}

static void mid_draw_keypanel(const session_key_t* s_key,
                              bool key_valid,
                              receiver_state_t state,
                              const char* uart_dev,
                              bool serial_open) {
    if (!win_mid) return;

    int h, w;
    getmaxyx(win_mid, h, w);
    (void)w;

    werase(win_mid);
    box(win_mid, 0, 0);
    
    wattron(win_mid, A_BOLD | COLOR_PAIR(4));
    mvwprintw(win_mid, 0, 2, " Key / Security ");
    wattroff(win_mid, A_BOLD | COLOR_PAIR(4));

    // Serial Status
    mvwprintw(win_mid, 2, 2, "Serial: ");
    if (serial_open) {
        wattron(win_mid, A_BOLD | COLOR_PAIR(1));
        wprintw(win_mid, "OPEN");
        wattroff(win_mid, A_BOLD | COLOR_PAIR(1));
    } else {
        wattron(win_mid, A_BOLD | COLOR_PAIR(2));
        wprintw(win_mid, "CLOSED");
        wattroff(win_mid, A_BOLD | COLOR_PAIR(2));
    }
    wprintw(win_mid, "   Dev: %s   State: %d", uart_dev, (int)state);

    // Key Valid Status
    mvwprintw(win_mid, 3, 2, "Key valid: ");
    if (key_valid) {
        wattron(win_mid, A_BOLD | COLOR_PAIR(1));
        wprintw(win_mid, "YES");
        wattroff(win_mid, A_BOLD | COLOR_PAIR(1));
    } else {
        wattron(win_mid, A_BOLD | COLOR_PAIR(2));
        wprintw(win_mid, "NO");
        wattroff(win_mid, A_BOLD | COLOR_PAIR(2));
    }

    if (key_valid && s_key) {
        wmove(win_mid, 4, 2);
        wprintw(win_mid, "Key ID: ");
        wattron(win_mid, COLOR_PAIR(3));
        for (size_t i = 0; i < SESSION_KEY_ID_SIZE; i++) wprintw(win_mid, "%02X ", s_key->key_id[i]);
        wattroff(win_mid, COLOR_PAIR(3));

        wmove(win_mid, 5, 2);
        wprintw(win_mid, "Key:    ");
        wattron(win_mid, COLOR_PAIR(3)); // Same Cyan for Key
        for (size_t i = 0; i < SESSION_KEY_SIZE; i++) wprintw(win_mid, "%02X ", s_key->cipher_key[i]);
        wattroff(win_mid, COLOR_PAIR(3));
    } else {
        mvwprintw(win_mid, 4, 2, "Key ID: (none)");
        mvwprintw(win_mid, 5, 2, "Key:    (none)");
    }

    // Shortcuts menu at bottom of mid panel
    int menu_r = h - 2;
    // Use A_DIM or just normal
    mvwprintw(win_mid, menu_r, 2, "[1] Send Key  [2] Challenge  [s] Stats  [c] Clear logs  [f] Force Key  [r] Reopen  [q] Quit");

    wrefresh(win_mid);
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
}

static void ui_shutdown(void) {
    if (win_log) delwin(win_log);
    if (win_cmd) delwin(win_cmd);
    if (win_log_border) delwin(win_log_border);
    if (win_cmd_border) delwin(win_cmd_border);
    if (win_mid) delwin(win_mid);
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

// --- Session Statistics ---
typedef struct {
    unsigned long total_pkts;
    unsigned long decrypt_success;
    unsigned long decrypt_fail;
    unsigned long replay_blocked;
    unsigned long timeouts;
    unsigned long bad_preamble;
} SessionStats;

int main(int argc, char* argv[]) {
    SessionStats stats = {0};

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
        printf("SST init failed.\n");
        return 1;
    }

    session_key_list_t* key_list = get_session_key(sst, NULL);

    // --- Serial Init (Before UI) ---
    // Initialize serial first so any perror/printf issues don't corrupt the ncurses window
    // and so we know the state immediately.
    int fd = -1;
    fd = init_serial(UART_DEVICE, UART_BAUDRATE_TERMIOS);
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    ui_init();
    atexit(ui_shutdown);

    if (fd < 0) {
        log_printf("Warning: serial not open (%s). Press 'r' to retry.", UART_DEVICE);
    }

    if (!key_list || key_list->num_key == 0) {
        log_printf("No session key.\n");
        // Don't return 1 here, let the UI stay up so user can see error
        // return 1; 
    }

    session_key_t s_key = {0}; 
    if (key_list && key_list->num_key > 0) {
        s_key = key_list->s_key[0];
    }
    
    bool key_valid = (key_list && key_list->num_key > 0);
    receiver_state_t state = STATE_IDLE;

    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));

    // --- Receiver state + replay window ---
    struct timespec state_deadline = (struct timespec){0, 0};
    time_t last_key_req_time = 0;

    // nonce setup
    replay_window_t rwin;
    replay_window_init(&rwin, NONCE_SIZE, NONCE_HISTORY_SIZE);

    // Challenge tracking for HMAC verification
    uint8_t pending_challenge[CHALLENGE_SIZE] = {0};
    bool challenge_active = false;

    // Initial key push retry machinery
    struct timespec next_send = {0};
    clock_gettime(CLOCK_MONOTONIC, &next_send);  // send immediately

    const uint8_t preamble[2] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2};
    // Automatic handshake REMOVED - waitForUser to press [1]

    // UART framing state
    uint8_t byte = 0;
    int uart_state = 0;

    log_printf("Listening for encrypted message...\n");
    if (fd >= 0) tcflush(fd, TCIFLUSH);

    uint8_t pending_key[SESSION_KEY_SIZE] = {0};
    int last_countdown = -1;

    while (1) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

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
                        write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                        write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
                        cmd_printf("Error: Failed to send session key.");
                    } else {
                        tcdrain(fd);
                        cmd_printf("✓ Session key sent.");
                    }
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                    break;
                }

                case 'f':
                case 'F': {
                    cmd_printf("[Shortcut] Force Fetch New Key from SST...");
                    // Try fetch new list first without freeing old one
                    session_key_list_t* new_key_list = get_session_key(sst, NULL);
                    
                    if (!new_key_list || new_key_list->num_key == 0) {
                         // Failed.
                         cmd_printf("Error: Failed to fetch new key from SST.");
                         
                         if (errno == EAGAIN || errno == EWOULDBLOCK) {
                             cmd_printf("Error detail: Resource temporary unavailable (EAGAIN).");
                             cmd_printf("Try again in a moment.");
                         }
                         
                         cmd_printf("Keeping current session key.");
                         if (new_key_list) free_session_key_list_t(new_key_list);
                    } else {
                        // Success, replace old list
                        if (key_list) free_session_key_list_t(key_list);
                        key_list = new_key_list;
                        
                        s_key = key_list->s_key[0];
                        key_valid = true;
                        cmd_printf("✓ New key fetched from SST.");
                        
                        // Auto-send like '1'
                        if (fd >= 0) {
                            if (write_all(fd, preamble, sizeof preamble) < 0 ||
                                write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                                write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
                                cmd_printf("Error: Failed to send new key to Pico.");
                            } else {
                                tcdrain(fd);
                                cmd_printf("✓ New session key sent to Pico.");
                            }
                        } else {
                            cmd_printf("Warning: Serial closed. Key updated locally but not sent.");
                        }
                    }
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
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
                        last_countdown = 5; // Reset countdown
                        cmd_print_partial("✓ Challenge sent. Waiting for HMAC response... ");
                    }
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                    break;
                }

                case 's':
                case 'S': {
                    cmd_printf("--- Session Statistics ---");
                    cmd_printf("Packets RX:      %lu", stats.total_pkts);
                    cmd_printf("Decrypt Success: %lu", stats.decrypt_success);
                    cmd_printf("Decrypt Fail:    %lu", stats.decrypt_fail);
                    cmd_printf("Replays Blocked: %lu", stats.replay_blocked);
                    cmd_printf("Timeouts:        %lu", stats.timeouts);
                    cmd_printf("Bad Preambles:   %lu", stats.bad_preamble);
                    if (key_valid) {
                         cmd_hex("Key ID: ", s_key.key_id, SESSION_KEY_ID_SIZE);
                    }
                    cmd_printf("--------------------------");
                    break;
                }

                case 'c':
                case 'C': {
                    werase(win_log);
                    wrefresh(win_log);
                    cmd_printf("Logs cleared.");
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
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
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

        // --- Handle Countdown Display ---
        if (state == STATE_WAITING_FOR_HMAC_RESP) {
            int remaining = (int)(state_deadline.tv_sec - now_ts.tv_sec);
            if (remaining < 0) remaining = 0;
            if (remaining != last_countdown) {
                cmd_print_partial("%d.. ", remaining);
                last_countdown = remaining;
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
                // The partial line "3.. 2.. 1.. 0.." needs a newline before the error
                cmd_printf("\nHMAC challenge timed out. Pico did not respond.\n");
                stats.timeouts++;
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
                        log_printf("Waiting: got 0x%02X (Hint: Check optical alignment)\n", byte);
                    }
                    break;
                case 1:
                    if (byte == PREAMBLE_BYTE_2) {
                        uart_state = 2;
                    } else {
                        log_printf("Bad second preamble byte: 0x%02X\n", byte);
                        stats.bad_preamble++;
                        uart_state = 0;
                    }
                    break;

                case 2:
                    if (byte == MSG_TYPE_ENCRYPTED) {
                        stats.total_pkts++;
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
                            stats.replay_blocked++;
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
                                stats.decrypt_success++;

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
                                        write(fd, key_list->s_key[0].key_id, SESSION_KEY_ID_SIZE);
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
                                stats.decrypt_fail++;
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