#ifndef CRC16_H
#define CRC16_H

#include <stdint.h>
#include <stddef.h>

/**
 * CRC-16-CCITT (polynomial 0x1021, init 0xFFFF)
 * Used for frame validation in LiFi protocol.
 */
static inline uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/**
 * Append CRC16 to buffer (big-endian)
 */
static inline void crc16_append(uint8_t *buf, size_t data_len) {
    uint16_t crc = crc16_ccitt(buf, data_len);
    buf[data_len] = (crc >> 8) & 0xFF;
    buf[data_len + 1] = crc & 0xFF;
}

/**
 * Validate CRC16 at end of buffer
 * Returns 1 if valid, 0 if invalid
 */
static inline int crc16_validate(const uint8_t *buf, size_t total_len) {
    if (total_len < 2) return 0;
    size_t data_len = total_len - 2;
    uint16_t computed = crc16_ccitt(buf, data_len);
    uint16_t received = ((uint16_t)buf[data_len] << 8) | buf[data_len + 1];
    return computed == received;
}

#endif /* CRC16_H */
