#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#include "../../include/protocol.h"

void print_usage(const char *prog) {
    printf("Usage: %s <device> <baud> [message]\n", prog);
    printf("\nArguments:\n");
    printf("  device   The serial port path (e.g., /dev/serial0, /dev/ttyAMA1, /dev/ttyUSB0)\n");
    printf("  baud     The transmission speed in bits per second.\n");
    printf("  message  Optional custom text to send (default: \"SPEED TEST DATA\")\n");
    printf("\nSupported Baud Rates:\n");
    printf("  9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 576000,\n");
    printf("  921600, 1000000, 1152000, 1500000, 2000000, 2500000, 3000000,\n");
    printf("  3500000, 4000000\n");
    printf("\nExample:\n");
    printf("  %s /dev/ttyAMA1 1000000 \"LiFi Test\"\n", prog);
}

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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    int baud_num = atoi(argv[2]);
    const char *message = (argc > 3) ? argv[3] : "SPEED TEST DATA";

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

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    tty.c_iflag &= ~IGNBRK;         // disable break processing
    tty.c_lflag = 0;                // no signaling chars, no echo, no canonical processing
    tty.c_oflag = 0;                // no remapping, no delays
    tty.c_cc[VMIN]  = 0;            // read doesn't block
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl
    tty.c_cflag |= (CLOCAL | CREAD); // ignore modem controls, enable reading
    tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return 1;
    }

    printf("Sending LiFi message on %s at %d baud...\n", device, baud_num);
    printf("Message: \"%s\"\n", message);

    // Send Preamble
    unsigned char preamble[] = {PREAMBLE_BYTE_1, PREAMBLE_BYTE_2, PREAMBLE_BYTE_3, PREAMBLE_BYTE_4};
    if (write(fd, preamble, 4) != 4) {
        perror("write preamble");
        return 1;
    }

    // Send Message
    if (write(fd, message, strlen(message)) != (ssize_t)strlen(message)) {
        perror("write message");
        return 1;
    }
    
    // Send Newline for readability on receiver
    write(fd, "\n", 1);

    tcdrain(fd);    // wait for transmission to finish
    close(fd);
    printf("Done.\n");

    return 0;
}
