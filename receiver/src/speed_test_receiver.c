#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#define PREAMBLE_BYTE_1 0xAB
#define PREAMBLE_BYTE_2 0xCD
#define PREAMBLE_BYTE_3 0xEF
#define PREAMBLE_BYTE_4 0x12

speed_t get_baud(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 500000: return B500000;
        case 576000: return B576000;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 1152000: return B1152000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        case 2500000: return B2500000;
        case 3000000: return B3000000;
        case 3500000: return B3500000;
        case 4000000: return B4000000;
        default: return B0;
    }
}

void print_usage(const char *prog) {
    printf("Usage: %s <device> <baud>\n", prog);
    printf("\nArguments:\n");
    printf("  device  The serial port path (e.g., /dev/serial0, /dev/ttyAMA1, /dev/ttyUSB0)\n");
    printf("  baud    The transmission speed in bits per second.\n");
    printf("\nSupported Baud Rates:\n");
    printf("  9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 576000,\n");
    printf("  921600, 1000000, 1152000, 1500000, 2000000, 2500000, 3000000,\n");
    printf("  3500000, 4000000\n");
    printf("\nExample:\n");
    printf("  %s /dev/serial0 1000000\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    int baud_num = atoi(argv[2]);

    speed_t baud = get_baud(baud_num);
    if (baud == B0) {
        fprintf(stderr, "Unsupported baud rate: %d\n", baud_num);
        return 1;
    }

    int fd = open(device, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open serial");
        return 1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return 1;
    }

    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;            // blocking read (wait for at least 1 byte)
    tty.c_cc[VTIME] = 0;            // no timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return 1;
    }

    printf("\033[1;32mLiFi Speed Test Receiver Started\033[0m\n");
    printf("Device: %s | Baud: %d\n", device, baud_num);
    printf("Listening for preamble 0x%02X%02X%02X%02X...\n\n", 
           PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4);

    unsigned char byte;
    int state = 0;
    char buffer[1024];
    int buf_idx = 0;

    while (read(fd, &byte, 1) > 0) {
        switch (state) {
            case 0:
                if (byte == PREAMBLE_BYTE_1) state = 1;
                break;
            case 1:
                if (byte == PREAMBLE_BYTE_2) state = 2;
                else state = 0;
                break;
            case 2:
                if (byte == PREAMBLE_BYTE_3) state = 3;
                else state = 0;
                break;
            case 3:
                if (byte == PREAMBLE_BYTE_4) {
                    state = 4;
                    buf_idx = 0;
                    printf("\033[1;36m[VALID PREAMBLE]\033[0m RX: ");
                } else {
                    state = 0;
                }
                break;
            case 4:
                if (byte == '\n' || byte == '\r') {
                    buffer[buf_idx] = '\0';
                    printf("\033[1;32m✓ %s\033[0m\n", buffer);
                    state = 0;
                } else if (buf_idx < sizeof(buffer) - 1) {
                    buffer[buf_idx++] = byte;
                    putchar(byte);
                    fflush(stdout);
                } else {
                    // Buffer overflow
                    buffer[buf_idx] = '\0';
                    printf("... [Buffer Full]\n");
                    state = 0;
                }
                break;
        }
    }

    close(fd);
    return 0;
}
