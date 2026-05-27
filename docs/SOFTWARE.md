# LiFi-Auth: Software Reference (Linux Side)

## Linux Receiver Applications

All built from `receiver/` and output to `artifacts/receiver/`.

### `ask_receiver`

**Source:** `receiver/src/ask_receiver.c`
**Purpose:** Main listener — receives encrypted LiFi messages, decrypts, displays

- ncurses UI with 3 panes: log, status, command input
- Auto-connects to last known LiFi sender ID
- Performs HMAC challenge-response authentication
- Decrypts with `sst_decrypt_gcm()`
- Color-coded output: green=success, red=error, cyan=challenge, yellow=timeout
- Logs to `receiver_ask_debug.log`

### `flash_receiver`

**Source:** `receiver/src/flash_receiver.c`
**Purpose:** Session receiver — handles full encrypted session messages including file payloads

- Same ncurses UI as ask_receiver
- Handles heatshrink-compressed payloads
- Logs to `receiver_debug.log`

### `keys_receiver`

**Source:** `receiver/src/keys_receiver.c`
**Purpose:** Key provisioning — distributes and updates session keys

- Manages session key distribution from IoTAuth server to Pico sender
- Same ncurses UI
- Logs to `receiver_keys_debug.log`

### `speed_test_receiver`

**Source:** `receiver/src/speed_test_receiver.c`
**Purpose:** Throughput benchmarking tool

- Opens serial port (any device + baud rate as CLI args)
- Detects 4-byte preamble (0xAB 0xCD 0xEF 0x12)
- Prints received payloads, measures throughput
- Usage: `./speed_test_receiver /dev/serial0 1000000`
- Supports baud rates: 9600 → 4,000,000

---

## Shared Receiver Utilities

### Serial Port (`receiver/src/serial_linux.c`)

```c
int init_serial(const char *device, int baud);
```

Opens serial port with raw termios:
- 8N1, no flow control, no parity
- VMIN=1 (block until 1 byte), VTIME=30 (3s timeout)
- Returns file descriptor

### Replay Window (`receiver/src/replay_window.c`)

```c
bool replay_window_seen(const uint8_t *nonce);
void replay_window_add(const uint8_t *nonce);
```

Circular buffer of 64 × 12-byte nonces. Prevents replay attacks without requiring timestamps.

### Utilities (`receiver/src/utils.c`)

```c
void print_hex(const uint8_t *buf, size_t len);
int  read_exact(int fd, uint8_t *buf, size_t len);
void rand_bytes(uint8_t *buf, size_t len);   // reads /dev/urandom
```

---

## Web Dashboard

**Source:** `sender/dashboard/app.py`
**Launch:** `./dashboard.sh`
**URL:** `http://localhost:5000`

### Features

- Auto-detects Pico on `/dev/ttyACM0`, `/dev/ttyACM1`, `/dev/ttyACM2`
- Real-time bidirectional serial I/O via WebSocket (Socket.IO)
- Send arbitrary commands to Pico
- Button shortcuts: print key, slot status, clear slot, reboot, help
- Bulk text/file upload — Pico handles compression + encryption
- LED mask control (White/Green/Blue/Red on/off)
- Color-coded output matching Pico output conventions

### Threading Model

```python
serial_lock = threading.Lock()   # Protects serial write/close
# Daemon thread: continuously reads serial → emits to WebSocket
# Main thread: Flask + Socket.IO event loop
```

### Key Socket.IO Events

| Event | Direction | Action |
|-------|-----------|--------|
| `send_command` | Client→Server | Write string to serial |
| `serial_output` | Server→Client | Received serial line |
| `connection_status` | Server→Client | Connected/disconnected |

---

## `rx_monitor.py` (Root Level)

**Purpose:** Minimal serial monitor for the receiver Pico

```python
# Connects to /dev/ttyACM0 at 115200
# Prints every received line
# Auto-reconnects on disconnect (2s delay)
# DTR asserted to trigger stdio_usb on Pico
```

Usage:
```bash
python3 rx_monitor.py
```

This is the primary tool used during bring-up and debugging. Run it immediately after flashing a new `.uf2` to catch startup messages.

---

## `sender/send_file.py`

**Purpose:** CLI file sender to Pico

```bash
python3 send_file.py /dev/ttyACM0 myfile.txt
```

Reads file contents, appends newline, writes to serial. Pico detects the newline as end-of-message trigger and processes for transmission.

---

## IoTAuth Integration (`deps/sst-c-api/`)

### SST C API

The `sst-c-api` git submodule provides secure session key management:

```c
// Initialize SST context with entity credentials
SST_ctx_t *ctx = init_SST(config_path);

// Request session key for a purpose
session_key_t key = get_session_key(ctx, "lifi-auth");

// Secure server connection for key exchange
secure_connect_to_server(ctx, server_ip, server_port);
```

Key structures:
```c
typedef struct {
    uint8_t  id[8];          // Key identifier
    uint8_t  mac_key[16];    // HMAC key
    uint8_t  cipher_key[16]; // AES key
    uint8_t  mode;           // Cipher mode
    uint32_t validity;       // Expiry
} session_key_t;

typedef struct {
    char    auth_server_ip[64];
    uint16_t auth_server_port;
    char    entity_name[64];
    char    cert_path[256];
    char    key_path[256];
} config_t;
```

### Credentials Location

`receiver/config/credentials/` — contains:
- `Net1.ClientKey.pem` — entity private key
- `Auth101EntityCert.pem` — entity certificate

---

## Build System

### Quick Build

```bash
# Build Pico sender firmware
./set_build.sh pico
./make_build.sh

# Build Linux receiver apps
./set_build.sh receiver
./make_build.sh

# Build Pico 2 receiver firmware (manual, separate CMake project)
cd receiver_pico/build
make
```

### Artifact Collection

`make_build.sh` collects outputs into `artifacts/` with:
- Timestamp-stamped directories
- SHA256 checksums for each binary
- Keeps last 3 builds, deletes older ones
- Handles picotool setup automatically

### mbedTLS Configuration

`config/mbedtls_config.h` enables only needed modules:

| Module | Flag |
|--------|------|
| AES | `MBEDTLS_AES_C` |
| GCM | `MBEDTLS_GCM_C` |
| SHA-256 | `MBEDTLS_SHA256_C` |
| HMAC / MD | `MBEDTLS_MD_C` |
| PRNG | `MBEDTLS_CTR_DRBG_C` |
| Entropy | `MBEDTLS_ENTROPY_C` |

Disabled: self-tests, filesystem I/O, timing (incompatible with RP2040).
