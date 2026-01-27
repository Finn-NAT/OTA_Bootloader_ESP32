/*******************************************************************************
  UART Bootloader Source File for ESP32

  Summary:
    Implements a UART bootloader compatible with Microchip's host protocol.

  Description:
    Receives firmware image packets over UART using the same framing and
    command set as Microchip's bootloader (BL_CMD_* / BL_RESP_*). The payload is
    written into an application partition so that the existing btl_host.py tool
    can be reused without modifications.
 *******************************************************************************/

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
#include "driver/uart.h"
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

#ifndef CONFIG_BOOTLOADER_UART_TIMEOUT_MS
#define CONFIG_BOOTLOADER_UART_TIMEOUT_MS 100
#endif

#ifndef CONFIG_BOOTLOADER_UART_VERSION_MAJOR
#define CONFIG_BOOTLOADER_UART_VERSION_MAJOR 1
#endif

#ifndef CONFIG_BOOTLOADER_UART_VERSION_MINOR
#define CONFIG_BOOTLOADER_UART_VERSION_MINOR 0
#endif

// *****************************************************************************
// *****************************************************************************
// Section: Type Definitions
// *****************************************************************************
// *****************************************************************************

#ifndef CONFIG_BOOTLOADER_UART_RX_BUFFER_SIZE
#define CONFIG_BOOTLOADER_UART_RX_BUFFER_SIZE 2048
#endif

#ifndef CONFIG_BOOTLOADER_UART_BAUD_RATE
#define CONFIG_BOOTLOADER_UART_BAUD_RATE 115200
#endif

#ifndef CONFIG_BOOTLOADER_UART_TXD
#define CONFIG_BOOTLOADER_UART_TXD 14
#endif

#ifndef CONFIG_BOOTLOADER_UART_RXD
#define CONFIG_BOOTLOADER_UART_RXD 13
#endif

#define FLASH_OTA_START             (0x110000UL)
#define FLASH_OTA_LENGTH            (0x100000UL)
#define PAGE_SIZE               (512UL)
#define ERASE_BLOCK_SIZE        (8192UL)
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

static const char *TAG = "UART_OTA_BOOTLOADER";

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

static bool uart_bl_init_done = false;

static bool uart_bl_active = false;

static TickType_t last_byte_tick = 0;
static uint32_t inter_byte_timeout_count = 0;

static const esp_partition_t *active_partition = NULL;
static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t ota_handle = 0;
static bool ota_started = false;
static uint32_t total_bytes_written = 0;

static void uart_bootloader_init(void);
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
    uart_write_bytes(UART_NUM_1, (const char *)&resp, 1);
}

static void uart_bootloader_init(void)
{

    uart_config_t uart_config = {
        .baud_rate = CONFIG_BOOTLOADER_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1,
                                        CONFIG_BOOTLOADER_UART_RX_BUFFER_SIZE,
                                        0,
                                        0,
                                        NULL,
                                        0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1,
                                 CONFIG_BOOTLOADER_UART_TXD,
                                 CONFIG_BOOTLOADER_UART_RXD,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    // Mark initialization as done
    uart_bl_init_done = true;
}

static void input_task(void)
{
    static uint32_t ptr             = 0;
    static uint32_t size            = 0;
    static bool     header_received = false;
    uint8_t         *byte_buf       = (uint8_t *)&input_buffer[0];
    uint8_t         input_data      = 0;

    if (packet_received)
    {
        return;
    }

    int read = uart_read_bytes(UART_NUM_1, &input_data, sizeof(input_data), 10 / portTICK_PERIOD_MS);
    if (read <= 0)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((last_byte_tick != 0) && ((now - last_byte_tick) * portTICK_PERIOD_MS > CONFIG_BOOTLOADER_UART_TIMEOUT_MS))
    {
        header_received = false;
        ptr = 0;
        ESP_LOGI(TAG, "uart timeout, resetting packet state");
    }
    
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
                uart_bl_active    = true;
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
            ESP_LOGI(TAG, "Packet received: cmd=0x%02X, size=%" PRIu32, input_command, data_size);
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
    ESP_LOGD(TAG, "Written %" PRIu32 " bytes, total: %" PRIu32,
             bytes_to_write, total_bytes_written);

    flash_data_ready = false;
    write_response(BL_RESP_OK);
}

// *****************************************************************************
// *****************************************************************************
// Section: Bootloader Global Functions
// *****************************************************************************
// *****************************************************************************

void bootloader_UART_Tasks(void)
{
    if (!uart_bl_init_done)
    {
        uart_bootloader_init();
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
    } while (uart_bl_active);
}
