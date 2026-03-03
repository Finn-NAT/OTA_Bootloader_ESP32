#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"
#include "driver/gpio.h"
#include "driver/twai.h"

#include "bootloader_ota_can.h"

static const char *TAG_MAIN = "CAN_BOOTLOADER";

void app_main(void)
{
    ESP_LOGI(TAG_MAIN, "OTA App Start");
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    while (true)
    {
        bootloader_CAN_Tasks();
    }
}
