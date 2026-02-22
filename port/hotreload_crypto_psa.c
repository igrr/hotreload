/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hotreload_crypto_psa.c
 * @brief Crypto backend using PSA Crypto API (mbedTLS 4.x / IDF 6.x)
 */

#include "hotreload_crypto.h"
#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "hotreload_crypto";

static psa_key_id_t s_hmac_key_id = 0;

esp_err_t hotreload_crypto_init(const uint8_t *key, size_t key_len)
{
    if (key == NULL || key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)status);
        return ESP_FAIL;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, 8 * key_len);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));

    status = psa_import_key(&attr, key, key_len, &s_hmac_key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key failed: %d", (int)status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HMAC key imported (%d bytes)", (int)key_len);
    return ESP_OK;
}

void hotreload_crypto_deinit(void)
{
    if (s_hmac_key_id != 0) {
        psa_destroy_key(s_hmac_key_id);
        s_hmac_key_id = 0;
    }
}

esp_err_t hotreload_crypto_sha256_verify(const uint8_t *data, size_t data_len,
                                         const uint8_t *expected_hash)
{
    psa_status_t status = psa_hash_compare(PSA_ALG_SHA_256,
                                           data, data_len,
                                           expected_hash, HOTRELOAD_SHA256_LEN);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        ESP_LOGW(TAG, "SHA-256 mismatch (corrupted upload)");
        return ESP_ERR_INVALID_STATE;
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compare failed: %d", (int)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t hotreload_crypto_hmac_verify(const uint8_t *data, size_t data_len,
                                       const uint8_t *expected_hmac)
{
    psa_status_t status = psa_mac_verify(s_hmac_key_id,
                                         PSA_ALG_HMAC(PSA_ALG_SHA_256),
                                         data, data_len,
                                         expected_hmac, HOTRELOAD_HMAC_LEN);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        ESP_LOGW(TAG, "HMAC-SHA256 mismatch (authentication failed)");
        return ESP_FAIL;
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_mac_verify failed: %d", (int)status);
        return ESP_FAIL;
    }
    return ESP_OK;
}
