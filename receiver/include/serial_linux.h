// include/serial_linux.h
#pragma once
int init_serial(const char* device, int baudrate);

// Same as init_serial(), but accepts ANY integer baud rate (not just the
// fixed Bxxxxxx constants) via the Linux termios2/BOTHER ioctl extension.
// Used by dash_receiver's dashboard-driven "type any baud" control.
int init_serial_baud(const char* device, int baud);
