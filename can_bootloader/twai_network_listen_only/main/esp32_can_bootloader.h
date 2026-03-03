#ifndef ESP32_BOOTLOADER_CAN_H
#define ESP32_BOOTLOADER_CAN_H

/* Dùng để include các thư viện cần thiết cho bootloader CAN trên ESP32 */
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
#include <stdbool.h>
#include <stdint.h>
/*///////////////////////////////////////////////////////////////////////*/

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

#define CONFIG_BOOTLOADER_CAN_RX_QUEUE_SIZE 1000
#define CONFIG_BOOTLOADER_CAN_TX_QUEUE_SIZE 100

/* CAN IDs - Host sends to 0x100, Device sends to 0x101 */
#define ESP32_CAN_HOST_TO_DEVICE_ID   0x100
#define ESP32_CAN_DEVICE_TO_HOST_ID   0x101

#ifdef __cplusplus
extern "C" {
#endif

#define BOOTLOADER_DEBUG(...) printf("CAN_Bootloader: "); printf(__VA_ARGS__); printf("\n")

#define BOOTLOADER_DELAY( ms ) vTaskDelay(pdMS_TO_TICKS(ms))

void esp32_can_bootloader_init(void);

void esp32_can_write_response(uint8_t resp);

void esp32_write_to_flash(const uint32_t *data, uint32_t size);

uint32_t esp32_get_tick_count(void);

uint32_t esp32_crc(uint32_t start_addr, uint32_t size, uint32_t *crc_tab);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_BOOTLOADER_CAN_H */
