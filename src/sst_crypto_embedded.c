// sst_crypto_embedded.c
#include "../include/sst_crypto_embedded.h"

#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

int sst_hmac_sha256(const uint8_t *key, const uint8_t *input, size_t input_len, uint8_t *output) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return -1; // Should not happen if SHA256 is enabled
    }
    
    // Use SST_KEY_SIZE (16 bytes) to match Pico's key storage
    return mbedtls_md_hmac(md_info, key, SST_KEY_SIZE, input, input_len, output);
}

int sst_encrypt_gcm(const uint8_t *key, const uint8_t *nonce,
                    const uint8_t *input, size_t input_len, uint8_t *ciphertext,
                    uint8_t *tag) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        return ret;
    }

    ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, input_len, nonce,
                                    SST_NONCE_SIZE, NULL, 0, input, ciphertext,
                                    SST_TAG_SIZE, tag);

    mbedtls_gcm_free(&gcm);
    return ret;
}

int sst_decrypt_gcm(const uint8_t *key, const uint8_t *nonce,
                    const uint8_t *ciphertext, size_t ciphertext_len,
                    const uint8_t *tag, uint8_t *output) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        return ret;
    }

    ret = mbedtls_gcm_auth_decrypt(&gcm, ciphertext_len, nonce, SST_NONCE_SIZE,
                                   NULL, 0, tag, SST_TAG_SIZE, ciphertext,
                                   output);

    mbedtls_gcm_free(&gcm);
    return ret;
}
