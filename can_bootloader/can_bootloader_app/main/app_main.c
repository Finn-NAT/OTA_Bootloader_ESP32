#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp32_can_bootloader.h"

static const char *TAG = "CAN_BOOTLOADER_APP";

/**
 * @brief Copy current running firmware to factory partition and boot from there
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t copy_ota_to_factory_and_boot(void)
{
    esp_err_t err;
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *factory_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);

    if (running_partition == NULL)
    {
        ESP_LOGE(TAG, "Running partition not found!");
        return ESP_ERR_NOT_FOUND;
    }
    
    if (factory_partition == NULL)
    {
        ESP_LOGE(TAG, "Factory partition not found!");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Running from: %s (0x%08" PRIx32 ")", 
             running_partition->label, running_partition->address);
    ESP_LOGI(TAG, "Will copy to: %s (0x%08" PRIx32 ", size: %" PRIu32 ")", 
             factory_partition->label, factory_partition->address, factory_partition->size);
    
    /* Erase factory partition */
    ESP_LOGI(TAG, "Erasing factory partition...");
    err = esp_partition_erase_range(factory_partition, 0, factory_partition->size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase factory partition: %s", esp_err_to_name(err));
        return err;
    }
    
    /* Copy in chunks */
    const size_t CHUNK_SIZE = 4096;
    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return ESP_ERR_NO_MEM;
    }
    
    /* Copy the whole running partition (safe & simple).
     * The unused area is typically 0xFF and does not affect image validity.
     */
    size_t total_size = running_partition->size;
    if (total_size > factory_partition->size)
    {
        total_size = factory_partition->size;
    }
    
    ESP_LOGI(TAG, "Copying %" PRIu32 " bytes from OTA to factory...", (uint32_t)total_size);
    
    size_t bytes_copied = 0;
    while (bytes_copied < total_size)
    {
        size_t to_copy = (total_size - bytes_copied > CHUNK_SIZE) ? 
                         CHUNK_SIZE : (total_size - bytes_copied);
        
        err = esp_partition_read(running_partition, bytes_copied, buffer, to_copy);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to read at offset 0x%x: %s", bytes_copied, esp_err_to_name(err));
            free(buffer);
            return err;
        }
        
        err = esp_partition_write(factory_partition, bytes_copied, buffer, to_copy);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to write at offset 0x%x: %s", bytes_copied, esp_err_to_name(err));
            free(buffer);
            return err;
        }
        
        bytes_copied += to_copy;
        
        /* Progress log every 64KB */
        if ((bytes_copied % (64 * 1024)) == 0)
        {
            ESP_LOGI(TAG, "Progress: %" PRIu32 " / %" PRIu32 " bytes", 
                     (uint32_t)bytes_copied, (uint32_t)total_size);
        }
    }
    
    free(buffer);
    ESP_LOGI(TAG, "Copy complete! %" PRIu32 " bytes written.", (uint32_t)bytes_copied);
    
    /* Set boot partition to factory */
    err = esp_ota_set_boot_partition(factory_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Boot partition set to factory. Rebooting in 2 seconds...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    
    return ESP_OK; /* Never reached */
}

/**
 * @brief Check current partition and handle OTA-to-factory copy if needed
 */
static void check_and_handle_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    
    ESP_LOGI(TAG, "Current partition: %s (type=%d, subtype=%d)", 
             running->label, running->type, running->subtype);
    
    /* Check if running from OTA partition (subtype >= OTA_0) */
    if (running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0)
    {
        ESP_LOGW(TAG, "Running from OTA partition (%s), will copy to factory...", 
                 running->label);
        
        esp_err_t err = copy_ota_to_factory_and_boot();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to copy to factory: %s", esp_err_to_name(err));
            ESP_LOGE(TAG, "Continuing with current partition...");
        }
        /* If copy failed, continue running from OTA */
    }
    else
    {
        ESP_LOGI(TAG, "Running from factory partition - normal operation");
    }
}

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

    /* Check partition and copy OTA to factory if needed */
    check_and_handle_partition();

    /* Run CAN bootloader (blocking) */
    while (true)
    {
        bootloader_CAN_Tasks();
    }
}
