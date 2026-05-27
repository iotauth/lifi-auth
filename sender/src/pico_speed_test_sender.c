#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "lifi_multi_tx.pio.h"
#include "../../include/protocol.h"

#define PIO_TX_PIN_BASE   6
#define PIO_TX_PIN_COUNT  4

#define PIN_WHITE  6
#define PIN_GREEN  7
#define PIN_BLUE   8
#define PIN_RED    9

static PIO pio = pio0;
static uint sm = 0;
static uint offset = 0;
static uint32_t current_baud = 9600;
static uint8_t active_mask = 0x0F;
static bool loop_mode = false;
static uint32_t loop_delay_ms = 1000;
static bool raw_mode = false;  // NEW: raw mode flag

const char* pin_name(uint pin) {
    switch(pin) {
        case PIN_WHITE: return "WHITE";
        case PIN_GREEN: return "GREEN";
        case PIN_BLUE:  return "BLUE";
        case PIN_RED:   return "RED";
        default:        return "UNKNOWN";
    }
}

void print_status() {
    printf("\n=== STATUS ===\n");
    printf("Baud rate : %lu\n", current_baud);
    printf("LED mask  : 0x%02X (%u)\n", active_mask, active_mask);
    printf("Mode      : %s\n", raw_mode ? "RAW" : "SST (Preamble)");
    printf("Loop mode : %s (delay: %lums)\n", loop_mode ? "ON" : "OFF", loop_delay_ms);
    printf("Pins:\n");
    for (int i = 0; i < PIO_TX_PIN_COUNT; i++) {
        uint pin = PIO_TX_PIN_BASE + i;
        printf("  Pin %u (%s): %s\n", pin, pin_name(pin),
               (active_mask & (1 << i)) ? "ENABLED" : "DISABLED");
    }
    printf("==============\n");
}

void print_help() {
    printf("\n=== COMMANDS ===\n");
    printf("  baud <val>        : Set baud rate (e.g. baud 1000000)\n");
    printf("  white on/off      : Toggle white channel\n");
    printf("  green on/off      : Toggle green channel\n");
    printf("  blue  on/off      : Toggle blue channel\n");
    printf("  red   on/off      : Toggle red channel\n");
    printf("  all               : Enable all channels\n");
    printf("  none              : Disable all channels\n");
    printf("  on <pin>          : Turn single LED on steady (6=W 7=G 8=B 9=R)\n");
    printf("  off <pin>         : Turn single LED off\n");
    printf("  blink <pin> <ms>  : Blink single LED at interval\n");
    printf("  send <msg>        : Send message (SST mode: with preamble)\n");
    printf("  raw <msg>         : Send raw bytes no preamble no framing\n");
    printf("  rawbits <bits>    : Send raw 1s and 0s (e.g. rawbits 10110010)\n");
    printf("  rawmode on/off    : Toggle raw mode for send command\n");
    printf("  loop <msg>        : Repeatedly send message\n");
    printf("  loopdelay <ms>    : Set loop delay in ms\n");
    printf("  stoploop          : Stop loop mode\n");
    printf("  test [n]          : Auto-benchmark 4 baud rates, n pkts each (default 50)\n");
    printf("  status            : Show current status\n");
    printf("  help              : Show this menu\n");
    printf("================\n");
}

void update_baud(uint32_t baud) {
    current_baud = baud;
    float div = (float)clock_get_hz(clk_sys) / (baud * 8.0f);
    pio_sm_set_clkdiv(pio, sm, div);
    printf("Baud rate set to %lu\n", baud);
}

void set_led_mask(uint8_t mask) {
    active_mask = mask;
    for (int i = 0; i < PIO_TX_PIN_COUNT; i++) {
        uint pin = PIO_TX_PIN_BASE + i;
        if (mask & (1 << i)) {
            gpio_set_function(pin, GPIO_FUNC_PIO0);
            printf("  Pin %u (%s) ENABLED\n", pin, pin_name(pin));
        } else {
            gpio_set_function(pin, GPIO_FUNC_SIO);
            gpio_set_dir(pin, GPIO_OUT);
            gpio_put(pin, 0);
            printf("  Pin %u (%s) DISABLED\n", pin, pin_name(pin));
        }
    }
}

void led_on(uint pin) {
    if (pin < PIO_TX_PIN_BASE || pin >= PIO_TX_PIN_BASE + PIO_TX_PIN_COUNT) {
        printf("Invalid pin %u\n", pin); return;
    }
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 1);
    printf("Pin %u (%s) ON\n", pin, pin_name(pin));
}

void led_off(uint pin) {
    if (pin < PIO_TX_PIN_BASE || pin >= PIO_TX_PIN_BASE + PIO_TX_PIN_COUNT) {
        printf("Invalid pin %u\n", pin); return;
    }
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    gpio_put(pin, 0);
    printf("Pin %u (%s) OFF\n", pin, pin_name(pin));
}

void led_blink(uint pin, uint32_t delay_ms) {
    if (pin < PIO_TX_PIN_BASE || pin >= PIO_TX_PIN_BASE + PIO_TX_PIN_COUNT) {
        printf("Invalid pin %u\n", pin); return;
    }
    printf("Blinking pin %u (%s) at %lums — press any key to stop\n", pin, pin_name(pin), delay_ms);
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, GPIO_OUT);
    while (true) {
        gpio_put(pin, 1);
        sleep_ms(delay_ms);
        gpio_put(pin, 0);
        sleep_ms(delay_ms);
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT) {
            printf("\nBlink stopped.\n");
            gpio_put(pin, 0);
            break;
        }
    }
}

void solo_channel(uint pin) {
    if (pin < PIO_TX_PIN_BASE || pin >= PIO_TX_PIN_BASE + PIO_TX_PIN_COUNT) {
        printf("Invalid pin %u\n", pin); return;
    }
    uint8_t mask = 1 << (pin - PIO_TX_PIN_BASE);
    printf("Solo mode: only %s enabled\n", pin_name(pin));
    set_led_mask(mask);
}

void lifi_send_byte(uint8_t byte) {
    pio_sm_put_blocking(pio, sm, (uint32_t)byte);
}

// SST mode — full preamble + message + newline
void lifi_send_message(const char* msg) {
    lifi_send_byte(PREAMBLE_BYTE_1);
    lifi_send_byte(PREAMBLE_BYTE_2);
    lifi_send_byte(PREAMBLE_BYTE_3);
    lifi_send_byte(PREAMBLE_BYTE_4);
    while (*msg) {
        lifi_send_byte((uint8_t)*msg++);
    }
    lifi_send_byte('\n');
}

// RAW mode — just bytes, no framing
void lifi_send_raw(const char* msg) {
    printf("[RAW] Sending %u bytes: ", (unsigned)strlen(msg));
    while (*msg) {
        printf("0x%02X ", (uint8_t)*msg);
        lifi_send_byte((uint8_t)*msg++);
    }
    printf("\n");
}

// RAW BITS mode — send a string of '1' and '0' chars directly as individual bit pulses
// Each '1' sends 0xFF, each '0' sends 0x00 — single byte per bit for easy scope viewing
void lifi_send_rawbits(const char* bits) {
    printf("[RAWBITS] Sending: %s\n", bits);
    while (*bits) {
        if (*bits == '1') {
            lifi_send_byte(0xFF);
        } else if (*bits == '0') {
            lifi_send_byte(0x00);
        }
        // skip anything else
        bits++;
    }
    printf("[RAWBITS] Done\n");
}

// ─── Auto Benchmark ──────────────────────────────────────────────────────────
#define TEST_BAUD_COUNT 4
static const uint32_t TEST_BAUDS[TEST_BAUD_COUNT] = {9600, 100000, 500000, 1000000};

static bool test_sleep_abortable(uint32_t ms) {
    uint32_t done = 0;
    while (done < ms) {
        sleep_ms(10);
        done += 10;
        if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) return false;
    }
    return true;
}

static void drain_stdin_buf(void) {
    sleep_ms(50);
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
}

void run_auto_test(uint32_t n, const uint32_t *bauds, int n_bauds) {
    loop_mode = false;  // stop any running loop
    char tmp[48];

    printf("\n=== AUTO BENCHMARK: %lu pkts x %d rates ===\n", n, n_bauds);
    fflush(stdout);

    for (int s = 0; s < n_bauds; s++) {
        uint32_t baud = bauds[s];

        printf("[TEST] switch baud=%lu step=%d/%d\n", baud, s + 1, n_bauds);
        fflush(stdout);

        // Signal receiver to switch baud (sent at current baud)
        snprintf(tmp, sizeof(tmp), "__BAUD:%lu__", baud);
        lifi_send_message(tmp);

        // 500 ms: receiver decodes + switches its PIO clkdiv
        if (!test_sleep_abortable(500)) goto aborted;

        // Now switch our own baud and settle
        update_baud(baud);
        if (!test_sleep_abortable(100)) goto aborted;

        // Signal test start
        lifi_send_message("__TEST_START__");
        sleep_ms(30);

        // Transmit n test packets
        for (uint32_t i = 1; i <= n; i++) {
            if (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) goto aborted;
            snprintf(tmp, sizeof(tmp), "PKT%04lu", i);
            lifi_send_message(tmp);

            // Baud-aware gap so receiver keeps up
            uint32_t gap = (baud <= 9600) ? 30 : (baud <= 100000) ? 10 : 5;
            sleep_ms(gap);

            if (i % 10 == 0 || i == n) {
                printf("[TEST] tx baud=%lu n=%lu/%lu\n", baud, i, n);
                fflush(stdout);
            }
        }

        // Signal test end with sent count embedded
        snprintf(tmp, sizeof(tmp), "__TEST_END:%lu__", n);
        lifi_send_message(tmp);
        printf("[TEST] done baud=%lu\n", baud);
        fflush(stdout);

        // Let receiver print and rx_monitor.py POST before next baud switch
        if (!test_sleep_abortable(1200)) goto aborted;
    }

    // Reset both sides to 9600
    lifi_send_message("__BAUD:9600__");
    if (!test_sleep_abortable(400)) goto aborted;
    update_baud(9600);
    lifi_send_message("__DONE__");
    printf("[TEST] complete\n=================\n");
    fflush(stdout);
    return;

aborted:
    drain_stdin_buf();
    update_baud(9600);
    printf("[TEST] ABORTED — baud reset to 9600\n");
    fflush(stdout);
}
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== Pico LiFi Multi-Channel Tester ===\n");
    printf("Pins: 6=WHITE  7=GREEN  8=BLUE  9=RED\n");
    print_help();

    offset = pio_add_program(pio, &lifi_multi_tx_program);
    float div = (float)clock_get_hz(clk_sys) / (current_baud * 8.0f);
    lifi_multi_tx_program_init(pio, sm, offset, PIO_TX_PIN_BASE, PIO_TX_PIN_COUNT, div);
    set_led_mask(active_mask);

    static char loop_msg[256] = {0};
    char cmd[256];

    while (true) {
        if (loop_mode && strlen(loop_msg) > 0) {
            printf("[LOOP] Sending: %s\n", loop_msg);
            if (raw_mode) {
                lifi_send_raw(loop_msg);
            } else {
                lifi_send_message(loop_msg);
            }
            uint32_t elapsed = 0;
            while (elapsed < loop_delay_ms) {
                sleep_ms(10);
                elapsed += 10;
                int c = getchar_timeout_us(0);
                if (c != PICO_ERROR_TIMEOUT) {
                    loop_mode = false;
                    printf("\nLoop stopped by keypress.\n");
                    break;
                }
            }
            continue;
        }

        printf("\n> ");
        fflush(stdout);

        if (fgets(cmd, sizeof(cmd), stdin)) {
            char *newline = strchr(cmd, '\n');
            if (newline) *newline = '\0';
            if (strlen(cmd) == 0) continue;

            printf("[CMD] '%s'\n", cmd);

            if (strncmp(cmd, "baud ", 5) == 0) {
                update_baud(strtoul(cmd + 5, NULL, 10));

            } else if (strncmp(cmd, "mask ", 5) == 0 || strncmp(cmd, "leds ", 5) == 0) {
                set_led_mask((uint8_t)strtoul(cmd + 5, NULL, 10));

            } else if (strncmp(cmd, "on ", 3) == 0) {
                led_on((uint)strtoul(cmd + 3, NULL, 10));

            } else if (strncmp(cmd, "off ", 4) == 0) {
                led_off((uint)strtoul(cmd + 4, NULL, 10));

            } else if (strncmp(cmd, "blink ", 6) == 0) {
                char *args = cmd + 6;
                uint pin = (uint)strtoul(args, &args, 10);
                uint32_t delay = strlen(args) > 1 ? strtoul(args + 1, NULL, 10) : 500;
                led_blink(pin, delay);

            } else if (strncmp(cmd, "solo ", 5) == 0) {
                solo_channel((uint)strtoul(cmd + 5, NULL, 10));

            } else if (strcmp(cmd, "white on") == 0) {
                active_mask |= (1 << 0); set_led_mask(active_mask);
            } else if (strcmp(cmd, "white off") == 0) {
                active_mask &= ~(1 << 0); set_led_mask(active_mask);

            } else if (strcmp(cmd, "green on") == 0) {
                active_mask |= (1 << 1); set_led_mask(active_mask);
            } else if (strcmp(cmd, "green off") == 0) {
                active_mask &= ~(1 << 1); set_led_mask(active_mask);

            } else if (strcmp(cmd, "blue on") == 0) {
                active_mask |= (1 << 2); set_led_mask(active_mask);
            } else if (strcmp(cmd, "blue off") == 0) {
                active_mask &= ~(1 << 2); set_led_mask(active_mask);

            } else if (strcmp(cmd, "red on") == 0) {
                active_mask |= (1 << 3); set_led_mask(active_mask);
            } else if (strcmp(cmd, "red off") == 0) {
                active_mask &= ~(1 << 3); set_led_mask(active_mask);

            } else if (strcmp(cmd, "all") == 0) {
                set_led_mask(0x0F);

            } else if (strcmp(cmd, "none") == 0) {
                set_led_mask(0x00);

            } else if (strncmp(cmd, "send ", 5) == 0) {
                if (raw_mode) {
                    printf("[RAW MODE] Sending raw: %s\n", cmd + 5);
                    lifi_send_raw(cmd + 5);
                } else {
                    printf("Sending: %s\n", cmd + 5);
                    lifi_send_message(cmd + 5);
                }

            } else if (strncmp(cmd, "raw ", 4) == 0) {
                // Always raw regardless of rawmode flag
                lifi_send_raw(cmd + 4);

            } else if (strncmp(cmd, "rawbits ", 8) == 0) {
                lifi_send_rawbits(cmd + 8);

            } else if (strncmp(cmd, "rawmode ", 8) == 0) {
                if (strcmp(cmd + 8, "on") == 0) {
                    raw_mode = true;
                    printf("Raw mode ON — send command will skip preamble\n");
                } else if (strcmp(cmd + 8, "off") == 0) {
                    raw_mode = false;
                    printf("Raw mode OFF — send command uses SST preamble\n");
                } else {
                    printf("Usage: rawmode on | rawmode off\n");
                }

            } else if (strncmp(cmd, "loop ", 5) == 0) {
                strncpy(loop_msg, cmd + 5, sizeof(loop_msg) - 1);
                loop_mode = true;
                printf("Loop mode ON: '%s' every %lums [%s]\n", 
                       loop_msg, loop_delay_ms, raw_mode ? "RAW" : "SST");

            } else if (strncmp(cmd, "loopdelay ", 10) == 0) {
                loop_delay_ms = strtoul(cmd + 10, NULL, 10);
                printf("Loop delay set to %lums\n", loop_delay_ms);

            } else if (strcmp(cmd, "stoploop") == 0) {
                loop_mode = false;
                printf("Loop mode OFF\n");

            } else if (strcmp(cmd, "status") == 0) {
                print_status();

            } else if (strcmp(cmd, "help") == 0) {
                print_help();

            } else if (strcmp(cmd, "test") == 0 || strncmp(cmd, "test ", 5) == 0) {
                char *p = cmd + 4;
                uint32_t n = 50;
                uint32_t custom_bauds[32];
                int custom_n = 0;
                if (*p == ' ') {
                    p++;
                    n = strtoul(p, &p, 10);
                    if (n == 0 || n > 10000) n = 50;
                    if (*p == ' ') {
                        p++;
                        while (*p && custom_n < 32) {
                            uint32_t b = strtoul(p, &p, 10);
                            if (b >= 1000 && b <= 4000000) custom_bauds[custom_n++] = b;
                            if (*p == ',') p++;
                        }
                    }
                }
                if (custom_n > 0)
                    run_auto_test(n, custom_bauds, custom_n);
                else
                    run_auto_test(n, TEST_BAUDS, TEST_BAUD_COUNT);

            } else {
                printf("Unknown command: '%s' — type 'help'\n", cmd);
            }
        }
    }
    return 0;
}