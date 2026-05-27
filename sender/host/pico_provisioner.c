/**
 * pico_provisioner.c
 *
 * Laptop-side SST key provisioner for the Pico sender.
 *
 * Authenticates with the SST Auth server as a registered entity,
 * retrieves a session key, and pushes it down to the Pico over UART
 * using the MSG_TYPE_KEY framing the Pico firmware already understands.
 *
 * After this runs, the Pico is a fully provisioned SST node and can:
 *   - Encrypt LiFi frames with the Auth-issued session key
 *   - Participate in the SST HS1/HS2/HS3 mutual auth over LiFi
 *
 * Usage:
 *   ./pico_provisioner <config_path> <serial_device>
 *
 * Example:
 *   ./pico_provisioner host.config /dev/ttyACM0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "c_api.h"
#include "../../include/protocol.h"

/* Must match Pico firmware: sst_crypto_embedded.h */
#define PROV_KEY_SIZE    16   /* SST_KEY_SIZE  – AES-128 cipher key */
#define PROV_KEY_ID_SIZE  8   /* SST_KEY_ID_SIZE */
#define PROV_MAC_KEY_SIZE 32  /* SST_MAC_KEY_SIZE */

/* Payload = KEY_ID + CIPHER_KEY + MAC_KEY */
#define PROV_PAYLOAD_LEN (PROV_KEY_ID_SIZE + PROV_KEY_SIZE + PROV_MAC_KEY_SIZE)

static int open_serial(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", dev, strerror(errno));
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        fprintf(stderr, "tcgetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, B1000000);
    cfsetispeed(&tty, B1000000);

    /* 8N1, no flow control, raw mode */
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void print_hex(const char *label, const uint8_t *b, size_t n) {
    printf("%s", label);
    for (size_t i = 0; i < n; i++) printf("%02X ", b[i]);
    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <config_path> <serial_device>\n", argv[0]);
        fprintf(stderr, "  e.g: %s host.config /dev/ttyACM0\n", argv[0]);
        return 1;
    }

    const char *config_path = argv[1];
    const char *serial_dev  = argv[2];

    /* ── 1. Authenticate with SST Auth ── */
    printf("[SST] Initializing context from: %s\n", config_path);
    SST_ctx_t *ctx = init_SST(config_path);
    if (!ctx) {
        fprintf(stderr, "[SST] init_SST() failed.\n");
        return 1;
    }
    printf("[SST] Context ready. Requesting session key from Auth...\n");

    session_key_list_t *key_list = get_session_key(ctx, NULL);
    if (!key_list || key_list->num_key == 0) {
        fprintf(stderr, "[SST] Failed to get session key from Auth.\n");
        free_SST_ctx_t(ctx);
        return 1;
    }

    session_key_t *k = &key_list->s_key[0];

    printf("[SST] Got session key.\n");
    print_hex("  Key ID:     ", k->key_id,     PROV_KEY_ID_SIZE);
    print_hex("  Cipher Key: ", k->cipher_key, PROV_KEY_SIZE);
    print_hex("  MAC Key:    ", k->mac_key,    PROV_MAC_KEY_SIZE);

    /* ── 2. Open serial to Pico ── */
    printf("[UART] Opening %s at 1 Mbps...\n", serial_dev);
    int fd = open_serial(serial_dev);
    if (fd < 0) {
        free_session_key_list_t(key_list);
        free_SST_ctx_t(ctx);
        return 1;
    }
    printf("[UART] Serial open.\n");

    /* ── 3. Build and send MSG_TYPE_KEY frame ──
     *
     * Frame layout (matches Pico firmware MSG_TYPE_KEY handler):
     *   [PREAMBLE:4][0x10:1][LEN_HI:1][LEN_LO:1][KEY_ID:8][CIPHER_KEY:16][MAC_KEY:32]
     */
    uint8_t header[7] = {
        PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4,
        MSG_TYPE_KEY,
        (PROV_PAYLOAD_LEN >> 8) & 0xFF,
        PROV_PAYLOAD_LEN & 0xFF
    };

    printf("[UART] Sending key to Pico...\n");

    if (write_all(fd, header,      sizeof(header))   != 0 ||
        write_all(fd, k->key_id,   PROV_KEY_ID_SIZE)  != 0 ||
        write_all(fd, k->cipher_key, PROV_KEY_SIZE)   != 0 ||
        write_all(fd, k->mac_key,  PROV_MAC_KEY_SIZE) != 0) {
        fprintf(stderr, "[UART] Write failed: %s\n", strerror(errno));
        close(fd);
        free_session_key_list_t(key_list);
        free_SST_ctx_t(ctx);
        return 1;
    }

    tcdrain(fd);
    printf("[UART] Key sent successfully.\n");
    printf("\nPico is now provisioned as an SST node.\n");
    printf("Key ID (broadcast over LiFi): ");
    for (int i = 0; i < PROV_KEY_ID_SIZE; i++) printf("%02X", k->key_id[i]);
    printf("\n");

    /* ── 5. Save key material for dashboard HMAC verification ── */
    FILE *kf = fopen("session_key.json", "w");
    if (kf) {
        fprintf(kf, "{\n");
        fprintf(kf, "  \"key_id\": \"");
        for (int i = 0; i < PROV_KEY_ID_SIZE; i++) fprintf(kf, "%02x", k->key_id[i]);
        fprintf(kf, "\",\n  \"cipher_key\": \"");
        for (int i = 0; i < PROV_KEY_SIZE; i++) fprintf(kf, "%02x", k->cipher_key[i]);
        fprintf(kf, "\",\n  \"mac_key\": \"");
        for (int i = 0; i < PROV_MAC_KEY_SIZE; i++) fprintf(kf, "%02x", k->mac_key[i]);
        fprintf(kf, "\"\n}\n");
        fclose(kf);
        printf("[KEY] Key material saved to session_key.json (for dashboard)\n");
    } else {
        fprintf(stderr, "[KEY] Warning: could not write session_key.json\n");
    }

    /* ── 4. Cleanup ── */
    close(fd);
    free_session_key_list_t(key_list);
    free_SST_ctx_t(ctx);
    return 0;
}
