/**
 * @file lifi_key_sender.c
 * @brief Demonstration tool: Simulates the Sender Device (Pico).
 * 
 * 1. Connects to Auth.
 * 2. Requests a Session Key.
 * 3. Writes the received Key ID to 's_key_id_received.dat'.
 * 
 * This allows 'lifi_key_receiver' to read that file and demonstrate retrieval.
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

    // 2. Request Session Key (Sender logic)
    // Passing NULL as 2nd arg means "I don't have a list, give me a new key for the entity in config"
    printf("Requesting new Session Key from Auth...\n");
    session_key_list_t *s_key_list = get_session_key(ctx, NULL);
    if (!s_key_list || s_key_list->num_key == 0) {
        SST_print_error_exit("Failed to receive session key.");
    }

    session_key_t *s_key = &s_key_list->s_key[0];
    unsigned int key_id_int = convert_skid_buf_to_int(s_key->key_id, SESSION_KEY_ID_SIZE);

    printf("\nSUCCESS: Received Session Key!\n");
    printf("Key ID: %u\n", key_id_int);

    // 3. Simulate LiFi Transmission: Write Key ID to file
    const char *outfile = "s_key_id_received.dat";
    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        SST_print_error_exit("Failed to open %s for writing.", outfile);
    }
    // Write raw bytes as the receiver expects
    fwrite(s_key->key_id, 1, SESSION_KEY_ID_SIZE, fp);
    fclose(fp);

    printf("wrote Key ID to '%s'.\n", outfile);
    printf("Run 'lifi_key_receiver' now to demonstrate key retrieval.\n");

    // 4. Cleanup
    free_session_key_list_t(s_key_list);
    free_SST_ctx_t(ctx);

    return 0;
}
