// src/serial_linux.c
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// Linux supports arbitrary (non-standard) baud rates via the termios2/
// BOTHER ioctl extension — the fixed Bxxxxxx constants glibc's <termios.h>
// exposes only cover a preset list; cfsetispeed() rejects anything else
// with EINVAL. We define the kernel-ABI struct/ioctl numbers ourselves
// under different names instead of including <asm/termbits.h>, which
// would redeclare `struct termios` and conflict with glibc's <termios.h>
// already included above.
#ifndef BOTHER
#define BOTHER 0010000
#endif

struct k_termios2 {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[19];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

#define K_TCGETS2 _IOR('T', 0x2A, struct k_termios2)
#define K_TCSETS2 _IOW('T', 0x2B, struct k_termios2)

int init_serial(const char* device, int baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY);
    if (fd == -1) {
        perror("open serial");
        return -1;
    }

    struct termios opt;
    if (tcgetattr(fd, &opt) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfmakeraw(&opt);
    cfsetispeed(&opt, baudrate);
    cfsetospeed(&opt, baudrate);
    opt.c_cflag |= (CLOCAL | CREAD);
    opt.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    opt.c_cflag |= CS8;
    opt.c_cc[VMIN] = 1;
    opt.c_cc[VTIME] = 30;  // 3 second timeout (was 0.1s) for large transfers

    if (tcsetattr(fd, TCSANOW, &opt) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }
    return fd;
}

int init_serial_baud(const char* device, int baud) {
    int fd = open(device, O_RDWR | O_NOCTTY);
    if (fd == -1) {
        perror("open serial");
        return -1;
    }

    struct termios opt;
    if (tcgetattr(fd, &opt) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfmakeraw(&opt);
    opt.c_cflag |= (CLOCAL | CREAD);
    opt.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    opt.c_cflag |= CS8;
    opt.c_cc[VMIN] = 1;
    opt.c_cc[VTIME] = 30;  // 3 second timeout (was 0.1s) for large transfers

    if (tcsetattr(fd, TCSANOW, &opt) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    // Baud rate goes through the BOTHER ioctl instead of cfsetispeed() so
    // any typed value works, not just the fixed Bxxxxxx list.
    struct k_termios2 tio2;
    if (ioctl(fd, K_TCGETS2, &tio2) != 0) {
        perror("TCGETS2");
        close(fd);
        return -1;
    }
    tio2.c_cflag &= ~CBAUD;
    tio2.c_cflag |= BOTHER;
    tio2.c_ispeed = (speed_t)baud;
    tio2.c_ospeed = (speed_t)baud;
    if (ioctl(fd, K_TCSETS2, &tio2) != 0) {
        perror("TCSETS2");
        close(fd);
        return -1;
    }

    return fd;
}
