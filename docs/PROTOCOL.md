# LiFi-Auth: Protocol & Message Format

## Preamble

Every message starts with a fixed 4-byte preamble for frame synchronization:

```c
// include/protocol.h
PREAMBLE_BYTE_1 = 0xAB
PREAMBLE_BYTE_2 = 0xCD
PREAMBLE_BYTE_3 = 0xEF
PREAMBLE_BYTE_4 = 0x12
```

## Message Frame Structure

```
[PREAMBLE 4B] [MSG_TYPE 1B] [PAYLOAD NB] [GCM_TAG 16B] [CRC16 2B]
  AB CD EF 12     0x02       ciphertext    auth tag      checksum
```

### Message Types

| Value | Name | Description |
|-------|------|-------------|
| 0x02 | ENCRYPTED | Standard encrypted payload |
| 0x04 | CHALLENGE | HMAC challenge (key exchange) |
| 0x05 | RESPONSE | HMAC response |
| 0x06 | FILE | File transfer |
| 0x10 | KEY | Key provisioning |

## Encryption

- **Algorithm:** AES-128-GCM
- **Key size:** 16 bytes (SESSION_KEY_SIZE)
- **Nonce size:** 12 bytes (NONCE_SIZE)
- **Tag size:** 16 bytes (TAG_SIZE)

### Nonce Construction

```
[8-byte boot salt (random)] [4-byte message counter (big-endian)]
```

- Boot salt: generated once at startup via `mbedTLS CTR-DRBG` seeded with hardware RNG
- Message counter: monotonically increasing, stored atomically
- Reboot triggered when counter exhausted (prevents GCM nonce reuse)

### Encryption Call

```c
// src/sst_crypto_embedded.c
sst_encrypt_gcm(key_16b, nonce_12b, plaintext, len, ciphertext_out, tag_16b_out)
sst_decrypt_gcm(key_16b, nonce_12b, ciphertext, len, tag_16b, plaintext_out)
```

## HMAC Challenge-Response (Key Exchange)

Used during key provisioning to authenticate the receiver:

```c
sst_hmac_sha256(session_key, challenge_32b, 32, response_32b)
```

State machine in `receiver/include/key_exchange.h`:
- `IDLE` → `WAITING_FOR_YES` → `WAITING_FOR_ACK` → `WAITING_FOR_HMAC_RESP`

ACK tokens: `"ACK"`, `"KEY_OK"`, `"I have the key"`

## Frame Integrity

CRC16-CCITT appended to every frame:
- Polynomial: 0x1021
- Initial value: 0xFFFF
- Functions: `crc16_ccitt()`, `crc16_append()`, `crc16_validate()`

## Physical Layer

- **Baud rate:** 1,000,000 bps (1 Mbps)
- **Encoding:** 8N1 UART (8 data bits, no parity, 1 stop bit)
- **TX:** PIO state machine on 4 pins simultaneously (GP6=White, GP7=Green, GP8=Blue, GP9=Red)
- **RX:** PIO state machine on GP27 (comparator output from TLV3501)
- **Polarity:** Inverted — photodiode HIGH = no light (idle), LOW = light (signal). Corrected in firmware with `byte = ~(word >> 24)`

## Compression

Payloads are compressed with heatshrink (LZ77-style) before encryption when using smart buffering:
- 2ms accumulation window collects incoming serial data
- Single compressed+encrypted frame sent per window
- Buffer size: 8KB max

## Replay Attack Prevention

Circular buffer of 64 recent nonces in `receiver/src/replay_window.c`:
- `replay_window_seen(nonce)` — returns true if nonce already used
- `replay_window_add(nonce)` — adds nonce to window
- Window size: 64 entries × 12 bytes each
