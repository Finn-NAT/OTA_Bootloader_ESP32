#include "can_bootloader.h"
#include "esp32_can_bootloader.h"

// *****************************************************************************
// *****************************************************************************
// Section: Type Definitions
// *****************************************************************************
// *****************************************************************************

#define CONFIG_BOOTLOADER_CAN_TIMEOUT_MS 100

#define FLASH_OTA_START             (0x110000UL)
#define FLASH_OTA_LENGTH            (0x100000UL)
#define PAGE_SIZE               (512UL)
#define ERASE_BLOCK_SIZE        (4096UL)
#define PAGES_IN_ERASE_BLOCK    (ERASE_BLOCK_SIZE / PAGE_SIZE)

#define GUARD_OFFSET            0
#define CMD_OFFSET              2
#define ADDR_OFFSET             0
#define SIZE_OFFSET             1
#define DATA_OFFSET             1
#define CRC_OFFSET              0

#define CMD_SIZE                (1U)
#define GUARD_SIZE              (4U)
#define SIZE_SIZE               (4U)
#define OFFSET_SIZE             (4U)
#define CRC_SIZE                (4U)
#define HEADER_SIZE             (CMD_SIZE + GUARD_SIZE + SIZE_SIZE)
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

static uint32_t last_byte_tick = 0;
static uint32_t inter_byte_timeout_count = 0;

static uint32_t total_bytes_written = 0;

static uint16_t bootloader_GetVersion( void );
static uint32_t bootloader_CRCGenerate(uint32_t start_addr, uint32_t size);
static void can_bootloader_init(void);

static void input_task(void);
static void command_task(void);
static void flash_task(void);

static inline void write_response(uint8_t resp)
{
    esp32_can_write_response(resp);
}

#define BTL_MAJOR_VERSION       1U
#define BTL_MINOR_VERSION       0U

static uint16_t bootloader_GetVersion( void )
{
    /* Function can be overriden with custom implementation */
    uint16_t btlVersion = (((BTL_MAJOR_VERSION & (uint16_t)0xFFU) << 8) | (BTL_MINOR_VERSION & (uint16_t)0xFFU));

    return btlVersion;
}

static uint32_t bootloader_CRCGenerate(uint32_t start_addr, uint32_t size)
{
    uint32_t   i, j, value;
    uint32_t   crc_tab[256];
    uint8_t    buffer[256];

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

    return esp32_crc(start_addr, size, crc_tab);
}

static void can_bootloader_init(void)
{
    esp32_can_bootloader_init();

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
    if (rx_msg.identifier != ESP32_CAN_HOST_TO_DEVICE_ID)
    {
        return;
    }

    uint32_t now = esp_get_tick_count();
    
    /* Check for timeout between CAN frames */
    if ((last_byte_tick != 0) && ((now - last_byte_tick) > CONFIG_BOOTLOADER_CAN_TIMEOUT_MS))
    {
        header_received = false;
        ptr = 0;
        BOOTLOADER_DEBUG("CAN timeout, resetting packet state");
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
                BOOTLOADER_DEBUG("Packet received: cmd=0x%02X, size=%" PRIu32, input_command, data_size);
            }
        }
    }

    last_byte_tick = esp32_get_tick_count();
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
        flash_addr = (input_buffer[ADDR_OFFSET] & OFFSET_ALIGN_MASK);

        if (unlock_begin <= flash_addr && flash_addr < unlock_end)
        {
            for (i = 0; i < WORDS(DATA_SIZE); i++)
            {
                flash_data[i] = input_buffer[i + DATA_OFFSET];
            }

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
            BOOTLOADER_DEBUG("OTA not started, cannot verify");
            write_response(BL_RESP_CRC_FAIL);
        }
        else
        {
            /* Calculate CRC of written data */
            crc_gen = bootloader_CRCGenerate(unlock_begin, (unlock_end - unlock_begin));

            BOOTLOADER_DEBUG("CRC expected: 0x%08" PRIx32 ", calculated: 0x%08" PRIx32, crc, crc_gen);

            if (crc != crc_gen)
            {
                BOOTLOADER_DEBUG("CRC mismatch!");
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
                        BOOTLOADER_DEBUG("Image validation failed, image is corrupted");
                    }
                    else
                    {
                        BOOTLOADER_DEBUG("esp_ota_end failed (%s)", esp_err_to_name(err));
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
                        BOOTLOADER_DEBUG("esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
                        write_response(BL_RESP_CRC_FAIL);
                    }
                    else
                    {
                        BOOTLOADER_DEBUG("OTA successful! Total bytes written: %" PRIu32, total_bytes_written);
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

        BOOTLOADER_DEBUG("Restarting in 1 second...");
        BOOTLOADER_DELAY(1000);

        system_restart();
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
    uint32_t bytes_to_write = (data_size - 4U);

    esp32_write_to_flash(flash_data, bytes_to_write);

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
