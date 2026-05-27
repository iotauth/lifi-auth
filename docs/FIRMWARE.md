# LiFi-Auth: Firmware Reference

## Sender Firmware (RP2040 Pico) — `lifi_session_sender`

**Source:** `sender/src/lifi_session_sender.c`
**PIO:** `sender/src/lifi_multi_tx.pio`
**CMake:** `sender/CMakeLists.txt`

### What It Does

1. Loads session key from flash (slot A or B)
2. Waits for plaintext message on USB serial
3. Encrypts with AES-128-GCM (fresh nonce per message)
4. Transmits: preamble + ciphertext + tag over 4 LED channels via PIO
5. Echoes transmission details back to USB for verification

### PIO TX State Machine (`lifi_multi_tx.pio`)

```
Input:  1 byte from OSR (TX FIFO)
Output: 4 GPIO pins (GP6-9) simultaneously
Format: 8N1 UART at 1 Mbps
Timing: 8 PIO cycles per bit
```

- All 4 LEDs transmit identical data
- LED mask controls which channels are active: bits = W G B R
- Clock divider: `sys_clock / (BAUD_RATE * 8)` — e.g. 125MHz / 8M = 15.625

### Key Flash Layout

```
Flash end
  ├─ [-1 sector] Index block   — last used slot (A=0 / B=1)
  ├─ [-2 sector] Slot B        — 256-byte key_flash_block_t
  └─ [-3 sector] Slot A        — 256-byte key_flash_block_t
```

Each `key_flash_block_t`:
```c
typedef struct {
    uint8_t  key_id[8];   // Key identifier
    uint8_t  key[16];     // AES-128 session key
    uint8_t  hash[32];    // SHA-256(key_id || key) for corruption detection
    uint32_t magic;       // 0x53455353 ('SESS') — validity marker
} key_flash_block_t;
```

### Nonce Generation

```c
// 8-byte boot salt (random, generated once at startup)
// 4-byte message counter (big-endian, atomic increment)
nonce[0..7]  = random_salt
nonce[8..11] = counter (big-endian)
```

Counter exhaustion → mandatory reboot (prevents GCM nonce reuse catastrophe).

### USB Commands (sender)

| Command | Action |
|---------|--------|
| `print slot key` | Print current key in hex |
| `slot status` | Show which slots are valid |
| `clear slot A/B/*` | Erase flash slot(s) |
| `use slot A/B` | Switch active key slot |
| `switch slot` | Toggle to other slot |
| `new key [-f]` | Receive new key via UART (-f forces even if slot not empty) |
| `key <hex>` | Manually set key from hex string |
| `leds <mask>` | Set LED output mask (bits: W G B R) |
| `reboot` | Restart device |

---

## Receiver Firmware (RP2350 Pico 2) — `lifi_pico2_rx`

**Source:** `receiver_pico/src/main.c`
**PIO:** `receiver_pico/src/lifi_rx.pio`

### What It Does

1. Initializes PIO RX on GP27 at 1 Mbps
2. Receives bytes from PIO FIFO
3. Inverts received byte (`~byte`) — corrects for reverse-biased photodiode
4. Runs preamble detection state machine
5. Collects payload after preamble until `\n` or `\r`
6. Prints `[RX #N] <message>` over USB at 115200 baud
7. Prints heartbeat every 2 seconds so you know USB is alive

### PIO RX State Machine (`lifi_rx.pio`)

```
Waits for start bit (HIGH on inverted line = light pulse beginning)
Samples 8 bits at center of each bit period
Auto-pushes 8-bit word to ISR (right-justified, bits in [31:24])
```

**Polarity:** Line idle = HIGH (no light). Start bit = first LOW (light on). Data is inverted from normal UART convention because the photodiode outputs HIGH when dark.

**Reading from PIO FIFO:**
```c
uint32_t word = pio_sm_get(pio, sm);
uint8_t byte = ~(uint8_t)(word >> 24);  // right-shift + invert
```

**Clock divider:**
```c
float div = clock_get_hz(clk_sys) / (BAUD_RATE * 8.0f);
// e.g. 150MHz / 8,000,000 = 18.75
```

### Preamble State Machine

```
STATE_HUNT    — looking for 0xAB
STATE_PRE1    — got 0xAB, looking for 0xCD
STATE_PRE2    — got 0xCD, looking for 0xEF
STATE_PRE3    — got 0xEF, looking for 0x12
STATE_PAYLOAD — accumulate bytes until \n/\r → print message
```

Any mismatch in PRE states resets to STATE_HUNT.

### USB Commands (receiver Pico 2)

Send at 115200 baud via any serial terminal:

| Command | Action |
|---------|--------|
| `raw on` | Print every received byte as `0xHH 'c'` |
| `raw off` | Return to preamble-framing mode |
| `status` | Print: pin, baud, mode (RAW/SST), message count |
| `pintest` | Sample GP27 for 3s, report transition count and idle level |

---

## Bring-Up / Debug Firmware — `raw_bit_monitor`

**Source:** `receiver_pico/src/raw_bit_monitor.c`

Used only for hardware bring-up of the analog front-end. Not part of normal operation.

### Startup Sequence (after flashing)

1. **3s sleep** — connect `rx_monitor.py` during this window
2. **I2C bus scan** — finds MCP4725 at 0x60-0x63
3. **Programs DAC** — sets threshold to ~0.8V
4. **Pull-up/pull-down test** — 1000 samples each; diagnoses floating vs. driven GP27
5. **DAC sweep** — ramps 0V→3.3V looking for first comparator flip (finds OPA floor)
6. **DAC toggle test** — alternates 0V ↔ 3.3V; verifies DAC physically reaches TLV IN+
7. **Bit flood** — prints 200-bit lines of `1`/`0` continuously, with `% HIGH` summary every 10 loops

### Diagnostic Outputs

```
GP27 pull-up:   X/1000 HIGH
GP27 pull-down: X/1000 HIGH
>>> FLOATING (wire GP27 to TLV OUT!)       ← pull-up HIGH, pull-down LOW
>>> actively driven HIGH                   ← both HIGH = comparator driving
>>> actively driven LOW                    ← both LOW
>>> toggling                               ← signal present

>>> Comparator flipped LOW at DAC=0xXXX (X.XXXv) — OPA floor ~X.XXXv
>>> Comparator never flipped — OPA above 3.3V or inputs swapped?

DAC=0.0V -> GP27 HIGH (500/500)           ← toggle test result
DAC=3.3V -> GP27 HIGH (500/500)
If all lines show HIGH: DAC output not wired to comparator IN pin.
```

---

## Speed Test Firmwares

### `pico_speed_test_sender` (RP2040)

**Source:** `sender/src/pico_speed_test_sender.c`

Sends repeated test frames at configurable baud rate for throughput measurement.

Commands:
- `baud <rate>` — change baud rate at runtime
- `send <msg>` — send test message
- `white/green/blue/red on/off` — individual LED control
- `raw <bits>` — raw bit transmission
- `loop on/off` — continuous transmission mode
- `status` — show current config

### `speed_test_receiver` (Linux)

**Source:** `receiver/src/speed_test_receiver.c`

Listens on a serial port, detects 4-byte preamble, counts and prints received payloads.
Usage: `./speed_test_receiver /dev/serial0 1000000`
