#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#define CONFIG_FILE "side.config"

void get_config_values(const char *filename, char *ip_out, int *port_out) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("Failed to open config file");
        exit(1);
    }

    char line[256];
    char *ip_key = "auth.ip.address=";
    char *port_key = "auth.port.number=";

    int ip_found = 0;
    int port_found = 0;

    while (fgets(line, sizeof(line), f)) {
        // Remove comments if any (naive check for #)
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        // Trim newline
        char *newline = strchr(line, '\n');
        if (newline) *newline = '\0';
        // Trim CR for Windows files
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        char *ptr;
        if ((ptr = strstr(line, ip_key)) == line) {
            strcpy(ip_out, ptr + strlen(ip_key));
            // Naive trim leading spaces
            while(*ip_out == ' ') memmove(ip_out, ip_out+1, strlen(ip_out));
            // Trim trailing spaces
            for(int i=strlen(ip_out)-1; i>=0; i--) {
                if(ip_out[i] == ' ') ip_out[i] = '\0';
                else break;
            }
            ip_found = 1;
        }
        else if ((ptr = strstr(line, port_key)) == line) {
            *port_out = atoi(ptr + strlen(port_key));
            port_found = 1;
        }
    }
    fclose(f);

    if (!ip_found || !port_found) {
        printf("Error: Could not find auth.ip.address or auth.port.number in %s\n", filename);
        exit(1);
    }
}

int main() {
    char target_ip[64] = {0};
    int target_port = 0;

    get_config_values(CONFIG_FILE, target_ip, &target_port);

    printf("Parsed configuration from %s:\n", CONFIG_FILE);
    printf("  Target IP:   %s\n", target_ip);
    printf("  Target Port: %d\n", target_port);
    printf("--------------------------------------\n");

    printf("Starting connectivity test...\n");

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket() failed");
        return 1;
    }
    printf("Socket created (fd=%d).\n", sock);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(target_ip);
    serv_addr.sin_port = htons(target_port);

    printf("Attempting connect()...\n");
    int ret = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    
    if (ret < 0) {
        printf("connect() failed! ret=%d, errno=%d (%s)\n", ret, errno, strerror(errno));
        close(sock);
        return 1;
    }

    printf("SUCCESS! Connected to Auth server.\n");
    
    printf("Attempting to read 1 byte...\n");
    char buf[1];
    ssize_t n = read(sock, buf, 1);
    if (n < 0) {
         printf("read() failed! errno=%d (%s)\n", errno, strerror(errno));
    } else if (n == 0) {
         printf("read() returned 0 (Server closed connection immediately).\n");
    } else {
         printf("read() got 1 byte: 0x%02X\n", (unsigned char)buf[0]);
    }

    close(sock);
    return 0;
}
