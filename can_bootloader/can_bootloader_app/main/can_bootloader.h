/*******************************************************************************
  CAN Bootloader Header File (Platform-Independent)

  Summary:
    Platform-independent CAN bootloader protocol compatible with Microchip's
    host protocol (btl_host_can.py).

  Description:
    This module implements the CAN bootloader protocol logic including packet
    parsing, command handling, and state machine. It uses a HAL abstraction
    layer to interface with platform-specific CAN and flash operations.
 *******************************************************************************/

#ifndef CAN_BOOTLOADER_H
#define CAN_BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// *****************************************************************************
// Section: Configuration Defaults
// *****************************************************************************

#ifndef CAN_BL_TIMEOUT_MS
#define CAN_BL_TIMEOUT_MS               100
#endif

// *****************************************************************************
// Section: Protocol Constants
// *****************************************************************************

/* CAN IDs - Host sends to 0x100, Device sends to 0x101 */
#define CAN_BL_HOST_TO_DEVICE_ID        0x100
#define CAN_BL_DEVICE_TO_HOST_ID        0x101

/* Flash/Memory constants */
#define CAN_BL_PAGE_SIZE                (512UL)
#define CAN_BL_ERASE_BLOCK_SIZE         (4096UL)
#define CAN_BL_DATA_SIZE                CAN_BL_ERASE_BLOCK_SIZE

/* Protocol frame offsets (in uint32_t words) */
#define CAN_BL_GUARD_OFFSET             0
#define CAN_BL_CMD_OFFSET               2
#define CAN_BL_ADDR_OFFSET              0
#define CAN_BL_SIZE_OFFSET              1
#define CAN_BL_DATA_OFFSET              1
#define CAN_BL_CRC_OFFSET               0

/* Protocol sizes (in bytes) */
#define CAN_BL_CMD_SIZE                 1
#define CAN_BL_GUARD_SIZE               4
#define CAN_BL_SIZE_SIZE                4
#define CAN_BL_OFFSET_SIZE              4
#define CAN_BL_HEADER_SIZE              (CAN_BL_CMD_SIZE + CAN_BL_GUARD_SIZE + CAN_BL_SIZE_SIZE)

/* Alignment masks */
#define CAN_BL_OFFSET_ALIGN_MASK        (~(CAN_BL_ERASE_BLOCK_SIZE) + 1U)

/* Guard value - "MCHP" in little-endian */
#define CAN_BL_GUARD_VALUE              (0x5048434DUL)

/* Bootloader Commands */
#define BL_CMD_UNLOCK                   (0xA0U)
#define BL_CMD_DATA                     (0xA1U)
#define BL_CMD_VERIFY                   (0xA2U)
#define BL_CMD_RESET                    (0xA3U)
#define BL_CMD_READ_VERSION             (0xA4U)

/* Bootloader Responses */
typedef enum {
    BL_RESP_OK          = 0x50,
    BL_RESP_ERROR       = 0x51,
    BL_RESP_INVALID     = 0x52,
    BL_RESP_CRC_OK      = 0x53,
    BL_RESP_CRC_FAIL    = 0x54,
} can_bl_response_t;

// *****************************************************************************
// Section: Buffer Size Macros
// *****************************************************************************

#define CAN_BL_WORDS(x)                 ((uint32_t)((x) / sizeof(uint32_t)))
#define CAN_BL_INPUT_BUFFER_WORDS       CAN_BL_WORDS(CAN_BL_OFFSET_SIZE + CAN_BL_DATA_SIZE)
#define CAN_BL_FLASH_BUFFER_WORDS       CAN_BL_WORDS(CAN_BL_DATA_SIZE)

// *****************************************************************************
// Section: CAN Message Structure (Platform-Independent)
// *****************************************************************************

typedef struct {
    uint32_t identifier;
    uint8_t  data_length_code;
    uint8_t  data[8];
} can_bl_message_t;

// *****************************************************************************
// Section: HAL Function Pointer Types (Platform Abstraction)
// *****************************************************************************

/**
 * @brief CAN receive function type
 * @param msg Pointer to message structure to fill
 * @param timeout_ms Timeout in milliseconds
 * @return true if message received, false on timeout/error
 */
typedef bool (*can_bl_hal_can_receive_t)(can_bl_message_t *msg, uint32_t timeout_ms);

/**
 * @brief CAN transmit function type
 * @param msg Pointer to message to transmit
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_can_transmit_t)(const can_bl_message_t *msg, uint32_t timeout_ms);

/**
 * @brief Get current tick count in milliseconds
 * @return Current tick count
 */
typedef uint32_t (*can_bl_hal_get_tick_ms_t)(void);

/**
 * @brief Delay function type
 * @param ms Milliseconds to delay
 */
typedef void (*can_bl_hal_delay_ms_t)(uint32_t ms);

/**
 * @brief Flash/OTA begin function type
 * @param flash_start Expected flash start address from host
 * @param flash_length Expected flash length from host
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_flash_begin_t)(uint32_t flash_start, uint32_t flash_length);

/**
 * @brief Flash/OTA write function type
 * @param data Pointer to data
 * @param len Length in bytes
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_flash_write_t)(const void *data, uint32_t len);

/**
 * @brief Flash/OTA read function type (for CRC calculation)
 * @param offset Offset from OTA partition start
 * @param data Buffer to read into
 * @param len Length in bytes
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_flash_read_t)(uint32_t offset, void *data, uint32_t len);

/**
 * @brief Flash/OTA end and validate function type
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_flash_end_t)(void);

/**
 * @brief Flash/OTA abort function type
 */
typedef void (*can_bl_hal_flash_abort_t)(void);

/**
 * @brief Set boot partition function type
 * @return true on success, false on error
 */
typedef bool (*can_bl_hal_set_boot_partition_t)(void);

/**
 * @brief System reset function type
 */
typedef void (*can_bl_hal_system_reset_t)(void);

/**
 * @brief Log function type
 * @param fmt Format string
 */
typedef void (*can_bl_hal_log_t)(const char *fmt, ...);

// *****************************************************************************
// Section: HAL Interface Structure
// *****************************************************************************

typedef struct {
    /* CAN operations */
    can_bl_hal_can_receive_t        can_receive;
    can_bl_hal_can_transmit_t       can_transmit;
    
    /* System operations */
    can_bl_hal_get_tick_ms_t        get_tick_ms;
    can_bl_hal_delay_ms_t           delay_ms;
    can_bl_hal_system_reset_t       system_reset;
    
    /* Flash/OTA operations */
    can_bl_hal_flash_begin_t        flash_begin;
    can_bl_hal_flash_write_t        flash_write;
    can_bl_hal_flash_read_t         flash_read;
    can_bl_hal_flash_end_t          flash_end;
    can_bl_hal_flash_abort_t        flash_abort;
    can_bl_hal_set_boot_partition_t set_boot_partition;
    
    /* Logging (optional, can be NULL) */
    can_bl_hal_log_t                log_info;
    can_bl_hal_log_t                log_error;
} can_bl_hal_t;

// *****************************************************************************
// Section: Bootloader Context Structure
// *****************************************************************************

typedef struct {
    /* Input buffer for receiving packets */
    uint32_t input_buffer[CAN_BL_INPUT_BUFFER_WORDS];
    
    /* Flash data buffer */
    uint32_t flash_data[CAN_BL_FLASH_BUFFER_WORDS];
    
    /* Address and size tracking */
    uint32_t flash_addr;
    uint32_t unlock_begin;
    uint32_t unlock_end;
    uint32_t data_size;
    uint32_t total_bytes_written;
    
    /* Packet parsing state */
    uint32_t rx_ptr;
    uint32_t rx_size;
    bool header_received;
    
    /* Command state */
    uint8_t input_command;
    bool packet_received;
    bool flash_data_ready;
    
    /* Bootloader state */
    bool initialized;
    bool active;
    bool ota_started;
    
    /* Timing */
    uint32_t last_rx_tick_ms;
    
} can_bl_context_t;

// *****************************************************************************
// Section: Public API
// *****************************************************************************

/**
 * @brief Initialize bootloader context
 * @param ctx Pointer to context
 */
void can_bl_init(can_bl_context_t *ctx);

/**
 * @brief Run the CAN bootloader task (call repeatedly in main loop)
 * @param ctx Pointer to context
 * @param hal Pointer to HAL interface
 * @return true if bootloader is active, false if idle
 */
bool can_bl_task(can_bl_context_t *ctx, const can_bl_hal_t *hal);

/**
 * @brief Reset bootloader state
 * @param ctx Pointer to context
 * @param hal Pointer to HAL interface (for flash_abort if needed)
 */
void can_bl_reset(can_bl_context_t *ctx, const can_bl_hal_t *hal);

/**
 * @brief Get bootloader version
 * @param major Pointer to store major version
 * @param minor Pointer to store minor version
 */
void can_bl_get_version(uint8_t *major, uint8_t *minor);

/**
 * @brief Set bootloader version
 * @param major Major version
 * @param minor Minor version
 */
void can_bl_set_version(uint8_t major, uint8_t minor);

/**
 * @brief Check if bootloader is currently active (in OTA session)
 * @param ctx Pointer to context
 * @return true if active
 */
bool can_bl_is_active(const can_bl_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CAN_BOOTLOADER_H */
