#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp32_can_bootloader.h"

static const char *TAG = "CAN_BOOTLOADER_APP";

void app_main(void)
{
    ESP_LOGI(TAG, "CAN Bootloader Application Started");
    
    /* Initialize NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Run CAN bootloader (blocking) */
    while (true)
    {
        bootloader_CAN_Tasks();
    }
}
