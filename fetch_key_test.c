#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "c_api.h"  // SST C-API

// Hardcoded Key ID from user request: 00 00 00 00 00 9A 1D 59
// This corresponds to a 64-bit integer.
// 0x9A1D59 = 10100057 decimal? No.
// 0x9A = 154, 0x1D=29, 0x59=89.
int main() {
    printf("Starting Session Key Fetch Test...\n");

    // 1. Initialize SST Context
    const char *config_path = "side.config";
    printf("Initializing SST with config: %s\n", config_path);
    
    // We need to resolve path properly normally, but assuming cwd is correct
    SST_ctx_t *ctx = init_SST(config_path);
    if (!ctx) {
        printf("ERROR: init_SST failed. Check config path and certs.\n");
        return 1;
    }
    printf("SST Context initialized.\n");
    // Explicitly initialize purpose_index as it might be garbage from malloc
    ctx->config->purpose_index = 0;

    // 2. Prepare Empty Key List
    session_key_list_t *key_list = init_empty_session_key_list();

    // 3. Request Valid Key from Auth (Validation Step 1)
    printf("Requesting NEW session key from Auth...\n");
    session_key_list_t *new_keys = get_session_key(ctx, NULL);
    
    if (!new_keys || new_keys->num_key == 0) {
        printf("ERROR: Failed to get ANY new key from Auth.\n");
        if (new_keys) free_session_key_list_t(new_keys);
        free_session_key_list_t(key_list);
        free_SST_ctx_t(ctx);
        return 1;
    }

    session_key_t *valid_key = &new_keys->s_key[0];
    printf("SUCCESS! Received New Key.\n");
    printf("Key ID: ");
    for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X ", valid_key->key_id[i]);
    printf("\n");

    // Copy ID for the test
    unsigned char target_id[SESSION_KEY_ID_SIZE];
    memcpy(target_id, valid_key->key_id, SESSION_KEY_ID_SIZE);
    
    // Free the list to prevent "local cache" hits (we want to force fetch)
    // free_session_key_list_t(new_keys); // Actually, we might keep it to compare, but freeing ensures we fetch remotely? 
    // Wait, get_session_key_by_ID might check "existing_s_key_list" which is key_list (empty).
    // The "Auth" server has it. The "Local" client has it in new_keys.
    // We want to fetch it FROM AUTH again.
    // So we invoke get_session_key_by_ID with 'key_list' which is currently empty.
    
    free_session_key_list_t(new_keys); 

    // 4. Fetch Key by ID (Validation Step 2)
    printf("----------------------------------------\n");
    printf("Now testing fetch by ID (Simulating Receiver)...\n");
    printf("Requesting Key ID: ");
    for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X ", target_id[i]);
    printf("\n");

    printf("Calling get_session_key_by_ID()...\n");
    session_key_t *found_key = get_session_key_by_ID(target_id, ctx, key_list);

    if (found_key) {
        printf("\nSUCCESS! Key Fetched by ID.\n");
        printf("Key ID: ");
        for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X ", found_key->key_id[i]);
        printf("\nCipher Key: ");
        for(int i=0; i<found_key->cipher_key_size; i++) printf("%02X ", found_key->cipher_key[i]);
        printf("\n");
    } else {
        printf("\nFAILURE. Key not found or connection failed.\n");
        SST_print_error("Detailed Failure Information");
    }

    // Cleanup
    free_session_key_list_t(key_list);
    free_SST_ctx_t(ctx);
    return 0;
}
