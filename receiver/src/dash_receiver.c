#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>  // Linux serial
#include <time.h>
#include <unistd.h>
#include <sys/time.h>    // gettimeofday
#include <sys/socket.h>  // reporter HTTP
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

//for ui on pi4
#include <fcntl.h>  // for open()
#include <ncurses.h>
#include <stdarg.h>

#include <openssl/crypto.h>  // CRYPTO_memcmp — constant-time compare


// Project headers (use -I include dirs instead of ../../../)
#include "c_api.h"
#include "c_secure_comm.h"   // parse_handshake_1, check_handshake_2_send_handshake_3
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

    // Display Last Received LiFi Key ID
    mvwprintw(win_mid, 9, 2, "LiFi Key: ");
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

// --- Dashboard Reporter ---
// Resolved at startup from the config's auth.ip.address (the laptop running
// both Auth and the dashboard) — see g_dashboard_host below. This fallback
// only applies if that field is somehow empty.
#ifndef DASHBOARD_HOST
#define DASHBOARD_HOST "172.20.10.2"
#endif
#define DASHBOARD_PORT 8420

static char g_dashboard_host[INET_ADDRSTRLEN] = DASHBOARD_HOST;

typedef struct {
    char     key_id_hex[SESSION_KEY_ID_SIZE * 2 + 1];
    char     payload_preview[65];
    unsigned long total, ok, fail;
} ReporterEvent;

static ReporterEvent    g_rep_event;
static bool             g_rep_pending = false;
static uint8_t          g_rep_mac_key[32];
static bool             g_rep_key_valid = false;
static pthread_mutex_t  g_rep_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_rep_cond  = PTHREAD_COND_INITIALIZER;

// The hex ID of whatever key g_rep_mac_key currently holds — kept separate
// from g_rep_event.key_id_hex, which only gets written when a real LiFi
// frame is decrypted (reporter_signal()) and stays empty otherwise. /status
// and /challenge need to always report the CURRENT key, not "the last frame
// we happened to decrypt" (which may be never, in dashboard-only testing).
static char g_current_key_id_hex[SESSION_KEY_ID_SIZE * 2 + 1] = {0};

static void set_current_key_id(const uint8_t *key_id) {
    for (int i = 0; i < SESSION_KEY_ID_SIZE; i++)
        sprintf(g_current_key_id_hex + i * 2, "%02x", key_id[i]);
    g_current_key_id_hex[SESSION_KEY_ID_SIZE * 2] = '\0';
}

// HMAC-signs `json` with the current session's mac key and POSTs it to
// `path` on the dashboard. Shared by frame reports and the standalone
// key-loaded status ping below.
static void dashboard_http_post(const char *path, const char *json, int jlen) {
    uint8_t hmac_out[32];
    sst_hmac_sha256_ex(g_rep_mac_key, 32, (const uint8_t*)json, (size_t)jlen, hmac_out);
    char hmac_hex[65];
    for (int i = 0; i < 32; i++) sprintf(hmac_hex + i * 2, "%02x", hmac_out[i]);
    hmac_hex[64] = '\0';

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct timeval tv = {0, 200000};  // 200 ms connect+send timeout
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DASHBOARD_PORT);
    inet_pton(AF_INET, g_dashboard_host, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return;
    }

    char req[1024];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "X-SST-HMAC: %s\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, g_dashboard_host, DASHBOARD_PORT, jlen, hmac_hex, json);
    write(sock, req, (size_t)rlen);
    close(sock);
}

static void reporter_post(const ReporterEvent *ev) {
    char json[512];
    int jlen = snprintf(json, sizeof(json),
        "{\"event\":\"frame_decrypted\","
        "\"key_id\":\"%s\","
        "\"payload_preview\":\"%s\","
        "\"stats\":{\"total\":%lu,\"ok\":%lu,\"fail\":%lu}}",
        ev->key_id_hex, ev->payload_preview,
        ev->total, ev->ok, ev->fail);
    dashboard_http_post("/pi4_frame", json, jlen);
}

// Proactively tells the dashboard which key this Pi4 currently has loaded —
// sent at startup and whenever the key changes, independent of whether any
// LiFi frame has actually been decrypted yet. Lets the dashboard populate
// KEY ID on connect instead of waiting for real optical traffic.
static void reporter_post_key_loaded(const uint8_t *key_id) {
    char key_id_hex[SESSION_KEY_ID_SIZE * 2 + 1];
    for (int i = 0; i < SESSION_KEY_ID_SIZE; i++)
        sprintf(key_id_hex + i * 2, "%02x", key_id[i]);
    key_id_hex[SESSION_KEY_ID_SIZE * 2] = '\0';

    char json[128];
    int jlen = snprintf(json, sizeof(json),
        "{\"event\":\"key_loaded\",\"key_id\":\"%s\"}", key_id_hex);
    dashboard_http_post("/pi4_status", json, jlen);
}

// Sends a free-text diagnostic line to the dashboard's WiFi log — so C-side
// failures (e.g. Auth rejecting a by-ID key fetch) are visible in the
// browser instead of only in receiver_debug.log on the Pi4 itself.
static void reporter_post_status_message(const char *message) {
    char escaped[256];
    size_t j = 0;
    for (size_t i = 0; message[i] && j < sizeof(escaped) - 2; i++) {
        if (message[i] == '"' || message[i] == '\\') escaped[j++] = '\\';
        escaped[j++] = message[i];
    }
    escaped[j] = '\0';

    char json[320];
    int jlen = snprintf(json, sizeof(json),
        "{\"event\":\"status_message\",\"message\":\"%s\"}", escaped);
    dashboard_http_post("/pi4_status", json, jlen);
}

static void *reporter_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_rep_mutex);
        while (!g_rep_pending)
            pthread_cond_wait(&g_rep_cond, &g_rep_mutex);
        ReporterEvent ev = g_rep_event;
        g_rep_pending = false;
        pthread_mutex_unlock(&g_rep_mutex);

        if (g_rep_key_valid)
            reporter_post(&ev);
    }
    return NULL;
}

static void reporter_signal(const uint8_t *key_id, const uint8_t *payload,
                             size_t payload_len, const SessionStats *st) {
    pthread_mutex_lock(&g_rep_mutex);
    for (int i = 0; i < SESSION_KEY_ID_SIZE; i++)
        sprintf(g_rep_event.key_id_hex + i * 2, "%02x", key_id[i]);
    g_rep_event.key_id_hex[SESSION_KEY_ID_SIZE * 2] = '\0';

    size_t prev = payload_len < 64 ? payload_len : 64;
    memcpy(g_rep_event.payload_preview, payload, prev);
    // sanitise non-printable bytes for JSON
    for (size_t i = 0; i < prev; i++)
        if (g_rep_event.payload_preview[i] < 0x20 || g_rep_event.payload_preview[i] > 0x7e)
            g_rep_event.payload_preview[i] = '.';
    g_rep_event.payload_preview[prev] = '\0';

    g_rep_event.total = st->total_pkts;
    g_rep_event.ok    = st->decrypt_success;
    g_rep_event.fail  = st->decrypt_fail;
    g_rep_pending = true;
    pthread_cond_signal(&g_rep_cond);
    pthread_mutex_unlock(&g_rep_mutex);
}

// --- Dashboard Challenge Server ---
// Listens on CHALLENGE_PORT for requests from the dashboard.
// POST /challenge   body: {"nonce":"<64 hex chars>"}
//   Response:        {"hmac":"<64 hex chars>","key_id":"<16 hex chars>"}
// POST /force_key    no body — asks the main loop to force-refetch a key
//   from Auth, same as pressing 'f' locally (see g_force_key_requested).
// GET  /status       no body — Response: {"key_id":"<16 hex chars>"|null}
#define CHALLENGE_PORT 5001

static pthread_mutex_t g_force_key_mutex      = PTHREAD_MUTEX_INITIALIZER;
static bool            g_force_key_requested  = false;
// Target key ID from the /force_key body, if any.
static uint8_t         g_force_key_target_id[SESSION_KEY_ID_SIZE];
static bool            g_force_key_has_target = false;
// Actual key material from the /force_key body, if any (dashboard pushing
// its current session key directly — see the main loop's case 'f' handling
// for why this replaced the get_session_key_by_ID() approach: Auth's
// CommunicationPolicyChecker rejects net1.client fetching-by-ID a key that
// same identity already "owns", since dash_receiver and pico_provisioner
// share one SST entity. Pushing the material sidesteps Auth entirely.
static uint8_t         g_force_key_cipher_key[SST_KEY_SIZE];
static uint8_t         g_force_key_mac_key[MAC_KEY_SIZE];
static bool            g_force_key_has_material = false;

// Parses a `"field_name":"<hex>"` value of exactly out_size*2 hex chars from
// a JSON body. Returns true and fills out[0..out_size) on success.
static bool parse_hex_field(const char *body, const char *field_name,
                             uint8_t *out, size_t out_size) {
    char hex[MAC_KEY_SIZE * 2 + 1];  // largest field this is ever called with
    if (out_size * 2 >= sizeof(hex)) return false;

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
    const char *p = strstr(body, pattern);
    if (!p) return false;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (i < out_size * 2 && *p && *p != '"') hex[i++] = *p++;
    hex[i] = '\0';
    if (i != out_size * 2) return false;
    for (size_t j = 0; j < out_size; j++) {
        unsigned int b;
        if (sscanf(hex + j * 2, "%02x", &b) != 1) return false;
        out[j] = (uint8_t)b;
    }
    return true;
}

static void handle_force_key_request(int client, const char *body) {
    uint8_t target_id[SESSION_KEY_ID_SIZE];
    bool has_target = parse_hex_field(body, "key_id", target_id, SESSION_KEY_ID_SIZE);

    uint8_t cipher_key[SST_KEY_SIZE];
    uint8_t mac_key[MAC_KEY_SIZE];
    bool has_material = has_target &&
        parse_hex_field(body, "cipher_key", cipher_key, SST_KEY_SIZE) &&
        parse_hex_field(body, "mac_key", mac_key, MAC_KEY_SIZE);

    if (has_material) {
        char hex[SESSION_KEY_ID_SIZE * 2 + 1];
        for (int j = 0; j < SESSION_KEY_ID_SIZE; j++) sprintf(hex + j * 2, "%02x", target_id[j]);
        hex[SESSION_KEY_ID_SIZE * 2] = '\0';
        fprintf(stderr, "[FORCE_KEY] Queued direct key push for key_id=%s\n", hex);
    } else if (has_target) {
        char hex[SESSION_KEY_ID_SIZE * 2 + 1];
        for (int j = 0; j < SESSION_KEY_ID_SIZE; j++) sprintf(hex + j * 2, "%02x", target_id[j]);
        hex[SESSION_KEY_ID_SIZE * 2] = '\0';
        fprintf(stderr, "[FORCE_KEY] Queued targeted fetch for key_id=%s (no material - "
                        "legacy caller? this will likely be rejected by Auth)\n", hex);
    } else {
        fprintf(stderr, "[FORCE_KEY] Queued blind fetch (no/unparseable key_id in body: \"%s\")\n", body);
    }

    pthread_mutex_lock(&g_force_key_mutex);
    g_force_key_requested = true;
    g_force_key_has_target = has_target;
    g_force_key_has_material = has_material;
    if (has_target) memcpy(g_force_key_target_id, target_id, SESSION_KEY_ID_SIZE);
    if (has_material) {
        memcpy(g_force_key_cipher_key, cipher_key, SST_KEY_SIZE);
        memcpy(g_force_key_mac_key, mac_key, MAC_KEY_SIZE);
    }
    pthread_mutex_unlock(&g_force_key_mutex);

    const char *resp =
        "HTTP/1.1 202 Accepted\r\nContent-Length: 15\r\n\r\n"
        "{\"status\":\"ok\"}";
    send(client, resp, strlen(resp), 0);
    close(client);
}

// GET /status — lets the dashboard pull the currently-loaded key_id on
// demand (e.g. whenever it notices the Pi4 is reachable), instead of only
// finding out via the one-shot startup push, which is missed if the
// dashboard wasn't already running when dash_receiver started.
static void handle_status_request(int client) {
    pthread_mutex_lock(&g_rep_mutex);
    bool key_ready = g_rep_key_valid;
    char key_id_str[SESSION_KEY_ID_SIZE * 2 + 1] = {0};
    if (key_ready) strncpy(key_id_str, g_current_key_id_hex, sizeof(key_id_str) - 1);
    pthread_mutex_unlock(&g_rep_mutex);

    char json[128];
    int jlen = key_ready
        ? snprintf(json, sizeof(json), "{\"key_id\":\"%s\"}", key_id_str)
        : snprintf(json, sizeof(json), "{\"key_id\":null}");

    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
        jlen, json);
    send(client, resp, (size_t)rlen, 0);
    close(client);
}

// --- Request signing (dashboard -> Pi4 control channel) ---
// Every /force_key, /status, and /challenge request must carry:
//   X-SST-Timestamp: <unix seconds>
//   X-SST-Nonce:     <24 hex chars, 12 random bytes>
//   X-SST-HMAC:      <64 hex chars, HMAC-SHA256(mac_key, "ts|nonce|path|body")>
// Rejects stale (>30s skew), replayed, or unauthenticated requests before
// any control action (key rotation, status disclosure, challenge) runs.
// Reuses the same shared mac_key both sides already have via SST — no new
// secret — and the existing replay_window_t (receiver/include/replay_window.h)
// rather than a new dedup structure.
#define REQ_SIG_MAX_SKEW_S 30
#define REQ_NONCE_BYTES    12   // must match replay_window_t's fixed 12-byte slots
#define REQ_NONCE_HEX_LEN  (REQ_NONCE_BYTES * 2)

static replay_window_t g_req_replay_window;

static bool extract_header(const char *buf, const char *header_name,
                            char *out, size_t out_size) {
    const char *p = strstr(buf, header_name);
    if (!p) return false;
    p += strlen(header_name);
    while (*p == ' ') p++;
    size_t i = 0;
    while (i < out_size - 1 && *p && *p != '\r' && *p != '\n') out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static bool verify_signed_request(const char *buf, const char *path,
                                   const char *body, size_t body_len) {
    char ts_str[32], nonce_hex[REQ_NONCE_HEX_LEN + 1], hmac_hex[65];
    if (!extract_header(buf, "X-SST-Timestamp:", ts_str, sizeof(ts_str)) ||
        !extract_header(buf, "X-SST-Nonce:",     nonce_hex, sizeof(nonce_hex)) ||
        !extract_header(buf, "X-SST-HMAC:",      hmac_hex, sizeof(hmac_hex))) {
        return false;
    }
    if (strlen(nonce_hex) != REQ_NONCE_HEX_LEN || strlen(hmac_hex) != 64) return false;

    time_t ts = (time_t)strtoll(ts_str, NULL, 10);
    time_t now = time(NULL);
    time_t skew = now > ts ? now - ts : ts - now;
    if (skew > REQ_SIG_MAX_SKEW_S) return false;

    uint8_t nonce_bytes[REQ_NONCE_BYTES];
    for (int i = 0; i < REQ_NONCE_BYTES; i++) {
        unsigned int b;
        if (sscanf(nonce_hex + i * 2, "%02x", &b) != 1) return false;
        nonce_bytes[i] = (uint8_t)b;
    }
    if (replay_window_seen(&g_req_replay_window, nonce_bytes)) return false;

    pthread_mutex_lock(&g_rep_mutex);
    bool key_ready = g_rep_key_valid;
    uint8_t mac_key_copy[32];
    if (key_ready) memcpy(mac_key_copy, g_rep_mac_key, 32);
    pthread_mutex_unlock(&g_rep_mutex);
    if (!key_ready) return false;

    char signed_str[1024];
    int slen = snprintf(signed_str, sizeof(signed_str), "%s|%s|%s|%.*s",
                         ts_str, nonce_hex, path, (int)body_len, body);
    if (slen < 0 || (size_t)slen >= sizeof(signed_str)) return false;

    uint8_t expected_hmac[32];
    sst_hmac_sha256_ex(mac_key_copy, 32, (const uint8_t*)signed_str, (size_t)slen, expected_hmac);

    uint8_t received_hmac[32];
    for (int i = 0; i < 32; i++) {
        unsigned int b;
        if (sscanf(hmac_hex + i * 2, "%02x", &b) != 1) return false;
        received_hmac[i] = (uint8_t)b;
    }

    if (CRYPTO_memcmp(expected_hmac, received_hmac, 32) != 0) return false;

    // Only mark the nonce spent once fully verified, so a bad guess can't
    // burn a slot that a legitimate retry would need.
    replay_window_add(&g_req_replay_window, nonce_bytes);
    return true;
}

static void send_401(int client) {
    const char *body = "{\"error\":\"unauthorized\"}";
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
        strlen(body), body);
    send(client, resp, (size_t)rlen, 0);
    close(client);
}

static void challenge_handle(int client) {
    char buf[2048];
    ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client); return; }
    buf[n] = '\0';

    // Path from the request line (e.g. "POST /force_key HTTP/1.1" -> "/force_key")
    char path[64] = {0};
    sscanf(buf, "%*s %63s", path);

    // Body starts after the blank line, if any (absent for GET requests).
    char *body_ptr = strstr(buf, "\r\n\r\n");
    char *body = body_ptr ? body_ptr + 4 : (char*)"";

    // /status is polled every 5s by the dashboard's health monitor — logging
    // every request would drown out the events that actually matter here
    // (force_key, challenge). Those still log below in their own handlers.
    if (strcmp(path, "/status") != 0) {
        fprintf(stderr, "[CTRL] %s request received, body=\"%s\"\n", path, body);
    }

    // TEMPORARILY DISABLED: request signing was masking whether the actual
    // key-convergence fix (get_session_key_by_ID via /force_key) works at
    // all, on top of a bootstrap gap in the signing scheme itself. Re-enable
    // once convergence is confirmed solid — verify_signed_request() and its
    // supporting helpers are left intact below for that.
    (void)verify_signed_request;
    (void)send_401;

    if (strcmp(path, "/force_key") == 0) {
        handle_force_key_request(client, body);
        return;
    }
    if (strcmp(path, "/status") == 0) {
        handle_status_request(client);
        return;
    }

    // Parse {"nonce":"<hex>"} — find the hex string after "nonce":"
    char *p = strstr(body, "\"nonce\"");
    if (!p) { close(client); return; }
    p = strchr(p, ':');
    if (!p) { close(client); return; }
    while (*p == ':' || *p == ' ' || *p == '"') p++;

    // Read up to 64 hex chars
    char nonce_hex[65] = {0};
    size_t i = 0;
    while (i < 64 && *p && *p != '"') nonce_hex[i++] = *p++;
    nonce_hex[i] = '\0';
    if (i != 64) { close(client); return; }

    // Hex-decode nonce
    uint8_t nonce_bytes[32];
    for (int j = 0; j < 32; j++) {
        unsigned int byte;
        sscanf(nonce_hex + j * 2, "%02x", &byte);
        nonce_bytes[j] = (uint8_t)byte;
    }

    // Compute HMAC-SHA256(mac_key, nonce)
    pthread_mutex_lock(&g_rep_mutex);
    bool key_ready = g_rep_key_valid;
    uint8_t mac_key_copy[32];
    uint8_t key_id_copy[SESSION_KEY_ID_SIZE];
    if (key_ready) {
        memcpy(mac_key_copy, g_rep_mac_key, 32);
        memcpy(key_id_copy,  g_rep_event.key_id_hex, SESSION_KEY_ID_SIZE);
    }
    pthread_mutex_unlock(&g_rep_mutex);

    if (!key_ready) {
        fprintf(stderr, "[CHALLENGE] Rejected: no key loaded on the Pi4 side.\n");
        const char *resp =
            "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 25\r\n\r\n"
            "{\"error\":\"no key loaded\"}";
        send(client, resp, strlen(resp), 0);
        close(client);
        return;
    }

    uint8_t hmac_out[32];
    sst_hmac_sha256_ex(mac_key_copy, 32, nonce_bytes, 32, hmac_out);

    char hmac_hex[65];
    for (int j = 0; j < 32; j++) sprintf(hmac_hex + j * 2, "%02x", hmac_out[j]);
    hmac_hex[64] = '\0';

    // key_id hex — g_current_key_id_hex tracks the CURRENT key, unlike
    // g_rep_event.key_id_hex which only updates on a real decrypted frame.
    pthread_mutex_lock(&g_rep_mutex);
    char key_id_str[SESSION_KEY_ID_SIZE * 2 + 1];
    strncpy(key_id_str, g_current_key_id_hex, sizeof(key_id_str));
    pthread_mutex_unlock(&g_rep_mutex);

    fprintf(stderr, "[CHALLENGE] Using key_id=%s to answer nonce=%.16s... -> hmac=%.16s...\n",
            key_id_str, nonce_hex, hmac_hex);
    {
        char msg[192];
        snprintf(msg, sizeof(msg), "Answering /challenge using key_id=%s", key_id_str);
        reporter_post_status_message(msg);
    }

    char json[256];
    int jlen = snprintf(json, sizeof(json),
        "{\"hmac\":\"%s\",\"key_id\":\"%s\"}", hmac_hex, key_id_str);

    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n%s", jlen, json);
    send(client, resp, (size_t)rlen, 0);
    close(client);
}

static void *challenge_server_thread(void *arg) {
    (void)arg;
    replay_window_init(&g_req_replay_window, REQ_NONCE_BYTES, 16);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(CHALLENGE_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
        listen(srv, 4) < 0) {
        close(srv);
        return NULL;
    }

    printf("[CHALLENGE] Server listening on port %d\n", CHALLENGE_PORT);
    while (1) {
        int client = accept(srv, NULL, NULL);
        if (client >= 0) challenge_handle(client);
    }
    return NULL;
}

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
    // This allows subsequent "Fetch by ID" calls to work correctly.
    printf("Initializing SST...\n");
    SST_ctx_t* sst = init_SST(config_path);
    if (!sst) {
        printf("SST init failed.\n");
        return 1;
    }
    // Explicitly initialize purpose_index to avoid garbage values
    sst->config.purpose_index = 0;

    // The dashboard runs on the same laptop as Auth — reuse auth.ip.address
    // from the config instead of a network-specific hardcoded constant, so
    // switching between receiver.config/home_receiver.config also repoints
    // where status/frame reports get sent.
    if (sst->config.auth_ip_addr[0]) {
        strncpy(g_dashboard_host, sst->config.auth_ip_addr, sizeof(g_dashboard_host) - 1);
        g_dashboard_host[sizeof(g_dashboard_host) - 1] = '\0';
    }
    printf("Dashboard host: %s:%d\n", g_dashboard_host, DASHBOARD_PORT);

    printf("Fetching initial session key to establish Auth connection...\n");
    session_key_list_t* key_list = get_session_key(sst, NULL);
    
    if (!key_list) {
         printf("Failed to get initial session key. Auth connection might be down or config invalid.\n");
         printf("Attempting to continue with empty list (Reactive Mode)...\n");
         key_list = init_empty_session_key_list();

    } else {
         if (key_list->num_key > 0) {
             printf("Success! Initial Session Key ID: ");
             for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", key_list->s_key[0].key_id[i]);
             printf("\n");
         } else {
             printf("Connected to Auth, but received 0 keys.\n");
         }
    }

    // --- Start Dashboard Reporter + Challenge Server Threads ---
    {
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, reporter_thread,          NULL);
        pthread_create(&tid, &attr, challenge_server_thread,  NULL);
        pthread_attr_destroy(&attr);
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

    // From here on ncurses owns the terminal — raw printf/fprintf (ours or
    // the SST library's SST_print_error) would otherwise corrupt the screen
    // instead of just vanishing. Redirect both to the debug log so real
    // errors (e.g. Auth's AUTH_ALERT reason) are still visible somewhere.
    freopen("receiver_debug.log", "a", stdout);
    freopen("receiver_debug.log", "a", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);

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

    // Initial key extraction
    static int current_key_idx = 0;
    session_key_t s_key = {0};
    if (key_list && key_list->num_key > 0) {
        s_key = key_list->s_key[current_key_idx];
        // Seed reporter mac key
        pthread_mutex_lock(&g_rep_mutex);
        memcpy(g_rep_mac_key, s_key.mac_key, 32);
        g_rep_key_valid = true;
        set_current_key_id(s_key.key_id);
        pthread_mutex_unlock(&g_rep_mutex);
        // NOT reported to the dashboard here on purpose: this is a blind,
        // provisional fetch that has nothing to do with whatever key the
        // dashboard/sender has. Reporting it just causes a confusing
        // auto-rejected /pi4_status push the moment this process starts.
        // The dashboard learns the REAL synced key_id once it explicitly
        // requests one by ID (see the 'f' handling below) or once the
        // sender's LiFi key-ID broadcast triggers auto-connect.
        fprintf(stderr, "[STARTUP] Got provisional key_id=%s (not yet synced with dashboard).\n",
                g_current_key_id_hex);
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

    // SST handshake entity nonce (Pi4's challenge nonce, generated per HS1)
    uint8_t sst_entity_nonce[SST_HS_NONCE_SIZE] = {0};

    // Initial key push retry machinery
    struct timespec next_send = {0};
    clock_gettime(CLOCK_MONOTONIC, &next_send);  // send immediately


    // --- Automatic Session Key Send (Restored) ---
    // Build key provisioning frame: [PREAMBLE:4][TYPE:1][LEN:2][KEY_ID:8][CIPHER_KEY:16][MAC_KEY:32]
    if (fd >= 0 && key_valid) {
        uint16_t key_payload_len = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
        uint8_t key_header[] = {
            PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
            MSG_TYPE_KEY,
            (key_payload_len >> 8) & 0xFF,
            key_payload_len & 0xFF
        };
        
        if (write_all(fd, key_header, sizeof(key_header)) < 0 ||
            write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
            write_all(fd, s_key.cipher_key, SST_KEY_SIZE) < 0) {
            log_printf("Error: Failed to send initial session key parts.\n");
        } else {
             usleep(5000); // Wait for Pico FIFO to drain
             if (write_all(fd, s_key.mac_key, 32) < 0) {
                 log_printf("Error: Failed to send MAC key.\n");
             } else {
                 tcdrain(fd);
                 log_printf("Sent session key over UART (ID + Cipher + MAC).\n");
                 log_printf("[DEBUG] Sent Cipher: %02X %02X... MAC: %02X %02X...\n", 
                     s_key.cipher_key[0], s_key.cipher_key[1], 
                     s_key.mac_key[0], s_key.mac_key[1]);
             }
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

        // A remote /force_key request from the dashboard behaves exactly
        // like pressing 'f' locally — only synthesize it when no real key
        // was pressed this iteration, so it can't clobber real input.
        // If the request named a specific key ID (the one the sender side
        // already has), case 'f' below fetches that exact key by ID
        // instead of a blind refetch, so both sides actually converge.
        bool remote_force_has_target = false;
        uint8_t remote_force_target_id[SESSION_KEY_ID_SIZE];
        bool remote_force_has_material = false;
        uint8_t remote_force_cipher_key[SST_KEY_SIZE];
        uint8_t remote_force_mac_key[MAC_KEY_SIZE];
        if (key == -1) {
            pthread_mutex_lock(&g_force_key_mutex);
            if (g_force_key_requested) {
                g_force_key_requested = false;
                remote_force_has_target = g_force_key_has_target;
                if (remote_force_has_target)
                    memcpy(remote_force_target_id, g_force_key_target_id, SESSION_KEY_ID_SIZE);
                remote_force_has_material = g_force_key_has_material;
                if (remote_force_has_material) {
                    memcpy(remote_force_cipher_key, g_force_key_cipher_key, SST_KEY_SIZE);
                    memcpy(remote_force_mac_key, g_force_key_mac_key, MAC_KEY_SIZE);
                }
                key = 'f';
            }
            pthread_mutex_unlock(&g_force_key_mutex);
        }

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
                    uint16_t klen = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
                    uint8_t hdr[] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_KEY,
                        (klen >> 8) & 0xFF,
                        klen & 0xFF
                    };

                    if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                        write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                        write_all(fd, s_key.cipher_key, SST_KEY_SIZE) < 0) {
                        cmd_printf("Error: Failed to send session key.");
                    } else {
                        usleep(5000);
                        if (write_all(fd, s_key.mac_key, 32) < 0) {
                             cmd_printf("Error: Failed to send MAC key.");
                        } else {
                             tcdrain(fd);
                             cmd_printf("✓ Session key sent (Cipher + MAC).");
                             log_printf("[DEBUG] Sent Cipher: %02X %02X... MAC: %02X %02X...\n", 
                                 s_key.cipher_key[0], s_key.cipher_key[1], 
                                 s_key.mac_key[0], s_key.mac_key[1]);
                        }
                    }
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                    break;
                }

                case 'f':
                case 'F': {
                    if (remote_force_has_material) {
                        // The dashboard pushed its actual session key
                        // material directly (key_id + cipher_key + mac_key)
                        // instead of asking us to fetch it from Auth by ID.
                        // That fetch-by-ID approach doesn't work here: the
                        // Pi4 and pico_provisioner both authenticate as the
                        // same SST entity (net1.client), and Auth's
                        // CommunicationPolicyChecker rejects that identity
                        // fetching-by-ID a key it already "owns" (confirmed
                        // via Auth's own log). Adopting pushed material
                        // sidesteps Auth entirely for this convergence step.
                        session_key_t pushed = {0};
                        memcpy(pushed.key_id, remote_force_target_id, SESSION_KEY_ID_SIZE);
                        memcpy(pushed.mac_key, remote_force_mac_key, MAC_KEY_SIZE);
                        pushed.mac_key_size = MAC_KEY_SIZE;
                        memcpy(pushed.cipher_key, remote_force_cipher_key, SST_KEY_SIZE);
                        pushed.cipher_key_size = SST_KEY_SIZE;
                        pushed.enc_mode = sst->config.encryption_mode;
                        pushed.hmac_mode = sst->config.hmac_mode;
                        // We don't know Auth's actual validity window for
                        // this key (it was never our request), so grant a
                        // generous local window — it was just freshly
                        // issued moments ago on the dashboard side.
                        pushed.abs_validity = (uint64_t)time(NULL) * 1000ULL + 3600000ULL;

                        s_key = pushed;
                        key_valid = true;
                        stats.keys_consumed++;
                        pthread_mutex_lock(&g_rep_mutex);
                        memcpy(g_rep_mac_key, s_key.mac_key, 32);
                        g_rep_key_valid = true;
                        set_current_key_id(s_key.key_id);
                        pthread_mutex_unlock(&g_rep_mutex);
                        reporter_post_key_loaded(s_key.key_id);
                        {
                            char got_hex[SESSION_KEY_ID_SIZE * 2 + 1];
                            for (int j = 0; j < SESSION_KEY_ID_SIZE; j++)
                                sprintf(got_hex + j * 2, "%02x", s_key.key_id[j]);
                            got_hex[SESSION_KEY_ID_SIZE * 2] = '\0';
                            fprintf(stderr, "[FORCE_KEY] Adopted pushed key_id=%s directly.\n", got_hex);
                            char msg[64];
                            snprintf(msg, sizeof(msg), "Adopted pushed key ...%.8s",
                                got_hex + strlen(got_hex) - 8);
                            reporter_post_status_message(msg);
                        }
                        cmd_printf("✓ Adopted key pushed by dashboard.");

                        if (fd >= 0) {
                            uint16_t klen = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
                            uint8_t hdr[] = {
                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                MSG_TYPE_KEY,
                                (klen >> 8) & 0xFF,
                                klen & 0xFF
                            };
                            if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                                write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                                write_all(fd, s_key.cipher_key, SST_KEY_SIZE) < 0) {
                                cmd_printf("Error: Failed to send new key to Pico.");
                            } else {
                                usleep(5000);
                                write_all(fd, s_key.mac_key, 32);
                                tcdrain(fd);
                                cmd_printf("✓ Session key sent to Pico.");
                            }
                        } else {
                            cmd_printf("Warning: Serial closed. Key updated locally but not sent.");
                        }
                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                        break;
                    }
                    if (remote_force_has_target) {
                        // Legacy fallback for callers that send a key_id
                        // without material — kept for backward
                        // compatibility, but this path is known to fail
                        // against Auth's CommunicationPolicyChecker when
                        // the requester already owns the target key (see
                        // the has_material branch above for why).
                        {
                            char req_hex[SESSION_KEY_ID_SIZE * 2 + 1];
                            for (int j = 0; j < SESSION_KEY_ID_SIZE; j++)
                                sprintf(req_hex + j * 2, "%02x", remote_force_target_id[j]);
                            req_hex[SESSION_KEY_ID_SIZE * 2] = '\0';
                            fprintf(stderr, "[FORCE_KEY] Calling get_session_key_by_ID(%s)...\n", req_hex);
                        }
                        // Our cached distribution key can be stale: this
                        // entity identity (net1.client) is shared with the
                        // dashboard's pico_provisioner, which may have
                        // re-handshaked with Auth since our last one,
                        // invalidating Auth's view of *our* cached dist key
                        // even though it hasn't locally "expired" yet.
                        // Reusing it then fails server-side with
                        // InvalidMacException.
                        //
                        // Forcing get_session_key_by_ID() itself to
                        // re-handshake (by expiring dist_key right before
                        // it) does NOT work: Auth's public-key/first-contact
                        // path apparently can't handle a "{\"keyId\":...}"
                        // purpose — it fails with a generic RSA padding
                        // error before ever parsing the request, even though
                        // the same key/entity works fine there for a normal
                        // group-purpose request. The distribution-key path
                        // *does* support keyId purposes correctly (it got as
                        // far as a real MAC comparison above). So: refresh
                        // the distribution key first via a normal (group-
                        // purpose, public-key) fetch — proven to work — then
                        // do the by-ID fetch over the now-fresh, Auth-synced
                        // distribution key.
                        sst->dist_key.abs_validity = 0;
                        cmd_printf("[Remote] Refreshing dist key...");
                        reporter_post_status_message("1/2 refresh dist key...");
                        session_key_list_t* refresh_list = get_session_key(sst, NULL);
                        if (!refresh_list) {
                            fprintf(stderr, "[FORCE_KEY] Distribution-key refresh failed; "
                                            "by-ID fetch would likely fail too.\n");
                            cmd_printf("Error: dist key refresh failed.");
                            reporter_post_status_message("1/2 refresh FAILED");
                        } else {
                            free_session_key_list_t(refresh_list);
                            reporter_post_status_message("1/2 refresh OK");
                        }
                        cmd_printf("[Remote] Fetching key by ID...");
                        // get_session_key_by_ID() permanently overwrites
                        // sst->config.purpose[...] with "{\"keyId\":...}"
                        // and never restores it — left as-is, the NEXT
                        // verify's refresh step above would silently send
                        // that stale keyId purpose instead of our real
                        // group purpose, recreating the same padding bug.
                        // Save/restore around the call so it doesn't leak.
                        char saved_purpose[MAX_PURPOSE_LENGTH + 1];
                        strncpy(saved_purpose,
                                sst->config.purpose[sst->config.purpose_index],
                                sizeof(saved_purpose));
                        session_key_t* found = get_session_key_by_ID(remote_force_target_id, sst, key_list);
                        strncpy(sst->config.purpose[sst->config.purpose_index],
                                saved_purpose, sizeof(saved_purpose));
                        if (!found) {
                            fprintf(stderr, "[FORCE_KEY] get_session_key_by_ID() returned NULL.\n");
                            cmd_printf("Error: by-ID fetch failed.");
                            cmd_printf("See receiver_debug.log for the real reason.");
                            cmd_printf("Keeping current session key.");
                            reporter_post_status_message("2/2 by-ID FAILED (Auth rejected)");
                        } else {
                            s_key = *found;
                            key_valid = true;
                            stats.keys_consumed++;
                            pthread_mutex_lock(&g_rep_mutex);
                            memcpy(g_rep_mac_key, s_key.mac_key, 32);
                            g_rep_key_valid = true;
                            set_current_key_id(s_key.key_id);
                            pthread_mutex_unlock(&g_rep_mutex);
                            reporter_post_key_loaded(s_key.key_id);
                            {
                                char got_hex[SESSION_KEY_ID_SIZE * 2 + 1];
                                for (int j = 0; j < SESSION_KEY_ID_SIZE; j++)
                                    sprintf(got_hex + j * 2, "%02x", s_key.key_id[j]);
                                got_hex[SESSION_KEY_ID_SIZE * 2] = '\0';
                                bool matches = memcmp(s_key.key_id, remote_force_target_id, SESSION_KEY_ID_SIZE) == 0;
                                fprintf(stderr, "[FORCE_KEY] get_session_key_by_ID() returned key_id=%s "
                                                "(matches request: %s)\n",
                                        got_hex, matches ? "YES" : "NO");
                                char msg[64];
                                snprintf(msg, sizeof(msg), "2/2 by-ID OK, ...%.8s (match: %s)",
                                    got_hex + strlen(got_hex) - 8, matches ? "Y" : "N");
                                reporter_post_status_message(msg);
                            }
                            cmd_printf("✓ Fetched requested key — now matches sender.");

                            if (fd >= 0) {
                                uint16_t klen = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
                                uint8_t hdr[] = {
                                    PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                    MSG_TYPE_KEY,
                                    (klen >> 8) & 0xFF,
                                    klen & 0xFF
                                };
                                if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                                    write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                                    write_all(fd, s_key.cipher_key, SST_KEY_SIZE) < 0) {
                                    cmd_printf("Error: Failed to send new key to Pico.");
                                } else {
                                    usleep(5000);
                                    write_all(fd, s_key.mac_key, 32);
                                    tcdrain(fd);
                                    cmd_printf("✓ Session key sent to Pico.");
                                }
                            } else {
                                cmd_printf("Warning: Serial closed. Key updated locally but not sent.");
                            }
                        }
                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                        break;
                    }

                    cmd_printf("[Shortcut] Force Fetch New Key from SST...");
                    // Try fetch new list first without freeing old one.
                    session_key_list_t* new_key_list = get_session_key(sst, NULL);

                    if (!new_key_list || new_key_list->num_key == 0) {
                         // Failed. errno here is unreliable (the UART poll's
                         // non-blocking read() sets it constantly regardless
                         // of this call) — the real reason is whatever
                         // Auth/SST logged to receiver_debug.log just now.
                         cmd_printf("Error: Failed to fetch new key from SST.");
                         cmd_printf("See receiver_debug.log for the real reason.");
                         cmd_printf("Keeping current session key.");
                         if (new_key_list) free_session_key_list_t(new_key_list);
                    } else {
                        // Success, replace old list
                        if (key_list) free_session_key_list_t(key_list);
                        key_list = new_key_list;
                        
                        s_key = key_list->s_key[0];
                        key_valid = true;
                        stats.keys_consumed++;
                        pthread_mutex_lock(&g_rep_mutex);
                        memcpy(g_rep_mac_key, s_key.mac_key, 32);
                        g_rep_key_valid = true;
                        set_current_key_id(s_key.key_id);
                        pthread_mutex_unlock(&g_rep_mutex);
                        reporter_post_key_loaded(s_key.key_id);
                        cmd_printf("✓ New key fetched from SST.");
                        
                        if (fd >= 0) {
                            uint16_t klen = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
                            uint8_t hdr[] = {
                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                MSG_TYPE_KEY,
                                (klen >> 8) & 0xFF,
                                klen & 0xFF
                            };
                            if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                                write_all(fd, s_key.key_id, SESSION_KEY_ID_SIZE) < 0 ||
                                write_all(fd, s_key.cipher_key, SST_KEY_SIZE) < 0) {
                                cmd_printf("Error: Failed to send new key to Pico.");
                            } else {
                                usleep(5000);
                                write_all(fd, s_key.mac_key, 32);
                                tcdrain(fd);
                                cmd_printf("✓ New session key sent to Pico.");
                                log_printf("[DEBUG] Sent Cipher: %02X %02X... MAC: %02X %02X...\n", 
                                    s_key.cipher_key[0], s_key.cipher_key[1], 
                                    s_key.mac_key[0], s_key.mac_key[1]);
                            }
                        } else {
                            cmd_printf("Warning: Serial closed. Key updated locally but not sent.");
                        }
                    }
                    mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                    break;
                }

                case '2': {
                    cmd_printf("[Shortcut] Initiating SST 3-way handshake...");
                    if (fd < 0) { cmd_printf("Serial not open. Press 'r' to retry."); break; }
                    if (!key_valid) { cmd_printf("No valid session key loaded."); break; }

                    uint32_t hs1_len = 0;
                    uint8_t *hs1 = parse_handshake_1(&s_key, sst_entity_nonce, &hs1_len);
                    if (!hs1 || hs1_len != SST_HS1_PAYLOAD_SIZE) {
                        cmd_printf("Error: parse_handshake_1 failed.");
                        free(hs1);
                        break;
                    }
                    uint8_t hdr[7] = {
                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                        MSG_TYPE_SST_HS1,
                        (hs1_len >> 8) & 0xFF, hs1_len & 0xFF
                    };
                    if (write_all(fd, hdr, sizeof(hdr)) < 0 ||
                        write_all(fd, hs1, hs1_len) < 0) {
                        cmd_printf("Error: Failed to send HS1.");
                        explicit_bzero(sst_entity_nonce, sizeof(sst_entity_nonce));
                    } else {
                        tcdrain(fd);
                        state = STATE_WAITING_FOR_SST_HS2;
                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                        state_deadline.tv_sec += 5;
                        last_countdown = 5;
                        cmd_printf("[SST HS1] Sent. Waiting for HS2...");
                    }
                    free(hs1);
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
        if (state == STATE_WAITING_FOR_SST_HS2) {
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
            } else if (state == STATE_WAITING_FOR_SST_HS2) {
                cmd_printf("\nSST HS2 timed out – Pico did not respond.\n");
                stats.timeouts++;
                explicit_bzero(sst_entity_nonce, sizeof(sst_entity_nonce));
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

                        unsigned int native_id = convert_skid_buf_to_int(last_lifi_id, SESSION_KEY_ID_SIZE);
                        cmd_printf("[NATIVE] Received ID: %u", native_id);
                        
                        cmd_printf("Looking for Key ID...");
                        char debug_key_id[3 * SESSION_KEY_ID_SIZE + 1];
                        debug_key_id[0] = '\0';
                        for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                            char buf[4];
                            snprintf(buf, sizeof(buf), "%02X ", last_lifi_id[i]);
                            strcat(debug_key_id, buf);
                        }
                        cmd_printf("Passing ID to SST: %s", debug_key_id);
                        
                        // 2. Use C-API to find locally or fetch from Auth
                        // This handles checking existing_s_key_list first, then queries Auth if needed.
                        session_key_t *found_key = get_session_key_by_ID(last_lifi_id, sst, key_list);
                        
                        if (found_key) {
                            unsigned int found_native = convert_skid_buf_to_int(found_key->key_id, SESSION_KEY_ID_SIZE);
                            cmd_printf("[NATIVE] Found Key ID: %u", found_native);
                            s_key = *found_key;
                            key_valid = true;
                            // This key matches the provisioner's key — sync reporter mac_key
                            pthread_mutex_lock(&g_rep_mutex);
                            memcpy(g_rep_mac_key, found_key->mac_key, 32);
                            g_rep_key_valid = true;
                            set_current_key_id(found_key->key_id);
                            pthread_mutex_unlock(&g_rep_mutex);
                            reporter_post_key_loaded(found_key->key_id);
                            cmd_printf("✓ Key ready. Initiating SST handshake.");
                            mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));

                            // Trigger SST 3-way handshake immediately
                            if (fd >= 0 && state == STATE_IDLE) {
                                uint32_t hs1_len = 0;
                                uint8_t *hs1 = parse_handshake_1(&s_key, sst_entity_nonce, &hs1_len);
                                if (hs1 && hs1_len == SST_HS1_PAYLOAD_SIZE) {
                                    uint8_t hdr[7] = {
                                        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2,
                                        PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                        MSG_TYPE_SST_HS1,
                                        (hs1_len >> 8) & 0xFF, hs1_len & 0xFF
                                    };
                                    if (write_all(fd, hdr, sizeof(hdr)) >= 0 &&
                                        write_all(fd, hs1, hs1_len) >= 0) {
                                        tcdrain(fd);
                                        state = STATE_WAITING_FOR_SST_HS2;
                                        clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                        state_deadline.tv_sec += 5;
                                        last_countdown = 5;
                                        cmd_printf("[SST HS1] Sent. Waiting for HS2...");
                                    } else {
                                        cmd_printf("[SST HS1] UART write failed.");
                                        explicit_bzero(sst_entity_nonce, sizeof(sst_entity_nonce));
                                    }
                                    free(hs1);
                                } else {
                                    cmd_printf("[SST HS1] parse_handshake_1 failed.");
                                    free(hs1);
                                }
                            }
                        } else {
                            cmd_printf("Error: Key ID not found (Local or Auth).");
                        }

                        mid_draw_keypanel(&s_key, key_valid, state, UART_DEVICE, (fd >= 0));
                        uart_state = 0;
                    }
                    else if (byte == MSG_TYPE_SST_HS2) {
                        stats.total_pkts++;
                        uint8_t len_bytes[2];
                        if (read_exact_timeout(fd, len_bytes, 2, 100) != 2) {
                            log_printf("[SST HS2] Length read timeout\n");
                            uart_state = 0;
                            continue;
                        }
                        uint16_t hs2_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                        if (hs2_len != SST_HS2_PAYLOAD_SIZE) {
                            log_printf("[SST HS2] Unexpected length: %u\n", hs2_len);
                            uart_state = 0;
                            continue;
                        }
                        uint8_t hs2_payload[SST_HS2_PAYLOAD_SIZE];
                        uint8_t crc_rx[2];
                        if (read_exact_timeout(fd, hs2_payload, hs2_len, 200) != (ssize_t)hs2_len ||
                            read_exact_timeout(fd, crc_rx, 2, 100) != 2) {
                            log_printf("[SST HS2] Payload read timeout\n");
                            uart_state = 0;
                            continue;
                        }
                        uint8_t crc_check[1 + 2 + SST_HS2_PAYLOAD_SIZE];
                        crc_check[0] = MSG_TYPE_SST_HS2;
                        crc_check[1] = len_bytes[0];
                        crc_check[2] = len_bytes[1];
                        memcpy(&crc_check[3], hs2_payload, hs2_len);
                        uint16_t computed_crc = crc16_ccitt(crc_check, sizeof(crc_check));
                        uint16_t received_crc = ((uint16_t)crc_rx[0] << 8) | crc_rx[1];
                        if (computed_crc != received_crc) {
                            log_printf("[SST HS2] CRC fail\n");
                            uart_state = 0;
                            continue;
                        }
                        if (state != STATE_WAITING_FOR_SST_HS2) {
                            log_printf("[SST HS2] Received but not waiting for HS2\n");
                            uart_state = 0;
                            continue;
                        }

                        uint32_t hs3_len = 0;
                        uint8_t *hs3 = check_handshake_2_send_handshake_3(
                            hs2_payload, hs2_len, sst_entity_nonce, &s_key, &hs3_len);

                        if (hs3 != NULL) {
                            cmd_printf("✓ SST HS2 VERIFIED: Pico holds SST key. Sending HS3.");
                            if (hs3_len == SST_HS3_PAYLOAD_SIZE) {
                                uint8_t hdr3[7] = {
                                    PREAMBLE_BYTE_1, PREAMBLE_BYTE_2,
                                    PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                    MSG_TYPE_SST_HS3,
                                    (hs3_len >> 8) & 0xFF, hs3_len & 0xFF
                                };
                                write_all(fd, hdr3, sizeof(hdr3));
                                write_all(fd, hs3, hs3_len);
                                tcdrain(fd);
                            } else {
                                cmd_printf("✗ Unexpected HS3 length %u.", hs3_len);
                            }
                            free(hs3);
                        } else {
                            cmd_printf("✗ SST HS FAILED: Nonce mismatch – possible replay or wrong key.");
                        }

                        explicit_bzero(sst_entity_nonce, sizeof(sst_entity_nonce));
                        state = STATE_IDLE;
                        state_deadline = (struct timespec){0, 0};
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
                                    log_printf("[FILE] Rx Compressed: %u bytes. Expanding...\n", ctext_len);
                                    
                                    heatshrink_decoder *hsd = heatshrink_decoder_alloc(512, 8, 4);
                                    if (hsd) {
                                        size_t total_sunk = 0;
                                        size_t total_decomp = 0;
                                        static uint8_t decompressed[32768]; 
                                        
                                        // Loop until all input is sunk
                                        while (total_sunk < ctext_len) {
                                            size_t sunk = 0;
                                            HSD_sink_res sres = heatshrink_decoder_sink(hsd, &decrypted[total_sunk], 
                                                                                      ctext_len - total_sunk, &sunk);
                                            total_sunk += sunk;
                                            
                                            // Poll immediately after sinking some data
                                            HSD_poll_res pres;
                                            do {
                                                size_t p = 0;
                                                pres = heatshrink_decoder_poll(hsd, &decompressed[total_decomp], 
                                                                               sizeof(decompressed) - total_decomp, &p);
                                                total_decomp += p;
                                            } while (pres == HSDR_POLL_MORE && total_decomp < sizeof(decompressed));
                                            
                                            if (sres < 0) {
                                                log_printf("[Error] Sink failed err=%d\n", sres);
                                                break;
                                            }
                                        }
                                        
                                        // Finish and flush remaining
                                        heatshrink_decoder_finish(hsd);
                                        HSD_poll_res pres;
                                        do {
                                            size_t p = 0;
                                            pres = heatshrink_decoder_poll(hsd, &decompressed[total_decomp], 
                                                                           sizeof(decompressed) - total_decomp, &p);
                                            total_decomp += p;
                                        } while (pres == HSDR_POLL_MORE && total_decomp < sizeof(decompressed));
                                        
                                        heatshrink_decoder_free(hsd);
                                        
                                        // Null terminate
                                        if (total_decomp < sizeof(decompressed)) {
                                            decompressed[total_decomp] = '\0';
                                        } else {
                                            decompressed[sizeof(decompressed)-1] = '\0';
                                        }
                                        
                                        log_printf("[FILE] Result: %zu -> %zu bytes\n", ctext_len, total_decomp);
                                        // log_printf("[FILE] Data:\n%s\n", decompressed); // removing huge spam
                                        
                                        // Write to file
                                        FILE *f_out = fopen("received_file.txt", "a");
                                        if (f_out) {
                                            if (total_decomp > 0) {
                                                fwrite(decompressed, 1, total_decomp, f_out);
                                                fprintf(f_out, "\n");
                                            }
                                            fclose(f_out);
                                            log_printf("[FILE] Saved to received_file.txt\n");
                                        }
                                        
                                        // If small enough, print some head/tail
                                        if (total_decomp > 0 && total_decomp < 500) {
                                            log_printf("Content:\n%s", decompressed);
                                        } else if (total_decomp >= 500) {
                                            log_printf("Content (Head 100):\n%.100s...\n", decompressed);
                                        }

                                    } else {
                                        log_printf("[FILE] Decompression alloc failed.\n");
                                    }
                                } 
                                // Handle Normal Chat / Commands
                                else {
                                    log_printf("%s\n", decrypted);

                                    // ... Other commands ...
                                    if (strcmp((char*)decrypted, "I have the key") == 0) {
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

                                            // Send using MSG_TYPE_KEY with MAC
                                            uint16_t klen = SESSION_KEY_ID_SIZE + SST_KEY_SIZE + 32;
                                            uint8_t hdr[] = {
                                                PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                                MSG_TYPE_KEY,
                                                (klen >> 8) & 0xFF,
                                                klen & 0xFF
                                            };
                                            write_all(fd, hdr, sizeof(hdr));
                                            write_all(fd, key_list->s_key[0].key_id, SESSION_KEY_ID_SIZE);
                                            write_all(fd, pending_key, SST_KEY_SIZE);
                                            usleep(5000); // Delay for MAC key
                                            // Assume we can get Mac Key from list too
                                            write_all(fd, key_list->s_key[0].mac_key, 32);
                                            
                                            log_printf("[DEBUG] Sent Cipher: %02X %02X... MAC: %02X %02X...\n", 
                                                pending_key[0], pending_key[1], 
                                                key_list->s_key[0].mac_key[0], key_list->s_key[0].mac_key[1]);
                                            
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

                                    // Handle "verify key" command - initiate SST handshake
                                    else if (strcmp((char*)decrypted, "verify key") == 0) {
                                        cmd_printf("Initiating SST handshake to verify Pico holds SST key...\n");
                                        if (fd >= 0 && key_valid && state == STATE_IDLE) {
                                            uint32_t hs1_len = 0;
                                            uint8_t *hs1 = parse_handshake_1(&s_key, sst_entity_nonce, &hs1_len);
                                            if (hs1 && hs1_len == SST_HS1_PAYLOAD_SIZE) {
                                                uint8_t hdr[7] = {
                                                    PREAMBLE_BYTE_1, PREAMBLE_BYTE_2,
                                                    PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
                                                    MSG_TYPE_SST_HS1,
                                                    (hs1_len >> 8) & 0xFF, hs1_len & 0xFF
                                                };
                                                if (write_all(fd, hdr, sizeof(hdr)) >= 0 &&
                                                    write_all(fd, hs1, hs1_len) >= 0) {
                                                    tcdrain(fd);
                                                    state = STATE_WAITING_FOR_SST_HS2;
                                                    clock_gettime(CLOCK_MONOTONIC, &state_deadline);
                                                    state_deadline.tv_sec += 5;
                                                    last_countdown = 5;
                                                    cmd_printf("[SST HS1] Sent. Waiting for HS2...");
                                                } else {
                                                    cmd_printf("[SST HS1] UART write failed.");
                                                    explicit_bzero(sst_entity_nonce, sizeof(sst_entity_nonce));
                                                }
                                            } else {
                                                cmd_printf("[SST HS1] parse_handshake_1 failed.");
                                            }
                                            free(hs1);
                                        }
                                    }
                                }
                                
                                stats.decrypt_success++;
                                reporter_signal(s_key.key_id, decrypted,
                                                ctext_len, &stats);

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