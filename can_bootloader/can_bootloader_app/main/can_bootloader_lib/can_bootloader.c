/*******************************************************************************
  CAN Bootloader Source File (Platform-Independent)

  Summary:
    Implements CAN bootloader protocol compatible with Microchip's host protocol.

  Description:
    Platform-independent implementation of CAN bootloader. Uses HAL abstraction
    for CAN, flash, and system operations to support multiple MCU platforms.
 *******************************************************************************/

#include "can_bootloader.h"
#include <string.h>
#include <inttypes.h>


// *****************************************************************************
// Section: Private Constants
// *****************************************************************************

static const uint8_t btl_guard[CAN_BL_GUARD_SIZE] = {0x4D, 0x43, 0x48, 0x50};

// *****************************************************************************
// Section: Version Info
// *****************************************************************************

static uint8_t g_btl_major_version = 1U;
static uint8_t g_btl_minor_version = 0U;

// *****************************************************************************
// Section: Private Helper Functions
// *****************************************************************************

static void write_response(const can_bl_hal_t *hal, uint8_t resp)
{
    can_bl_message_t tx_msg = {
        .identifier = CAN_BL_DEVICE_TO_HOST_ID,
        .data_length_code = 1,
        .data = {resp, 0, 0, 0, 0, 0, 0, 0}
    };
    
    if (hal->can_transmit != NULL)
    {
        hal->can_transmit(&tx_msg, 100);
    }
}

static void log_info(const can_bl_hal_t *hal, const char *msg)
{
    if (hal->log_info != NULL)
    {
        hal->log_info("%s", msg);
    }
}

static void log_error(const can_bl_hal_t *hal, const char *msg)
{
    if (hal->log_error != NULL)
    {
        hal->log_error("%s", msg);
    }
}

// *****************************************************************************
// Section: CRC32 Calculation
// *****************************************************************************

static uint32_t calculate_crc32(can_bl_context_t *ctx, const can_bl_hal_t *hal,
                                 uint32_t size)
{
    uint32_t i, j, value;
    uint32_t crc_tab[256];
    uint32_t crc = 0xFFFFFFFFU;
    uint8_t  buffer[256];
    uint32_t offset = 0;
    uint32_t remaining = size;
    uint32_t chunk_size;

    /* Build CRC32 table (polynomial 0xEDB88320) */
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

    /* Read from flash and compute CRC */
    while (remaining > 0)
    {
        chunk_size = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;

        if (hal->flash_read == NULL || !hal->flash_read(offset, buffer, chunk_size))
        {
            log_error(hal, "CRC: Failed to read flash");
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

// *****************************************************************************
// Section: Input Processing
// *****************************************************************************

static void input_task(can_bl_context_t *ctx, const can_bl_hal_t *hal)
{
    uint8_t *byte_buf = (uint8_t *)ctx->input_buffer;
    can_bl_message_t rx_msg;

    if (ctx->packet_received)
    {
        return;
    }

    /* Try to receive a CAN message */
    if (hal->can_receive == NULL || !hal->can_receive(&rx_msg, 10))
    {
        return;
    }

    /* Filter only messages from host */
    if (rx_msg.identifier != CAN_BL_HOST_TO_DEVICE_ID)
    {
        return;
    }

    uint32_t now = 0;
    if (hal->get_tick_ms != NULL)
    {
        now = hal->get_tick_ms();
    }
    
    /* Check for timeout between CAN frames */
    if ((ctx->last_rx_tick_ms != 0) && 
        ((now - ctx->last_rx_tick_ms) > CAN_BL_TIMEOUT_MS))
    {
        ctx->header_received = false;
        ctx->rx_ptr = 0;
        log_info(hal, "CAN timeout, resetting packet state");
    }
    
    /* Process each byte in the CAN frame */
    for (int i = 0; i < rx_msg.data_length_code; i++)
    {
        uint8_t input_data = rx_msg.data[i];
        
        if (!ctx->header_received)
        {
            byte_buf[ctx->rx_ptr++] = input_data;

            /* Check for each guard byte and discard if mismatch */
            if (ctx->rx_ptr <= CAN_BL_GUARD_SIZE)
            {
                if (input_data != btl_guard[ctx->rx_ptr - 1U])
                {
                    ctx->rx_ptr = 0;
                }
            }
            else if (ctx->rx_ptr == CAN_BL_HEADER_SIZE)
            {
                if (ctx->input_buffer[CAN_BL_GUARD_OFFSET] != CAN_BL_GUARD_VALUE)
                {
                    write_response(hal, BL_RESP_ERROR);
                }
                else
                {
                    ctx->rx_size = ctx->input_buffer[CAN_BL_SIZE_OFFSET];
                    ctx->input_command = (uint8_t)ctx->input_buffer[CAN_BL_CMD_OFFSET];
                    ctx->header_received = true;
                    ctx->active = true;
                }
                ctx->rx_ptr = 0;
            }
        }
        else /* header_received == true */
        {
            if (ctx->rx_ptr < ctx->rx_size)
            {
                byte_buf[ctx->rx_ptr++] = input_data;
            }

            if (ctx->rx_ptr == ctx->rx_size)
            {
                ctx->data_size = ctx->rx_size;
                ctx->rx_ptr = 0;
                ctx->rx_size = 0;
                ctx->packet_received = true;
                ctx->header_received = false;
            }
        }
    }

    if (hal->get_tick_ms != NULL)
    {
        ctx->last_rx_tick_ms = hal->get_tick_ms();
    }
}

// *****************************************************************************
// Section: Command Processing
// *****************************************************************************

static void command_task(can_bl_context_t *ctx, const can_bl_hal_t *hal)
{
    if (ctx->input_command == BL_CMD_UNLOCK)
    {
        uint32_t begin = ctx->input_buffer[CAN_BL_ADDR_OFFSET];
        uint32_t size  = ctx->input_buffer[CAN_BL_SIZE_OFFSET];
        uint32_t end   = begin + size;

        hal->log_debug("UNLOCK: begin=0x%" PRIx32 ", size=0x%" PRIx32 ", end=0x%" PRIx32,
                       begin, size, end);

        /* Validate with platform - let HAL decide if addresses are valid */
        if (hal->flash_begin != NULL && hal->flash_begin(begin, size))
        {
            ctx->unlock_begin = begin;
            ctx->unlock_end = end;
            ctx->ota_started = true;
            ctx->total_bytes_written = 0;
            write_response(hal, BL_RESP_OK);
            log_info(hal, "UNLOCK: OTA session started");
        }
        else
        {
            ctx->unlock_begin = 0;
            ctx->unlock_end = 0;
            write_response(hal, BL_RESP_ERROR);
            log_error(hal, "UNLOCK: Invalid address range or flash_begin failed");
        }
    }
    else if (ctx->input_command == BL_CMD_DATA)
    {
        ctx->flash_addr = (ctx->input_buffer[CAN_BL_ADDR_OFFSET] & CAN_BL_OFFSET_ALIGN_MASK);

        if (ctx->unlock_begin <= ctx->flash_addr && ctx->flash_addr < ctx->unlock_end)
        {
            /* Copy data to flash buffer */
            for (uint32_t i = 0; i < CAN_BL_FLASH_BUFFER_WORDS; i++)
            {
                ctx->flash_data[i] = ctx->input_buffer[i + CAN_BL_DATA_OFFSET];
            }
            ctx->flash_data_ready = true;
        }
        else
        {
            write_response(hal, BL_RESP_ERROR);
            log_error(hal, "DATA: Address out of unlock range");
        }
    }
    else if (ctx->input_command == BL_CMD_READ_VERSION)
    {
        write_response(hal, BL_RESP_OK);
        write_response(hal, g_btl_major_version);
        write_response(hal, g_btl_minor_version);
    }
    else if (ctx->input_command == BL_CMD_VERIFY)
    {
        uint32_t crc_expected = ctx->input_buffer[CAN_BL_CRC_OFFSET];

        if (!ctx->ota_started)
        {
            log_error(hal, "VERIFY: OTA not started");
            write_response(hal, BL_RESP_CRC_FAIL);
        }
        else
        {
            /* Calculate CRC of written data */
            uint32_t crc_calculated = calculate_crc32(ctx, hal, ctx->total_bytes_written);

            if (crc_expected != crc_calculated)
            {
                log_error(hal, "VERIFY: CRC mismatch");
                if (hal->flash_abort != NULL)
                {
                    hal->flash_abort();
                }
                ctx->ota_started = false;
                write_response(hal, BL_RESP_CRC_FAIL);
            }
            else
            {
                if (hal->flash_end != NULL && hal->flash_end())
                {
                    /* Set boot partition */
                    if (hal->set_boot_partition != NULL && hal->set_boot_partition())
                    {
                        log_info(hal, "VERIFY: set_boot_partition successful");
                        ctx->ota_started = false;
                        write_response(hal, BL_RESP_CRC_OK);
                    }
                    else
                    {
                        log_error(hal, "VERIFY: set_boot_partition failed");
                        write_response(hal, BL_RESP_CRC_FAIL);
                    }
                }
                else
                {
                    log_error(hal, "VERIFY: flash_end failed");
                    ctx->ota_started = false;
                    write_response(hal, BL_RESP_CRC_FAIL);
                }
            }
        }
    }
    else if (ctx->input_command == BL_CMD_RESET)
    {
        write_response(hal, BL_RESP_OK);
        log_info(hal, "RESET: Restarting...");
        
        if (hal->delay_ms != NULL)
        {
            hal->delay_ms(1000);
        }
        
        if (hal->system_reset != NULL)
        {
            hal->system_reset();
        }
    }
    else
    {
        write_response(hal, BL_RESP_INVALID);
    }

    ctx->packet_received = false;
}

// *****************************************************************************
// Section: Flash Task
// *****************************************************************************

static void flash_task(can_bl_context_t *ctx, const can_bl_hal_t *hal)
{
    /* data_size includes 4-byte address prefix */
    uint32_t bytes_to_write = ctx->data_size - 4U;

    if (hal->flash_write != NULL && hal->flash_write(ctx->flash_data, bytes_to_write))
    {
        ctx->total_bytes_written += bytes_to_write;
        write_response(hal, BL_RESP_OK);
    }
    else
    {
        log_error(hal, "FLASH: Write failed");
        write_response(hal, BL_RESP_ERROR);
    }

    ctx->flash_data_ready = false;
}

// *****************************************************************************
// Section: Public API Implementation
// *****************************************************************************

void can_bl_init(can_bl_context_t *ctx)
{
    memset(ctx, 0, sizeof(can_bl_context_t));
    ctx->initialized = true;
}

bool can_bl_task(can_bl_context_t *ctx, const can_bl_hal_t *hal)
{
    if (ctx == NULL || hal == NULL)
    {
        return false;
    }

    if (!ctx->initialized)
    {
        can_bl_init(ctx);
    }

    /* Process input */
    input_task(ctx, hal);

    /* Handle flash write or command */
    if (ctx->flash_data_ready)
    {
        flash_task(ctx, hal);
    }
    else if (ctx->packet_received)
    {
        command_task(ctx, hal);
    }

    return ctx->active;
}

void can_bl_reset(can_bl_context_t *ctx, const can_bl_hal_t *hal)
{
    if (ctx == NULL)
    {
        return;
    }

    if (ctx->ota_started && hal != NULL && hal->flash_abort != NULL)
    {
        hal->flash_abort();
    }

    can_bl_init(ctx);
}

void can_bl_get_version(uint8_t *major, uint8_t *minor)
{
    if (major != NULL)
    {
        *major = g_btl_major_version;
    }
    if (minor != NULL)
    {
        *minor = g_btl_minor_version;
    }
}

void can_bl_set_version(uint8_t major, uint8_t minor)
{
    g_btl_major_version = major;
    g_btl_minor_version = minor;
}

bool can_bl_is_active(const can_bl_context_t *ctx)
{
    return (ctx != NULL) && ctx->active;
}
