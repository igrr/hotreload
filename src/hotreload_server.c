/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hotreload_server.c
 * @brief HTTP server for receiving ELF updates over the network
 */

#include <string.h>
#include "hotreload.h"
#include "hotreload_crypto.h"
#include "hotreload_hmac_key.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "hotreload_server";

// Server state
static httpd_handle_t s_server = NULL;
static hotreload_server_config_t s_config = {0};
static uint8_t *s_upload_buffer = NULL;
static size_t s_upload_size = 0;

// Convert a hex character to its 4-bit value, returns -1 on invalid input
static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode a hex string to bytes.  Returns number of bytes written, or -1 on error.
static int hex_decode(const char *hex, size_t hex_len, uint8_t *out, size_t out_size)
{
    if (hex_len % 2 != 0 || hex_len / 2 > out_size) {
        return -1;
    }
    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_char_to_nibble(hex[2 * i]);
        int lo = hex_char_to_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

// Send a 403 Forbidden response
static void send_403(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, message);
}

// Verify X-Hotreload-SHA256 and X-Hotreload-HMAC headers against body
static esp_err_t verify_upload_hmac(httpd_req_t *req,
                                     const uint8_t *body, size_t body_len)
{
    char sha256_hex[65] = {0};  // 64 hex chars + null
    char hmac_hex[65] = {0};

    // Extract headers
    if (httpd_req_get_hdr_value_str(req, "X-Hotreload-SHA256",
                                     sha256_hex, sizeof(sha256_hex)) != ESP_OK) {
        ESP_LOGW(TAG, "Missing X-Hotreload-SHA256 header");
        send_403(req, "Missing X-Hotreload-SHA256 header\n");
        return ESP_FAIL;
    }

    if (httpd_req_get_hdr_value_str(req, "X-Hotreload-HMAC",
                                     hmac_hex, sizeof(hmac_hex)) != ESP_OK) {
        ESP_LOGW(TAG, "Missing X-Hotreload-HMAC header");
        send_403(req, "Missing X-Hotreload-HMAC header\n");
        return ESP_FAIL;
    }

    // Decode hex
    uint8_t expected_sha256[HOTRELOAD_SHA256_LEN];
    uint8_t expected_hmac[HOTRELOAD_HMAC_LEN];

    if (hex_decode(sha256_hex, 64, expected_sha256, sizeof(expected_sha256)) != HOTRELOAD_SHA256_LEN) {
        ESP_LOGW(TAG, "Invalid X-Hotreload-SHA256 hex");
        send_403(req, "Invalid X-Hotreload-SHA256 value\n");
        return ESP_FAIL;
    }
    if (hex_decode(hmac_hex, 64, expected_hmac, sizeof(expected_hmac)) != HOTRELOAD_HMAC_LEN) {
        ESP_LOGW(TAG, "Invalid X-Hotreload-HMAC hex");
        send_403(req, "Invalid X-Hotreload-HMAC value\n");
        return ESP_FAIL;
    }

    // Step 1: SHA-256 integrity check (fast reject of corrupted uploads)
    esp_err_t err = hotreload_crypto_sha256_verify(body, body_len, expected_sha256);
    if (err != ESP_OK) {
        send_403(req, "SHA-256 integrity check failed\n");
        return ESP_FAIL;
    }

    // Step 2: HMAC-SHA256 authentication
    err = hotreload_crypto_hmac_verify(body, body_len, expected_hmac);
    if (err != ESP_OK) {
        send_403(req, "HMAC authentication failed\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HMAC verification passed");
    return ESP_OK;
}

// POST /upload handler - receives ELF file
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Receiving upload, content_length=%d", req->content_len);

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content");
        return ESP_FAIL;
    }

    if ((size_t)req->content_len > s_config.max_elf_size) {
        ESP_LOGE(TAG, "ELF too large: %d > %d", req->content_len, (int)s_config.max_elf_size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ELF too large");
        return ESP_FAIL;
    }

    // Allocate buffer for upload
    s_upload_buffer = malloc(req->content_len);
    if (s_upload_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for upload", req->content_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Receive the file
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, (char *)(s_upload_buffer + received),
                                  req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Retry on timeout
            }
            ESP_LOGE(TAG, "Receive error: %d", ret);
            free(s_upload_buffer);
            s_upload_buffer = NULL;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }

    s_upload_size = received;
    ESP_LOGI(TAG, "Received %d bytes", (int)s_upload_size);

    // Verify HMAC before writing to flash
    esp_err_t err = verify_upload_hmac(req, s_upload_buffer, s_upload_size);
    if (err != ESP_OK) {
        free(s_upload_buffer);
        s_upload_buffer = NULL;
        s_upload_size = 0;
        return ESP_FAIL;  // Response already sent by verify_upload_hmac
    }

    // Write to partition
    err = hotreload_update_partition(s_config.partition_label,
                                     s_upload_buffer, s_upload_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update partition: %d", err);
        free(s_upload_buffer);
        s_upload_buffer = NULL;
        s_upload_size = 0;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
        return ESP_FAIL;
    }

    // Send success response
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: ELF uploaded and written to flash\n");

    // Free the buffer - it's now in flash
    free(s_upload_buffer);
    s_upload_buffer = NULL;
    s_upload_size = 0;

    return ESP_OK;
}

// GET /pending handler - check if an update is pending
static esp_err_t pending_get_handler(httpd_req_t *req)
{
    bool pending = hotreload_update_available();

    httpd_resp_set_type(req, "application/json");
    if (pending) {
        httpd_resp_sendstr(req, "{\"pending\":true}\n");
    } else {
        httpd_resp_sendstr(req, "{\"pending\":false}\n");
    }

    return ESP_OK;
}

// GET /status handler - returns server status
static esp_err_t status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Hotreload server running\n");
    return ESP_OK;
}

esp_err_t hotreload_server_start(const hotreload_server_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize HMAC crypto with the build-time key
    esp_err_t err = hotreload_crypto_init(hotreload_hmac_key, hotreload_hmac_key_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize HMAC crypto");
        return err;
    }

    // Copy config
    s_config = *config;
    if (s_config.partition_label == NULL) {
        s_config.partition_label = "hotreload";
    }
    if (s_config.port == 0) {
        s_config.port = 8080;
    }
    if (s_config.max_elf_size == 0) {
        s_config.max_elf_size = 128 * 1024;  // 128KB default
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = s_config.port;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    // Increase stack size for file operations
    httpd_config.stack_size = 8192;

    err = httpd_start(&s_server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        hotreload_crypto_deinit();
        return err;
    }

    // Register URI handlers
    static const httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };

    static const httpd_uri_t pending_uri = {
        .uri = "/pending",
        .method = HTTP_GET,
        .handler = pending_get_handler,
    };

    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };

    httpd_register_uri_handler(s_server, &upload_uri);
    httpd_register_uri_handler(s_server, &pending_uri);
    httpd_register_uri_handler(s_server, &status_uri);

    // Get and display the server URL with IP address
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Hotreload server started at http://" IPSTR ":%d",
                     IP2STR(&ip_info.ip), s_config.port);
        } else {
            ESP_LOGI(TAG, "Hotreload server started on port %d", s_config.port);
        }
    } else {
        ESP_LOGI(TAG, "Hotreload server started on port %d", s_config.port);
    }
    ESP_LOGI(TAG, "  POST /upload  - Upload ELF to flash");
    ESP_LOGI(TAG, "  GET  /pending - Check if update is pending");
    ESP_LOGI(TAG, "  GET  /status  - Server status");

    return ESP_OK;
}

esp_err_t hotreload_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = httpd_stop(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %d", err);
        return err;
    }

    s_server = NULL;
    memset(&s_config, 0, sizeof(s_config));

    if (s_upload_buffer != NULL) {
        free(s_upload_buffer);
        s_upload_buffer = NULL;
    }
    s_upload_size = 0;

    hotreload_crypto_deinit();

    ESP_LOGI(TAG, "Hotreload server stopped");

    return ESP_OK;
}
