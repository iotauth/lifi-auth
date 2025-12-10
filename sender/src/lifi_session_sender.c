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
#define MSG_TYPE_KEY_ID 0x03

#define SESSION_KEY_ID_SIZE 8

// --- Custom Flash Storage Logic for Key ID + Key ---
// We redefine necessary constants/structs here to avoid modifying pico_handler.c

#define FLASH_KEY_MAGIC 0x53455353  // 'SESS'
#define SLOT_A_SECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define SLOT_B_SECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define INDEX_SECTOR_OFFSET (PICO_FLASH_SIZE_BYTES - 1 * FLASH_SECTOR_SIZE)
#define FLASH_SLOT_A_OFFSET (SLOT_A_SECTOR_OFFSET)
#define FLASH_SLOT_B_OFFSET (SLOT_B_SECTOR_OFFSET)
#define FLASH_SLOT_INDEX_OFFSET (INDEX_SECTOR_OFFSET)
#define SLOT_INDEX_MAGIC 0xA5

typedef struct {
    uint8_t key_id[SESSION_KEY_ID_SIZE];
    uint8_t key[SST_KEY_SIZE];
    uint8_t hash[32];  // SHA-256 hash of (key_id + key)
    uint32_t magic;
} key_id_flash_block_t;

_Static_assert(sizeof(key_id_flash_block_t) <= 256, "key_id_flash_block_t must fit in 256B");

void compute_key_id_hash(const uint8_t *id, const uint8_t *key, uint8_t *out_hash) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
    mbedtls_sha256_update(&ctx, id, SESSION_KEY_ID_SIZE);
    mbedtls_sha256_update(&ctx, key, SST_KEY_SIZE);
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);
}

bool validate_flash_block_with_id(const key_id_flash_block_t *block) {
    if (block->magic != FLASH_KEY_MAGIC) return false;
    uint8_t expected_hash[32];
    compute_key_id_hash(block->key_id, block->key, expected_hash);
    return memcmp(block->hash, expected_hash, 32) == 0;
}

bool read_key_with_id_from_slot(uint32_t offset, uint8_t *out_key, uint8_t *out_id) {
    const key_id_flash_block_t *slot = (const key_id_flash_block_t *)(XIP_BASE + offset);
    if (validate_flash_block_with_id(slot)) {
        memcpy(out_key, slot->key, SST_KEY_SIZE);
        memcpy(out_id, slot->key_id, SESSION_KEY_ID_SIZE);
        return true;
    }
    return false;
}

bool write_key_with_id_to_slot(uint32_t offset, const uint8_t *key, const uint8_t *id) {
    key_id_flash_block_t block = {0};
    memcpy(block.key, key, SST_KEY_SIZE);
    memcpy(block.key_id, id, SESSION_KEY_ID_SIZE);
    compute_key_id_hash(block.key_id, block.key, block.hash);
    block.magic = FLASH_KEY_MAGIC;

    uint8_t page[FLASH_PAGE_SIZE] = {0};
    memcpy(page, &block, sizeof(block));

    const uint32_t sector = (offset == FLASH_SLOT_A_OFFSET) ? SLOT_A_SECTOR_OFFSET : SLOT_B_SECTOR_OFFSET;

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(sector, FLASH_SECTOR_SIZE);
    flash_range_program(offset, page, FLASH_PAGE_SIZE);
    restore_interrupts(ints);

    return true;
}

bool load_session_key_with_id(uint8_t *out_key, uint8_t *out_id) {
    if (read_key_with_id_from_slot(FLASH_SLOT_B_OFFSET, out_key, out_id)) return true;
    if (read_key_with_id_from_slot(FLASH_SLOT_A_OFFSET, out_key, out_id)) return true;
    return false;
}

bool store_session_key_with_id(const uint8_t *key, const uint8_t *id) {
    uint8_t temp_k[SST_KEY_SIZE];
    uint8_t temp_i[SESSION_KEY_ID_SIZE];
    bool a_valid = read_key_with_id_from_slot(FLASH_SLOT_A_OFFSET, temp_k, temp_i);
    return a_valid ? write_key_with_id_to_slot(FLASH_SLOT_B_OFFSET, key, id)
                   : write_key_with_id_to_slot(FLASH_SLOT_A_OFFSET, key, id);
}

// --- Modified Receive Function ---
bool receive_new_key_with_id_with_timeout(uint8_t *key_out, uint8_t *id_out, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (uart_is_readable(UART_ID) && uart_getc(UART_ID) == PREAMBLE_BYTE_1) {
            while (!uart_is_readable(UART_ID)) {} 
            if (uart_getc(UART_ID) == PREAMBLE_BYTE_2) {
                printf("Receiving new session key (ID + Key)...\n");
                
                // Read ID
                size_t received_id = 0;
                while (received_id < SESSION_KEY_ID_SIZE && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
                    if (uart_is_readable(UART_ID)) {
                        id_out[received_id++] = uart_getc(UART_ID);
                    }
                }
                
                // Read Key
                size_t received_key = 0;
                while (received_key < SST_KEY_SIZE && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
                    if (uart_is_readable(UART_ID)) {
                        key_out[received_key++] = uart_getc(UART_ID);
                    }
                }
                
                return (received_id == SESSION_KEY_ID_SIZE && received_key == SST_KEY_SIZE);
            }
        }
    }
    return false;
}

int main() {
    stdio_init_all();
    pico_prng_init();
    sleep_ms(3000); 
    pico_nonce_init();

    int current_slot = 0; 
    int saved_slot = load_last_used_slot(); // Still use shared logic for slot index if compatible
    if (saved_slot == 0 || saved_slot == 1) {
        current_slot = saved_slot;
    }

#define RED_PIN 0
#define GREEN_PIN 1
#define BLUE_PIN 2

    printf("PICO STARTED (Session ID Sender Mode)\n");
    gpio_init(RED_PIN); gpio_set_dir(RED_PIN, GPIO_OUT); gpio_put(RED_PIN, 0);
    gpio_init(GREEN_PIN); gpio_set_dir(GREEN_PIN, GPIO_OUT); gpio_put(GREEN_PIN, 0);
    gpio_init(BLUE_PIN); gpio_set_dir(BLUE_PIN, GPIO_OUT); gpio_put(BLUE_PIN, 0);
    gpio_init(25); gpio_set_dir(25, GPIO_OUT);

    uart_init(UART_ID_DEBUG, BAUD_RATE);
    gpio_set_function(UART_TX_PIN_DEBUG, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN_DEBUG, GPIO_FUNC_UART);

    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    while (uart_is_readable(UART_ID)) { volatile uint8_t _ = uart_getc(UART_ID); }

    uint8_t session_key[SST_KEY_SIZE] = {0};
    uint8_t session_key_id[SESSION_KEY_ID_SIZE] = {0};

    // Load Key + ID
    if (!load_session_key_with_id(session_key, session_key_id)) {
        printf("No valid session key found. Waiting for one (ID+Key)...\n");

        if (receive_new_key_with_id_with_timeout(session_key, session_key_id, 20000)) {
            printf("Received Key ID: ");
            for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", session_key_id[i]);
            printf("\n");
            print_hex("Received Session Key: ", session_key, SST_KEY_SIZE);

            if (store_session_key_with_id(session_key, session_key_id)) {
                 // Determine slot written (simple check)
                 uint8_t tmp_k[SST_KEY_SIZE];
                 uint8_t tmp_i[SESSION_KEY_ID_SIZE];
                 int written_slot = -1;
                 
                 for(int slot=0; slot<=1; slot++) {
                     uint32_t off = (slot==0)?FLASH_SLOT_A_OFFSET:FLASH_SLOT_B_OFFSET;
                     if(read_key_with_id_from_slot(off, tmp_k, tmp_i) && 
                        memcmp(tmp_k, session_key, SST_KEY_SIZE)==0) {
                            written_slot = slot;
                            break;
                        }
                 }
                 
                 if (written_slot >= 0) {
                     current_slot = written_slot;
                     store_last_used_slot((uint8_t)current_slot); // Use shared helper
                     pico_nonce_on_key_change();
                     printf("Key+ID saved to flash slot %c.\n", current_slot == 0 ? 'A' : 'B');
                 }
            } else {
                printf("Failed to save key to flash.\n");
                return 1;
            }
        } else {
            printf("Timeout waiting for key.\n");
            return 1;
        }
    } else {
        printf("Loaded Key ID: ");
        for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X", session_key_id[i]);
        printf("\n");
        print_hex("Loaded Session Key: ", session_key, SST_KEY_SIZE);
    }

    char message_buffer[256];

    while (true) {
        printf("Enter a message to send over LiFi (or 'CMD: send key id'):\n");
        size_t msg_len = 0;
        int ch;
        uint8_t ciphertext[256] = {0};
        uint8_t tag[SST_TAG_SIZE] = {0};

        for (;;) {
            ch = getchar_timeout_us(1000);
            if (ch == PICO_ERROR_TIMEOUT) continue;
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
                message_buffer[msg_len++] = ch;
                putchar(ch);
            }
        }

        if (strncmp(message_buffer, "CMD:", 4) == 0) {
            const char *cmd = message_buffer + 4;
            if (strncmp(cmd, " send key id", 12) == 0) {
                 if (is_key_zeroed(session_key)) {
                    printf("No valid key.\n");
                    continue;
                }
                
                printf("Sending Key ID over LiFi...\n");
                uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
                uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
                uart_putc_raw(UART_ID, MSG_TYPE_KEY_ID);
                uart_write_blocking(UART_ID, session_key_id, SESSION_KEY_ID_SIZE);
                
                gpio_put(25, 1); sleep_ms(100); gpio_put(25, 0);
                printf("Key ID Sent.\n");
                continue;
            }
            
            // Should also handle "CMD: new key" to call receive_new_key_with_id_with_timeout...
            if (strncmp(cmd, " new key", 8) == 0) {
                printf("Waiting for NEW Key (ID+Key) on UART...\n");
                if (receive_new_key_with_id_with_timeout(session_key, session_key_id, 10000)) {
                    printf("New Key Received. Saving...\n");
                     if (store_session_key_with_id(session_key, session_key_id)) {
                         pico_nonce_on_key_change();
                         printf("Saved.\n");
                     }
                } else {
                    printf("Timeout.\n");
                }
                continue;
            }

            memset(message_buffer, 0, sizeof(message_buffer));
            continue;
        }

        if (msg_len > sizeof(ciphertext)) {
            printf("Message too long!\n");
            continue;
        }
        if (is_key_zeroed(session_key)) {
            printf("No valid key.\n");
            continue;
        }

        uint8_t nonce[SST_NONCE_SIZE];
        pico_nonce_generate(nonce);

        int ret = sst_encrypt_gcm(session_key, nonce, (const uint8_t *)message_buffer, msg_len, ciphertext, tag);
        if (ret != 0) {
            printf("Encryption failed! ret=%d\n", ret);
            continue;
        }

        uart_putc_raw(UART_ID, PREAMBLE_BYTE_1);
        uart_putc_raw(UART_ID, PREAMBLE_BYTE_2);
        uart_putc_raw(UART_ID, MSG_TYPE_ENCRYPTED);
        uart_write_blocking(UART_ID, nonce, SST_NONCE_SIZE);
        uint8_t len_bytes[2] = {(msg_len >> 8) & 0xFF, msg_len & 0xFF};
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
