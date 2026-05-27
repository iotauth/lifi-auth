#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"
#include "pico_handler.h"
#include "sst_crypto_embedded.h"  // print_hex, secure_zero, etc.

// Return true iff the effective session key changed (loaded, replaced, or
// cleared)
bool handle_commands(const char *cmd, uint8_t *session_key, int *current_slot) {
    if (strcmp(cmd, " print slot key") == 0) {
        print_hex("slot's session key: ", session_key, SST_KEY_SIZE);
        // Note: we can't easily print ID here without passing it in or reading from RAM global if we exposed it
        return false;
    } else if (strcmp(cmd, " slot status") == 0) {
        pico_print_slot_status(*current_slot);
        return false;
    } else if (strcmp(cmd, " clear slot A") == 0) {
        pico_clear_slot_verify(0);
        printf("Slot A cleared.\n");
        if (*current_slot == 0) {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            return true;  // effective key now none
        }
        return false;
    } else if (strcmp(cmd, " clear slot B") == 0) {
        pico_clear_slot_verify(1);
        printf("Slot B cleared.\n");
        if (*current_slot == 1) {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            return true;
        }
        return false;
    } else if (strcmp(cmd, " clear slot *") == 0) {
        pico_clear_slot_verify(0);
        pico_clear_slot_verify(1);
        printf("Both slots cleared.\n");
        keyram_clear();
        memset(session_key, 0, SST_KEY_SIZE);
        return true;

    } else if (strcmp(cmd, " switch slot") == 0) {
        int target_slot = (*current_slot == 0) ? 1 : 0;
        *current_slot = target_slot;
        uint8_t k[SST_KEY_SIZE];
        uint8_t id[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(target_slot, id, k)) {
            keyram_set_with_id(id, k);
            memcpy(session_key, k, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", id[i]);
            printf("\n");
            print_hex("RAM key: ", k, SST_KEY_SIZE);
            
            secure_zero(k, sizeof(k));
            printf("Current slot: %c\n", target_slot == 0 ? 'A' : 'B');
            return true;
        } else {
            // Empty slot logic: treat as valid "Zero Key" slot
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("00");
            printf("\n");
            uint8_t zeros[SST_KEY_SIZE] = {0};
            print_hex("RAM key: ", zeros, SST_KEY_SIZE);

            printf("Switched to Slot %c (Empty/Zeroed). Ready for new key.\n", target_slot == 0 ? 'A' : 'B');
            return true;
        }

    } else if (strcmp(cmd, " use slot A") == 0) {
        if (*current_slot == 0) return false; // Already on A
        *current_slot = 0;
        uint8_t k[SST_KEY_SIZE];
        uint8_t id[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(0, id, k)) {
            keyram_set_with_id(id, k);
            memcpy(session_key, k, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", id[i]);
            printf("\n");
            print_hex("RAM key: ", k, SST_KEY_SIZE);
            secure_zero(k, sizeof(k));
            return true;
        } else {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("00");
            printf("\n");
            uint8_t zeros[SST_KEY_SIZE] = {0};
            print_hex("RAM key: ", zeros, SST_KEY_SIZE);
            
            printf("Switched to Slot A (Empty/Zeroed). Ready to receive new key.\n");
            return true;
        }

    } else if (strcmp(cmd, " use slot B") == 0) {
        if (*current_slot == 1) return false; // Already on B
        *current_slot = 1;
        uint8_t k[SST_KEY_SIZE];
        uint8_t id[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(1, id, k)) {
            keyram_set_with_id(id, k);
            memcpy(session_key, k, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", id[i]);
            printf("\n");
            print_hex("RAM key: ", k, SST_KEY_SIZE);
            secure_zero(k, sizeof(k));
            return true;
        } else {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            
            printf("Key ID: ");
            for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("00");
            printf("\n");
            uint8_t zeros[SST_KEY_SIZE] = {0};
            print_hex("RAM key: ", zeros, SST_KEY_SIZE);
            
            printf("Switched to Slot B (Empty/Zeroed). Ready to receive new key.\n");
            return true;
        }

    } else if (strcmp(cmd, " new key -f") == 0) {
        printf("Waiting 3 seconds for new key (forced)...\n");
        uint8_t newk[SST_KEY_SIZE] = {0};
        uint8_t newid[SST_KEY_ID_SIZE] = {0};
        
        if (!receive_new_key_with_timeout(newid, newk, 3000)) {
            printf("No key received.\n");
            return false;
        }
        // Write explicitly to current slot as requested by user
        if (!pico_write_key_to_slot(*current_slot, newid, newk)) {
            printf("Flash write failed.\n");
            secure_zero(newk, sizeof(newk));
            return false;
        }

        store_last_used_slot((uint8_t)*current_slot);

        keyram_set_with_id(newid, newk);
        memcpy(session_key, newk, SST_KEY_SIZE);
        printf("New key stored (forced) and loaded to RAM (slot %c).\n",
               *current_slot ? 'B' : 'A');
        
        printf("Received Key ID: ");
        for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", newid[i]);
        printf("\n");
        print_hex("Received new key: ", newk, SST_KEY_SIZE);
        
        secure_zero(newk, sizeof(newk));
        return true;

    } else if (strcmp(cmd, " new key") == 0) {
        // Only accept if current slot is empty
        uint8_t tmp[SST_KEY_SIZE];
        if (pico_read_key_from_slot(*current_slot, tmp)) {
            printf("Slot %c occupied. Use 'new key -f' to overwrite.\n",
                   *current_slot ? 'B' : 'A');
            secure_zero(tmp, sizeof(tmp));
            return false;
        }
        secure_zero(tmp, sizeof(tmp));

        printf("Waiting 3 seconds for new key...\n");
        uint8_t newk[SST_KEY_SIZE] = {0};
        uint8_t newid[SST_KEY_ID_SIZE] = {0};
        
        if (!receive_new_key_with_timeout(newid, newk, 3000)) {
            printf("No key received.\n");
            return false;
        }
        // Write explicitly to current slot
        if (!pico_write_key_to_slot(*current_slot, newid, newk)) {
            printf("Flash write failed.\n");
            secure_zero(newk, sizeof(newk));
            return false;
        }

        store_last_used_slot((uint8_t)*current_slot);

        keyram_set_with_id(newid, newk);
        memcpy(session_key, newk, SST_KEY_SIZE);
        printf("New key stored and loaded to RAM (slot %c).\n",
               *current_slot ? 'B' : 'A');
               
        printf("Received Key ID: ");
        for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", newid[i]);
        printf("\n");       
        print_hex("Received new key: ", newk, SST_KEY_SIZE);
        secure_zero(newk, sizeof(newk));
        return true;

    } else if (strcmp(cmd, " print slot key *") == 0) {
        pico_print_key_from_slot(0);
        pico_print_key_from_slot(1);
        return false;

    } else if (strcmp(cmd, " reboot") == 0) {
        printf("Rebooting...\n");
        sleep_ms(500);
        pico_reboot();
        return false;

    } else if (strncmp(cmd, " key ", 5) == 0) {
        // Command format: "CMD: key <hex_string>"
        // Expects strictly SST_KEY_SIZE bytes (e.g. 32 hex chars for 16-byte key)
        const char *hex_str = cmd + 5;
        // Skip leading spaces
        while (*hex_str == ' ') hex_str++;

        size_t expected_hex_len = SST_KEY_SIZE * 2;
        if (strlen(hex_str) < expected_hex_len) {
            printf("[Error] Key too short. Expected %d hex chars.\n", expected_hex_len);
            return false;
        }

        uint8_t new_key[SST_KEY_SIZE];
        // Parse hex string
        for (size_t i = 0; i < SST_KEY_SIZE; i++) {
            char byte_str[3] = { hex_str[i*2], hex_str[i*2+1], '\0' };
            char *endptr;
            new_key[i] = (uint8_t)strtoul(byte_str, &endptr, 16);
            if (*endptr != '\0') {
                 printf("[Error] Invalid hex character at pos %d\n", i*2);
                 return false;
            }
        }

        // Apply new key
        // We set ID to all zeros (or a specific "MANUAL" pattern) to distinguish
        uint8_t new_id[SST_KEY_ID_SIZE] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x01}; 
        
        // Write to current slot
        if (!pico_write_key_to_slot(*current_slot, new_id, new_key)) {
            printf("[Error] Flash write failed.\n");
            return false;
        }
        
        store_last_used_slot((uint8_t)*current_slot);
        keyram_set_with_id(new_id, new_key);
        memcpy(session_key, new_key, SST_KEY_SIZE);
        
        printf("Manual Key Set (Slot %c).\n", *current_slot ? 'B' : 'A');
        printf("Key ID: ");
        for(int i=0; i<SST_KEY_ID_SIZE; i++) printf("%02X", new_id[i]);
        printf("\n");
        print_hex("New Key: ", new_key, SST_KEY_SIZE);
        
        return true;

    } else if (strncmp(cmd, " leds ", 6) == 0) {
        // Command format: "CMD: leds <mask_hex>"
        // <mask_hex> is 1 hex char (0-F) representing 4 bits: R B G W
        const char *hex_str = cmd + 6;
        while (*hex_str == ' ') hex_str++;
        
        char *endptr;
        unsigned long mask = strtoul(hex_str, &endptr, 16);
        
        // Pass to main loop via a global or extern function
        // defined in lifi_session_sender.c
        extern void set_led_mask(uint8_t mask);
        set_led_mask((uint8_t)mask);
        
        printf("LED Mask Set: %02X\n", (uint8_t)mask);
        return false;

    } else if (strcmp(cmd, " help") == 0) {
        printf("Available Commands:\n");
        printf("  CMD: print slot key      (print key in current slot)\n");
        printf("  CMD: print slot key *    (print keys in all slots)\n");
        printf("  CMD: key <hex>           (manually set session key)\n");
        printf("  CMD: leds <hex>          (set active LED mask: 1=W, 2=G, 4=B, 8=R)\n");
        printf("  CMD: clear slot A\n");
        printf("  CMD: clear slot B\n");
        printf("  CMD: clear slot *        (clear all slot keys)\n");
        printf("  CMD: use slot A\n");
        printf("  CMD: use slot B\n");
        printf(
            "  CMD: new key           (request new key only if current slot is "
            "empty)\n");
        printf("  CMD: new key -f        (force overwrite current slot)\n");
        printf(
            "  CMD: slot status       (show slot validity and active slot)\n");
        printf("  CMD: reboot\n");
        printf("  CMD: help\n");
        return false;
    } else {
        printf("Unknown command. Type CMD: help\n");
        return false;
    }
}
