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
    FILE *f = fopen("receiver_keys_debug.log", "a");
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

    int mid_h = 14;                 // Increased height for Cipher + MAC keys
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
    mvwprintw(win_log_border, 0, 2, " KEYS / Sender Log ");
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
        wprintw(win_mid, "Cipher Key:");
        wattron(win_mid, COLOR_PAIR(3));
        
        unsigned int c_len = s_key->cipher_key_size;
        if (c_len == 0 || c_len > 32) c_len = 32;

        for (size_t i = 0; i < c_len; i++) wprintw(win_mid, "%02X ", s_key->cipher_key[i]);
        wattroff(win_mid, COLOR_PAIR(3));

        // Print MAC Key
        wmove(win_mid, 6, 2);
        wprintw(win_mid, "MAC Key:   ");
        wattron(win_mid, COLOR_PAIR(5)); // Magenta for MAC
        
        unsigned int m_len = s_key->mac_key_size;
        if (m_len == 0 || m_len > 32) m_len = 32;

        for (size_t i = 0; i < m_len; i++) {
            if (i == 16) mvwprintw(win_mid, 7, 13, "%s", ""); // Wrap
            wprintw(win_mid, "%02X ", s_key->mac_key[i]);
        }
        wattroff(win_mid, COLOR_PAIR(5));
    } else {
        mvwprintw(win_mid, 4, 2, "Key ID: (none)");
        mvwprintw(win_mid, 5, 2, "Key:    (none)");
    }

    // Shortcuts menu at bottom of mid panel
    int menu_r = h - 2;
    // Use A_DIM or just normal
    mvwprintw(win_mid, menu_r, 2, "[1] Send Key  [n] Rotate Key  [f] Force New  [s] Stats  [c] Clear  [p] Save  [q] Quit");

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
    unsigned long keys_consumed;
} SessionStats;

int main(int argc, char* argv[]) {
    SessionStats stats = {0};

    const char* config_path = NULL;

    if (argc > 2) {
        fprintf(stderr, "Error: Too many arguments.\n");
        fprintf(stderr, "Usage: %s [<path/to/receiver.config>]\n",
                argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    } else {
#ifdef DEFAULT_SST_CONFIG_PATH
        config_path = DEFAULT_SST_CONFIG_PATH;
#endif
    }

    // Resolve / chdir and pick the config filename (host-only; Pico stub is
    // no-op)
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);

    printf("Using config file: %s\n", config_path);

    // --- Init Key List (Secure Startup) ---
    // Update: Fetch a fresh key at startup to establish valid session with Auth.
    // KEY MANAGER MODE: Always fetch fresh keys
    printf("Initializing SST (Key Manager Mode)...\n");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        printf("SST init failed.\n");
        return 1;
    }
    // Explicitly initialize purpose_index to avoid garbage values
    sst->config.purpose_index = 0;

    printf("Fetching fresh session keys from Auth...\n");
    session_key_list_t* key_list = get_session_key(sst, NULL);
    
    if (!key_list) {
         printf("Failed to get initial session key. Auth connection might be down or config invalid.\n");
         printf("Attempting to continue with empty list (Reactive Mode)...\n");
         key_list = init_empty_session_key_list();
    } else {
         if (key_list->num_key > 0) {
             printf("Success! Fetched %d keys.\n", key_list->num_key);
             printf("Initial Session Key ID: ");
             for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", key_list->s_key[0].key_id[i]);
             printf("\n");
         } else {
             printf("Connected to Auth, but received 0 keys.\n");
         }
    }

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
    }

    // Initial key extraction
    static int current_key_idx = 0;
    session_key_t s_key = {0}; 
    if (key_list && key_list->num_key > 0) {
        s_key = key_list->s_key[current_key_idx];
    }
    
    bool key_valid = (key_list && key_list->num_key > 0);
    receiver_state_t state = STATE_IDLE;

    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));

    // --- Receiver state + replay window ---
    struct timespec state_deadline = (struct timespec){0, 0};
    
    const uint8_t preamble[4] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4};

    // --- Automatic Session Key Send --
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

    log_printf("Key Manager Ready. Waiting for commands...\n");
    if (fd >= 0) tcflush(fd, TCIFLUSH);

    // KEY MANAGER LOOP: Only handle keyboard, no LiFi RX
    while (1) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

        // --- Handle Keyboard Shortcuts ---
        int key = getch();
        if (key == ERR) key = -1;

        if (key != -1) {
            switch (key) {
                // ... (previous cases) ...
                case 'n':
                case 'N': {
                    if (key_list && key_list->num_key > 1) {
                        current_key_idx = (current_key_idx + 1) % key_list->num_key;
                        s_key = key_list->s_key[current_key_idx];
                        
                        cmd_printf("Rotated to Local Key #%d (Total: %d)", current_key_idx + 1, key_list->num_key);
                        unsigned int nid = convert_skid_buf_to_int(s_key.key_id, SESSION_KEY_ID_SIZE);
                        cmd_printf("Active Key ID: %u", nid);
                        
                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                    } else {
                        cmd_printf("Cannot rotate: Only 1 key in local list.");
                    }
                    break;
                }

                case '1': {
                    cmd_printf("[Shortcut] Sending session key to Pico...");
                    if (fd < 0) { cmd_printf("Serial not open. Press 'r' to retry."); break; }
                    if (!key_valid) { cmd_printf("No valid session key loaded."); break; }

                    // Build key provisioning frame: [PREAMBLE:4][TYPE:1][LEN:2][ID][CIPHER][MAC]
                    uint16_t klen = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE + 32;
                    uint8_t hdr[] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_KEY,
                        (klen >> 8) & 0xFF,
                        klen & 0xFF
                    };

                    if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                        write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                        write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0 ||
                        write_all(fd, s_key.mac_key, 32) < 0) {
                        cmd_printf("Error: Failed to send session key.");
                    } else {
                        tcdrain(fd);
                        cmd_printf("✓ Session key sent (Cipher + MAC).");
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
                            // [ID:8][CIPHER:16][MAC:32]
                            uint16_t klen = SESSION_KEY_ID_SIZE + SESSION_KEY_SIZE + 32;
                            uint8_t hdr[] = {
                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                MSG_TYPE_KEY,
                                (klen >> 8) & 0xFF,
                                klen & 0xFF 
                            };

                            if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                                write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                                write_all(fd, s_key.cipher_key, SESSION_KEY_SIZE) < 0 ||
                                write_all(fd, s_key.mac_key, 32) < 0) {
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

                case 's':
                case 'S': {
                    cmd_printf("--- Session Statistics ---");
                    cmd_printf("Packets RX:      %lu", stats.total_pkts);
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
                    // ... (same as original)
                    if (f) {
                        time_t now = time(NULL);
                        char *tstr = ctime(&now);
                         if (tstr && strlen(tstr) > 0) tstr[strlen(tstr)-1] = '\0';
                        fprintf(f, "[%s] Stats Snapshot\n", tstr ? tstr : "Unknown");
                        fclose(f);
                        cmd_printf("Stats saved to session_stats.txt");
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
        usleep(1000); // 1ms
    }

    close(fd);
    free_session_key_list_t(key_list);
    free_SST_ctx_t(sst);
    return 0;
}
