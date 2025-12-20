#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>  // Linux serial
#include <time.h>
#include <unistd.h>
#include <sys/time.h> // For gettimeofday

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
#include "heatshrink_decoder.h"
#include "../../include/crc16.h"
#include "utils.h"


static WINDOW *win_log = NULL;
static WINDOW *win_mid = NULL;
static WINDOW *win_cmd = NULL;

// Helper to print with color highlights based on keywords
static void wprint_styled_core(WINDOW *win, bool newline, const char *fmt, va_list ap) {
    if (!win) return;
    
    char buf[4096]; // Increased buffer size
    vsnprintf(buf, sizeof(buf), fmt, ap);

    // Write to file for debugging
    FILE *f = fopen("receiver_debug.log", "a");
    if (f) {
        fprintf(f, "%s%s", buf, newline ? "\n" : "");
        fclose(f);
    }

    // Simple keyword matching for styling
    int color = 0;
    int attr = 0;

    if (strstr(buf, "Error") || strstr(buf, "Failed") || strstr(buf, "Closed") || 
        strstr(buf, "NO") || strstr(buf, "Warning")) {
        color = 2; // Red
        attr = A_BOLD;
    } else if (strstr(buf, "Success") || strstr(buf, "OPEN") || strstr(buf, "YES") || 
               strstr(buf, "✓") || strstr(buf, "ACK") || strstr(buf, "VERIFIED")) {
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

// New globals for Auto-Connect feature
static uint8_t last_lifi_id[SESSION_KEY_ID_SIZE] = {0};
static bool lifi_id_seen = false;

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

    // Display Last Received LiFi Key ID
    mvwprintw(win_mid, 7, 2, "LiFi Key: ");
    if (lifi_id_seen) {
        wattron(win_mid, COLOR_PAIR(3));
        for (size_t i = 0; i < SESSION_KEY_ID_SIZE; i++) wprintw(win_mid, "%02X ", last_lifi_id[i]);
        wattroff(win_mid, COLOR_PAIR(3));
    } else {
        wprintw(win_mid, "(waiting)");
    }

    // Shortcuts menu at bottom of mid panel
    int menu_r = h - 2;
    // Use A_DIM or just normal
    mvwprintw(win_mid, menu_r, 2, "[1] Send Key  [2] Challenge  [s] Stats  [c] Clear  [p] Save  [f] Force Key  [r] Reopen  [q] Quit");

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

// Helper: Read exact bytes with timeout for non-blocking FD
// timeout_ms: Max time to wait for the ENTIRE buffer to be filled
static ssize_t read_exact_timeout(int fd, void *buf, size_t len, int timeout_ms) {
    uint8_t *p = (uint8_t *)buf;
    size_t received = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);
    
    while (received < len) {
        // Check timeout
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed_ms >= timeout_ms) {
            return -1; // Timeout
        }
        
        // Wait for data
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (timeout_ms - elapsed_ms) * 1000;
        
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);
        if (ret > 0) {
            ssize_t n = read(fd, p + received, len - received);
            if (n > 0) {
                received += n;
            } else if (n == 0) {
                return -1; // EOF/Closed
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return -1; // Error
            }
        } else if (ret == 0) {
            return -1; // Timeout
        } else {
            return -1; // Error
        }
    }
    return received;
}

// --- Session Statistics ---
typedef struct {
    unsigned long total_pkts;
    unsigned long decrypt_success;
    unsigned long decrypt_fail;
    unsigned long replay_blocked;
    unsigned long timeouts;
    unsigned long bad_preamble;
    unsigned long keys_consumed;
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

    const uint8_t preamble[4] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4};

    // --- Automatic Session Key Send (Restored) ---
    // Build key provisioning frame: [PREAMBLE:4][TYPE:1][LEN:2][KEY_ID:8][KEY:16]
    if (fd >= 0 && key_valid) {
        uint16_t key_payload_len = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE;
        uint8_t key_header[] = {
            PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
            MSG_TYPE_KEY,
            (key_payload_len >> 8) & 0xFF,
            key_payload_len & 0xFF
        };
        
        if (write_all(fd, key_header, sizeof(key_header)) < 0 ||
            write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
            write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0) {
            log_printf("Error: Failed to send initial session key.\n");
        } else {
            tcdrain(fd);
            log_printf("Sent session key over UART (4-byte preamble + KEY_ID + KEY).\n");
        }
    }

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

                    // Build key provisioning frame: [PREAMBLE:4][TYPE:1][LEN:2][KEY_ID:8][KEY:16]
                    uint16_t klen = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE;
                    uint8_t hdr[] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_KEY,
                        (klen >> 8) & 0xFF,
                        klen & 0xFF
                    };

                    if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
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
                        stats.keys_consumed++;
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

                    // Use 4-byte preamble + type + length
                    uint8_t header[] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_CHALLENGE,
                        (CHALLENGE_SIZE >> 8) & 0xFF,  // Length high byte
                        CHALLENGE_SIZE & 0xFF          // Length low byte
                    };
                    
                    if (write_all(fd, header, sizeof(header)) < 0 ||
                        write_all(fd, pending_challenge, CHALLENGE_SIZE) < 0) {
                        cmd_printf("Error: Failed to send challenge.");
                    } else {
                        tcdrain(fd);
                        
                        // Pre-calculate expected HMAC for display
                        uint8_t expected_hmac[HMAC_SIZE];
                        sst_hmac_sha256(s_key.cipher_key, pending_challenge, CHALLENGE_SIZE, expected_hmac);
                        
                        cmd_print_partial("Challenge sent. [Exp: ");
                        for(int i=0; i<4; i++) wprintw(win_cmd, "%02X", expected_hmac[i]); // Direct wprintw to continue line
                        wprintw(win_cmd, "..] ");
                        wrefresh(win_cmd); // Refresh

                        state = STATE_WAITING_FOR_HMAC_RESP;
                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                        state_deadline.tv_sec += 5;
                        challenge_active = true;
                        last_countdown = 5; // Reset countdown
                        cmd_print_partial("Waiting... ");
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
                    cmd_printf("Keys Consumed:   %lu", stats.keys_consumed);
                    cmd_printf("--------------------------");
                    break;
                }

                case 'c':
                case 'C': {
                    werase(win_log);
                    wrefresh(win_log);

                    werase(win_cmd);
                    wrefresh(win_cmd);

                    unsigned long saved = stats.keys_consumed;
                    memset(&stats, 0, sizeof(stats));
                    stats.keys_consumed = saved;
                    cmd_printf("Logs and Statistics (except Keys) cleared.");
                    break;
                }

                case 'p':
                case 'P': {
                    FILE *f = fopen("session_stats.txt", "a");
                    if (f) {
                        time_t now = time(NULL);
                        char *tstr = ctime(&now);
                        if (tstr && strlen(tstr) > 0) tstr[strlen(tstr)-1] = '\0';

                        fprintf(f, "[%s] Stats Snapshot\n", tstr ? tstr : "Unknown");
                        fprintf(f, "Packets RX:      %lu\n", stats.total_pkts);
                        fprintf(f, "Decrypt Success: %lu\n", stats.decrypt_success);
                        fprintf(f, "Decrypt Fail:    %lu\n", stats.decrypt_fail);
                        fprintf(f, "Replays Blocked: %lu\n", stats.replay_blocked);
                        fprintf(f, "Timeouts:        %lu\n", stats.timeouts);
                        fprintf(f, "Bad Preambles:   %lu\n", stats.bad_preamble);
                        fprintf(f, "Keys Consumed:   %lu\n", stats.keys_consumed);
                        fprintf(f, "--------------------------\n");
                        fclose(f);
                        cmd_printf("Stats saved to session_stats.txt");
                    } else {
                        cmd_printf("Error: Failed to write stats.");
                    }
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
            // Activity Blink (Top Right)
            static int act_ctr = 0;
            if (++act_ctr % 10 == 0) {
                mvwprintw(win_log_border, 0, getmaxx(win_log_border)-4, "%c", (act_ctr/10)%2 ? '*' : ' ');
                wrefresh(win_log_border);
            }

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
                    if (byte == PREAMBLE_BYTE_3) {
                        uart_state = 3;
                    } else {
                        uart_state = 0;
                    }
                    break;
                case 3:
                    if (byte == PREAMBLE_BYTE_4) {
                        uart_state = 4; // Preamble complete, next is TYPE
                    } else {
                        uart_state = 0;
                    }
                    break;

                case 4:
                    // TYPE byte received
                    if (byte == MSG_TYPE_KEY_ID_ONLY) {
                        uint8_t packet_type = byte; 
                        stats.total_pkts++;
                        
                        // Read length (2 bytes)
                        uint8_t len_bytes[2];
                        if (read_exact_timeout(fd, len_bytes, 2, 100) != 2) {
                            log_printf("Failed to read length (Key ID)\n");
                            uart_state = 0;
                            continue;
                        }
                        uint16_t payload_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                        
                        if (payload_len > 64) { // Sanity check
                             log_printf("Invalid Key ID len: %u\n", payload_len);
                             uart_state = 0;
                             continue;
                        }
                        
                        uint8_t payload[payload_len];
                        uint8_t crc_bytes[2];
                        
                        if (read_exact_timeout(fd, payload, payload_len, 100) != (ssize_t)payload_len ||
                            read_exact_timeout(fd, crc_bytes, 2, 100) != 2) {
                            log_printf("Failed to read payload/CRC (Key ID)\n");
                            uart_state = 0;
                            continue;
                        }
                        
                        // Verify CRC
                        uint8_t crc_buf[1 + 2 + 64]; // Max size
                        size_t crc_idx = 0;
                        crc_buf[crc_idx++] = packet_type;
                        crc_buf[crc_idx++] = len_bytes[0];
                        crc_buf[crc_idx++] = len_bytes[1];
                        memcpy(&crc_buf[crc_idx], payload, payload_len);
                        crc_idx += payload_len;
                        
                        uint16_t computed_crc = crc16_ccitt(crc_buf, crc_idx);
                        uint16_t received_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
                        
                        if (computed_crc != received_crc) {
                            log_printf("CRC fail on Key ID pkt\n");
                            uart_state = 0;
                            continue;
                        }
                        
                        log_printf("[KEY ID] Received: ");
                        for(int i=0; i<payload_len; i++) {
                            // Using private internal method of log_printf to stay on same line? 
                            // iterating log_printf calls creates newlines usually.
                            // Let's just format it into a string first.
                        }
                        char hex_str[3 * payload_len + 1];
                        hex_str[0] = '\0';
                        for(int i=0; i<payload_len; i++) {
                            char tmp[5];
                            snprintf(tmp, sizeof(tmp), "%02X ", payload[i]);
                            strcat(hex_str, tmp);
                        }
                        log_printf("[KEY ID] Peer ID: %s", hex_str);
                        
                        // --- AUTO-CONNECT LOGIC ---
                        // 1. Store the ID
                        memcpy(last_lifi_id, payload, SESSION_KEY_ID_SIZE);
                        lifi_id_seen = true;
                        
                        cmd_printf("Looking for Key ID...");
                        
                        // 2. Use C-API to find locally or fetch from Auth
                        // This handles checking existing_s_key_list first, then queries Auth if needed.
                        session_key_t *found_key = get_session_key_by_ID(last_lifi_id, sst, key_list);
                        
                        if (found_key) {
                            s_key = *found_key;
                            key_valid = true;
                            
                            // Check if it was a local find or a fetch (heuristic: pointer check)
                            bool is_local = false;
                            if (key_list && key_list->s_key) {
                                // If pointer is within the array bounds of our local list
                                if (found_key >= key_list->s_key && 
                                    found_key < key_list->s_key + key_list->num_key) {
                                    is_local = true;
                                }
                            }
                            
                            if (is_local) {
                                cmd_printf("✓ Found in local cache.");
                            } else {
                                cmd_printf("✓ Fetched from Auth Server!");
                            }
                        } else {
                             cmd_printf("Error: Key ID not found (Local or Auth).");
                        }

                        // Update UI immediately
                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                        
                        uart_state = 0;
                    }
                    else if (byte == MSG_TYPE_ENCRYPTED || byte == MSG_TYPE_FILE) {
                        uint8_t packet_type = byte; 
                        stats.total_pkts++;
                        
                        // Read length (2 bytes)
                        uint8_t len_bytes[2];
                        if (read_exact_timeout(fd, len_bytes, 2, 100) != 2) {
                            log_printf("Failed to read length\n");
                            uart_state = 0;
                            continue;
                        }
                        
                        // Length = NONCE + CIPHERTEXT + TAG
                        uint16_t payload_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                        if (payload_len < NONCE_SIZE + TAG_SIZE || payload_len > MAX_MSG_LEN) {
                            log_printf("Invalid payload length: %u bytes\n", payload_len);
                            uart_state = 0;
                            continue;
                        }
                        
                        // Calculate ciphertext length
                        uint16_t ctext_len = payload_len - NONCE_SIZE - TAG_SIZE;
                        
                        uint8_t nonce[NONCE_SIZE];
                        uint8_t ciphertext[ctext_len];
                        uint8_t tag[TAG_SIZE];
                        uint8_t crc_bytes[2];
                        
                        // Read all payload components with timeout (give enough time for full payload)
                        int chunk_timeout = 200; // ms
                        ssize_t nonce_read = read_exact_timeout(fd, nonce, NONCE_SIZE, chunk_timeout);
                        ssize_t cipher_read = read_exact_timeout(fd, ciphertext, ctext_len, chunk_timeout + (ctext_len/10)); // Scaling timeout for size
                        ssize_t tag_read = read_exact_timeout(fd, tag, TAG_SIZE, chunk_timeout);
                        ssize_t crc_read = read_exact_timeout(fd, crc_bytes, 2, chunk_timeout);
                        
                        if (nonce_read != NONCE_SIZE || cipher_read != (ssize_t)ctext_len ||
                            tag_read != TAG_SIZE || crc_read != 2) {
                            log_printf("Read fail: nonce=%zd/%d, cipher=%zd/%u, tag=%zd/%d, crc=%zd/2\n",
                                       nonce_read, NONCE_SIZE, cipher_read, ctext_len, 
                                       tag_read, TAG_SIZE, crc_read);
                            uart_state = 0;
                            continue;
                        }
                                                
                        // Validate CRC16
                        uint8_t crc_buf[1 + 2 + NONCE_SIZE + MAX_MSG_LEN + TAG_SIZE];
                        size_t crc_idx = 0;
                        crc_buf[crc_idx++] = packet_type;
                        crc_buf[crc_idx++] = len_bytes[0];
                        crc_buf[crc_idx++] = len_bytes[1];
                        memcpy(&crc_buf[crc_idx], nonce, NONCE_SIZE); crc_idx += NONCE_SIZE;
                        memcpy(&crc_buf[crc_idx], ciphertext, ctext_len); crc_idx += ctext_len;
                        memcpy(&crc_buf[crc_idx], tag, TAG_SIZE); crc_idx += TAG_SIZE;
                        
                        uint16_t computed_crc = crc16_ccitt(crc_buf, crc_idx);
                        uint16_t received_crc = ((uint16_t)crc_bytes[0] << 8) | crc_bytes[1];
                        
                        if (computed_crc != received_crc) {
                            log_printf("CRC16 mismatch! computed=0x%04X received=0x%04X\n", 
                                       computed_crc, received_crc);
                            
                            // Dump failed packet to debug log for analysis
                            FILE *f = fopen("receiver_debug.log", "a");
                            if (f) {
                                fprintf(f, "CRC FAIL: Comp:0x%04X Recv:0x%04X Len:%u\nPayload: ", 
                                        computed_crc, received_crc, payload_len);
                                for(size_t i=0; i<crc_idx; i++) fprintf(f, "%02X ", crc_buf[i]);
                                fprintf(f, "\n");
                                fclose(f);
                            }
                            
                            stats.decrypt_fail++;
                            uart_state = 0;
                            continue;
                        }
                        
                        // --- Nonce Replay Check ---
                        if (replay_window_seen(&rwin, nonce)) {
                            log_printf("Nonce replayed! Rejecting message.\\n");
                            stats.replay_blocked++;
                            uart_state = 0;
                            continue;
                        }
                        replay_window_add(&rwin, nonce);
                        
                        uint8_t decrypted[ctext_len + 1];  // for null-terminator

                        if (!key_valid) {  // Skip decryption if key was
                                           // cleared and not yet rotated
                            log_printf(
                                "No valid session key. Rejecting encrypted "
                                "message.\\n");
                            uart_state = 0;
                            continue;
                        }

                        int ret = sst_decrypt_gcm(s_key.cipher_key, nonce,
                                                  ciphertext, ctext_len, tag,
                                                  decrypted);

                        if (ret == 0) {  // Successful decryption
                            decrypted[ctext_len] = '\0';  // Null-terminate

                                // Handle File Transfer
                                if (packet_type == MSG_TYPE_FILE) {
                                    heatshrink_decoder *hsd = heatshrink_decoder_alloc(256, 8, 4);
                                    if (hsd) {
                                        size_t sunk = 0;
                                        heatshrink_decoder_sink(hsd, decrypted, ctext_len, &sunk);
                                        
                                        // Increased buffer for large files
                                        uint8_t decompressed[16384];
                                        size_t total_decomp = 0;
                                        HSD_poll_res pres;
                                        
                                        // First poll to get initial output
                                        do {
                                            size_t p = 0;
                                            pres = heatshrink_decoder_poll(hsd, &decompressed[total_decomp], 
                                                                           sizeof(decompressed) - total_decomp, &p);
                                            total_decomp += p;
                                        } while (pres == HSDR_POLL_MORE && total_decomp < sizeof(decompressed));
                                        
                                        // Finish decoder and poll remaining output
                                        heatshrink_decoder_finish(hsd);
                                        do {
                                            size_t p = 0;
                                            pres = heatshrink_decoder_poll(hsd, &decompressed[total_decomp], 
                                                                           sizeof(decompressed) - total_decomp, &p);
                                            total_decomp += p;
                                        } while (pres == HSDR_POLL_MORE && total_decomp < sizeof(decompressed));
                                        
                                        heatshrink_decoder_free(hsd);
                                        
                                        // Null terminate for safer printing (if text)
                                        if (total_decomp < sizeof(decompressed)) decompressed[total_decomp] = '\0';
                                        
                                        log_printf("[FILE] Decompressed %u -> %zu bytes\n", ctext_len, total_decomp);
                                        log_printf("[FILE] Content: %s\n", decompressed);

                                        FILE *f_out = fopen("received_file.txt", "a");
                                        if (f_out) {
                                            if (total_decomp > 0) {
                                                fwrite(decompressed, 1, total_decomp, f_out);
                                                fprintf(f_out, "\n");
                                            }
                                            fclose(f_out);
                                        } else {
                                            log_printf(" (Save failed)\n");
                                        }
                                    } else {
                                        log_printf("[FILE] Decompression alloc failed.\n");
                                    }
                                } 
                                // Handle Normal Chat / Commands
                                else {
                                    log_printf("%s\n", decrypted);
                                    
                                    // If waiting for HMAC response, check prefix
                                    if (challenge_active && 
                                        strncmp((char*)decrypted, HMAC_RESPONSE_PREFIX, 5) == 0) {
                                        // ... HMac Verification Logic ...
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
                                            cmd_printf("\n✅ HMAC VERIFIED! Pico identity confirmed.\n");
                                        } else {
                                            cmd_printf("\n❌ HMAC FAILED! Invalid response.\n");
                                        }
                                        
                                        explicit_bzero(pending_challenge, sizeof(pending_challenge));
                                        challenge_active = false;
                                        state = STATE_IDLE;
                                    }

                                    // ... Other commands ...
                                    else if (strcmp((char*)decrypted, "I have the key") == 0) {
                                         log_printf("Pico has confirmed receiving the key.\n");
                                    }
                                    
                                    // Handle "new key -f" (Force Update)
                                    else if (strcmp((char*)decrypted, "new key -f") == 0) {
                                        cmd_printf("Received 'new key -f' command. Requesting new key...\n");

                                        free_session_key_list_t(key_list);
                                        key_list = get_session_key(sst, init_empty_session_key_list());
                                        
                                        if (!key_list || key_list->num_key == 0) {
                                            cmd_printf("Failed to fetch new session key.\n");
                                        } else {
                                            memcpy(pending_key, key_list->s_key[0].cipher_key, SESSION_KEY_SIZE);
                                            stats.keys_consumed++;
                                            cmd_hex("New Session Key (pending ACK): ", pending_key, SESSION_KEY_SIZE);
                                            key_valid = true;

                                            // Send using MSG_TYPE_KEY
                                            uint16_t klen = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE;
                                            uint8_t hdr[] = {
                                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                                MSG_TYPE_KEY,
                                                (klen >> 8) & 0xFF,
                                                klen & 0xFF
                                            };
                                            write_all(fd, hdr, sizeof(hdr));
                                            write_all(fd, key_list->s_key[0].key_id, SESSION_KEY_ID_SIZE);
                                            write_all(fd, pending_key, SESSION_KEY_SIZE);
                                            
                                            // 5ms sleep to let transmission complete
                                            usleep(5000);  
                                            
                                            cmd_printf("Sent new session key to Pico. Waiting 5s for ACK...\n");
                                            state = STATE_WAITING_FOR_ACK;
                                            clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                            state_deadline.tv_sec += 5;
                                        }
                                    }
                                    
                                    // Handle "new key" (Rate Limited Request)
                                    else if (strcmp((char*)decrypted, "new key") == 0) {
                                        time_t now = time(NULL);    
                                        if (now - last_key_req_time < KEY_UPDATE_COOLDOWN_S) {
                                            cmd_printf("Rate limit: another new key request too soon. Ignoring.\n");
                                        } else {
                                            last_key_req_time = now;
                                            cmd_printf("Received 'new key' command. Waiting 5s for 'yes' confirmation...\n");
                                            state = STATE_WAITING_FOR_YES;
                                            clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                            state_deadline.tv_sec += 5;
                                        }
                                    }
                                    
                                    // Handle key confirmation ACK
                                    else if (state == STATE_WAITING_FOR_ACK && strcmp((char*)decrypted, "ACK") == 0) {
                                        cmd_printf("ACK received. Finalizing key update.\n");
                                        memcpy(s_key.cipher_key, pending_key, SESSION_KEY_SIZE);
                                        // Also copy ID if we tracked pending ID, but for now assuming list[0] is source of truth
                                        if (key_list && key_list->num_key > 0) {
                                            memcpy(s_key.key_id, key_list->s_key[0].key_id, SESSION_KEY_ID_SIZE);
                                        }
                                        
                                        explicit_bzero(pending_key, sizeof(pending_key));
                                        cmd_hex("New key is now active: ", s_key.cipher_key, SESSION_KEY_SIZE);
                                        
                                        state = STATE_IDLE;
                                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                                    }

                                    // Handle "verify key" command - initiate HMAC challenge
                                    else if (strcmp((char*)decrypted, "verify key") == 0) {
                                        cmd_printf("Initiating HMAC challenge to verify Pico has session key...\n");
                                        
                                        if (rand_bytes(pending_challenge, CHALLENGE_SIZE) != 0) {
                                            cmd_printf("Failed to generate challenge nonce.\n");
                                        } else {
                                            uint8_t header[] = {
                                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                                MSG_TYPE_CHALLENGE,
                                                (CHALLENGE_SIZE >> 8) & 0xFF,
                                                CHALLENGE_SIZE & 0xFF
                                            };
                                            write_all(fd, header, sizeof(header));
                                            write_all(fd, pending_challenge, CHALLENGE_SIZE);
                                            tcdrain(fd);
                                            
                                            state = STATE_WAITING_FOR_HMAC_RESP;
                                            clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                            state_deadline.tv_sec += 5;
                                            challenge_active = true;
                                            last_countdown = 5;
                                            
                                            cmd_print_partial("Challenge sent. Waiting for HMAC response...\n");
                                        }
                                    }
                                }
                                
                                stats.decrypt_success++;

                            } else {
                                // AES-GCM decryption failed
                                log_printf("Decryption failed: %d\n", ret);
                                stats.decrypt_fail++;
                            }

                        uart_state = 0;  // Reset uart_state machine
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