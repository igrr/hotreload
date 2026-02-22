/**
 * ESP-IDF Hot Reload Example
 *
 * This example demonstrates the hot reload functionality:
 * 1. Connects to WiFi
 * 2. Loads a reloadable ELF module from flash
 * 3. Starts an HTTP server for over-the-air updates
 * 4. Main loop calls reloadable functions, then checks for updates
 *    at a safe point where no reloadable code is on the stack
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
    hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(hotreload_load(&config));
    reloadable_init();

    // Start the HTTP server
    hotreload_server_config_t server_config = HOTRELOAD_SERVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(hotreload_server_start(&server_config));

    ESP_LOGI(TAG, "Hot reload server running on port 8080");
    ESP_LOGI(TAG, "To update code: idf.py reload --url http://<device-ip>:8080");
    ESP_LOGI(TAG, "To watch and auto-reload: idf.py watch --url http://<device-ip>:8080");

    // Main loop
    while (1) {
        reloadable_hello("World");

        // Check for updates at a safe point (no reloadable code on the stack)
        if (hotreload_update_available()) {
            ESP_LOGI(TAG, "Update available, reloading...");
            ESP_ERROR_CHECK(hotreload_reload(&config));
            reloadable_init();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
