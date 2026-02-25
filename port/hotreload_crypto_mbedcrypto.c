/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hotreload_crypto_mbedcrypto.c
 * @brief Crypto backend using mbedTLS legacy API (mbedTLS 3.x / IDF 5.x)
 */

#include "hotreload_crypto.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"
#include "mbedtls/constant_time.h"
#include <string.h>

static const char *TAG = "hotreload_crypto";

/* Max key size: 64 bytes covers SHA-256 block size for any practical HMAC key */
#define HMAC_KEY_MAX_LEN 64

static uint8_t s_hmac_key[HMAC_KEY_MAX_LEN];
static size_t  s_hmac_key_len = 0;

esp_err_t hotreload_crypto_init(const uint8_t *key, size_t key_len)
{
    if (key == NULL || key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (key_len > HMAC_KEY_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_hmac_key, key, key_len);
    s_hmac_key_len = key_len;

    ESP_LOGI(TAG, "HMAC key stored (%d bytes)", (int)key_len);
    return ESP_OK;
}

void hotreload_crypto_deinit(void)
{
    memset(s_hmac_key, 0, sizeof(s_hmac_key));
    s_hmac_key_len = 0;
}

esp_err_t hotreload_crypto_sha256_verify(const uint8_t *data, size_t data_len,
                                         const uint8_t *expected_hash)
{
    uint8_t actual[HOTRELOAD_SHA256_LEN];

    int ret = mbedtls_sha256(data, data_len, actual, 0 /* is224 = false */);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_sha256 failed: -0x%04x", -ret);
        return ESP_FAIL;
    }

    if (mbedtls_ct_memcmp(actual, expected_hash, HOTRELOAD_SHA256_LEN) != 0) {
        ESP_LOGW(TAG, "SHA-256 mismatch (corrupted upload)");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t hotreload_crypto_hmac_verify(const uint8_t *data, size_t data_len,
                                       const uint8_t *expected_hmac)
{
    uint8_t actual[HOTRELOAD_HMAC_LEN];

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "SHA-256 digest info not available");
        return ESP_FAIL;
    }

    int ret = mbedtls_md_hmac(md_info,
                               s_hmac_key, s_hmac_key_len,
                               data, data_len,
                               actual);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_md_hmac failed: -0x%04x", -ret);
        return ESP_FAIL;
    }

    if (mbedtls_ct_memcmp(actual, expected_hmac, HOTRELOAD_HMAC_LEN) != 0) {
        ESP_LOGW(TAG, "HMAC-SHA256 mismatch (authentication failed)");
        return ESP_FAIL;
    }

    return ESP_OK;
}
