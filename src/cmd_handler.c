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
            
            printf("Switched to Key ID: ");
            for(int i=0; i<4; i++) printf("%02X", id[i]);
            printf("...\n");
            print_hex("RAM key: ", k, SST_KEY_SIZE);
            
            secure_zero(k, sizeof(k));
            printf("Current slot: %c\n", target_slot == 0 ? 'A' : 'B');
            return true;
        } else {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            printf("Switched to Slot %c (invalid/empty). Ready for new key.\n", target_slot == 0 ? 'A' : 'B');
            return true;
        }

    } else if (strcmp(cmd, " use slot A") == 0) {
        *current_slot = 0;
        uint8_t k[SST_KEY_SIZE];
        uint8_t id[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(0, id, k)) {
            keyram_set_with_id(id, k);
            memcpy(session_key, k, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            print_hex("Using key from Slot A. RAM key: ", k, SST_KEY_SIZE);
            secure_zero(k, sizeof(k));
            return true;
        } else {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            printf("Slot A invalid or empty. Ready to receive new key.\n");
            return true;  // effective key changed (now none)
        }

    } else if (strcmp(cmd, " use slot B") == 0) {
        *current_slot = 1;
        uint8_t k[SST_KEY_SIZE];
        uint8_t id[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(1, id, k)) {
            keyram_set_with_id(id, k);
            memcpy(session_key, k, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            print_hex("Using key from Slot B. RAM key: ", k, SST_KEY_SIZE);
            secure_zero(k, sizeof(k));
            return true;
        } else {
            keyram_clear();
            memset(session_key, 0, SST_KEY_SIZE);
            store_last_used_slot((uint8_t)*current_slot);
            printf("Slot B invalid or empty. Ready to receive new key.\n");
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
        if (!store_session_key(newid, newk)) {
            printf("Flash write failed.\n");
            secure_zero(newk, sizeof(newk));
            return false;
        }

        // Figure out which slot now holds the new key
        int written_slot = -1;
        uint8_t tmp_k[SST_KEY_SIZE];
        uint8_t tmp_i[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(0, tmp_i, tmp_k) &&
            memcmp(tmp_k, newk, SST_KEY_SIZE) == 0)
            written_slot = 0;
        else if (pico_read_key_pair_from_slot(1, tmp_i, tmp_k) &&
                 memcmp(tmp_k, newk, SST_KEY_SIZE) == 0)
            written_slot = 1;
        secure_zero(tmp_k, sizeof(tmp_k));

        if (written_slot >= 0) {
            *current_slot = written_slot;
            store_last_used_slot((uint8_t)*current_slot);
        }

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
        if (!store_session_key(newid, newk)) {
            printf("Flash write failed.\n");
            secure_zero(newk, sizeof(newk));
            return false;
        }

        // Determine which slot took it
        int written_slot = -1;
        uint8_t tmp_k[SST_KEY_SIZE];
        uint8_t tmp_i[SST_KEY_ID_SIZE];
        
        if (pico_read_key_pair_from_slot(0, tmp_i, tmp_k) &&
            memcmp(tmp_k, newk, SST_KEY_SIZE) == 0)
            written_slot = 0;
        else if (pico_read_key_pair_from_slot(1, tmp_i, tmp_k) &&
                 memcmp(tmp_k, newk, SST_KEY_SIZE) == 0)
            written_slot = 1;
        secure_zero(tmp_k, sizeof(tmp_k));

        if (written_slot >= 0) {
            *current_slot = written_slot;
            store_last_used_slot((uint8_t)*current_slot);
        }

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
