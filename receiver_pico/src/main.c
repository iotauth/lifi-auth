#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "lifi_rx.pio.h"
#include "../../include/protocol.h"
#include "../../include/crc16.h"
#include "../../include/sst_crypto_embedded.h"

#define RX_PIN    27

typedef enum {
    STATE_HUNT = 0,
    STATE_PRE1,
    STATE_PRE2,
    STATE_PRE3,
    STATE_PAYLOAD
} rx_state_t;

static PIO        pio          = pio0;
static uint       sm           = 0;
static uint32_t   current_baud = 100000;
static char       buf[512];
static int        buf_idx      = 0;
static rx_state_t state        = STATE_HUNT;
static uint32_t   msg_count    = 0;
static bool       raw_mode     = false;

// Auto-benchmark state
static bool       test_active  = false;
static uint32_t   test_recv    = 0;
static uint32_t   test_baud    = 0;

static char cmd[64];
static int  cmd_idx = 0;

// Session key for on-device decrypt, loaded over USB via "key <hex>".
// RAM-only by design: the dashboard re-pushes it on every RX (re)connect
// and whenever a new key is provisioned, so this debug board never holds
// a second, potentially-stale copy of the key in flash.
static uint8_t session_key[SST_KEY_SIZE];
static bool    key_loaded = false;

// Nonce replay window — mirrors dash_receiver.c's `rwin` (the LiFi data-frame
// instance, cap = NONCE_HISTORY_SIZE = 64; NOT the separate 16-slot window
// that guards the WiFi control channel). Plain FIFO ring buffer: exact-match
// scan across all `replay_cap` slots, oldest slot silently overwritten on
// wraparound — no explicit eviction step, matching the reference exactly.
#define REPLAY_CAP_MAX 64
static uint8_t  replay_buf[REPLAY_CAP_MAX][NONCE_SIZE];
static uint32_t replay_idx = 0;
static uint32_t replay_cap = REPLAY_CAP_MAX;

static bool replay_seen(const uint8_t *nonce) {
    for (uint32_t i = 0; i < replay_cap; i++) {
        if (memcmp(replay_buf[i], nonce, NONCE_SIZE) == 0) return true;
    }
    return false;
}

static void replay_add(const uint8_t *nonce) {
    memcpy(replay_buf[replay_idx], nonce, NONCE_SIZE);
    replay_idx = (replay_idx + 1) % replay_cap;
}

// Snapshot of the last fully-processed frame, for the "replay" console
// command — re-injects identical already-captured bytes through the same
// pipeline with no new hardware (thesis_plan.md's approved substitute for a
// live relay rig).
static uint8_t  last_frame[512];
static int      last_frame_len          = 0;
static uint8_t  last_frame_type         = 0;
static uint16_t last_frame_declared_len = 0;
static bool     last_frame_valid        = false;

// A separately-frozen copy of last_frame, only updated by the "pin" command.
// Needed because last_frame auto-updates on every real frame — for the
// replay boundary test we need to replay one *specific* captured frame
// after other legitimate traffic has cycled through and evicted its nonce,
// so it can't be the thing that keeps auto-updating out from under us.
static uint8_t  pinned_frame[512];
static int      pinned_frame_len          = 0;
static uint8_t  pinned_frame_type         = 0;
static uint16_t pinned_frame_declared_len = 0;
static bool     pinned_frame_valid        = false;

// Binary-frame tracking (set once the byte after the preamble is classified)
static bool     frame_type_known    = false;
static bool     frame_is_binary     = false;
static uint8_t  frame_type          = 0;
static int      len_bytes_read      = 0;
static uint16_t bin_payload_len     = 0;  // NONCE+CIPHERTEXT+TAG length, from LEN field
static uint32_t bin_bytes_remaining = 0;  // bytes still needed after LEN parsed (+CRC16)
static uint32_t frame_start_ms      = 0;

static const char *msg_type_name(uint8_t t) {
    switch (t) {
        case MSG_TYPE_ENCRYPTED:   return "ENCRYPTED";
        case MSG_TYPE_CHALLENGE:   return "CHALLENGE(legacy)";
        case MSG_TYPE_RESPONSE:    return "RESPONSE(legacy)";
        case MSG_TYPE_FILE:        return "FILE";
        case MSG_TYPE_KEY_ID_ONLY: return "KEY_ID_ONLY";
        case MSG_TYPE_SST_HS1:     return "SST_HS1";
        case MSG_TYPE_SST_HS2:     return "SST_HS2";
        case MSG_TYPE_SST_HS3:     return "SST_HS3";
        case MSG_TYPE_KEY:         return "KEY";
        default:                   return "UNKNOWN";
    }
}

// True for any byte value that is a real MSG_TYPE_* (binary, length-prefixed
// framing per lifi_session_sender.c). Anything else is treated as the legacy
// newline-terminated ASCII test protocol (pico_speed_test_sender.c).
static bool is_binary_frame_type(uint8_t t) {
    switch (t) {
        case MSG_TYPE_ENCRYPTED:
        case MSG_TYPE_CHALLENGE:
        case MSG_TYPE_RESPONSE:
        case MSG_TYPE_FILE:
        case MSG_TYPE_KEY_ID_ONLY:
        case MSG_TYPE_SST_HS1:
        case MSG_TYPE_SST_HS2:
        case MSG_TYPE_SST_HS3:
        case MSG_TYPE_KEY:
            return true;
        default:
            return false;
    }
}

static void print_hex_dump(const uint8_t *data, int len) {
    for (int i = 0; i < len; i += 16) {
        printf("  %04X: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < len) printf("%02X ", data[i + j]);
            else             printf("   ");
        }
        printf(" ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = data[i + j];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        printf("\n");
    }
}

#define DIVIDER_WIDTH 68

static void print_divider(char c) {
    for (int i = 0; i < DIVIDER_WIDTH; i++) putchar(c);
    putchar('\n');
}

// Prints a labeled hex field, wrapping at 16 bytes/line with the
// continuation aligned under the first byte column.
static void print_hex_field(const char *label, const uint8_t *data, int len) {
    printf("  %-7s: ", label);
    if (len <= 0) {
        printf("(none)\n");
        return;
    }
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < len) printf("\n           ");
    }
    printf(" (%d byte%s)\n", len, len == 1 ? "" : "s");
}

// Breaks a completed binary frame down into its protocol fields:
// [TYPE:1][LEN:2][NONCE:12][CIPHERTEXT:n][TAG:16][CRC16:2]
static void print_frame_fields(const uint8_t *buf, int total_len, uint16_t declared_len) {
    int nonce_off  = 3;
    int cipher_len = (int)declared_len - NONCE_SIZE - TAG_SIZE;
    if (cipher_len < 0) cipher_len = 0;
    int cipher_off = nonce_off + NONCE_SIZE;
    int tag_off    = cipher_off + cipher_len;
    int crc_off    = total_len - CRC16_SIZE;

    printf("  TYPE   : %02X\n", buf[0]);
    printf("  LEN    : %02X %02X (%u)\n", buf[1], buf[2], declared_len);
    print_hex_field("NONCE", &buf[nonce_off], NONCE_SIZE);
    print_hex_field("CIPHER", &buf[cipher_off], cipher_len);
    print_hex_field("TAG", &buf[tag_off], TAG_SIZE);
    printf("  CRC16  : %02X %02X\n", buf[crc_off], buf[crc_off + 1]);
}

typedef enum {
    DEC_SKIPPED_TYPE,  // not MSG_TYPE_ENCRYPTED, nothing to decode here
    DEC_REPLAY,        // nonce already seen — rejected before attempting decrypt
    DEC_NO_KEY,        // no session key loaded
    DEC_MALFORMED,     // frame fields don't add up
    DEC_FAIL,          // GCM auth/decrypt failed (wrong key or corrupted frame)
    DEC_OK,
} decrypt_result_t;

// On-device AES-GCM decrypt, using whatever key was loaded via "key <hex>".
// Only MSG_TYPE_ENCRYPTED carries a plain-text message straight from
// sst_encrypt_gcm(); MSG_TYPE_FILE is heatshrink-compressed on top of that
// and isn't decoded here. `is_replay` short-circuits before ever touching
// the ciphertext, matching dash_receiver.c's check-before-decrypt ordering.
static decrypt_result_t decrypt_frame(const uint8_t *buf, int total_len, uint16_t declared_len,
                                       uint8_t type, bool is_replay,
                                       char *out_text, size_t out_text_cap) {
    if (is_replay) {
        printf("  MESSAGE: <REPLAY REJECTED - nonce already seen>\n");
        return DEC_REPLAY;
    }
    if (type != MSG_TYPE_ENCRYPTED) {
        printf("  MESSAGE: (type 0x%02X isn't plain-encrypted text; not decoded here)\n", type);
        return DEC_SKIPPED_TYPE;
    }
    if (!key_loaded) {
        printf("  MESSAGE: <no key loaded - use: key <%d-hex-chars>>\n", SST_KEY_SIZE * 2);
        return DEC_NO_KEY;
    }

    int nonce_off  = 3;
    int cipher_len = (int)declared_len - NONCE_SIZE - TAG_SIZE;
    int cipher_off = nonce_off + NONCE_SIZE;
    int tag_off    = cipher_off + cipher_len;

    if (cipher_len < 0 || cipher_off + cipher_len + TAG_SIZE > total_len) {
        printf("  MESSAGE: <malformed frame, can't decrypt>\n");
        return DEC_MALFORMED;
    }

    static uint8_t plaintext[513];  // matches buf[512] + NUL terminator
    int ret = sst_decrypt_gcm(session_key, &buf[nonce_off], &buf[cipher_off],
                               (size_t)cipher_len, &buf[tag_off], plaintext);
    if (ret != 0) {
        printf("  MESSAGE: <decrypt failed (ret=%d) - wrong key or corrupted frame>\n", ret);
        return DEC_FAIL;
    }
    plaintext[cipher_len] = '\0';
    printf("  MESSAGE: \"%s\"\n", plaintext);
    if (out_text && out_text_cap > 0) {
        strncpy(out_text, (const char *)plaintext, out_text_cap - 1);
        out_text[out_text_cap - 1] = '\0';
    }
    return DEC_OK;
}

static const char *decrypt_result_name(decrypt_result_t d) {
    switch (d) {
        case DEC_OK:           return "ok";
        case DEC_REPLAY:       return "REPLAY";
        case DEC_NO_KEY:       return "no_key";
        case DEC_MALFORMED:    return "malformed";
        case DEC_FAIL:         return "fail";
        case DEC_SKIPPED_TYPE: default: return "skipped";
    }
}

// Handles one fully-received binary frame: CRC check -> replay check
// (reject+skip decrypt if seen) -> mark spent -> decrypt. Matches
// dash_receiver.c:2082's ordering exactly. Called both from the live
// receive path and from the "replay" console command (with replay_test=true
// so the header/log clearly mark it as a re-injection, not a fresh receipt).
static void process_complete_frame(const uint8_t *frame_buf, int frame_len, uint8_t f_type,
                                    uint16_t f_declared_len, uint32_t elapsed_ms, bool replay_test) {
    int  crc_ok    = crc16_validate(frame_buf, frame_len);
    bool is_replay = false;
    const int nonce_off = 3;

    if (crc_ok && f_type == MSG_TYPE_ENCRYPTED && frame_len >= nonce_off + NONCE_SIZE) {
        is_replay = replay_seen(&frame_buf[nonce_off]);
        if (!is_replay) replay_add(&frame_buf[nonce_off]);
    }

    print_divider('=');
    printf(" RX #%-4lu  %-10s (0x%02X)  %3d bytes  %4lu ms  CRC: %s%s\n",
           msg_count, msg_type_name(f_type), f_type,
           frame_len, elapsed_ms, crc_ok ? "OK" : "MISMATCH",
           replay_test ? "  [REPLAY-TEST]" : "");
    print_divider('-');

    char text[64] = "";
    decrypt_result_t dec = DEC_SKIPPED_TYPE;
    if (crc_ok) {
        print_frame_fields(frame_buf, frame_len, f_declared_len);
        dec = decrypt_frame(frame_buf, frame_len, f_declared_len, f_type, is_replay, text, sizeof(text));
    } else {
        printf("  (CRC mismatch - skipping field breakdown/decrypt)\n");
        print_hex_dump(frame_buf, frame_len);
    }
    print_divider('=');
    printf("\n");

    // Single-line, greppable summary for host-side tooling (e.g.
    // thesis_eval/liveness_monitor.py) — decoupled from the pretty box above
    // so that can keep changing without breaking log parsers.
    printf("[EVT] msg=%lu type=0x%02X crc=%s replay=%s decrypt=%s text=\"%s\"\n",
           msg_count, f_type, crc_ok ? "ok" : "fail",
           is_replay ? "YES" : "no", decrypt_result_name(dec), text);
    fflush(stdout);

    if (!replay_test && frame_len > 0 && (size_t)frame_len <= sizeof(last_frame)) {
        memcpy(last_frame, frame_buf, (size_t)frame_len);
        last_frame_len          = frame_len;
        last_frame_type         = f_type;
        last_frame_declared_len = f_declared_len;
        last_frame_valid        = true;
    }
}

int main() {
    stdio_init_all();
    sleep_ms(3000);  // Allow USB to enumerate

    printf("\n");
    print_divider('=');
    printf(" Pico 2 LiFi Receiver\n");
    print_divider('=');
    printf(" RX pin   : GP%d\n", RX_PIN);
    printf(" Baud     : %lu\n", current_baud);
    printf(" Preamble : 0x%02X 0x%02X 0x%02X 0x%02X\n",
           PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4);
    printf(" Commands : raw on/off | status | pintest | baud <rate> | key <hex>\n");
    printf("            replaycap <n> | replay | pin | replaypin\n");
    print_divider('=');
    printf(" Listening...\n\n");
    fflush(stdout);

    uint offset = pio_add_program(pio, &lifi_rx_program);
    float div = (float)clock_get_hz(clk_sys) / (current_baud * 8.0f);
    lifi_rx_program_init(pio, sm, offset, RX_PIN, div);

    uint32_t last_heartbeat = 0;

    while (true) {
        // Heartbeat every 2s so we know USB output is working
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_heartbeat >= 2000) {
            printf("[ALIVE] %s | baud=%lu | msgs=%lu\n",
                   raw_mode ? "RAW MODE" : "listening...", current_baud, msg_count);
            fflush(stdout);
            last_heartbeat = now;
        }

        // Non-blocking USB command input
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            if (c == '\n' || c == '\r') {
                cmd[cmd_idx] = '\0';
                cmd_idx = 0;
                if (strcmp(cmd, "raw on") == 0) {
                    raw_mode = true;
                    printf("Raw mode ON\n");
                } else if (strcmp(cmd, "raw off") == 0) {
                    raw_mode = false;
                    printf("Raw mode OFF\n");
                } else if (strcmp(cmd, "status") == 0) {
                    printf("RX: GP%d | Baud: %lu | Mode: %s | Msgs: %lu | Key: %s\n",
                           RX_PIN, current_baud, raw_mode ? "RAW" : "SST", msg_count,
                           key_loaded ? "loaded" : "none");
                } else if (strcmp(cmd, "pintest") == 0) {
                    printf("Sampling GP%d for 3s...\n", RX_PIN);
                    fflush(stdout);
                    uint32_t end = to_ms_since_boot(get_absolute_time()) + 3000;
                    uint32_t transitions = 0;
                    bool last = gpio_get(RX_PIN);
                    while (to_ms_since_boot(get_absolute_time()) < end) {
                        bool cur = gpio_get(RX_PIN);
                        if (cur != last) { transitions++; last = cur; }
                    }
                    printf("GP%d transitions in 3s: %lu (idle level: %d)\n",
                           RX_PIN, transitions, (int)gpio_get(RX_PIN));
                    fflush(stdout);
                } else if (strncmp(cmd, "key ", 4) == 0) {
                    // Accepts either a bare 32-hex-char string or the
                    // space-separated "DE AF 98 ..." format that the
                    // sender's own print_hex()/"CMD: print slot key" uses,
                    // so its output can be pasted here verbatim.
                    const char *p = cmd + 4;
                    uint8_t new_key[SST_KEY_SIZE];
                    size_t  byte_count = 0;
                    bool    ok = true;
                    while (*p != '\0' && byte_count < SST_KEY_SIZE) {
                        while (*p == ' ') p++;
                        if (*p == '\0') break;
                        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
                            ok = false;
                            break;
                        }
                        char byte_str[3] = { p[0], p[1], '\0' };
                        new_key[byte_count++] = (uint8_t)strtoul(byte_str, NULL, 16);
                        p += 2;
                    }
                    while (*p == ' ') p++;
                    if (!ok || byte_count != SST_KEY_SIZE || *p != '\0') {
                        printf("Invalid key. Expected %d hex bytes (spaces OK), got %zu valid byte(s).\n",
                               SST_KEY_SIZE, byte_count);
                    } else {
                        memcpy(session_key, new_key, SST_KEY_SIZE);
                        key_loaded = true;
                        printf("Session key loaded (%d bytes). Decryption enabled.\n", SST_KEY_SIZE);
                    }
                } else if (strncmp(cmd, "baud ", 5) == 0) {
                    uint32_t b = (uint32_t)strtoul(cmd + 5, NULL, 10);
                    if (b < 1000 || b > 4000000) {
                        printf("Invalid baud rate (1000-4000000)\n");
                    } else {
                        current_baud = b;
                        float d = (float)clock_get_hz(clk_sys) / (current_baud * 8.0f);
                        pio_sm_set_clkdiv(pio, sm, d);
                        printf("Baud set to %lu (div=%.3f)\n", current_baud, d);
                    }
                } else if (strncmp(cmd, "replaycap ", 10) == 0) {
                    uint32_t n = (uint32_t)strtoul(cmd + 10, NULL, 10);
                    if (n < 1 || n > REPLAY_CAP_MAX) {
                        printf("Invalid replay window size (1-%d)\n", REPLAY_CAP_MAX);
                    } else {
                        replay_cap = n;
                        replay_idx = 0;
                        memset(replay_buf, 0, sizeof(replay_buf));
                        printf("Replay window capacity set to %lu (cleared)\n", replay_cap);
                    }
                } else if (strcmp(cmd, "replay") == 0) {
                    if (!last_frame_valid) {
                        printf("No captured frame yet to replay.\n");
                    } else {
                        printf("Replaying last captured frame (%d bytes)...\n", last_frame_len);
                        process_complete_frame(last_frame, last_frame_len, last_frame_type,
                                                last_frame_declared_len, 0, true);
                    }
                } else if (strcmp(cmd, "pin") == 0) {
                    if (!last_frame_valid) {
                        printf("No captured frame yet to pin.\n");
                    } else {
                        memcpy(pinned_frame, last_frame, (size_t)last_frame_len);
                        pinned_frame_len          = last_frame_len;
                        pinned_frame_type         = last_frame_type;
                        pinned_frame_declared_len = last_frame_declared_len;
                        pinned_frame_valid        = true;
                        printf("Pinned frame (%d bytes) for later replay via 'replaypin'.\n", pinned_frame_len);
                    }
                } else if (strcmp(cmd, "replaypin") == 0) {
                    if (!pinned_frame_valid) {
                        printf("No pinned frame yet — use 'pin' first.\n");
                    } else {
                        printf("Replaying PINNED frame (%d bytes)...\n", pinned_frame_len);
                        process_complete_frame(pinned_frame, pinned_frame_len, pinned_frame_type,
                                                pinned_frame_declared_len, 0, true);
                    }
                } else if (strlen(cmd) > 0) {
                    printf("Unknown: '%s'\n", cmd);
                }
                fflush(stdout);
            } else if (cmd_idx < (int)sizeof(cmd) - 1) {
                cmd[cmd_idx++] = (char)c;
            }
        }

        // PIO RX
        if (pio_sm_get_rx_fifo_level(pio, sm) == 0) continue;

        uint32_t word = pio_sm_get(pio, sm);
        // Right-shift: data in bits [31:24]. Invert for reverse-biased photodiode.
        uint8_t byte = ~(uint8_t)(word >> 24);

        if (raw_mode) {
            printf("[RAW] 0x%02X '%c'\n", byte, (byte >= 32 && byte < 127) ? byte : '.');
            fflush(stdout);
            continue;
        }

        switch (state) {
            case STATE_HUNT:
                if (byte == PREAMBLE_BYTE_1) {
                    state = STATE_PRE1;
                } else {
                    printf("[NOISE] 0x%02X\n", byte);
                    fflush(stdout);
                }
                break;
            case STATE_PRE1:
                if (byte == PREAMBLE_BYTE_2) {
                    state = STATE_PRE2;
                } else {
                    printf("[NOISE] 0xAB 0x%02X (preamble broke after byte 1)\n", byte);
                    fflush(stdout);
                    state = STATE_HUNT;
                }
                break;
            case STATE_PRE2:
                if (byte == PREAMBLE_BYTE_3) {
                    state = STATE_PRE3;
                } else {
                    printf("[NOISE] 0xAB 0xCD 0x%02X (preamble broke after byte 2)\n", byte);
                    fflush(stdout);
                    state = STATE_HUNT;
                }
                break;
            case STATE_PRE3:
                if (byte == PREAMBLE_BYTE_4) {
                    printf(">> preamble locked\n");
                    fflush(stdout);
                    state            = STATE_PAYLOAD;
                    buf_idx          = 0;
                    frame_type_known = false;
                    frame_start_ms   = to_ms_since_boot(get_absolute_time());
                } else {
                    printf("[NOISE] 0xAB 0xCD 0xEF 0x%02X (preamble broke after byte 3)\n", byte);
                    fflush(stdout);
                    state = STATE_HUNT;
                }
                break;
            case STATE_PAYLOAD:
                // First byte after the preamble decides the lane: a real
                // MSG_TYPE_* means a binary, length-prefixed frame (real
                // sender traffic); anything else is the legacy newline-
                // terminated ASCII test protocol (speed-test harness).
                if (!frame_type_known) {
                    frame_type_known = true;
                    frame_type       = byte;
                    frame_is_binary  = is_binary_frame_type(byte);
                    len_bytes_read   = 0;
                    bin_payload_len  = 0;
                    buf[buf_idx++]   = (char)byte;
                    break;
                }

                if (frame_is_binary) {
                    buf[buf_idx++] = (char)byte;

                    if (len_bytes_read < 2) {
                        bin_payload_len = (uint16_t)((bin_payload_len << 8) | byte);
                        len_bytes_read++;
                        if (len_bytes_read == 2) {
                            bin_bytes_remaining = (uint32_t)bin_payload_len + CRC16_SIZE;
                            printf(">> %s (0x%02X), len=%u, awaiting %u more bytes...\n",
                                   msg_type_name(frame_type), frame_type,
                                   bin_payload_len, bin_bytes_remaining);
                            if ((size_t)buf_idx + bin_bytes_remaining >= sizeof(buf)) {
                                printf(">> WARNING: frame won't fit in %zu-byte debug buffer, will truncate\n",
                                       sizeof(buf));
                            }
                            fflush(stdout);
                        }
                        break;
                    }

                    bin_bytes_remaining--;
                    bool full     = (bin_bytes_remaining == 0);
                    bool overflow = (buf_idx >= (int)sizeof(buf) - 1);
                    if (full) {
                        msg_count++;
                        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - frame_start_ms;
                        process_complete_frame((const uint8_t *)buf, buf_idx, frame_type,
                                                bin_payload_len, elapsed, false);
                        state = STATE_HUNT;
                    } else if (overflow) {
                        msg_count++;
                        print_divider('=');
                        printf(" RX #%-4lu  %-10s (0x%02X)  %3d bytes  CRC: SKIPPED  [TRUNCATED]\n",
                               msg_count, msg_type_name(frame_type), frame_type, buf_idx);
                        print_divider('-');
                        printf("  (frame incomplete — raw bytes below)\n");
                        print_hex_dump((const uint8_t *)buf, buf_idx);
                        print_divider('=');
                        printf("\n");
                        printf("[EVT] msg=%lu type=0x%02X crc=skipped replay=no decrypt=skipped text=\"\"\n",
                               msg_count, frame_type);
                        fflush(stdout);
                        state = STATE_HUNT;
                    }
                    break;
                }

                // --- Legacy ASCII test protocol (newline-terminated) ---
                if (byte == '\n' || byte == '\r') {
                    buf[buf_idx] = '\0';
                    msg_count++;

                    if (strncmp(buf, "__BAUD:", 7) == 0) {
                        uint32_t nb = (uint32_t)strtoul(buf + 7, NULL, 10);
                        if (nb >= 1000 && nb <= 4000000) {
                            current_baud = nb;
                            float d = (float)clock_get_hz(clk_sys) / (current_baud * 8.0f);
                            pio_sm_set_clkdiv(pio, sm, d);
                            printf("[TEST] baud_switch=%lu\n", current_baud);
                            fflush(stdout);
                        }
                    } else if (strcmp(buf, "__TEST_START__") == 0) {
                        test_active = true;
                        test_recv   = 0;
                        test_baud   = current_baud;
                        printf("[TEST_START] baud=%lu\n", test_baud);
                        fflush(stdout);
                    } else if (strncmp(buf, "__TEST_END:", 11) == 0) {
                        uint32_t sent = (uint32_t)strtoul(buf + 11, NULL, 10);
                        test_active = false;
                        printf("[TEST_RESULT] baud=%lu sent=%lu recv=%lu\n",
                               test_baud, sent, test_recv);
                        fflush(stdout);
                    } else if (strcmp(buf, "__DONE__") == 0) {
                        printf("[TEST_DONE]\n");
                        fflush(stdout);
                    } else if (test_active && strncmp(buf, "PKT", 3) == 0) {
                        test_recv++;  // count silently during test
                    } else {
                        print_divider('-');
                        printf(" RX #%-4lu  TEXT  %d bytes\n", msg_count, buf_idx);
                        printf("  \"%s\"\n", buf);
                        print_divider('-');
                        printf("\n");
                        fflush(stdout);
                    }

                    state = STATE_HUNT;
                } else if (buf_idx < (int)sizeof(buf) - 1) {
                    buf[buf_idx++] = (char)byte;
                } else {
                    buf[buf_idx] = '\0';
                    msg_count++;
                    print_divider('-');
                    printf(" RX #%-4lu  TEXT  %d bytes  [TRUNCATED]\n", msg_count, buf_idx);
                    printf("  \"%s\"\n", buf);
                    print_divider('-');
                    printf("\n");
                    fflush(stdout);
                    state = STATE_HUNT;
                }
                break;
        }
    }
    return 0;
}
