#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Project headers
#include "c_api.h"           // SST_ctx_t, session_key_list_t, session_key_t, init_SST, get_session_key, etc.
#include "config_handler.h"  // change_directory_to_config_path, get_config_path
#include "protocol.h"        // if you need sizes/constants
#include "sst_crypto_embedded.h"
#include "utils.h"           // print_hex, etc.

// If your headers don't define this, we define it here.
// Adjust to match your actual key_id length if needed.
#ifndef SESSION_KEY_ID_SIZE
#define SESSION_KEY_ID_SIZE 8
#endif

// ---- Printing helpers ----

static void print_session_key_details(const session_key_t *key) {
    if (!key) {
        printf("Session key is NULL.\n");
        return;
    }

    printf("\n================= SESSION KEY =================\n");

    // Assume fixed 8-byte key_id (or whatever your system uses)
    printf("Key ID: ");
    for (int i = 0; i < SESSION_KEY_ID_SIZE; i++) {
        printf("%02X", key->key_id[i]);
    }
    printf("\n");

    // Cipher key (size comes from the struct)
    print_hex("Cipher Key: ", key->cipher_key, key->cipher_key_size);

    // MAC key
    print_hex("MAC Key:    ", key->mac_key,    key->mac_key_size);

    printf("================================================\n\n");
}

// Read a line from stdin, trim newline.
static void read_line(char *buf, size_t size) {
    if (!fgets(buf, (int)size, stdin)) {
        buf[0] = '\0';
        return;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
}

// Uppercase in-place for easy command matching.
static void str_to_upper(char *s) {
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

int main(int argc, char *argv[]) {
    const char *config_path = NULL;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [<path/to/lifi_sender.config>]\n", argv[0]);
        return 1;
    } else if (argc == 2) {
        config_path = argv[1];
    }
#ifdef DEFAULT_SST_CONFIG_PATH
    // If no arg provided, fall back to compiled-in default
    if (!config_path) {
        config_path = DEFAULT_SST_CONFIG_PATH;
    }
#endif

    // Resolve path & chdir to config directory
    change_directory_to_config_path(config_path);
    config_path = get_config_path(config_path);

    printf("Using config file: %s\n", config_path);
    printf("Initializing SST context and connecting to Auth...\n");

    // Init SST/Auth context
    SST_ctx_t *sst = init_SST(config_path);
    if (!sst) {
        fprintf(stderr, "init_SST() failed. Check config/certs/auth server.\n");
        return 1;
    }

    // Initial key fetch
    session_key_list_t *key_list = get_session_key(sst, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "get_session_key() returned no keys.\n");
        if (key_list) {
            free_session_key_list_t(key_list);
        }
        free_SST_ctx_t(sst);
        return 1;
    }

    session_key_t *current_key = &key_list->s_key[0];

    printf("\n[Auth] Connected and received %d session key(s).\n",
           key_list->num_key);
    print_session_key_details(current_key);

    printf("Staying connected to Auth. Commands:\n");
    printf("  R  - Refresh keys (call get_session_key again)\n");
    printf("  K  - Print current key details\n");
    printf("  Q  - Quit\n\n");

    char line[64];

    for (;;) {
        printf("auth> ");
        fflush(stdout);

        read_line(line, sizeof(line));

        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;  // trim leading spaces
        str_to_upper(cmd);

        if (*cmd == '\0') {
            continue;  // empty line
        }

        if (cmd[0] == 'Q') {
            printf("Exiting.\n");
            break;
        }

        if (cmd[0] == 'K') {
            if (current_key) {
                print_session_key_details(current_key);
            } else {
                printf("No current key.\n");
            }
            continue;
        }

        if (cmd[0] == 'R') {
            printf("[Auth] Refreshing session keys from server...\n");

            free_session_key_list_t(key_list);
            key_list = get_session_key(sst, NULL);
            if (!key_list || key_list->num_key == 0) {
                fprintf(stderr, "get_session_key() after refresh returned no keys.\n");
                if (key_list) {
                    free_session_key_list_t(key_list);
                    key_list = NULL;
                }
                current_key = NULL;
            } else {
                current_key = &key_list->s_key[0];
                printf("[Auth] Got %d key(s) after refresh.\n", key_list->num_key);
                print_session_key_details(current_key);
            }
            continue;
        }

        printf("Unknown command. Use R (refresh), K (key), Q (quit).\n");
    }

    if (key_list) {
        free_session_key_list_t(key_list);
    }
    free_SST_ctx_t(sst);
    return 0;
}
