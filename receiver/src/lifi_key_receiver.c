/**
 * @file lifi_key_receiver.c
 * @brief Key receiver program for Pi 4 to get session key by ID from Auth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c_api.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <config_path>\n", argv[0]);
        return 1;
    }

    char *config_path = argv[1];

    // 1. Initialize SST Context
    SST_ctx_t *ctx = init_SST(config_path);
    if (!ctx) {
        SST_print_error_exit("Failed to initialize SST context.");
    }

    // 2. Read Session Key ID (Simulate receiving over LiFi)
    // For now, we read from a known file 's_key_id_received.dat'
    unsigned char target_key_id[SESSION_KEY_ID_SIZE];
    const char *key_id_file = "s_key_id_received.dat";
    FILE *fp = fopen(key_id_file, "rb");
    if (!fp) {
        // Fallback or exit? For this task, we expect the file.
        SST_print_error_exit("Could not open Key ID file: %s", key_id_file);
    }
    if (fread(target_key_id, 1, SESSION_KEY_ID_SIZE, fp) != SESSION_KEY_ID_SIZE) {
         SST_print_error_exit("Failed to read complete Key ID from file.");
    }
    fclose(fp);

    printf("Read Target Key ID: %u\n", convert_skid_buf_to_int(target_key_id, SESSION_KEY_ID_SIZE));

    // 3. Prepare Session Key List
    session_key_list_t *s_key_list = init_empty_session_key_list();
    if (!s_key_list) {
        SST_print_error_exit("Failed to initialize session key list.");
    }

    // 4. Request Session Key by ID
    printf("Requesting Session Key from Auth...\n");
    session_key_t *received_key = get_session_key_by_ID(target_key_id, ctx, s_key_list);

    if (received_key) {
        printf("\nSUCCESS: Received Session Key!\n");
        printf("Key ID: %u\n", convert_skid_buf_to_int(received_key->key_id, SESSION_KEY_ID_SIZE));
        
        // Optional: Print key bytes for verification (debugging only)
        // printf("Cipher Key: ");
        // for(int i=0; i<received_key->cipher_key_size; i++) printf("%02x", received_key->cipher_key[i]);
        // printf("\n");
    } else {
        SST_print_error("Failed to retrieve session key.");
    }

    // 5. Cleanup
    free_session_key_list_t(s_key_list);
    free_SST_ctx_t(ctx);

    return 0;
}
