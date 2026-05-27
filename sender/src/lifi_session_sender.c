#include <stdio.h>
#include <string.h>

#include "../../include/cmd_handler.h"
#include "../../include/pico_handler.h"
#include "../../include/protocol.h"
#include "../../include/sst_crypto_embedded.h"
#include "mbedtls/aes.h"
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
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "lifi_multi_tx.pio.h" // Generated header

#define UART_ID_DEBUG uart0
#define UART_RX_PIN_DEBUG 1
#define UART_TX_PIN_DEBUG 0

#define UART_ID uart1
#define UART_RX_PIN 5
// TX is now handled by PIO on multiple pins
#define PIO_TX_PIN_BASE 6 // GP6
#define PIO_TX_PIN_COUNT 4 // GP6, GP7, GP8, GP9

#define BAUD_RATE 1000000
#define SST_MAC_KEY_SIZE 32

// PIO Globals
PIO pio = pio0;
uint sm = 0;

// LED Mask Global (Default: All On = 0xF)
// Bit 0: GP6 (White)
// Bit 1: GP7 (Green)
// Bit 2: GP8 (Blue)
// Bit 3: GP9 (Red)
static uint8_t active_led_mask = 0x0F;

void set_led_mask(uint8_t mask) {
    active_led_mask = mask;
    for (int i = 0; i < PIO_TX_PIN_COUNT; i++) {
        uint pin = PIO_TX_PIN_BASE + i;
        if (mask & (1 << i)) {
            // Enable: Set to PIO function
            gpio_set_function(pin, GPIO_FUNC_PIO0);
        } else {
            // Disable: Set to SIO and drive Low
            gpio_set_function(pin, GPIO_FUNC_SIO);
            gpio_put(pin, 0);
        }
    }
}

void lifi_send_byte(uint8_t byte) {
    pio_sm_put_blocking(pio, sm, (uint32_t)byte);
}

void lifi_send_bytes(const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        lifi_send_byte(src[i]);
    }
}

// Helper for waiting? PIO blocks on FIFO full, so no wait needed unless we want to wait for drain.
void lifi_wait_tx() {
    // Wait until SM is stalled (TX buffer empty)
    // There isn't a direct "fifo empty" check that guarantees the shift register is empty too
    // checking stall is a decent proxy if we know we stopped pushing.
    while (!pio_sm_is_tx_fifo_empty(pio, sm)) tight_loop_contents();
    // sleep_us(100); // Small guard
}

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

    current_slot = 0; // Always default to A
    printf("Defaulting to Slot A (per configuration).\n");

    printf("PICO STARTED\n");

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);

    uart_init(UART_ID_DEBUG, BAUD_RATE);
    gpio_set_function(UART_TX_PIN_DEBUG, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN_DEBUG, GPIO_FUNC_UART);

    // Init UART for RX Only (TX is PIO)
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    // gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART); // Disable Hardware UART TX on GP4

    // Init PIO for Multi-Channel TX
    uint offset = pio_add_program(pio, &lifi_multi_tx_program);
    
    // Calculate 1Mbps divider
    // sys_clk / (div * cycles_per_bit) = baud
    // cycles_per_bit in PIO = 8 (see .pio file)
    // 125MHz / (div * 8) = 1MHz => div = 125/8 = 15.625
    float div = (float)clock_get_hz(clk_sys) / (8 * BAUD_RATE);
    
    lifi_multi_tx_program_init(pio, sm, offset, PIO_TX_PIN_BASE, PIO_TX_PIN_COUNT, div);
    
    // Set initial mask (Enables PIO function on all pins)
    set_led_mask(0x0F);

    // Flush UART input buffer to discard any leftover or garbage data.
    while (uart_is_readable(UART_ID)) {
        volatile uint8_t _ = uart_getc(UART_ID);
    }

    uint8_t session_key[SST_KEY_SIZE] = {0};
    uint8_t session_mac_key[SST_MAC_KEY_SIZE] = {0}; // 32 bytes
    uint8_t session_key_id[SST_KEY_ID_SIZE] = {0};
    // Preserved across HS1→HS3: Pico's own nonce sent in HS2; zeroed after HS3 verify
    static uint8_t saved_pico_nonce[SST_HS_NONCE_SIZE];

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
        size_t msg_len = 0;
        int ch;  // character
        memset(ciphertext, 0, sizeof(ciphertext));
        uint8_t tag[SST_TAG_SIZE] = {0};

        for (;;) {
            // Drain all available UART bytes before checking USB (prevents FIFO overflow)
            while (uart_is_readable(UART_ID)) {
                static uint8_t uart_byte;
                static int challenge_state = 0;
                
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
                        if (uart_byte == MSG_TYPE_SST_HS1) {
                            // SST 3-way handshake step 1 received from Pi4
                            // Format: [KEY_ID:8][IV:16][AES-128-CBC({0x01,nonce_e}):16][HMAC-SHA256:32]
                            uint8_t len_bytes[2];
                            if (!uart_read_blocking_timeout_us(UART_ID, len_bytes, 2, 50000)) {
                                printf("[SST HS1] Timeout reading length. Flushing.\n");
                                while (uart_is_readable(UART_ID)) (void)uart_getc(UART_ID);
                                challenge_state = 0; break;
                            }
                            uint16_t hs1_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                            if (hs1_len != SST_HS1_PAYLOAD_SIZE) {
                                printf("[SST HS1] Bad length %u. Flushing.\n", hs1_len);
                                while (uart_is_readable(UART_ID)) (void)uart_getc(UART_ID);
                                challenge_state = 0; break;
                            }

                            uint8_t hs1[SST_HS1_PAYLOAD_SIZE];
                            if (!uart_read_blocking_timeout_us(UART_ID, hs1, hs1_len, 200000)) {
                                printf("[SST HS1] Timeout reading payload.\n");
                                challenge_state = 0; break;
                            }

                            // Parse: key_id(8) | blob(64): iv(16)|ctext(16)|hmac(32)
                            const uint8_t *key_id   = hs1;
                            const uint8_t *blob     = hs1 + SESSION_KEY_ID_SIZE;
                            const uint8_t *iv       = blob;
                            const uint8_t *ctext    = blob + SST_HS_IV_SIZE;
                            const uint8_t *recv_mac = blob + SST_HS_IV_SIZE + 16;

                            // 1. Verify key_id
                            if (memcmp(key_id, session_key_id, SESSION_KEY_ID_SIZE) != 0) {
                                printf("[SST HS1] Key ID mismatch.\n");
                                challenge_state = 0; break;
                            }

                            // 2. Verify HMAC-SHA256(mac_key_32, IV||ctext)
                            uint8_t computed_mac[SST_HS_MAC_SIZE];
                            if (sst_hmac_sha256_ex(session_mac_key, SST_MAC_KEY_SIZE,
                                                   blob, SST_HS_IV_SIZE + 16,
                                                   computed_mac) != 0 ||
                                memcmp(computed_mac, recv_mac, SST_HS_MAC_SIZE) != 0) {
                                printf("[SST HS1] HMAC verification failed.\n");
                                secure_zero(computed_mac, sizeof(computed_mac));
                                challenge_state = 0; break;
                            }
                            secure_zero(computed_mac, sizeof(computed_mac));

                            // 3. AES-128-CBC decrypt → {indicator(1), entity_nonce(8), pkcs7_pad}
                            uint8_t decrypted[16];
                            if (sst_aes_128_cbc_decrypt(session_key, iv, ctext, 16, decrypted) != 0) {
                                printf("[SST HS1] Decrypt failed.\n");
                                challenge_state = 0; break;
                            }

                            // Validate PKCS7 pad (9 bytes input → 7 bytes of 0x07 padding)
                            uint8_t pad = decrypted[15];
                            if (pad < 1 || pad > 16) {
                                printf("[SST HS1] Invalid PKCS7 padding.\n");
                                secure_zero(decrypted, sizeof(decrypted));
                                challenge_state = 0; break;
                            }

                            // 4. Extract entity_nonce from decrypted[1..8]
                            uint8_t entity_nonce[SST_HS_NONCE_SIZE];
                            memcpy(entity_nonce, decrypted + 1, SST_HS_NONCE_SIZE);
                            secure_zero(decrypted, sizeof(decrypted));
                            printf("[SST HS1] OK. Building HS2.\n");

                            // 5. Generate pico_nonce (8 bytes)
                            uint8_t pico_nonce[SST_HS_NONCE_SIZE];
                            uint32_t r0 = get_rand_32(), r1 = get_rand_32();
                            memcpy(pico_nonce,     &r0, 4);
                            memcpy(pico_nonce + 4, &r1, 4);

                            // 6. Build HS2 plaintext: [0x02][pico_nonce:8][entity_nonce:8] = 17 bytes
                            uint8_t hs2_plain[1 + SST_HS_NONCE_SIZE * 2];
                            hs2_plain[0] = 2;
                            memcpy(hs2_plain + 1,                    pico_nonce,   SST_HS_NONCE_SIZE);
                            memcpy(hs2_plain + 1 + SST_HS_NONCE_SIZE, entity_nonce, SST_HS_NONCE_SIZE);

                            // 7. Generate IV2 (16 bytes)
                            uint8_t iv2[SST_HS_IV_SIZE];
                            uint32_t r2=get_rand_32(), r3=get_rand_32(),
                                     r4=get_rand_32(), r5=get_rand_32();
                            memcpy(iv2,    &r2,4); memcpy(iv2+4, &r3,4);
                            memcpy(iv2+8,  &r4,4); memcpy(iv2+12,&r5,4);

                            // 8. AES-128-CBC encrypt with PKCS7 → 32 bytes
                            uint8_t hs2_ctext[32];
                            size_t  hs2_ctext_len = 0;
                            if (sst_aes_128_cbc_encrypt_pkcs7(session_key, iv2,
                                    hs2_plain, sizeof(hs2_plain),
                                    hs2_ctext, &hs2_ctext_len) != 0) {
                                printf("[SST HS1] HS2 encrypt failed.\n");
                                secure_zero(entity_nonce, sizeof(entity_nonce));
                                secure_zero(pico_nonce,   sizeof(pico_nonce));
                                secure_zero(hs2_plain,    sizeof(hs2_plain));
                                challenge_state = 0; break;
                            }

                            // 9. Build HS2 blob = IV2(16)|ctext(32)|HMAC(32) = 80 bytes
                            uint8_t hs2_blob[SST_HS2_PAYLOAD_SIZE];
                            memcpy(hs2_blob,                          iv2,       SST_HS_IV_SIZE);
                            memcpy(hs2_blob + SST_HS_IV_SIZE,          hs2_ctext, hs2_ctext_len);
                            uint8_t hs2_mac[SST_HS_MAC_SIZE];
                            if (sst_hmac_sha256_ex(session_mac_key, SST_MAC_KEY_SIZE,
                                                   hs2_blob, SST_HS_IV_SIZE + hs2_ctext_len,
                                                   hs2_mac) != 0) {
                                printf("[SST HS1] HS2 HMAC failed.\n");
                                secure_zero(entity_nonce, sizeof(entity_nonce));
                                secure_zero(pico_nonce,   sizeof(pico_nonce));
                                secure_zero(hs2_plain,    sizeof(hs2_plain));
                                challenge_state = 0; break;
                            }
                            memcpy(hs2_blob + SST_HS_IV_SIZE + hs2_ctext_len, hs2_mac, SST_HS_MAC_SIZE);

                            // 10. Frame and send HS2 over LiFi
                            uint8_t hs2_len_bytes[2] = {
                                (SST_HS2_PAYLOAD_SIZE >> 8) & 0xFF,
                                 SST_HS2_PAYLOAD_SIZE & 0xFF
                            };
                            // CRC over TYPE|LEN|PAYLOAD
                            uint8_t crc_buf_hs2[1 + 2 + SST_HS2_PAYLOAD_SIZE];
                            crc_buf_hs2[0] = MSG_TYPE_SST_HS2;
                            crc_buf_hs2[1] = hs2_len_bytes[0];
                            crc_buf_hs2[2] = hs2_len_bytes[1];
                            memcpy(&crc_buf_hs2[3], hs2_blob, SST_HS2_PAYLOAD_SIZE);
                            uint16_t crc = crc16_ccitt(crc_buf_hs2, sizeof(crc_buf_hs2));
                            uint8_t crc_bytes[2] = {(crc >> 8) & 0xFF, crc & 0xFF};

                            lifi_send_byte(PREAMBLE_BYTE_1);
                            lifi_send_byte(PREAMBLE_BYTE_2);
                            lifi_send_byte(PREAMBLE_BYTE_3);
                            lifi_send_byte(PREAMBLE_BYTE_4);
                            lifi_send_byte(MSG_TYPE_SST_HS2);
                            lifi_send_bytes(hs2_len_bytes, 2);
                            lifi_send_bytes(hs2_blob, SST_HS2_PAYLOAD_SIZE);
                            lifi_send_bytes(crc_bytes, 2);
                            lifi_wait_tx();

                            // Save pico_nonce so HS3 handler can verify Pi4 echoed it correctly
                            memcpy(saved_pico_nonce, pico_nonce, SST_HS_NONCE_SIZE);

                            // Clear sensitive stack data (pico_nonce lives in saved_pico_nonce now)
                            secure_zero(entity_nonce, sizeof(entity_nonce));
                            secure_zero(pico_nonce,   sizeof(pico_nonce));
                            secure_zero(hs2_plain,    sizeof(hs2_plain));
                            secure_zero(hs2_ctext,    sizeof(hs2_ctext));
                            secure_zero(hs2_mac,      sizeof(hs2_mac));
                            printf("[SST HS2] Sent over LiFi. Waiting for HS3.\n");
                        }
                        else if (uart_byte == MSG_TYPE_SST_HS3) {
                            // HS3: Pi4→Pico over UART.
                            // Format: IV(16) | AES-CBC({0x03,entity_nonce,pico_nonce}→32)(32) | HMAC(32)
                            // Verifying HS3 proves Pi4 holds the SST key issued by Auth.
                            uint8_t len_bytes[2];
                            if (!uart_read_blocking_timeout_us(UART_ID, len_bytes, 2, 50000)) {
                                printf("[SST HS3] Timeout reading length.\n");
                                challenge_state = 0; break;
                            }
                            uint16_t hs3_len = ((uint16_t)len_bytes[0] << 8) | len_bytes[1];
                            if (hs3_len != SST_HS3_PAYLOAD_SIZE) {
                                printf("[SST HS3] Bad length %u.\n", hs3_len);
                                challenge_state = 0; break;
                            }
                            uint8_t hs3[SST_HS3_PAYLOAD_SIZE];
                            if (!uart_read_blocking_timeout_us(UART_ID, hs3, hs3_len, 200000)) {
                                printf("[SST HS3] Timeout reading payload.\n");
                                challenge_state = 0; break;
                            }

                            // Split: IV(16) | ctext(32) | HMAC(32)
                            const uint8_t *iv3    = hs3;
                            const uint8_t *ctext3 = hs3 + SST_HS_IV_SIZE;
                            const uint8_t *mac3   = hs3 + SST_HS_IV_SIZE + 32;

                            // 1. Verify HMAC(mac_key:32, IV||ctext:48)
                            uint8_t computed_mac3[SST_HS_MAC_SIZE];
                            if (sst_hmac_sha256_ex(session_mac_key, SST_MAC_KEY_SIZE,
                                                   hs3, SST_HS_IV_SIZE + 32,
                                                   computed_mac3) != 0 ||
                                memcmp(computed_mac3, mac3, SST_HS_MAC_SIZE) != 0) {
                                printf("[SST HS3] HMAC verification FAILED – Pi4 not trusted.\n");
                                secure_zero(computed_mac3, sizeof(computed_mac3));
                                secure_zero(saved_pico_nonce, sizeof(saved_pico_nonce));
                                challenge_state = 0; break;
                            }
                            secure_zero(computed_mac3, sizeof(computed_mac3));

                            // 2. AES-128-CBC decrypt → {indicator(1), entity_nonce(8), pico_nonce(8), pad}
                            uint8_t plain3[32];
                            if (sst_aes_128_cbc_decrypt(session_key, iv3, ctext3, 32, plain3) != 0) {
                                printf("[SST HS3] Decrypt failed.\n");
                                secure_zero(saved_pico_nonce, sizeof(saved_pico_nonce));
                                challenge_state = 0; break;
                            }

                            // 3. Validate PKCS7 padding and indicator byte
                            uint8_t pad3 = plain3[31];
                            bool hs3_ok = (pad3 >= 1 && pad3 <= 16 && plain3[0] == 0x03);

                            // 4. Compare echoed pico_nonce (plain3[9..16]) with what we sent in HS2
                            if (hs3_ok && memcmp(plain3 + 1 + SST_HS_NONCE_SIZE,
                                                 saved_pico_nonce, SST_HS_NONCE_SIZE) == 0) {
                                printf("[SST HS3] MUTUAL AUTH OK: Pi4 verified via SST Auth key.\n");
                                gpio_put(25, 1); // LED on = authenticated
                            } else {
                                printf("[SST HS3] FAILED: Pi4 nonce echo mismatch – not trusted.\n");
                            }

                            secure_zero(plain3, sizeof(plain3));
                            secure_zero(saved_pico_nonce, sizeof(saved_pico_nonce));
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
                 lifi_send_byte(PREAMBLE_BYTE_1);
                 lifi_send_byte(PREAMBLE_BYTE_2);
                 lifi_send_byte(PREAMBLE_BYTE_3);
                 lifi_send_byte(PREAMBLE_BYTE_4);
                 lifi_send_byte(MSG_TYPE_KEY_ID_ONLY);
                 lifi_send_bytes(len_bytes, 2);
                 lifi_send_bytes(payload, payload_len);
                 lifi_send_bytes(crc_bytes, 2);
                 lifi_wait_tx();
                 
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
        lifi_send_byte(PREAMBLE_BYTE_1);
        lifi_send_byte(PREAMBLE_BYTE_2);
        lifi_send_byte(PREAMBLE_BYTE_3);
        lifi_send_byte(PREAMBLE_BYTE_4);
        lifi_send_byte(current_msg_type);
        lifi_send_bytes(len_bytes, 2);
        sleep_us(250);  // 250us after header
        
        // Send nonce in small chunks
        lifi_send_bytes(nonce, SST_NONCE_SIZE);
        sleep_us(250);  // 250us after nonce
        
        // Send ciphertext in 256-byte chunks with delays
        const size_t CHUNK_SIZE = 256;
        for (size_t offset = 0; offset < msg_len; offset += CHUNK_SIZE) {
            size_t chunk = (msg_len - offset > CHUNK_SIZE) ? CHUNK_SIZE : (msg_len - offset);
            lifi_send_bytes(ciphertext + offset, chunk);
            sleep_us(250);  // 250us after each chunk
        }
        
        // Send tag
        lifi_send_bytes(tag, SST_TAG_SIZE);
        sleep_us(250);  // 250us after tag
        
        // Send CRC
        lifi_send_bytes(crc_bytes, 2);
        lifi_wait_tx();

        // Clear sensitive data from memory
        secure_zero(ciphertext, sizeof(ciphertext));
        secure_zero(tag, sizeof(tag));
        secure_zero(nonce, sizeof(nonce));
        secure_zero(message_buffer, sizeof(message_buffer));
    }

    return 0;
}
