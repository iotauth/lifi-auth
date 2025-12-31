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
#define SST_MAC_KEY_SIZE 32
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
    uint8_t session_mac_key[SST_MAC_KEY_SIZE] = {0}; // 32 bytes
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

    // Static buffers - too large for Pico's 8KB stack
    static char message_buffer[8192];
    static uint8_t ciphertext[8192];
    static uint8_t compressed_buf[8192];  // For FILEB/FILE compression
    static uint8_t crc_buf[1 + 2 + 12 + 8192 + 16];  // TYPE + LEN + NONCE + CIPHERTEXT + TAG

    while (true) {
        // can add function to show A(VALID): or A(INVALID): or B(VALID): etc..
        //  in message preview -- show validity and which slot we are on !
        size_t msg_len = 0;
        int ch;  // character
        memset(ciphertext, 0, sizeof(ciphertext));
        uint8_t tag[SST_TAG_SIZE] = {0};

        for (;;) {
            // Drain all available UART bytes before checking USB (prevents FIFO overflow)
            while (uart_is_readable(UART_ID)) {
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
                        else if (uart_byte == PREAMBLE_BYTE_1) challenge_state = 1; // Overlapping preamble start
                        else challenge_state = 0;
                        break;
                    case 2:
                        if (uart_byte == PREAMBLE_BYTE_3) challenge_state = 3;
                        else if (uart_byte == PREAMBLE_BYTE_1) challenge_state = 1;
                        else challenge_state = 0;
                        break;
                    case 3:
                        if (uart_byte == PREAMBLE_BYTE_4) challenge_state = 4;
                        else if (uart_byte == PREAMBLE_BYTE_1) challenge_state = 1;
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
                                
                                // DEBUG: Print what we are hashing
                                printf("DEBUG: Hashing Challenge[0..3]: %02X %02X %02X %02X using MAC_KEY[0..3]: %02X %02X %02X %02X\n",
                                    challenge_buffer[0], challenge_buffer[1], challenge_buffer[2], challenge_buffer[3],
                                    session_mac_key[0], session_mac_key[1], session_mac_key[2], session_mac_key[3]);

                                // Compute HMAC response using MAC KEY (32 bytes)
                                uint8_t hmac[HMAC_SIZE];
                                int hmac_ret = sst_hmac_sha256(session_mac_key, challenge_buffer, 
                                                         CHALLENGE_SIZE, hmac);
                                
                                if (hmac_ret == 0) {
                                    // Convert HMAC to hex string
                                    char hmac_msg[5 + HMAC_SIZE * 2 + 1];  // "HMAC:" + hex + null
                                    strcpy(hmac_msg, "HMAC:");
                                    for (int i = 0; i < HMAC_SIZE; i++) {
                                        sprintf(hmac_msg + 5 + (i * 2), "%02X", hmac[i]);
                                    }
                                    printf("Computed Response: %s\n", hmac_msg);
                                    
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
                                        uint8_t crc_buf_hmac[1 + 2 + SST_NONCE_SIZE + sizeof(hmac_msg) + SST_TAG_SIZE];
                                        size_t crc_idx = 0;
                                        crc_buf_hmac[crc_idx++] = MSG_TYPE_ENCRYPTED;
                                        crc_buf_hmac[crc_idx++] = len_bytes_out[0];
                                        crc_buf_hmac[crc_idx++] = len_bytes_out[1];
                                        memcpy(&crc_buf_hmac[crc_idx], nonce, SST_NONCE_SIZE); crc_idx += SST_NONCE_SIZE;
                                        memcpy(&crc_buf_hmac[crc_idx], ciphertext_hmac, msg_len_hmac); crc_idx += msg_len_hmac;
                                        memcpy(&crc_buf_hmac[crc_idx], tag_hmac, SST_TAG_SIZE); crc_idx += SST_TAG_SIZE;
                                        uint16_t crc = crc16_ccitt(crc_buf_hmac, crc_idx);
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
                                printf("\n[Error] Challenge timeout. Flushing RX.\n");
                                // Flush any remaining garbage (noise/partial frame) to clean the line
                                while (uart_is_readable(UART_ID)) {
                                    (void)uart_getc(UART_ID);
                                }
                            }
                        } 
                        else if (uart_byte == MSG_TYPE_KEY) {
                            // New key format: [LEN:2][KEY_ID:8][CIPHER_KEY:16][MAC_KEY:32]
                            uint8_t len_bytes[2];
                            uint8_t new_id[SST_KEY_ID_SIZE];
                            uint8_t new_key[SST_KEY_SIZE];
                            uint8_t new_mac_key[SST_MAC_KEY_SIZE];
                            
                            bool ok = uart_read_blocking_timeout_us(UART_ID, len_bytes, 2, 100000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_id, SST_KEY_ID_SIZE, 100000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_key, SST_KEY_SIZE, 100000);
                            if (ok) ok = uart_read_blocking_timeout_us(UART_ID, new_mac_key, SST_MAC_KEY_SIZE, 100000);
                            
                            if (ok) {
                                printf("\n[Received New Session Key via LiFi]\n");
                                printf("Received ID: ");
                                for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", new_id[i]);
                                printf("\n");
                                
                                // Write to CURRENT slot (Cipher Key only for now as flash struct unsure)
                                if (pico_write_key_to_slot(current_slot, new_id, new_key)) {
                                    store_last_used_slot((uint8_t)current_slot);
                                    
                                    // DEBUG: Print received keys (Full)
                                    printf("DEBUG: Recv Cipher: ");
                                    for(int i=0; i<SST_KEY_SIZE; i++) printf("%02X ", new_key[i]);
                                    printf("\nDEBUG: Recv MAC:    ");
                                    for(int i=0; i<SST_MAC_KEY_SIZE; i++) printf("%02X ", new_mac_key[i]);
                                    printf("\n");

                                    // Update RAM
                                    keyram_set_with_id(new_id, new_key);
                                    memcpy(session_key, new_key, SST_KEY_SIZE);
                                    memcpy(session_key_id, new_id, SST_KEY_ID_SIZE);
                                    memcpy(session_mac_key, new_mac_key, SST_MAC_KEY_SIZE);
                                    
                                    pico_nonce_on_key_change();
                                    
                                    printf("[Auto-Provision] Key saved to Slot %c and activated.\n", 
                                           current_slot == 0 ? 'A' : 'B');
                                    printf("(MAC Key updated in RAM)\n");
                                } else {
                                    printf("[Error] Failed to save key to flash.\n");
                                }
                            } else {
                                printf("\n[Error] Key update timeout (Waiting for MAC Key?). Flushing RX.\n");
                                while (uart_is_readable(UART_ID)) (void)uart_getc(UART_ID);
                            }
                        }
                        else {
                            // Unknown/Legacy fallback removed to strict protocol enforcement
                            // Reset state handled by loop break
                        }
                        challenge_state = 0;
                        break;
                }
            }
            
            // Use non-blocking read (0 timeout) or very short timeout to prevent
            // UART FIFO overflow. 1Mbps = ~10us/byte. 32-byte FIFO fills in 320us.
            ch = getchar_timeout_us(0);  // Non-blocking poll
            if (ch == PICO_ERROR_TIMEOUT) {
                // watchdog_update(); //when enabled
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                // Peak ahead 2ms to see if more data follows (Streaming/Paste)
                int next_ch = getchar_timeout_us(2000);
                
                if (next_ch != PICO_ERROR_TIMEOUT) {
                    // Start of Stream / Burst detected
                    if (msg_len < sizeof(message_buffer) - 1) {
                        message_buffer[msg_len++] = '\n'; 
                        putchar('\n');
                    }

                    // Process the peeked character aggressively
                    if (msg_len < sizeof(message_buffer) - 1) {
                         if (next_ch >= 32 && next_ch < 127) {
                              message_buffer[msg_len++] = next_ch;
                              putchar(next_ch);
                         } else if (next_ch == '\r' || next_ch == '\n') {
                              message_buffer[msg_len++] = '\n';
                              putchar('\n');
                         }
                    }
                    continue; // Keep buffering
                } else {
                    // Regular interactive Enter
                    message_buffer[msg_len] = '\0';
                    putchar('\n');
                    break;
                }
            }
            if ((ch == 127 || ch == 8) && msg_len > 0) {
                // Handle backspace
                msg_len--;
                printf("\b \b");
                continue;
            }
            if (msg_len < sizeof(message_buffer) - 1 && ch >= 32 && ch < 127) {
                message_buffer[msg_len++] = ch;
                putchar(ch);
            }
            
            if (msg_len >= sizeof(message_buffer) - 128) {
                message_buffer[msg_len] = '\0';
                printf("\n[Auto-Send: Buffer Full]\n");
                break;
            }
        }

        // Default message type (may be changed to MSG_TYPE_FILE if compressed later and NOT a Command)
        uint8_t current_msg_type = MSG_TYPE_ENCRYPTED;

        if (strncmp(message_buffer, "CMD:", 4) == 0) {
            // Extract the command part (skip the "CMD:" prefix)
            const char *cmd = message_buffer + 4;
            
            const char *cmd_trimmed = cmd;
            while (*cmd_trimmed == ' ') cmd_trimmed++; // Skip leading spaces for our check
            
            // Special Command: Send Key ID Plaintext
            if (strncmp(cmd_trimmed, "send_id", 7) == 0) {
                 printf("[TX] Sending Key ID...\n");
                 
                 // Payload = 8 bytes of Key ID
                 uint8_t payload[SESSION_KEY_ID_SIZE];
                 memcpy(payload, session_key_id, SESSION_KEY_ID_SIZE);
                 size_t payload_len = SESSION_KEY_ID_SIZE;
                 
                 // Calculate CRC: TYPE + LEN + PAYLOAD
                 uint8_t len_bytes[2] = {(payload_len >> 8) & 0xFF, payload_len & 0xFF};
                 size_t crc_idx = 0;
                 crc_buf[crc_idx++] = MSG_TYPE_KEY_ID_ONLY;
                 crc_buf[crc_idx++] = len_bytes[0];
                 crc_buf[crc_idx++] = len_bytes[1];
                 memcpy(&crc_buf[crc_idx], payload, payload_len);
                 crc_idx += payload_len;
                 
                 uint16_t crc = crc16_ccitt(crc_buf, crc_idx);
                 uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
                 
                 // Send Frame
                 uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
                 uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
                 uart_putc_raw(UART_ID, PREAMBLE_BYTE_3);
                 uart_putc_raw(UART_ID, PREAMBLE_BYTE_4);
                 uart_putc_raw(UART_ID, MSG_TYPE_KEY_ID_ONLY);
                 uart_write_blocking(UART_ID, len_bytes, 2);
                 uart_tx_wait_blocking(UART_ID);
                 
                 uart_write_blocking(UART_ID, payload, payload_len);
                 uart_tx_wait_blocking(UART_ID);
                 
                 uart_write_blocking(UART_ID, crc_bytes, 2);
                 uart_tx_wait_blocking(UART_ID);
                 
                 memset(message_buffer, 0, sizeof(message_buffer));
                 continue;
            }

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

        // Auto-Compression for large payloads (>128 bytes) logic
        // This effectively replaces the old FILEB: specific logic with a general purpose one.
        if (msg_len > 128) { 
             heatshrink_encoder *hse = heatshrink_encoder_alloc(8, 4);
             if (hse) {
                 size_t sunk = 0;
                 size_t comp_sz = 0;
                 
                 // Sink the whole payload to encoder
                 heatshrink_encoder_sink(hse, (uint8_t*)message_buffer, msg_len, &sunk);
                 
                 // Poll compressed data
                 HSE_poll_res pres;
                 do {
                     size_t p = 0;
                     pres = heatshrink_encoder_poll(hse, &compressed_buf[comp_sz], 
                                                  sizeof(compressed_buf) - comp_sz, &p);
                     comp_sz += p;
                 } while (pres == HSER_POLL_MORE && comp_sz < sizeof(compressed_buf));
                 
                 // Finish
                 if (heatshrink_encoder_finish(hse) == HSER_FINISH_MORE) {
                      size_t p = 0;
                      heatshrink_encoder_poll(hse, &compressed_buf[comp_sz], 
                                            sizeof(compressed_buf) - comp_sz, &p);
                      comp_sz += p;
                 }
                 
                 heatshrink_encoder_free(hse);
                 
                 // Only use compressed version if it is actually smaller
                 if (comp_sz < msg_len) {
                     printf("[Auto-Compress] %zu -> %zu bytes (%.1f%% saved)\n", 
                            msg_len, comp_sz, (1.0f - (float)comp_sz/msg_len)*100.0f);
                     
                     if (comp_sz < sizeof(message_buffer)) {
                         memcpy(message_buffer, compressed_buf, comp_sz);
                         msg_len = comp_sz;
                         current_msg_type = MSG_TYPE_FILE; // Signals receiver to decompress
                     }
                 }
             }
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
        
        // Build CRC buffer: TYPE + LEN + NONCE + CIPHERTEXT + TAG (uses static crc_buf)
        size_t crc_idx = 0;
        crc_buf[crc_idx++] = current_msg_type;
        crc_buf[crc_idx++] = len_bytes[0];
        crc_buf[crc_idx++] = len_bytes[1];
        memcpy(&crc_buf[crc_idx], nonce, SST_NONCE_SIZE); crc_idx += SST_NONCE_SIZE;
        memcpy(&crc_buf[crc_idx], ciphertext, msg_len); crc_idx += msg_len;
        memcpy(&crc_buf[crc_idx], tag, SST_TAG_SIZE); crc_idx += SST_TAG_SIZE;
        
        uint16_t crc = crc16_ccitt(crc_buf, crc_idx);
        uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};
        
        // Send preamble and header
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_3);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_4);
        uart_putc_raw(UART_ID, current_msg_type);
        uart_write_blocking(UART_ID, len_bytes, 2);
        uart_tx_wait_blocking(UART_ID);
        sleep_us(250);  // 250us after header
        
        // Send nonce in small chunks
        uart_write_blocking(UART_ID, nonce, SST_NONCE_SIZE);
        uart_tx_wait_blocking(UART_ID);
        sleep_us(250);  // 250us after nonce
        
        // Send ciphertext in 256-byte chunks with delays
        const size_t CHUNK_SIZE = 256;
        for (size_t offset = 0; offset < msg_len; offset += CHUNK_SIZE) {
            size_t chunk = (msg_len - offset > CHUNK_SIZE) ? CHUNK_SIZE : (msg_len - offset);
            uart_write_blocking(UART_ID, ciphertext + offset, chunk);
            uart_tx_wait_blocking(UART_ID);
            sleep_us(250);  // 250us after each chunk
        }
        
        // Send tag in chunks
        uart_write_blocking(UART_ID, tag, SST_TAG_SIZE);
        uart_tx_wait_blocking(UART_ID);
        sleep_us(250);  // 250us after tag
        
        // Send CRC
        uart_write_blocking(UART_ID, crc_bytes, 2);
        uart_tx_wait_blocking(UART_ID);

        // Clear sensitive data from memory
        secure_zero(ciphertext, sizeof(ciphertext));
        secure_zero(tag, sizeof(tag));
        secure_zero(nonce, sizeof(nonce));
        secure_zero(message_buffer, sizeof(message_buffer));
    }

    return 0;
}
