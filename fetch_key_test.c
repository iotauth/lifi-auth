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

    // 2. Prepare Empty Key List
    session_key_list_t *key_list = init_empty_session_key_list();

    // 3. Prepare Target Key ID
    // 00 00 00 00 00 9A 1D 59
    unsigned char target_id[SESSION_KEY_ID_SIZE] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x9A, 0x1D, 0x59
    };

    printf("Requesting Key ID: ");
    for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X ", target_id[i]);
    printf("\n");

    // 4. Fetch Key
    printf("Calling get_session_key_by_ID()...\n");
    session_key_t *found_key = get_session_key_by_ID(target_id, ctx, key_list);

    if (found_key) {
        printf("\nSUCCESS! Key Fetched.\n");
        printf("Key ID: ");
        for(int i=0; i<SESSION_KEY_ID_SIZE; i++) printf("%02X ", found_key->key_id[i]);
        printf("\nCipher Key: ");
        for(int i=0; i<SESSION_KEY_SIZE; i++) printf("%02X ", found_key->cipher_key[i]);
        printf("\n");
    } else {
        printf("\nFAILURE. Key not found or connection failed.\n");
    }

    // Cleanup
    free_session_key_list_t(key_list);
    free_SST_ctx_t(ctx);
    return 0;
}
