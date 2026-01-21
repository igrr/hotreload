/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "hotreload.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "hotreload_server";

// Server state
static httpd_handle_t s_server = NULL;
static hotreload_server_config_t s_config = {0};
static uint8_t *s_upload_buffer = NULL;
static size_t s_upload_size = 0;

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

    // Write to partition
    esp_err_t err = hotreload_update_partition(s_config.partition_label,
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

// POST /reload handler - triggers reload
static esp_err_t reload_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reload requested");

    hotreload_config_t config = {
        .partition_label = s_config.partition_label,
        .symbol_table = s_config.symbol_table,
        .symbol_names = s_config.symbol_names,
        .symbol_count = s_config.symbol_count,
    };

    esp_err_t err = hotreload_reload(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reload failed: %d", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reload failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: Reload complete\n");

    return ESP_OK;
}

// POST /upload-and-reload handler - upload and reload in one request
static esp_err_t upload_and_reload_post_handler(httpd_req_t *req)
{
    // First upload
    esp_err_t err = upload_post_handler(req);
    if (err != ESP_OK) {
        return err;  // Error already sent
    }

    // Then reload
    hotreload_config_t config = {
        .partition_label = s_config.partition_label,
        .symbol_table = s_config.symbol_table,
        .symbol_names = s_config.symbol_names,
        .symbol_count = s_config.symbol_count,
    };

    err = hotreload_reload(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reload failed: %d", err);
        // Can't send error - already sent OK for upload
        return ESP_FAIL;
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

    if (config->symbol_table == NULL || config->symbol_names == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_ERR_INVALID_STATE;
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

    esp_err_t err = httpd_start(&s_server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        return err;
    }

    // Register URI handlers
    static const httpd_uri_t upload_uri = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
    };

    static const httpd_uri_t reload_uri = {
        .uri = "/reload",
        .method = HTTP_POST,
        .handler = reload_post_handler,
    };

    static const httpd_uri_t upload_reload_uri = {
        .uri = "/upload-and-reload",
        .method = HTTP_POST,
        .handler = upload_and_reload_post_handler,
    };

    static const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };

    httpd_register_uri_handler(s_server, &upload_uri);
    httpd_register_uri_handler(s_server, &reload_uri);
    httpd_register_uri_handler(s_server, &upload_reload_uri);
    httpd_register_uri_handler(s_server, &status_uri);

    ESP_LOGI(TAG, "Hotreload server started on port %d", s_config.port);
    ESP_LOGI(TAG, "  POST /upload            - Upload ELF to flash");
    ESP_LOGI(TAG, "  POST /reload            - Reload from flash");
    ESP_LOGI(TAG, "  POST /upload-and-reload - Upload and reload");
    ESP_LOGI(TAG, "  GET  /status            - Server status");

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

    ESP_LOGI(TAG, "Hotreload server stopped");

    return ESP_OK;
}
