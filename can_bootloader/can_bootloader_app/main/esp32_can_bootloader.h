/*******************************************************************************
  ESP32 CAN Bootloader HAL Header File

  Summary:
    ESP32-specific HAL implementation for CAN bootloader.

  Description:
    Provides hardware abstraction layer for ESP32 platform including TWAI (CAN),
    OTA flash operations, and system functions.
 *******************************************************************************/

#ifndef ESP32_CAN_BOOTLOADER_H
#define ESP32_CAN_BOOTLOADER_H

#include "can_bootloader_lib/can_bootloader.h"

/* ESP-IDF includes */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/twai.h"
#include "sdkconfig.h"

#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// *****************************************************************************
// Section: Configuration
// *****************************************************************************

/* GPIO pins for CAN transceiver */
#ifndef ESP32_CAN_TX_GPIO
#define ESP32_CAN_TX_GPIO               13
#endif

#ifndef ESP32_CAN_RX_GPIO
#define ESP32_CAN_RX_GPIO               15
#endif

/* CAN bitrate configuration */
#ifndef ESP32_CAN_BITRATE
#define ESP32_CAN_BITRATE               500  /* kbit/s */
#endif

/* Queue sizes */
#ifndef ESP32_CAN_RX_QUEUE_SIZE
#define ESP32_CAN_RX_QUEUE_SIZE         1000
#endif

#ifndef ESP32_CAN_TX_QUEUE_SIZE
#define ESP32_CAN_TX_QUEUE_SIZE         100
#endif

/* Expected flash start address from host (for validation) */
#ifndef ESP32_OTA_EXPECTED_START
#define ESP32_OTA_EXPECTED_START        0x110000UL
#endif

/* Maximum OTA size */
#ifndef ESP32_OTA_MAX_SIZE
#define ESP32_OTA_MAX_SIZE              0x100000UL
#endif

// *****************************************************************************
// Section: Public API
// *****************************************************************************

/**
 * @brief Initialize ESP32 CAN (TWAI) hardware
 * @return true on success, false on error
 */
bool esp32_can_bl_hw_init(void);

/**
 * @brief Deinitialize ESP32 CAN hardware
 */
void esp32_can_bl_hw_deinit(void);

/**
 * @brief Get ESP32-specific HAL interface
 * @return Pointer to HAL interface structure
 */
const can_bl_hal_t* esp32_can_bl_get_hal(void);

/**
 * @brief Main bootloader entry point for ESP32
 * 
 * This function initializes the CAN hardware, creates the HAL interface,
 * and runs the bootloader task loop.
 */
void esp32_can_bootloader_run(void);

/**
 * @brief Legacy API compatibility - same as esp32_can_bootloader_run()
 */
void bootloader_CAN_Tasks(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_CAN_BOOTLOADER_H */
