#include <stdio.h>
#include <string.h>

#include "../../include/cmd_handler.h"
#include "../../include/pico_handler.h"
#include "../../include/protocol.h"
#include "../../include/sst_crypto_embedded.h"
#include "../../include/crc16.h"
#include "heatshrink_encoder.h"
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
/* Preamble and message types now in protocol.h */

// Helper: Read bytes with a total timeout
bool uart_read_blocking_timeout_us(uart_inst_t *uart, uint8_t *dst, size_t len, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    size_t received = 0;
    while (received < len) {
        if (time_reached(deadline)) return false;
        if (uart_is_readable(uart)) {
            dst[received++] = uart_getc(uart);
        }
    }
    return true;
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
    // boot with last saved slot -- IGNORED per user request
    // int saved_slot = load_last_used_slot();
    // if (saved_slot == 0 || saved_slot == 1) {
    //     current_slot = saved_slot;
    // } else {
    //     current_slot = 0;  // Default to A if not found
    // }
    current_slot = 0; // Always default to A
    printf("Defaulting to Slot A (per configuration).\n");

    printf("PICO STARTED\n");

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
    }

    uint8_t session_key[SST_KEY_SIZE] = {0};
    uint8_t session_key_id[SST_KEY_ID_SIZE] = {0};

    // Try to load an existing valid session key from flash
    if (!load_session_key(session_key_id, session_key)) {
        printf("No valid session key found. Entering command mode.\n");
        printf("Use 'CMD: new key' or similar to provision.\n");
    } else {
        printf("Using Key ID: ");
        for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", session_key_id[i]);
        printf("\n");
        print_hex("Using session key: ", session_key, SST_KEY_SIZE);
    }

    char message_buffer[8192];  // Increased for whole-file transfer

    while (true) {
        printf("Enter a message to send over LiFi:\n");
        // can add function to show A(VALID): or A(INVALID): or B(VALID): etc..
        //  in message preview -- show validity and which slot we are on !
        size_t msg_len = 0;
        int ch;  // character
        uint8_t ciphertext[8192] = {0};  // Increased to match message buffer
        uint8_t tag[SST_TAG_SIZE] = {0};

        for (;;) {
            // Check for incoming UART challenges from Pi4
            if (uart_is_readable(UART_ID)) {
                static uint8_t uart_byte;
                static int challenge_state = 0;
                static uint8_t challenge_buffer[CHALLENGE_SIZE];
                
                uart_byte = uart_getc(UART_ID);
                
                switch (challenge_state) {
                    case 0:
                        if (uart_byte == PREAMBLE_BYTE_1) challenge_state = 1;
                        break;
                    case 1:
                        if (uart_byte == PREAMBLE_BYTE_2) challenge_state = 2;
                        else challenge_state = 0;
                        break;
                    case 2:
                        if (uart_byte == PREAMBLE_BYTE_3) challenge_state = 3;
                        else challenge_state = 0;
                        break;
                    case 3:
                        if (uart_byte == PREAMBLE_BYTE_4) challenge_state = 4;
                        else challenge_state = 0;
                        break;
                    case 4:
                        // 4-byte preamble matched. Check type.
                        if (uart_byte == MSG_TYPE_CHALLENGE) {
                            // Read length (2 bytes) then challenge
                            uint8_t len_bytes[2];
                            if (uart_read_blocking_timeout_us(UART_ID, len_bytes, 2, 50000) &&
                                uart_read_blocking_timeout_us(UART_ID, challenge_buffer, CHALLENGE_SIZE, 100000)) {
                                printf("\n[Received HMAC challenge from Pi4]\n");
                                
                                // Compute HMAC response
                                uint8_t hmac[HMAC_SIZE];
                                int hmac_ret = sst_hmac_sha256(session_key, challenge_buffer, 
                                                         CHALLENGE_SIZE, hmac);
                                
                                if (hmac_ret == 0) {
                                    // Convert HMAC to hex string
                                    char hmac_msg[5 + HMAC_SIZE * 2 + 1];  // "HMAC:" + hex + null
                                    strcpy(hmac_msg, "HMAC:");
                                    for (int i = 0; i < HMAC_SIZE; i++) {
                                        sprintf(hmac_msg + 5 + (i * 2), "%02X", hmac[i]);
                                    }
                                    
                                    // Send HMAC response as encrypted LiFi message
                                    uint8_t nonce[SST_NONCE_SIZE];
                                    pico_nonce_generate(nonce);
                                    
                                    uint8_t ciphertext_hmac[sizeof(hmac_msg)];
                                    uint8_t tag_hmac[SST_TAG_SIZE];
                                    
                                    hmac_ret = sst_encrypt_gcm(session_key, nonce, 
                                                         (uint8_t*)hmac_msg, strlen(hmac_msg),
                                                         ciphertext_hmac, tag_hmac);
                                    
                                    if (hmac_ret == 0) {
                                        // Build frame with new format
                                        size_t msg_len_hmac = strlen(hmac_msg);
                                        size_t payload_len = SST_NONCE_SIZE + msg_len_hmac + SST_TAG_SIZE;
                                        uint8_t len_bytes_out[2] = {(payload_len >> 8) & 0xFF, payload_len & 0xFF};
                                        
                                        // CRC buffer
                                        uint8_t crc_buf[1 + 2 + SST_NONCE_SIZE + sizeof(hmac_msg) + SST_TAG_SIZE];
                                        size_t crc_idx = 0;
                                        crc_buf[crc_idx++] = MSG_TYPE_ENCRYPTED;
                                        crc_buf[crc_idx++] = len_bytes_out[0];
                                        crc_buf[crc_idx++] = len_bytes_out[1];
                                        memcpy(&crc_buf[crc_idx], nonce, SST_NONCE_SIZE); crc_idx += SST_NONCE_SIZE;
                                        memcpy(&crc_buf[crc_idx], ciphertext_hmac, msg_len_hmac); crc_idx += msg_len_hmac;
                                        memcpy(&crc_buf[crc_idx], tag_hmac, SST_TAG_SIZE); crc_idx += SST_TAG_SIZE;
                                        uint16_t crc = crc16_ccitt(crc_buf, crc_idx);
                                        uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
                                        
                                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
                                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
                                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_3);
                                        uart_putc_raw(UART_ID, PREAMBLE_BYTE_4);
                                        uart_putc_raw(UART_ID, MSG_TYPE_ENCRYPTED);
                                        uart_write_blocking(UART_ID, len_bytes_out, 2);
                                        uart_write_blocking(UART_ID, nonce, SST_NONCE_SIZE);
                                        uart_write_blocking(UART_ID, ciphertext_hmac, msg_len_hmac);
                                        uart_write_blocking(UART_ID, tag_hmac, SST_TAG_SIZE);
                                        uart_write_blocking(UART_ID, crc_bytes, 2);
                                        
                                        printf("[Sent HMAC response via LiFi]\n");
                                    }
                                }
                            } else {
                                printf("\n[Error] Challenge timeout.\n");
                            }
                        } 
                        else if (uart_byte == MSG_TYPE_KEY) {
                            // New key provisioning format: [LEN:2][KEY_ID:8][KEY:32]
                            uint8_t len_bytes[2];
                            uint8_t new_id[SST_KEY_ID_SIZE];
                            uint8_t new_key[SST_KEY_SIZE];
                            
                            bool ok = uart_read_blocking_timeout_us(UART_ID, len_bytes, 2, 50000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_id, SST_KEY_ID_SIZE, 100000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_key, SST_KEY_SIZE, 100000);
                            
                            if (ok) {
                                printf("\n[Received New Session Key via LiFi]\n");
                                printf("Received ID: ");
                                for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", new_id[i]);
                                printf("\n");
                                
                                // Write to CURRENT slot (auto-provision)
                                if (pico_write_key_to_slot(current_slot, new_id, new_key)) {
                                    store_last_used_slot((uint8_t)current_slot);
                                    
                                    // Update RAM
                                    keyram_set_with_id(new_id, new_key);
                                    memcpy(session_key, new_key, SST_KEY_SIZE);
                                    memcpy(session_key_id, new_id, SST_KEY_ID_SIZE);
                                    
                                    pico_nonce_on_key_change();
                                    
                                    printf("[Auto-Provision] Key saved to Slot %c and activated.\n", 
                                           current_slot == 0 ? 'A' : 'B');
                                    printf("Enter a message to send over LiFi:\n");
                                } else {
                                    printf("[Error] Failed to save key to flash.\n");
                                }
                            }
                        }
                        else {
                            // Old-style key provisioning (backwards compat) - first byte is KEY_ID[0]
                            uint8_t new_id[SST_KEY_ID_SIZE];
                            uint8_t new_key[SST_KEY_SIZE];
                            
                            new_id[0] = uart_byte; // We already consumed the first byte
                            
                            bool ok = uart_read_blocking_timeout_us(UART_ID, &new_id[1], SST_KEY_ID_SIZE - 1, 100000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_key, SST_KEY_SIZE, 100000);
                            
                            if (ok) {
                                printf("\n[Received New Session Key via LiFi (legacy)]\n");
                                printf("Received ID: ");
                                for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", new_id[i]);
                                printf("\n");
                                
                                if (pico_write_key_to_slot(current_slot, new_id, new_key)) {
                                    store_last_used_slot((uint8_t)current_slot);
                                    keyram_set_with_id(new_id, new_key);
                                    memcpy(session_key, new_key, SST_KEY_SIZE);
                                    memcpy(session_key_id, new_id, SST_KEY_ID_SIZE);
                                    pico_nonce_on_key_change();
                                    printf("[Auto-Provision] Key saved to Slot %c and activated.\n", 
                                           current_slot == 0 ? 'A' : 'B');
                                    printf("Enter a message to send over LiFi:\n");
                                } else {
                                    printf("[Error] Failed to save key to flash.\n");
                                }
                            }
                        }
                        challenge_state = 0;
                        break;
                }
            }
            
            ch = getchar_timeout_us(1000);  // poll USB CDC every 1 ms
            if (ch == PICO_ERROR_TIMEOUT) {
                // watchdog_update(); //when enabled
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                // User pressed Enter: end of input
                message_buffer[msg_len] = '\0';
                putchar('\n');
                break;
            }
            if ((ch == 127 || ch == 8) && msg_len > 0) {
                // Handle backspace
                msg_len--;
                printf("\b \b");  // visually erase the character in terminal
                continue;
            }
            if (msg_len < sizeof(message_buffer) - 1 && ch >= 32 && ch < 127) {
                // Accept only printable ASCII characters
                message_buffer[msg_len++] = ch;
                putchar(ch);
            }
        }

        // Check if the message is actually a command (starts with "CMD:")
        uint8_t current_msg_type = MSG_TYPE_ENCRYPTED;
        
        // Check for FILEB: prefix (File Blob - entire file in one message)
        if (strncmp(message_buffer, "FILEB:", 6) == 0) {
            char *payload = message_buffer + 6;
            size_t payload_len = msg_len - 6;
            
            // Unescape newlines: replace ␊ (UTF-8: E2 90 8A) with \n
            size_t write_idx = 0;
            for (size_t i = 0; i < payload_len; i++) {
                // Check for UTF-8 sequence E2 90 8A (symbol for newline)
                if (i + 2 < payload_len && 
                    (uint8_t)payload[i] == 0xE2 && 
                    (uint8_t)payload[i+1] == 0x90 && 
                    (uint8_t)payload[i+2] == 0x8A) {
                    payload[write_idx++] = '\n';
                    i += 2; // Skip the next 2 bytes
                } else {
                    payload[write_idx++] = payload[i];
                }
            }
            payload_len = write_idx;
            
            printf("[FILEB] Received %zu bytes. Compressing...\n", payload_len);
            
            // Compress the entire file
            heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 4);
            if (hse) {
                uint8_t compressed[8192];
                size_t total_sunk = 0;
                size_t comp_sz = 0;
                
                // IMPORTANT: Must loop on sink() - it may not consume all bytes at once
                while (total_sunk < payload_len) {
                    size_t sunk = 0;
                    HSE_sink_res sres = heatshrink_encoder_sink(hse, 
                        (uint8_t*)payload + total_sunk, 
                        payload_len - total_sunk, 
                        &sunk);
                    total_sunk += sunk;
                    
                    if (sres == HSER_SINK_ERROR_NULL) break;
                    
                    // Poll for output while sinking (encoder may need to flush)
                    HSE_poll_res pres;
                    do {
                        size_t p = 0;
                        pres = heatshrink_encoder_poll(hse, &compressed[comp_sz], 
                                                       sizeof(compressed) - comp_sz, &p);
                        comp_sz += p;
                    } while (pres == HSER_POLL_MORE && comp_sz < sizeof(compressed));
                }
                
                // Signal end of input
                heatshrink_encoder_finish(hse);
                
                // Poll for remaining output after finish
                HSE_poll_res pres;
                do {
                    size_t p = 0;
                    pres = heatshrink_encoder_poll(hse, &compressed[comp_sz], 
                                                   sizeof(compressed) - comp_sz, &p);
                    comp_sz += p;
                } while (pres == HSER_POLL_MORE && comp_sz < sizeof(compressed));
                
                heatshrink_encoder_free(hse);
                
                printf("[FILEB] Compressed %zu -> %zu bytes (%.1f%% reduction)\n", 
                       payload_len, comp_sz, 
                       100.0 * (1.0 - (double)comp_sz / payload_len));
                
                // Now transmit the compressed data as MSG_TYPE_FILE
                if (comp_sz > 0) {
                    memcpy(message_buffer, compressed, comp_sz);
                    msg_len = comp_sz;
                    current_msg_type = MSG_TYPE_FILE;
                } else {
                    printf("[FILEB] Compression produced 0 bytes. Sending raw.\n");
                    memmove(message_buffer, payload, payload_len);
                    msg_len = payload_len;
                    current_msg_type = MSG_TYPE_FILE;
                }
            } else {
                printf("[FILEB] Encoder alloc failed. Sending raw.\n");
                memmove(message_buffer, payload, payload_len);
                msg_len = payload_len;
                current_msg_type = MSG_TYPE_FILE;
            }
        }
        // Check for FILE: prefix (legacy single-chunk)
        else if (strncmp(message_buffer, "FILE:", 5) == 0) {
            char *payload = message_buffer + 5;
            size_t payload_len = msg_len - 5;
            
            // Try to compress
            heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 4);
            if (hse) {
                uint8_t compressed[8192];
                size_t total_sunk = 0;
                size_t comp_sz = 0;
                
                // Loop on sink() until all data consumed
                while (total_sunk < payload_len) {
                    size_t sunk = 0;
                    HSE_sink_res sres = heatshrink_encoder_sink(hse, 
                        (uint8_t*)payload + total_sunk, 
                        payload_len - total_sunk, 
                        &sunk);
                    total_sunk += sunk;
                    if (sres == HSER_SINK_ERROR_NULL) break;
                    
                    HSE_poll_res pres;
                    do {
                        size_t p = 0;
                        pres = heatshrink_encoder_poll(hse, &compressed[comp_sz], 
                                                       sizeof(compressed) - comp_sz, &p);
                        comp_sz += p;
                    } while (pres == HSER_POLL_MORE && comp_sz < sizeof(compressed));
                }
                
                heatshrink_encoder_finish(hse);
                
                // Poll remaining output after finish
                HSE_poll_res pres;
                do {
                    size_t p = 0;
                    pres = heatshrink_encoder_poll(hse, &compressed[comp_sz], 
                                                   sizeof(compressed) - comp_sz, &p);
                    comp_sz += p;
                } while (pres == HSER_POLL_MORE && comp_sz < sizeof(compressed));
                
                heatshrink_encoder_free(hse);
                
                if (comp_sz > 0) {
                    printf("[FILE] Compressed %zu -> %zu bytes.\n", payload_len, comp_sz);
                    memcpy(message_buffer, compressed, comp_sz);
                    msg_len = comp_sz;
                    current_msg_type = MSG_TYPE_FILE;
                } else {
                    printf("[FILE] Comp failed (0 bytes). Sending raw.\n");
                    memmove(message_buffer, payload, payload_len);
                    msg_len = payload_len;
                    current_msg_type = MSG_TYPE_FILE;
                }
            } else {
                printf("[FILE] Alloc failed. Sending raw.\n");
                memmove(message_buffer, payload, payload_len);
                msg_len = payload_len;
                current_msg_type = MSG_TYPE_FILE;
            }
        }

        if (strncmp(message_buffer, "CMD:", 4) == 0) {
            // Extract the command part (skip the "CMD:" prefix)
            const char *cmd = message_buffer + 4;

            // Run the command handler and check if it modified the active
            // session key (e.g., load new key, clear key, or switch slots).
            bool key_changed = handle_commands(cmd, session_key, &current_slot);

            // If the key was changed, reset the nonce generator so future
            // messages start fresh with a new salt+counter sequence tied to the
            // new key.
            if (key_changed) {
                pico_nonce_on_key_change();
                // Reload both from current slot to be safe and get the matching ID
                // Best practice: Reload from flash slot that is now active.
                if (current_slot == 0 || current_slot == 1) {
                    pico_read_key_pair_from_slot(current_slot, session_key_id, session_key);
                }
            }

            // Clear out the message buffer so stale command data isn’t reused.
            memset(message_buffer, 0, sizeof(message_buffer));

            // Skip the normal "send over LiFi" logic since this was a command.
            continue;
        }

        // Check if message exceeds the encryption buffer size
        if (msg_len > sizeof(ciphertext)) {
            printf("Message too long!\n");
            continue;
        }
        // Ensure session key is valid before proceeding with encryption
        if (is_key_zeroed(session_key)) {
            printf("No valid key in the current slot. Cannot send message.\n");
            printf("Use 'CMD: new key' or switch to a valid slot.\n");
            continue;
        }

        uint8_t nonce[SST_NONCE_SIZE];
        pico_nonce_generate(
            nonce);  // 96-bit nonce = boot_salt||counter (unique per message)

        int ret =
            sst_encrypt_gcm(session_key, nonce, (const uint8_t *)message_buffer,
                            msg_len, ciphertext, tag);
        if (ret != 0) {
            printf("Encryption failed! ret=%d\n", ret);
            continue;
        }

        // Build frame: [PREAMBLE:4][TYPE:1][LEN:2][NONCE:12][CIPHERTEXT:msg_len][TAG:16][CRC16:2]
        // Total payload after TYPE = NONCE + CIPHERTEXT + TAG = 12 + msg_len + 16
        size_t payload_len = SST_NONCE_SIZE + msg_len + SST_TAG_SIZE;
        uint8_t len_bytes[2] = {(payload_len >> 8) & 0xFF, payload_len & 0xFF};
        
        // Build CRC buffer: TYPE + LEN + NONCE + CIPHERTEXT + TAG
        uint8_t crc_buf[1 + 2 + SST_NONCE_SIZE + 8192 + SST_TAG_SIZE];  // Increased to match message buffer
        size_t crc_idx = 0;
        crc_buf[crc_idx++] = current_msg_type;
        crc_buf[crc_idx++] = len_bytes[0];
        crc_buf[crc_idx++] = len_bytes[1];
        memcpy(&crc_buf[crc_idx], nonce, SST_NONCE_SIZE); crc_idx += SST_NONCE_SIZE;
        memcpy(&crc_buf[crc_idx], ciphertext, msg_len); crc_idx += msg_len;
        memcpy(&crc_buf[crc_idx], tag, SST_TAG_SIZE); crc_idx += SST_TAG_SIZE;
        
        uint16_t crc = crc16_ccitt(crc_buf, crc_idx);
        uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
        
        // Send frame
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_3);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_4);
        uart_putc_raw(UART_ID, current_msg_type);
        uart_write_blocking(UART_ID, len_bytes, 2);
        uart_write_blocking(UART_ID, nonce, SST_NONCE_SIZE);
        uart_write_blocking(UART_ID, ciphertext, msg_len);
        uart_write_blocking(UART_ID, tag, SST_TAG_SIZE);
        uart_write_blocking(UART_ID, crc_bytes, 2);

        gpio_put(25, 1);
        sleep_ms(100);
        gpio_put(25, 0);

        // Clear sensitive data from memory
        secure_zero(ciphertext, sizeof(ciphertext));
        secure_zero(tag, sizeof(tag));
        secure_zero(nonce, sizeof(nonce));
        secure_zero(message_buffer, sizeof(message_buffer));
    }

    return 0;
}
