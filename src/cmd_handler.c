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

    } else if (strcmp(cmd, " help") == 0) {
        printf("Available Commands:\n");
        printf("  CMD: print slot key      (print key in current slot)\n");
        printf("  CMD: print slot key *    (print keys in all slots)\n");
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
