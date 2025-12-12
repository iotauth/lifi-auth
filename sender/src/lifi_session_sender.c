#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "../../include/cmd_handler.h"      // not strictly needed here, but kept for project consistency
#include "../../include/pico_handler.h"     // for pico_prng_init, pico_nonce_init if you want them later
#include "../../include/sst_crypto_embedded.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// UART & LiFi framing constants (match your receiver)
#define UART_ID_DEBUG       uart0
#define UART_RX_PIN_DEBUG   1
#define UART_TX_PIN_DEBUG   0

#define UART_ID             uart1
#define UART_RX_PIN         5
#define UART_TX_PIN         4

#define BAUD_RATE           1000000

#define PREAMBLE_BYTE_1     0xAB
#define PREAMBLE_BYTE_2     0xCD
#define MSG_TYPE_KEY_ID     0x03
#define SESSION_KEY_ID_SIZE 8

#define LED_PIN             25
#define RED_PIN             0
#define GREEN_PIN           1
#define BLUE_PIN            2

// Convert two hex characters (e.g., "AF") into a byte (0xAF).
static bool hexpair_to_byte(const char *hex, uint8_t *out) {
    char buf[3] = { hex[0], hex[1], 0 };
    char *end = NULL;
    long v = strtol(buf, &end, 16);
    if (*end != '\0' || v < 0 || v > 255) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

// Trim leading spaces in-place and return first non-space char pointer.
static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// Simple line reader using USB CDC, with backspace support.
// Reuses the style from your existing sender: getchar_timeout_us in a loop.
static void read_line(char *buf, size_t buf_size) {
    size_t len = 0;
    memset(buf, 0, buf_size);

    for (;;) {
        int ch = getchar_timeout_us(1000000);  // 1ms polling
        if (ch == PICO_ERROR_TIMEOUT) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            buf[len] = '\0';
            putchar('\n');
            fflush(stdout);
            return;
        }

        // handle backspace / delete
        if ((ch == 127 || ch == 8) && len > 0) {
            len--;
            printf("\b \b");
            fflush(stdout);
            continue;
        }

        // printable ASCII only
        if (ch >= 32 && ch < 127 && len < buf_size - 1) {
            buf[len++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  ID <16 hex chars>  - Set session Key ID (example: A1B2C3D4E5F60708)\n");
    printf("  SHOW               - Show current Key ID\n");
    printf("  SEND               - Send LiFi frame: AB CD 03 <KeyID>\n");
    printf("  HELP               - Show this help message\n");
}

// --- Main ---

int main() {
    stdio_init_all();
    sleep_ms(3000);  // Wait for USB to enumerate so prints are visible

    // Optional: if you want PRNG/nonce elsewhere later; harmless if unused
    pico_prng_init();
    pico_nonce_init();

    // LED + RGB pins for feedback
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    gpio_init(RED_PIN);   gpio_set_dir(RED_PIN, GPIO_OUT);   gpio_put(RED_PIN, 0);
    gpio_init(GREEN_PIN); gpio_set_dir(GREEN_PIN, GPIO_OUT); gpio_put(GREEN_PIN, 0);
    gpio_init(BLUE_PIN);  gpio_set_dir(BLUE_PIN, GPIO_OUT);  gpio_put(BLUE_PIN, 0);

    // Debug UART0 (if you want to mirror logs later)
    uart_init(UART_ID_DEBUG, BAUD_RATE);
    gpio_set_function(UART_TX_PIN_DEBUG, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN_DEBUG, GPIO_FUNC_UART);

    // LiFi UART1: TX = GPIO4 -> LED driver, RX = GPIO5 (unused here but configured)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Flush any garbage from UART1 RX
    while (uart_is_readable(UART_ID)) {
        (void)uart_getc(UART_ID);
    }

    printf("========================================\n");
    printf("  PICO LiFi Session Key ID Sender\n");
    printf("  UART1 @ %d baud (TX=GPIO4, RX=GPIO5)\n", BAUD_RATE);
    printf("  Frame: AB CD 03 <8-byte KeyID>\n");
    printf("========================================\n\n");

    uint8_t current_key_id[SESSION_KEY_ID_SIZE] = {0};
    bool key_id_set = false;

    char line[128];

    while (true) {
        printf("\n> ");
        fflush(stdout);

        read_line(line, sizeof(line));
        char *cmd = ltrim(line);

        if (*cmd == '\0') {
            continue; // empty line
        }

        // Normalize first token uppercase for simple matching
        // We’ll compare only first few chars anyway.
        if (strncasecmp(cmd, "HELP", 4) == 0) {
            print_help();
            continue;
        }

        if (strncasecmp(cmd, "SHOW", 4) == 0) {
            if (!key_id_set) {
                printf("Key ID is not set.\n");
            } else {
                printf("Current Key ID: ");
                for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                    printf("%02X", current_key_id[i]);
                }
                printf("\n");
            }
            continue;
        }

        if (strncasecmp(cmd, "ID", 2) == 0) {
            // Expect: ID <16 hex chars>
            cmd += 2;
            cmd = ltrim(cmd);

            if (*cmd == '\0') {
                printf("Usage: ID <16 hex chars>\n");
                continue;
            }

            // Take exactly first 16 non-space chars as hex
            char hexbuf[17] = {0};
            int hlen = 0;
            while (*cmd && *cmd != ' ' && *cmd != '\t' && hlen < 16) {
                hexbuf[hlen++] = *cmd++;
            }
            hexbuf[hlen] = '\0';

            if (hlen != 16) {
                printf("Expected exactly 16 hex characters, got %d.\n", hlen);
                printf("Example: ID A1B2C3D4E5F60708\n");
                continue;
            }

            bool ok = true;
            for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                if (!hexpair_to_byte(&hexbuf[i * 2], &current_key_id[i])) {
                    ok = false;
                    break;
                }
            }

            if (!ok) {
                printf("Invalid hex in Key ID. Use 0-9, A-F.\n");
                continue;
            }

            key_id_set = true;
            printf("Key ID set to: ");
            for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                printf("%02X", current_key_id[i]);
            }
            printf("\n");
            continue;
        }

        if (strncasecmp(cmd, "SEND", 4) == 0) {
            if (!key_id_set) {
                printf("No Key ID set. Use: ID <16 hex chars>\n");
                continue;
            }

            printf("Sending LiFi frame: AB CD 03 <KeyID>\n");
            printf("Bytes: ");
            printf("%02X %02X %02X ", PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, MSG_TYPE_KEY_ID);
            for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
                printf("%02X ", current_key_id[i]);
            }
            printf("\n");

            // Send over UART1 to LiFi LED path
            uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
            uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
            uart_putc_raw(UART_ID, MSG_TYPE_KEY_ID);
            uart_write_blocking(UART_ID, current_key_id, SESSION_KEY_ID_SIZE);

            // Blink LED as confirmation
            gpio_put(LED_PIN, 1);
            sleep_ms(100);
            gpio_put(LED_PIN, 0);

            printf("Frame sent.\n");
            continue;
        }

        // Unknown command
        printf("Unknown command. Type HELP for options.\n");
    }

    return 0;
}
