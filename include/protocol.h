#pragma once

/* -------- Protocol identity -------- */
#define PROTO_VERSION 1

/* -------- Framing -------- */
/* New 4-byte preamble (1 in 4 billion false positive rate) */
#define PREAMBLE_BYTE_1 0xAB
#define PREAMBLE_BYTE_2 0xCD
#define PREAMBLE_BYTE_3 0xEF
#define PREAMBLE_BYTE_4 0x12
#define PREAMBLE_SIZE   4

/* Message types */
#define MSG_TYPE_ENCRYPTED 0x02
#define MSG_TYPE_CHALLENGE 0x04
#define MSG_TYPE_RESPONSE  0x05
#define MSG_TYPE_FILE      0x06
#define MSG_TYPE_KEY_ID_ONLY 0x07 /* Plaintext Key ID broadcast */
#define MSG_TYPE_KEY       0x10  /* Key provisioning */

/* Cooldown to avoid thrashing key updates */
#define KEY_UPDATE_COOLDOWN_S 15

/* -------- Sizes -------- */
#define SESSION_KEY_SIZE 32  // AES-256-GCM (keep in sync with Pico)
#define SESSION_KEY_ID_SIZE 8 // Key ID size
#define NONCE_SIZE 12        // 96-bit GCM IV
#define TAG_SIZE 16
#define NONCE_HISTORY_SIZE 64
#define MAX_MSG_LEN 8192
#define CRC16_SIZE 2

/* -------- Shared tokens -------- */
#define KE_TOKEN_ACK_1 "ACK"
#define KE_TOKEN_ACK_2 "KEY_OK"
#define KE_TOKEN_ACK_3 "I have the key"
#define KE_TOKEN_YES "yes"

/* -------- Serial settings (Linux host only) -------- */
#ifdef __linux__
#define UART_DEVICE "/dev/serial0"
#include <termios.h>
#ifndef UART_BAUDRATE_TERMIOS
#define UART_BAUDRATE_TERMIOS B1000000
#endif
#endif

/* -------- Sanity checks -------- */
#if SESSION_KEY_SIZE != 32
#error "This project assumes a 32-byte session key for AES-256-GCM."
#endif
#if NONCE_SIZE != 12
#error "This project assumes a 12-byte GCM nonce."
#endif

// HMAC Handshake
#define CHALLENGE_SIZE      32
#define HMAC_SIZE           32

// HMAC response is sent as encrypted message with this prefix
#define HMAC_RESPONSE_PREFIX "HMAC:"
