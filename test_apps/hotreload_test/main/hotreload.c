#include <stdio.h>
#include "unity.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "protocol_examples_common.h"
#include "hotreload.h"
#include "reloadable.h"

static const char *TAG = "app_main";

/**
 * Integration test case for hot reload HTTP server.
 *
 * This test demonstrates the COOPERATIVE RELOAD pattern:
 * 1. Initialize networking and load the initial ELF
 * 2. Start the HTTP server (receives uploads but does NOT auto-reload)
 * 3. In main loop: call reloadable code, then check for updates at safe points
 * 4. Reload only when no reloadable code is on the call stack
 *
 * IMPORTANT: Reload must only happen at safe points where no reloadable
 * functions are on the call stack. If a reload happens while reloadable
 * code is executing, the application will crash when that code returns.
 *
 * The external pytest test (test_hotreload_e2e.py) will:
 * - Wait for server to start
 * - Modify and rebuild the reloadable code
 * - Send the new ELF via HTTP (upload only, no auto-reload)
 * - Wait for the app to detect the update and reload
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
    hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
    ret = hotreload_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ESP_LOGI(TAG, "Initial ELF loaded successfully");

    // Call the reloadable function to verify it works
    reloadable_init();
    reloadable_hello("from initial load");

    // Start the HTTP server for hot reload
    // Note: The server only receives uploads - it does NOT trigger reload.
    // The application must poll hotreload_update_available() and reload at safe points.
    hotreload_server_config_t server_config = HOTRELOAD_SERVER_CONFIG_DEFAULT();
    ret = hotreload_server_start(&server_config);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    ESP_LOGI(TAG, "Hotreload server started on port 8080");
    ESP_LOGI(TAG, "Ready for hot reload updates (cooperative reload enabled)");

    // Main loop demonstrating cooperative reload pattern
    for (int i = 0; i < 600; i++) {  // Run for up to 10 minutes (600 * 1s)
        // === STEP 1: Do work with reloadable code ===
        // All reloadable function calls happen here.
        // After this block completes, no reloadable code is on the stack.
        reloadable_hello("from main loop");

        // === STEP 2: Check for updates at SAFE POINT ===
        // This is safe because we're not inside any reloadable function.
        // The call stack only contains main app code at this point.
        if (hotreload_update_available()) {
            ESP_LOGI(TAG, "Update detected! Reloading at safe point...");
            ret = hotreload_reload(&config);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "Reload successful! Calling updated code:");
                reloadable_init();
                reloadable_hello("after reload");
            } else {
                ESP_LOGE(TAG, "Reload failed: %d", ret);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
        if ((i % 10) == 0) {
            ESP_LOGI(TAG, "Main loop iteration %d, waiting for updates...", i);
        }
    }

    ESP_LOGI(TAG, "Integration test complete (timeout)");
}

void app_main(void)
{
    printf("\n=== ESP-IDF Hot Reload Test Application ===\n");
    printf("Use Unity menu to select tests or integration mode.\n\n");

    // Use Unity interactive menu
    // pytest-embedded will send commands to select which tests to run
    unity_run_menu();
}
