#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "lifi_rx.pio.h"
#include "../../include/protocol.h"

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
static uint32_t   current_baud = 9600;
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

int main() {
    stdio_init_all();
    sleep_ms(3000);  // Allow USB to enumerate

    printf("\n=== Pico 2 LiFi Receiver ===\n");
    printf("RX pin  : GP%d\n", RX_PIN);
    printf("Baud    : %lu\n", current_baud);
    printf("Preamble: 0x%02X 0x%02X 0x%02X 0x%02X\n",
           PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4);
    printf("Commands: raw on/off | status | pintest | baud <rate>\n");
    printf("Listening...\n\n");
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
                    printf("RX: GP%d | Baud: %lu | Mode: %s | Msgs: %lu\n",
                           RX_PIN, current_baud, raw_mode ? "RAW" : "SST", msg_count);
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
                if (byte == PREAMBLE_BYTE_1) state = STATE_PRE1;
                break;
            case STATE_PRE1:
                state = (byte == PREAMBLE_BYTE_2) ? STATE_PRE2 : STATE_HUNT;
                break;
            case STATE_PRE2:
                state = (byte == PREAMBLE_BYTE_3) ? STATE_PRE3 : STATE_HUNT;
                break;
            case STATE_PRE3:
                if (byte == PREAMBLE_BYTE_4) {
                    state   = STATE_PAYLOAD;
                    buf_idx = 0;
                } else {
                    state = STATE_HUNT;
                }
                break;
            case STATE_PAYLOAD:
                if (byte == '\n' || byte == '\r') {
                    buf[buf_idx] = '\0';
                    msg_count++;

                    // --- Test protocol dispatch ---
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
                        printf("[RX #%lu] %s\n", msg_count, buf);
                        fflush(stdout);
                    }

                    state = STATE_HUNT;
                } else if (buf_idx < (int)sizeof(buf) - 1) {
                    buf[buf_idx++] = (char)byte;
                } else {
                    buf[buf_idx] = '\0';
                    msg_count++;
                    printf("[RX #%lu] %s ... [TRUNCATED]\n", msg_count, buf);
                    fflush(stdout);
                    state = STATE_HUNT;
                }
                break;
        }
    }
    return 0;
}
