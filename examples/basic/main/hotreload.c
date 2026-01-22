/**
 * ESP-IDF Hot Reload Example
 *
 * This example demonstrates the hot reload functionality:
 * 1. Connects to WiFi
 * 2. Loads a reloadable ELF module from flash
 * 3. Starts an HTTP server for over-the-air updates
 * 4. Periodically calls the reloadable function to show updates
 *
 * To update the code at runtime:
 *   idf.py reload --url http://<device-ip>:8080
 *
 * To watch for changes and auto-reload:
 *   idf.py watch --url http://<device-ip>:8080
 */

#include <stdio.h>
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_examples_common.h"
#include "hotreload.h"
#include "reloadable.h"

static const char *TAG = "hotreload_example";

void app_main(void)
{
    printf("\n");
    printf("========================================\n");
    printf("   ESP-IDF Hot Reload Example\n");
    printf("========================================\n\n");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Connect to WiFi (configure via menuconfig or use default)
    ESP_LOGI(TAG, "Connecting to network...");
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected!");

    // Load the reloadable ELF from flash
    ESP_LOGI(TAG, "Loading reloadable module...");
    hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
    ret = hotreload_load(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load reloadable module: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Module loaded successfully!");

    // Initialize and call the reloadable function
    reloadable_init();
    reloadable_hello("World");

    // Start the HTTP server for hot reload
    ESP_LOGI(TAG, "Starting hot reload server...");
    hotreload_server_config_t server_config = HOTRELOAD_SERVER_CONFIG_DEFAULT();
    ret = hotreload_server_start(&server_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Hot reload server running on port 8080");
    ESP_LOGI(TAG, "To update code: idf.py reload --url http://<device-ip>:8080");
    ESP_LOGI(TAG, "To watch and auto-reload: idf.py watch --url http://<device-ip>:8080");
    ESP_LOGI(TAG, "");

    // Main loop - periodically call the reloadable function
    // After a reload, the new code will be called automatically
    int counter = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        counter++;
        ESP_LOGI(TAG, "Calling reloadable function (iteration %d):", counter);
        reloadable_hello("from main loop");
    }
}
