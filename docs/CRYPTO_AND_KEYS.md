# LiFi-Auth: Cryptography & Key Management

## Cryptographic Primitives

All crypto implemented via **mbedTLS** vendored in `deps/`.
Thin wrappers live in `src/sst_crypto_embedded.c` / `include/sst_crypto_embedded.h`.

### AES-128-GCM Encryption

```c
int sst_encrypt_gcm(
    const uint8_t *key,        // 16 bytes
    const uint8_t *nonce,      // 12 bytes
    const uint8_t *plaintext,  // N bytes
    size_t         len,
    uint8_t       *ciphertext, // N bytes out
    uint8_t       *tag         // 16 bytes out
);

int sst_decrypt_gcm(
    const uint8_t *key,        // 16 bytes
    const uint8_t *nonce,      // 12 bytes
    const uint8_t *ciphertext, // N bytes
    size_t         len,
    const uint8_t *tag,        // 16 bytes (authentication tag)
    uint8_t       *plaintext   // N bytes out
);
```

Returns 0 on success. Decryption returns non-zero if tag verification fails (data tampered).

### HMAC-SHA256

```c
int sst_hmac_sha256(
    const uint8_t *key,     // 16 bytes
    const uint8_t *data,    // input
    size_t         data_len,
    uint8_t       *out      // 32 bytes
);
```

Used for challenge-response authentication during key exchange.

---

## Key Storage (Pico Flash)

### Flash Slot Layout

```
End of RP2040 flash (2MB)
├── Offset -1×FLASH_SECTOR (4KB): Index block
│     └── uint8_t: last_used_slot (0=A, 1=B)
├── Offset -2×FLASH_SECTOR:       Slot B
│     └── key_flash_block_t (256 bytes)
└── Offset -3×FLASH_SECTOR:       Slot A
      └── key_flash_block_t (256 bytes)
```

### Key Block Structure

```c
typedef struct {
    uint8_t  key_id[8];   // 8-byte key identifier
    uint8_t  key[16];     // 16-byte AES-128 session key
    uint8_t  hash[32];    // SHA-256(key_id || key) — integrity check
    uint32_t magic;       // 0x53455353 ('SESS') — slot occupied marker
} key_flash_block_t;      // total: 56 bytes, padded to 256 in flash
```

### Key Validation

On load:
1. Check `magic == 0x53455353`
2. Recompute SHA-256 over `key_id || key`
3. Compare with stored `hash`
4. If mismatch → slot corrupt, try other slot

### Key Loading Priority

```
1. Check index block for last_used_slot
2. Try that slot first
3. If invalid, try the other slot
4. If both invalid → no key loaded, device waits for provisioning
```

### Flash Write Safety

- Flash writes disabled during write operation (`irq_save()`)
- Entire 4KB sector erased before write (RP2040 requirement)
- SHA-256 hash written with key to detect partial writes

---

## Nonce Management

### Nonce Structure (12 bytes)

```
Bytes 0-7:   Random salt (generated once per boot via CTR-DRBG)
Bytes 8-11:  Message counter (32-bit, big-endian, atomically incremented)
```

### Nonce Uniqueness Guarantee

- Salt is unique per device boot (hardware entropy)
- Counter is unique per message within a boot
- Together: nonce is unique per message per key across reboots
- If counter wraps (2^32 messages) → device reboots immediately
- This makes AES-GCM nonce collision computationally infeasible

### PRNG Initialization

```c
void pico_prng_init(void);
// Seeds mbedTLS CTR-DRBG with get_rand_32() (RP2040 hardware RNG)
// Called once at startup before any crypto operations
```

---

## RAM Key Storage

Volatile RAM storage for in-use key (cleared on secure erase):

```c
void keyram_set(const uint8_t *key_id, const uint8_t *key);
void keyram_get(uint8_t *key_id_out, uint8_t *key_out);
void keyram_clear(void);  // secure_zero() — volatile write loop
```

`secure_zero()` macro prevents compiler from optimizing away the erase:
```c
#define secure_zero(ptr, len) do { \
    volatile uint8_t *p = (volatile uint8_t*)(ptr); \
    for (size_t i = 0; i < (len); i++) p[i] = 0; \
} while(0)
```

---

## Key Rotation (Dual Slot)

Enables zero-downtime key updates:

1. New key received via `new key` command → written to inactive slot
2. `use slot B` command → switches to new slot, updates index block
3. Old slot remains valid as fallback until explicitly cleared
4. `clear slot A` → erases old slot

Both slots can be independently valid. Device always starts with the last-used slot.

---

## Session Key Provisioning (IoTAuth Flow)

```
IoTAuth Server (deps/iotauth/)
     ↓ TLS + challenge-response
keys_receiver (Linux app)
     ↓ serial (USB)
Pico sender
     ↓ stores in flash slot
```

Key exchange state machine (`receiver/include/key_exchange.h`):
```
IDLE
  → send challenge (32 random bytes)
WAITING_FOR_YES
  → receiver ACKs with "ACK"
WAITING_FOR_ACK
  → receiver confirms with "KEY_OK"
WAITING_FOR_HMAC_RESP
  → receiver sends HMAC-SHA256(session_key, challenge)
  → verify HMAC → if match, provisioning complete
```

---

## Security Properties Summary

| Property | Mechanism |
|----------|-----------|
| Confidentiality | AES-128-GCM |
| Integrity | GCM authentication tag (16 bytes) |
| Authenticity | HMAC-SHA256 challenge-response |
| Replay prevention | 64-nonce sliding window on receiver |
| Nonce uniqueness | Boot salt + atomic counter |
| Key persistence | Dual flash slots with SHA-256 integrity |
| Key in RAM | Volatile zeroing on erase |
| Key rotation | Dual-slot hot-swap |
| Frame integrity | CRC16-CCITT in addition to GCM tag |
| Hardware RNG | RP2040 `get_rand_32()` for salt/PRNG seed |
