#include <stdio.h>
#include "unity.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_examples_common.h"
#include "hotreload.h"
#include "reloadable_util.h"
#include "reloadable.h"

static const char *TAG = "app_main";

/**
 * Integration test case for hot reload HTTP server.
 *
 * This test case:
 * 1. Initializes networking (using openeth in QEMU)
 * 2. Loads the initial reloadable ELF
 * 3. Starts the HTTP server for hot reload
 * 4. Runs in a loop, waiting for reload commands
 *
 * The external pytest test (test_hotreload_e2e.py) will:
 * - Wait for server to start
 * - Modify and rebuild the reloadable code
 * - Send the new ELF via HTTP
 * - Verify the reload succeeded
 */
TEST_CASE("hotreload_integration", "[integration]")
{
    // Initialize NVS (required for networking)
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Initialize networking
    ESP_LOGI(TAG, "Initializing netif...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "Creating event loop...");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Connecting to network...");
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Network connected");

    // Load the initial reloadable ELF
    ret = HOTRELOAD_LOAD_DEFAULT();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ESP_LOGI(TAG, "Initial ELF loaded successfully");

    // Call the reloadable function to verify it works
    reloadable_init();
    reloadable_hello("from initial load");

    // Start the HTTP server for hot reload
    ret = HOTRELOAD_SERVER_START_DEFAULT();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ESP_LOGI(TAG, "Hotreload server started on port 8080");
    ESP_LOGI(TAG, "Ready for hot reload updates!");

    // Keep running - the HTTP server runs in background
    // The pytest test will interact with the server and verify results
    for (int i = 0; i < 60; i++) {  // Run for up to 10 minutes (60 * 10s)
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Main loop running, waiting for updates...");
    }

    ESP_LOGI(TAG, "Integration test complete (timeout)");
}

void app_main(void)
{
    printf("\n=== ESP32 Hot Reload Test Application ===\n");
    printf("Use Unity menu to select tests or integration mode.\n\n");

    // Use Unity interactive menu
    // pytest-embedded will send commands to select which tests to run
    unity_run_menu();
}
