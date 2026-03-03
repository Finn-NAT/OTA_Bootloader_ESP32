#include "esp32_can_bootloader.h"

static const esp_partition_t *active_partition = NULL;
static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t ota_handle = 0;
static bool ota_started = false;

void esp32_can_bootloader_init(void)
{
    /* TWAI timing configuration for 500 kbit/s */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    
    /* TWAI filter configuration - accept all messages */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    /* TWAI general configuration */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(13, 15, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = CONFIG_BOOTLOADER_CAN_RX_QUEUE_SIZE;
    g_config.tx_queue_len = CONFIG_BOOTLOADER_CAN_TX_QUEUE_SIZE;

    /* Install TWAI driver */
    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK)
    {
        printf("ESP32_CAN_Bootloader: Failed to install TWAI driver: %s\n", esp_err_to_name(ret));
        return;
    }

    /* Start TWAI driver */
    ret = twai_start();
    if (ret != ESP_OK)
    {
        printf("ESP32_CAN_Bootloader: Failed to start TWAI driver: %s\n", esp_err_to_name(ret));
        twai_driver_uninstall();
        return;
    }

    printf("ESP32_CAN_Bootloader: CAN Bootloader initialized (TX: GPIO13, RX: GPIO15, 500kbit/s)\n");
}

void esp32_can_write_response(uint8_t resp)
{
    twai_message_t tx_msg = {
        .identifier = ESP32_CAN_DEVICE_TO_HOST_ID,
        .data_length_code = 1,
        .data = {resp}
    };
    
    esp_err_t ret = twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
    if (ret != ESP_OK)
    {
        printf("ESP32_CAN_Bootloader: Failed to send response: 0x%02X\n", resp);
    }
}

uint32_t esp32_get_tick_count(void)
{
    return esp_get_tick_count();
}

uint32_t esp32_crc(uint32_t start_addr, uint32_t size, uint32_t *crc_tab)
{
    uint32_t   crc = 0xffffffffU;
    uint8_t    buffer[256];
    uint32_t   offset;
    uint32_t   remaining;
    uint32_t   chunk_size;


    /* Calculate offset relative to partition start */
    offset = start_addr - (uint32_t)update_partition->address;
    remaining = size;

    /* Read from partition and compute CRC */
    while (remaining > 0)
    {
        chunk_size = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;

        if (esp_partition_read(update_partition, offset, buffer, chunk_size) != ESP_OK)
        {
            printf("ESP32_CAN_Bootloader: CRC Failed to read at offset 0x%" PRIx32 "\n", offset);
            return 0;
        }

        for (i = 0; i < chunk_size; i++)
        {
            crc = crc_tab[(crc ^ buffer[i]) & 0xFFU] ^ (crc >> 8);
        }

        offset += chunk_size;
        remaining -= chunk_size;
    }

    return crc;
}