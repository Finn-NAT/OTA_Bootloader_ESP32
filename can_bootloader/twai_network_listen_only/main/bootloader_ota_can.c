/*******************************************************************************
  CAN Bootloader Source File for ESP32

  Summary:
    Implements a CAN bootloader compatible with Microchip's host protocol.

  Description:
    Receives firmware image packets over CAN using the same framing and
    command set as Microchip's bootloader (BL_CMD_* / BL_RESP_*). The payload is
    written into an application partition so that the existing btl_host_can.py tool
    can be reused without modifications.
 *******************************************************************************/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "driver/twai.h"
#include "string.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include <inttypes.h>

/* Cache line size for different ESP32 chips */
#if CONFIG_IDF_TARGET_ESP32
    #define CACHE_LINE_SIZE 4       /* ESP32 original - no cache alignment needed */
#elif CONFIG_IDF_TARGET_ESP32S2
    #define CACHE_LINE_SIZE 32      /* ESP32-S2 */
#elif CONFIG_IDF_TARGET_ESP32S3
    #define CACHE_LINE_SIZE 64      /* ESP32-S3 (can be 32 or 64, use max) */
#elif CONFIG_IDF_TARGET_ESP32C3
    #define CACHE_LINE_SIZE 32      /* ESP32-C3 */
#elif CONFIG_IDF_TARGET_ESP32C6
    #define CACHE_LINE_SIZE 64      /* ESP32-C6 */
#elif CONFIG_IDF_TARGET_ESP32H2
    #define CACHE_LINE_SIZE 32      /* ESP32-H2 */
#elif CONFIG_IDF_TARGET_ESP32C2
    #define CACHE_LINE_SIZE 32      /* ESP32-C2 */
#elif CONFIG_IDF_TARGET_ESP32P4
    #define CACHE_LINE_SIZE 64      /* ESP32-P4 */
#else
    #define CACHE_LINE_SIZE 64      /* Default - use largest for safety */
#endif

#ifndef CONFIG_BOOTLOADER_CAN_TIMEOUT_MS
#define CONFIG_BOOTLOADER_CAN_TIMEOUT_MS 100
#endif

#ifndef CONFIG_BOOTLOADER_CAN_VERSION_MAJOR
#define CONFIG_BOOTLOADER_CAN_VERSION_MAJOR 1
#endif

#ifndef CONFIG_BOOTLOADER_CAN_VERSION_MINOR
#define CONFIG_BOOTLOADER_CAN_VERSION_MINOR 0
#endif

// *****************************************************************************
// *****************************************************************************
// Section: Type Definitions
// *****************************************************************************
// *****************************************************************************

#ifndef CONFIG_BOOTLOADER_CAN_RX_QUEUE_SIZE
#define CONFIG_BOOTLOADER_CAN_RX_QUEUE_SIZE 100
#endif

#ifndef CONFIG_BOOTLOADER_CAN_TX_QUEUE_SIZE
#define CONFIG_BOOTLOADER_CAN_TX_QUEUE_SIZE 100
#endif

/* CAN IDs - Host sends to 0x100, Device sends to 0x101 */
#define CAN_HOST_TO_DEVICE_ID   0x100
#define CAN_DEVICE_TO_HOST_ID   0x101

#define FLASH_OTA_START             (0x110000UL)
#define FLASH_OTA_LENGTH            (0x100000UL)
#define PAGE_SIZE               (512UL)
#define ERASE_BLOCK_SIZE        (4096UL)
#define PAGES_IN_ERASE_BLOCK    (ERASE_BLOCK_SIZE / PAGE_SIZE)

#define GUARD_OFFSET            0
#define CMD_OFFSET              2
#define ADDR_OFFSET             0
#define SIZE_OFFSET             1
#define DATA_OFFSET             (1U)
#define CRC_OFFSET              0

#define CMD_SIZE                (1U)
#define GUARD_SIZE              (4U)
#define SIZE_SIZE               (4U)
#define OFFSET_SIZE             4
#define CRC_SIZE                4
#define HEADER_SIZE             (GUARD_SIZE + SIZE_SIZE + CMD_SIZE)
#define DATA_SIZE               ERASE_BLOCK_SIZE

#define WORDS(x)                ((uint32_t)((x) / sizeof(uint32_t)))

#define OFFSET_ALIGN_MASK       (~(ERASE_BLOCK_SIZE) + 1U)
#define SIZE_ALIGN_MASK         (~(PAGE_SIZE) + 1U)

#define BL_GUARD_VALUE             (0x5048434DUL)

#define BL_CMD_UNLOCK              (0xA0U)
#define BL_CMD_DATA                (0xA1U)
#define BL_CMD_VERIFY              (0xA2U)
#define BL_CMD_RESET               (0xA3U)
#define BL_CMD_READ_VERSION        (0xA4U)

enum
{
    BL_RESP_OK          = 0x50,
    BL_RESP_ERROR       = 0x51,
    BL_RESP_INVALID     = 0x52,
    BL_RESP_CRC_OK      = 0x53,
    BL_RESP_CRC_FAIL    = 0x54,
};

// *****************************************************************************
// *****************************************************************************
// Section: Global objects
// *****************************************************************************
// *****************************************************************************

static const char *TAG = "CAN_OTA_BOOTLOADER";

static const uint8_t btl_guard[GUARD_SIZE] = {0x4D, 0x43, 0x48, 0x50};

#define CACHE_ALIGNED_ATTR __attribute__((aligned(CACHE_LINE_SIZE)))

static CACHE_ALIGNED_ATTR uint32_t input_buffer[WORDS(OFFSET_SIZE + DATA_SIZE)];

static CACHE_ALIGNED_ATTR uint32_t flash_data[WORDS(DATA_SIZE)];

static uint32_t flash_addr          = 0;

static uint32_t unlock_begin        = 0;
static uint32_t unlock_end          = 0;
static uint32_t data_size           = 0;

static uint8_t input_command = 0;
static bool packet_received = false;
static bool flash_data_ready = false;

static bool can_bl_init_done = false;

static bool can_bl_active = false;

static TickType_t last_byte_tick = 0;
static uint32_t inter_byte_timeout_count = 0;

/* Performance measurement timestamps */
static TickType_t packet_start_tick = 0;
static TickType_t packet_complete_tick = 0;
static TickType_t flash_start_tick = 0;
static TickType_t flash_complete_tick = 0;

static const esp_partition_t *active_partition = NULL;
static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t ota_handle = 0;
static bool ota_started = false;
static uint32_t total_bytes_written = 0;

static void can_bootloader_init(void);
static void input_task(void);
static void command_task(void);
static void flash_task(void);

#define BTL_MAJOR_VERSION       3U
#define BTL_MINOR_VERSION       7U
static uint16_t bootloader_GetVersion( void )
{
    /* Function can be overriden with custom implementation */
    uint16_t btlVersion = (((BTL_MAJOR_VERSION & (uint16_t)0xFFU) << 8) | (BTL_MINOR_VERSION & (uint16_t)0xFFU));

    return btlVersion;
}

uint32_t bootloader_CRCGenerate(uint32_t start_addr, uint32_t size)
{
    uint32_t   i, j, value;
    uint32_t   crc_tab[256];
    uint32_t   crc = 0xffffffffU;
    uint8_t    buffer[256];
    uint32_t   offset;
    uint32_t   remaining;
    uint32_t   chunk_size;

    if (update_partition == NULL)
    {
        ESP_LOGE(TAG, "CRC: update_partition is NULL");
        return 0;
    }

    /* Build CRC table */
    for (i = 0; i < 256U; i++)
    {
        value = i;

        for (j = 0; j < 8U; j++)
        {
            if ((value & 1U) != 0U)
            {
                value = (value >> 1) ^ 0xEDB88320U;
            }
            else
            {
                value >>= 1;
            }
        }
        crc_tab[i] = value;
    }

    /* Calculate offset relative to partition start */
    offset = start_addr - (uint32_t)update_partition->address;
    remaining = size;

    /* Read from partition and compute CRC */
    while (remaining > 0)
    {
        chunk_size = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;

        if (esp_partition_read(update_partition, offset, buffer, chunk_size) != ESP_OK)
        {
            ESP_LOGE(TAG, "CRC: Failed to read at offset 0x%" PRIx32, offset);
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

void bootloader_TriggerReset(void) {}

static inline void write_response(uint8_t resp)
{
    twai_message_t tx_msg = {
        .identifier = CAN_DEVICE_TO_HOST_ID,
        .data_length_code = 1,
        .data = {resp}
    };
    
    esp_err_t ret = twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to send response: 0x%02X", resp);
    }
}

static void can_bootloader_init(void)
{
    /* TWAI timing configuration for 500 kbit/s */
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    
    /* TWAI filter configuration - accept all messages */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    /* TWAI general configuration */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(14, 13, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = CONFIG_BOOTLOADER_CAN_RX_QUEUE_SIZE;
    g_config.tx_queue_len = CONFIG_BOOTLOADER_CAN_TX_QUEUE_SIZE;

    /* Install TWAI driver */
    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install TWAI driver: %s", esp_err_to_name(ret));
        return;
    }

    /* Start TWAI driver */
    ret = twai_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start TWAI driver: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return;
    }

    ESP_LOGI(TAG, "CAN Bootloader initialized (TX: GPIO14, RX: GPIO13, 500kbit/s)");
    
    // Mark initialization as done
    can_bl_init_done = true;
}

static void input_task(void)
{
    static uint32_t ptr             = 0;
    static uint32_t size            = 0;
    static bool     header_received = false;
    uint8_t         *byte_buf       = (uint8_t *)&input_buffer[0];
    twai_message_t  rx_msg;

    if (packet_received)
    {
        return;
    }

    /* Try to receive a CAN message */
    esp_err_t ret = twai_receive(&rx_msg, pdMS_TO_TICKS(10));
    if (ret != ESP_OK)
    {
        return;
    }

    /* Filter only messages from host */
    if (rx_msg.identifier != CAN_HOST_TO_DEVICE_ID)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    
    /* Check for timeout between CAN frames */
    if ((last_byte_tick != 0) && ((now - last_byte_tick) * portTICK_PERIOD_MS > CONFIG_BOOTLOADER_CAN_TIMEOUT_MS))
    {
        header_received = false;
        ptr = 0;
        ESP_LOGI(TAG, "CAN timeout, resetting packet state");
    }
    
    /* Process each byte in the CAN frame */
    for (int i = 0; i < rx_msg.data_length_code; i++)
    {
        uint8_t input_data = rx_msg.data[i];
        
        if (header_received == false)
        {
            byte_buf[ptr++] = input_data;

            // Check for each guard byte and discard if mismatch
            if (ptr <= GUARD_SIZE)
            {
                if (input_data != btl_guard[ptr-1U])
                {
                    ptr = 0;
                }
            }
            else if (ptr == HEADER_SIZE)
            {
                if (input_buffer[GUARD_OFFSET] != BL_GUARD_VALUE)
                {
                    write_response(BL_RESP_ERROR);
                }
                else
                {
                    size            = input_buffer[SIZE_OFFSET];
                    input_command   = (uint8_t)input_buffer[CMD_OFFSET];
                    header_received = true;
                    can_bl_active    = true;
                    packet_start_tick = xTaskGetTickCount();
                    ESP_LOGI(TAG, "[PERF] Packet header received, waiting for data...");
                }

                ptr = 0;
            }
            else
            {
                /* Nothing to do */
            }
        }
        else if (header_received == true)
        {
            if (ptr < size)
            {
                byte_buf[ptr] = input_data;
                ptr++;
            }

            if (ptr == size)
            {
                data_size = size;
                ptr = 0;
                size = 0;
                packet_received = true;
                header_received = false;
                packet_complete_tick = xTaskGetTickCount();
                uint32_t receive_time = (packet_complete_tick - packet_start_tick) * portTICK_PERIOD_MS;
                ESP_LOGI(TAG, "[PERF] Packet received: cmd=0x%02X, size=%" PRIu32 ", receive_time=%" PRIu32 "ms", 
                         input_command, data_size, receive_time);
            }
        }
    }

    last_byte_tick = xTaskGetTickCount();
}

/* Function to process the received command */
static void command_task(void)
{
    uint32_t i;

    if (BL_CMD_UNLOCK == input_command)
    {
        uint32_t begin  = input_buffer[ADDR_OFFSET];

        uint32_t end    = begin + input_buffer[SIZE_OFFSET];

        if ((begin == FLASH_OTA_START) && (end <= (FLASH_OTA_START + FLASH_OTA_LENGTH)))
        {
            unlock_begin = begin;
            unlock_end = end;
            write_response(BL_RESP_OK);
        }
        else
        {
            unlock_begin = 0;
            unlock_end = 0;
            write_response(BL_RESP_ERROR);
        }
    }
    else if (BL_CMD_DATA == input_command)
    {
        //ESP_LOGI(TAG, "Data command received");
        flash_addr = (input_buffer[ADDR_OFFSET] & OFFSET_ALIGN_MASK);

        if (unlock_begin <= flash_addr && flash_addr < unlock_end)
        {
            for (i = 0; i < WORDS(DATA_SIZE); i++)
            {
                flash_data[i] = input_buffer[i + DATA_OFFSET];
            }
            //ESP_LOGI(TAG, "Flash data ready at address 0x%" PRIx32, flash_addr);

            flash_data_ready = true;
        }
        else
        {
            write_response(BL_RESP_ERROR);
        }
    }
    else if (BL_CMD_READ_VERSION == input_command)
    {
        write_response(BL_RESP_OK);

        uint16_t btlVersion = bootloader_GetVersion();
        uint16_t btlVer = ((btlVersion >> 8U) & 0xFFU);

        write_response((uint8_t)btlVer);
        btlVer = (btlVersion & 0xFFU);
        write_response((uint8_t)btlVer);
    }
    else if (BL_CMD_VERIFY == input_command)
    {
        esp_err_t err;
        uint32_t crc        = input_buffer[CRC_OFFSET];
        uint32_t crc_gen    = 0;

        if (!ota_started || ota_handle == 0)
        {
            ESP_LOGE(TAG, "OTA not started, cannot verify");
            write_response(BL_RESP_CRC_FAIL);
        }
        else
        {
            /* Calculate CRC of written data */
            crc_gen = bootloader_CRCGenerate(unlock_begin, (unlock_end - unlock_begin));

            ESP_LOGI(TAG, "CRC expected: 0x%08" PRIx32 ", calculated: 0x%08" PRIx32, crc, crc_gen);

            if (crc != crc_gen)
            {
                ESP_LOGE(TAG, "CRC mismatch!");
                ota_handle = 0;
                ota_started = false;
                write_response(BL_RESP_CRC_FAIL);
            }
            else
            {
                /* End OTA process - this validates the image */
                err = esp_ota_end(ota_handle);
                if (err != ESP_OK)
                {
                    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                    {
                        ESP_LOGE(TAG, "Image validation failed, image is corrupted");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
                    }
                    ota_handle = 0;
                    ota_started = false;
                    write_response(BL_RESP_CRC_FAIL);
                }
                else
                {
                    /* Set the new partition as boot partition */
                    err = esp_ota_set_boot_partition(update_partition);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
                        write_response(BL_RESP_CRC_FAIL);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "OTA successful! Total bytes written: %" PRIu32, total_bytes_written);
                        ESP_LOGI(TAG, "Next boot partition: %s", update_partition->label);
                        ota_handle = 0;
                        ota_started = false;
                        write_response(BL_RESP_CRC_OK);
                    }
                }
            }
        }
    }
    else if (BL_CMD_RESET == input_command)
    {
        write_response(BL_RESP_OK);

        ESP_LOGI(TAG, "Restarting in 1 second...");
        vTaskDelay(pdMS_TO_TICKS(1000));

        esp_restart();
    }
    else
    {
        write_response(BL_RESP_INVALID);
    }

    packet_received = false;
}

/* Function to program received application firmware data into internal flash using ESP32 OTA */
static void flash_task(void)
{
    esp_err_t err;

    flash_start_tick = xTaskGetTickCount();
    uint32_t wait_time = (flash_start_tick - packet_complete_tick) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "[PERF] Flash task started, wait_time=%" PRIu32 "ms", wait_time);

    // data_size = Actual data bytes to write + Address 4 Bytes
    uint32_t bytes_to_write = (data_size - 4U);
    // ESP_LOGI(TAG, "Flash task: addr=0x%" PRIx32 ", size=%" PRIu32,
    //          flash_addr, bytes_to_write);

    /* If OTA not started yet, begin OTA process */
    if (!ota_started)
    {
        /* Get the next OTA partition to write to */
        update_partition = esp_ota_get_next_update_partition(NULL);
        if (update_partition == NULL)
        {
            ESP_LOGE(TAG, "Failed to get update partition");
            flash_data_ready = false;
            write_response(BL_RESP_ERROR);
            return;
        }

        ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%" PRIx32,
                 update_partition->label, (uint32_t)update_partition->address);

        /* Begin OTA with sequential writes (erase as needed) */
        err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            flash_data_ready = false;
            write_response(BL_RESP_ERROR);
            return;
        }

        ota_started = true;
        total_bytes_written = 0;
        ESP_LOGI(TAG, "OTA begin successful");
    }

    /* Write data to OTA partition */
    err = esp_ota_write(ota_handle, (const void *)flash_data, bytes_to_write);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed (%s) at offset 0x%" PRIx32,
                 esp_err_to_name(err), total_bytes_written);
        flash_data_ready = false;
        write_response(BL_RESP_ERROR);
        return;
    }

    total_bytes_written += bytes_to_write;
    
    flash_complete_tick = xTaskGetTickCount();
    uint32_t flash_time = (flash_complete_tick - flash_start_tick) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "[PERF] Flash complete: %" PRIu32 " bytes in %" PRIu32 "ms (%.2f KB/s)",
             bytes_to_write, flash_time, 
             flash_time > 0 ? (bytes_to_write / 1024.0) / (flash_time / 1000.0) : 0.0);

    flash_data_ready = false;
    
    TickType_t response_start = xTaskGetTickCount();
    write_response(BL_RESP_OK);
    TickType_t response_end = xTaskGetTickCount();
    uint32_t response_time = (response_end - response_start) * portTICK_PERIOD_MS;
    
    uint32_t total_time = (response_end - packet_start_tick) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "[PERF] Response sent in %" PRIu32 "ms. TOTAL packet time: %" PRIu32 "ms",
             response_time, total_time);
}

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Global Functions
// *****************************************************************************
// *****************************************************************************

void bootloader_CAN_Tasks(void)
{
    if (!can_bl_init_done)
    {
        can_bootloader_init();
    }

    do
    {
        input_task();

        if (flash_data_ready)
        {
            flash_task();
        }
        else if (packet_received)
        {
            command_task();
        }
        else
        {
            /* Nothing to do */
        }
    } while (can_bl_active);
}
