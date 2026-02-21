/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hotreload_crypto.h
 * @brief Internal crypto abstraction for HMAC-SHA256 verification
 *
 * This header defines the interface for SHA-256 and HMAC-SHA256 operations
 * used by the hotreload HTTP server. Backend implementations are selected
 * at build time based on the IDF/mbedTLS version:
 *   - IDF 6.x (mbedTLS 4.x): PSA Crypto API  (port/hotreload_crypto_psa.c)
 *   - IDF 5.x (mbedTLS 3.x): Legacy API       (port/hotreload_crypto_mbedcrypto.c)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define HOTRELOAD_SHA256_LEN 32
#define HOTRELOAD_HMAC_LEN   32

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the crypto subsystem
 *
 * Must be called once before any other crypto functions.
 * Safe to call multiple times.
 *
 * @param key       HMAC key bytes
 * @param key_len   Length of key in bytes
 * @return ESP_OK on success
 */
esp_err_t hotreload_crypto_init(const uint8_t *key, size_t key_len);

/**
 * @brief Deinitialize the crypto subsystem
 *
 * Releases any resources allocated by hotreload_crypto_init().
 */
void hotreload_crypto_deinit(void);

/**
 * @brief Verify SHA-256 hash of data
 *
 * @param data          Input data
 * @param data_len      Length of input data
 * @param expected_hash Expected SHA-256 hash (32 bytes)
 * @return ESP_OK if hash matches, ESP_ERR_INVALID_STATE if mismatch
 */
esp_err_t hotreload_crypto_sha256_verify(const uint8_t *data, size_t data_len,
                                         const uint8_t *expected_hash);

/**
 * @brief Verify HMAC-SHA256 of data using the key set in hotreload_crypto_init()
 *
 * Uses constant-time comparison internally.
 *
 * @param data          Input data
 * @param data_len      Length of input data
 * @param expected_hmac Expected HMAC-SHA256 value (32 bytes)
 * @return ESP_OK if HMAC matches, ESP_FAIL if mismatch
 */
esp_err_t hotreload_crypto_hmac_verify(const uint8_t *data, size_t data_len,
                                       const uint8_t *expected_hmac);

#ifdef __cplusplus
}
#endif
