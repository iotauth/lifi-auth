#include <stdio.h>
#include <string.h>

#include "../../include/cmd_handler.h"
#include "../../include/pico_handler.h"
#include "../../include/sst_crypto_embedded.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "mbedtls/sha256.h"
#include "pico/bootrom.h"
#include "pico/rand.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define UART_ID_DEBUG uart0
#define UART_RX_PIN_DEBUG 1
#define UART_TX_PIN_DEBUG 0

#define UART_ID uart1
#define UART_RX_PIN 5
#define UART_TX_PIN 4

#define BAUD_RATE 1000000
#define PREAMBLE_BYTE_1 0xAB
#define PREAMBLE_BYTE_2 0xCD

#define MSG_TYPE_ENCRYPTED 0x02
#define MSG_TYPE_CHALLENGE 0x10
#define MSG_TYPE_RESPONSE  0x11

#ifndef CHALLENGE_SIZE
#define CHALLENGE_SIZE 32
#endif

#ifndef HMAC_SIZE
#define HMAC_SIZE 32
#endif

#define KEY_ID_SIZE  8
#define NONCE_A_SIZE 16

// Simple KDF: derive two independent 32-byte keys from session_key
// cipher_key = SHA256(session_key || "enc")
// mac_key    = SHA256(session_key || "mac")
static void derive_cipher_and_mac_keys(const uint8_t session_key[SST_KEY_SIZE],
                                       uint8_t cipher_key[32],
                                       uint8_t mac_key[32]) {
    uint8_t buf[SST_KEY_SIZE + 3];

    memcpy(buf, session_key, SST_KEY_SIZE);

    // "enc"
    buf[SST_KEY_SIZE + 0] = 'e';
    buf[SST_KEY_SIZE + 1] = 'n';
    buf[SST_KEY_SIZE + 2] = 'c';
    mbedtls_sha256(buf, sizeof(buf), cipher_key, 0);

    // "mac"
    buf[SST_KEY_SIZE + 0] = 'm';
    buf[SST_KEY_SIZE + 1] = 'a';
    buf[SST_KEY_SIZE + 2] = 'c';
    mbedtls_sha256(buf, sizeof(buf), mac_key, 0);

    secure_zero(buf, sizeof(buf));
}

static void pico_random_bytes(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = get_rand_32();
        size_t copy = (len - i >= 4) ? 4 : (len - i);
        memcpy(buf + i, &r, copy);
    }
}

int main() {
    stdio_init_all();
    pico_prng_init();
    sleep_ms(3000);  // Wait for USB serial
    pico_nonce_init();

    // Enable watchdog with a 5-second timeout. It will be paused on debug.
    // watchdog_enable(5000, 1);

    int current_slot = 0;  // 0 = A, 1 = B

    if (watchdog_caused_reboot() && !stdio_usb_connected()) {
        printf("Rebooted via watchdog.\n");
    } else {
        printf("Fresh power-on boot or reboot via flash.\n");
    }

    // boot with last saved slot
    int saved_slot = load_last_used_slot();
    if (saved_slot == 0 || saved_slot == 1) {
        current_slot = saved_slot;
    } else {
        current_slot = 0;  // Default to A if not found
    }

#define RED_PIN 0
#define GREEN_PIN 1
#define BLUE_PIN 2

    printf("PICO STARTED\n");

    // Initialize RGB Channels for High-Speed LiFi
    gpio_init(RED_PIN);
    gpio_set_dir(RED_PIN, GPIO_OUT);
    gpio_put(RED_PIN, 0);
    gpio_init(GREEN_PIN);
    gpio_set_dir(GREEN_PIN, GPIO_OUT);
    gpio_put(GREEN_PIN, 0);
    gpio_init(BLUE_PIN);
    gpio_set_dir(BLUE_PIN, GPIO_OUT);
    gpio_put(BLUE_PIN, 0);

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);

    uart_init(UART_ID_DEBUG, BAUD_RATE);
    gpio_set_function(UART_TX_PIN_DEBUG, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN_DEBUG, GPIO_FUNC_UART);

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Flush UART input buffer to discard any leftover or garbage data.
    while (uart_is_readable(UART_ID)) {
        volatile uint8_t _ = uart_getc(UART_ID);
        (void)_;
    }

    uint8_t session_key[SST_KEY_SIZE] = {0};

    // Derived keys (keep them in RAM; re-derive whenever session_key changes)
    uint8_t cipher_key[32] = {0};
    uint8_t mac_key[32] = {0};

    // Try to load an existing valid session key from flash
    if (!load_session_key(session_key)) {
        printf("No valid session key found. Waiting for one...\n");

        // Wait up to 20 seconds to receive a new session key over UART
        if (receive_new_key_with_timeout(session_key, 20000)) {
            print_hex("Received session key: ", session_key, SST_KEY_SIZE);

            // Attempt to save the newly received key to flash
            if (store_session_key(session_key)) {
                uint8_t tmp[SST_KEY_SIZE];
                int written_slot = -1;

                // Find which flash slot (A or B) the key was written to
                for (int slot = 0; slot <= 1; slot++) {
                    if (pico_read_key_from_slot(slot, tmp) &&
                        memcmp(tmp, session_key, SST_KEY_SIZE) == 0) {
                        written_slot = slot;
                        break;
                    }
                }
                secure_zero(tmp, sizeof(tmp));

                if (written_slot >= 0) {
                    current_slot = written_slot;
                    // Save the last used slot index in flash
                    store_last_used_slot((uint8_t)current_slot);

                    // Reset the nonce tracking since the key has changed
                    pico_nonce_on_key_change();
                    printf("Key saved to flash slot %c.\n",
                           current_slot == 0 ? 'A' : 'B');
                } else {
                    printf(
                        "Warning: couldn't verify which slot has the new key.\n");
                }
            } else {
                printf("Failed to save key to flash.\n");
                return 1;
            }
        } else {
            printf("Timeout. No session key received. Aborting.\n");
            return 1;
        }
    } else {
        print_hex("Using session key: ", session_key, SST_KEY_SIZE);
    }

    // Derive cipher_key + mac_key once we have a session_key
    if (!is_key_zeroed(session_key)) {
        derive_cipher_and_mac_keys(session_key, cipher_key, mac_key);
    }

    char message_buffer[256];

    while (true) {
        printf("Enter a message to send over LiFi:\n");

        size_t msg_len = 0;
        int ch;  // character
        uint8_t ciphertext[256] = {0};
        uint8_t tag[SST_TAG_SIZE] = {0};

        for (;;) {
            ch = getchar_timeout_us(1000);  // poll USB CDC every 1 ms
            if (ch == PICO_ERROR_TIMEOUT) {
                // watchdog_update(); //when enabled
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                message_buffer[msg_len] = '\0';
                putchar('\n');
                break;
            }
            if ((ch == 127 || ch == 8) && msg_len > 0) {
                msg_len--;
                printf("\b \b");
                continue;
            }
            if (msg_len < sizeof(message_buffer) - 1 && ch >= 32 && ch < 127) {
                message_buffer[msg_len++] = (char)ch;
                putchar(ch);
            }
        }

        // Check if the message is actually a command (starts with "CMD:")
        if (strncmp(message_buffer, "CMD:", 4) == 0) {
            const char *cmd = message_buffer + 4;

            // Hardware Test Command for RGB PCB
            if (strncmp(cmd, " test rgb", 9) == 0) {
                printf("Testing RGB Channels (Red -> Green -> Blue)...\n");
                for (int i = 0; i < 3; i++) {
                    gpio_put(RED_PIN, 1);
                    sleep_ms(200);
                    gpio_put(RED_PIN, 0);
                    gpio_put(GREEN_PIN, 1);
                    sleep_ms(200);
                    gpio_put(GREEN_PIN, 0);
                    gpio_put(BLUE_PIN, 1);
                    sleep_ms(200);
                    gpio_put(BLUE_PIN, 0);
                }
                printf("RGB Test Complete.\n");
                memset(message_buffer, 0, sizeof(message_buffer));
                continue;
            }

            bool key_changed = handle_commands(cmd, session_key, &current_slot);

            if (key_changed) {
                pico_nonce_on_key_change();

                // Re-derive keys whenever session_key changes
                secure_zero(cipher_key, sizeof(cipher_key));
                secure_zero(mac_key, sizeof(mac_key));
                if (!is_key_zeroed(session_key)) {
                    derive_cipher_and_mac_keys(session_key, cipher_key, mac_key);
                }
            }

            memset(message_buffer, 0, sizeof(message_buffer));
            continue;
        }

        if (msg_len > sizeof(ciphertext)) {
            printf("Message too long!\n");
            continue;
        }

        // --- HMAC Challenge Check (Non-blocking) ---
        if (uart_is_readable(UART_ID)) {
            uint8_t pre = uart_getc(UART_ID);
            if (pre == PREAMBLE_BYTE_1) {
                if (uart_getc(UART_ID) == PREAMBLE_BYTE_2) {
                    uint8_t type = uart_getc(UART_ID);

                    if (type == MSG_TYPE_CHALLENGE) {
                        uint8_t key_id[KEY_ID_SIZE];
                        uint8_t nonce_A[NONCE_A_SIZE];
                        uint8_t mac_A[HMAC_SIZE];

                        for (int i = 0; i < KEY_ID_SIZE; i++) {
                            key_id[i] = uart_getc(UART_ID);
                        }
                        for (int i = 0; i < NONCE_A_SIZE; i++) {
                            nonce_A[i] = uart_getc(UART_ID);
                        }
                        for (int i = 0; i < HMAC_SIZE; i++) {
                            mac_A[i] = uart_getc(UART_ID);
                        }

                        if (is_key_zeroed(session_key)) {
                            printf("Ignoring challenge: no session key.\n");
                            secure_zero(key_id, sizeof(key_id));
                            secure_zero(nonce_A, sizeof(nonce_A));
                            secure_zero(mac_A, sizeof(mac_A));
                            continue;
                        }

                        // Build HMAC input: "CHAL" || key_id || nonce_A
                        uint8_t hmac_input[4 + KEY_ID_SIZE + NONCE_A_SIZE];
                        memcpy(hmac_input, "CHAL", 4);
                        memcpy(hmac_input + 4, key_id, KEY_ID_SIZE);
                        memcpy(hmac_input + 4 + KEY_ID_SIZE, nonce_A, NONCE_A_SIZE);

                        uint8_t expected_mac[HMAC_SIZE];
                        int ret = sst_hmac_sha256(mac_key, hmac_input,
                                                  sizeof(hmac_input), expected_mac);

                        if (ret != 0 || memcmp(expected_mac, mac_A, HMAC_SIZE) != 0) {
                            printf("Invalid challenge MAC. Ignoring.\n");
                            secure_zero(key_id, sizeof(key_id));
                            secure_zero(nonce_A, sizeof(nonce_A));
                            secure_zero(mac_A, sizeof(mac_A));
                            secure_zero(hmac_input, sizeof(hmac_input));
                            secure_zero(expected_mac, sizeof(expected_mac));
                            continue;
                        }

                        // Challenge verified -> respond
                        uint8_t nonce_B[NONCE_A_SIZE];
                        
                        pico_random_bytes(nonce_B, NONCE_A_SIZE);

                        // Build response MAC: "RESP" || key_id || nonce_A || nonce_B
                        uint8_t resp_input[4 + KEY_ID_SIZE + NONCE_A_SIZE * 2];
                        memcpy(resp_input, "RESP", 4);
                        memcpy(resp_input + 4, key_id, KEY_ID_SIZE);
                        memcpy(resp_input + 4 + KEY_ID_SIZE, nonce_A, NONCE_A_SIZE);
                        memcpy(resp_input + 4 + KEY_ID_SIZE + NONCE_A_SIZE,
                               nonce_B, NONCE_A_SIZE);

                        uint8_t mac_B[HMAC_SIZE];
                        sst_hmac_sha256(mac_key, resp_input, sizeof(resp_input), mac_B);

                        // Send response: [PRE1][PRE2][TYPE=RESPONSE][nonce_B][mac_B]
                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
                        uart_putc_raw(UART_ID, MSG_TYPE_RESPONSE);
                        uart_write_blocking(UART_ID, nonce_B, NONCE_A_SIZE);
                        uart_write_blocking(UART_ID, mac_B, HMAC_SIZE);

                        gpio_put(25, 1);
                        sleep_ms(50);
                        gpio_put(25, 0);
                        printf("Verified challenge + sent response.\n");

                        // Hygiene
                        secure_zero(key_id, sizeof(key_id));
                        secure_zero(nonce_A, sizeof(nonce_A));
                        secure_zero(mac_A, sizeof(mac_A));
                        secure_zero(hmac_input, sizeof(hmac_input));
                        secure_zero(expected_mac, sizeof(expected_mac));
                        secure_zero(nonce_B, sizeof(nonce_B));
                        secure_zero(resp_input, sizeof(resp_input));
                        secure_zero(mac_B, sizeof(mac_B));

                        // Done handling challenge; avoid mixing with CLI send
                        continue;
                    }
                }
            }
        }
        // -------------------------------------------

        // Ensure session key is valid before proceeding with encryption
        if (is_key_zeroed(session_key)) {
            printf("No valid key in the current slot. Cannot send message.\n");
            printf("Use 'CMD: new key' or switch to a valid slot.\n");
            continue;
        }

        uint8_t nonce[SST_NONCE_SIZE];
        pico_nonce_generate(nonce);

        // Use cipher_key for AES-GCM (not session_key)
        int ret = sst_encrypt_gcm(cipher_key, nonce, (const uint8_t *)message_buffer,
                                  msg_len, ciphertext, tag);
        if (ret != 0) {
            printf("Encryption failed! ret=%d\n", ret);
            continue;
        }

        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
        uart_putc_raw(UART_ID, MSG_TYPE_ENCRYPTED);
        uart_write_blocking(UART_ID, nonce, SST_NONCE_SIZE);

        uint8_t len_bytes[2] = {(uint8_t)((msg_len >> 8) & 0xFF),
                                (uint8_t)(msg_len & 0xFF)};
        uart_write_blocking(UART_ID, len_bytes, 2);

        uart_write_blocking(UART_ID, ciphertext, msg_len);
        uart_write_blocking(UART_ID, tag, SST_TAG_SIZE);

        gpio_put(25, 1);
        sleep_ms(100);
        gpio_put(25, 0);

        secure_zero(ciphertext, sizeof(ciphertext));
        secure_zero(tag, sizeof(tag));
        secure_zero(nonce, sizeof(nonce));
        secure_zero(message_buffer, sizeof(message_buffer));
    }

    return 0;
}
