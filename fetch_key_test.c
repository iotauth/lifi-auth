#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "c_api.h"  // SST C-API

// Hardcoded Key ID from user request: 00 00 00 00 00 9A 1D 59
// This corresponds to a 64-bit integer.
// 0x9A1D59 = 10100057 decimal.
int main() {
    printf("Starting Session Key Fetch Test (Mode: ID Only)...\n");

    // 1. Initialize SST Context
    const char *config_path = "side.config";
    printf("Initializing SST with config: %s\n", config_path);
    
    SST_ctx_t *ctx = init_SST(config_path);
    if (!ctx) {
        printf("ERROR: init_SST failed. Check config path and certs.\n");
        return 1;
    }
    printf("SST Context initialized.\n");
    
    // Explicitly initialize purpose_index as it might be garbage from malloc
    // This matches entity_downloader.c line 18
    ctx->config->purpose_index = 0;
    
    printf("Config Loaded:\n");
    printf("  Entity Name: %s\n", ctx->config->name);
    // purpose[0] might be overwritten by get_session_key_by_ID, but let's see initial state
    if (ctx->config->purpose[0]) {
        printf("  Initial Purpose[0]: %s\n", ctx->config->purpose[0]);
    } else {
        printf("  Initial Purpose[0]: (NULL)\n");
    }

    // 2. Prepare Empty Key List
    session_key_list_t *key_list = init_empty_session_key_list();

    // 3. Prepare Target Key ID
    // 00 00 00 00 00 9A 1D 59
    unsigned char target_id[SESSION_KEY_ID_SIZE] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x9A, 0x1D, 0x59
    };
    
    // 4. Fetch Key by ID (Validation Step 2)
    printf("----------------------------------------\n");
    printf("Testing fetch by ID (Simulating Receiver)...\n");
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
        // Use dynamic size from struct
        for(int i=0; i<found_key->cipher_key_size; i++) printf("%02X ", found_key->cipher_key[i]);
        printf("\n");
    } else {
        printf("\nFAILURE. Key not found or connection failed.\n");
        // Error details are printed by SST_print_error inside the library
    }

    // Cleanup
    free_session_key_list_t(key_list);
    free_SST_ctx_t(ctx);
    return 0;
}
