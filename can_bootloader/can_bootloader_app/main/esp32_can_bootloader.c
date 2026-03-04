/*******************************************************************************
  ESP32 CAN Bootloader HAL Implementation

  Summary:
    ESP32-specific HAL implementation for CAN bootloader.

  Description:
    Implements hardware abstraction layer for ESP32 platform including TWAI (CAN),
    OTA flash operations, and system functions.
 *******************************************************************************/

#include "esp32_can_bootloader.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <inttypes.h>

// *****************************************************************************
// Section: Private Variables
// *****************************************************************************

static const char *TAG = "ESP32_CAN_BL";

/* OTA state */
static const esp_partition_t *s_update_partition = NULL;
static esp_ota_handle_t s_ota_handle = 0;
static bool s_ota_active = false;
static bool s_hw_initialized = false;

/* Bootloader context */
static can_bl_context_t s_bl_context;

// *****************************************************************************
// Section: HAL Function Implementations
// *****************************************************************************

static bool hal_can_receive(can_bl_message_t *msg, uint32_t timeout_ms)
{
    twai_message_t rx_msg;
    
    esp_err_t ret = twai_receive(&rx_msg, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK)
    {
        return false;
    }
    
    msg->identifier = rx_msg.identifier;
    msg->data_length_code = rx_msg.data_length_code;
    memcpy(msg->data, rx_msg.data, 8);
    
    return true;
}

static bool hal_can_transmit(const can_bl_message_t *msg, uint32_t timeout_ms)
{
    twai_message_t tx_msg = {
        .identifier = msg->identifier,
        .data_length_code = msg->data_length_code,
        .data = {0}
    };
    memcpy(tx_msg.data, msg->data, msg->data_length_code);
    
    esp_err_t ret = twai_transmit(&tx_msg, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "CAN TX failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    return true;
}

static uint32_t hal_get_tick_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void hal_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void hal_system_reset(void)
{
    esp_restart();
}

static bool hal_flash_begin(uint32_t flash_start, uint32_t flash_length)
{
    /* Validate expected address range from host */
    if (flash_start != ESP32_OTA_EXPECTED_START)
    {
        ESP_LOGE(TAG, "Invalid start address: 0x%" PRIx32 " (expected 0x%" PRIx32 ")",
                 flash_start, (uint32_t)ESP32_OTA_EXPECTED_START);
        return false;
    }
    
    if (flash_length > ESP32_OTA_MAX_SIZE)
    {
        ESP_LOGE(TAG, "Size too large: %" PRIu32 " (max %" PRIu32 ")",
                 flash_length, (uint32_t)ESP32_OTA_MAX_SIZE);
        return false;
    }
    
    /* Get the next OTA partition */
    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL)
    {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }
    
    ESP_LOGI(TAG, "OTA partition: %s at 0x%" PRIx32 ", size: %" PRIu32,
             s_update_partition->label,
             (uint32_t)s_update_partition->address,
             (uint32_t)s_update_partition->size);
    
    /* Begin OTA with sequential writes */
    esp_err_t err = esp_ota_begin(s_update_partition, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_update_partition = NULL;
        return false;
    }
    
    s_ota_active = true;
    ESP_LOGI(TAG, "OTA begin successful");
    
    return true;
}

static bool hal_flash_write(const void *data, uint32_t len)
{
    if (!s_ota_active || s_ota_handle == 0)
    {
        ESP_LOGE(TAG, "OTA not started");
        return false;
    }
    
    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return false;
    }
    
    return true;
}

static bool hal_flash_read(uint32_t offset, void *data, uint32_t len)
{
    if (s_update_partition == NULL)
    {
        ESP_LOGE(TAG, "No update partition");
        return false;
    }
    
    esp_err_t err = esp_partition_read(s_update_partition, offset, data, len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_partition_read failed at offset 0x%" PRIx32 ": %s",
                 offset, esp_err_to_name(err));
        return false;
    }
    
    return true;
}

static bool hal_flash_end(void)
{
    if (!s_ota_active || s_ota_handle == 0)
    {
        ESP_LOGE(TAG, "OTA not active");
        return false;
    }
    
    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(TAG, "Image validation failed - corrupted image");
        }
        else
        {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        }
        s_ota_handle = 0;
        s_ota_active = false;
        return false;
    }
    
    s_ota_handle = 0;
    s_ota_active = false;
    
    return true;
}

static void hal_flash_abort(void)
{
    if (s_ota_active && s_ota_handle != 0)
    {
        esp_ota_abort(s_ota_handle);
        ESP_LOGW(TAG, "OTA aborted");
    }
    s_ota_handle = 0;
    s_ota_active = false;
    s_update_partition = NULL;
}

static bool hal_set_boot_partition(void)
{
    if (s_update_partition == NULL)
    {
        ESP_LOGE(TAG, "No update partition to set as boot");
        return false;
    }
    
    esp_err_t err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "Boot partition set to: %s", s_update_partition->label);
    return true;
}

static void hal_log_info(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char log_buf[256];
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", log_buf);
}

static void hal_log_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char log_buf[256];
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", log_buf);
}

static void hal_log_debug(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char log_buf[256];
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", log_buf);
}

// *****************************************************************************
// Section: HAL Interface Structure
// *****************************************************************************

static const can_bl_hal_t s_esp32_hal = {
    .can_receive        = hal_can_receive,
    .can_transmit       = hal_can_transmit,
    .get_tick_ms        = hal_get_tick_ms,
    .delay_ms           = hal_delay_ms,
    .system_reset       = hal_system_reset,
    .flash_begin        = hal_flash_begin,
    .flash_write        = hal_flash_write,
    .flash_read         = hal_flash_read,
    .flash_end          = hal_flash_end,
    .flash_abort        = hal_flash_abort,
    .set_boot_partition = hal_set_boot_partition,
    .log_info           = hal_log_info,
    .log_error          = hal_log_error,
    .log_debug          = hal_log_debug,
};

// *****************************************************************************
// Section: Public API Implementation
// *****************************************************************************

bool esp32_can_bl_hw_init(void)
{
    if (s_hw_initialized)
    {
        return true;
    }
    
    /* TWAI timing configuration */
#if ESP32_CAN_BITRATE == 1000
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
#elif ESP32_CAN_BITRATE == 800
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_800KBITS();
#elif ESP32_CAN_BITRATE == 500
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
#elif ESP32_CAN_BITRATE == 250
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
#elif ESP32_CAN_BITRATE == 125
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_125KBITS();
#else
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
#endif
    
    /* Accept all messages */
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    /* General configuration */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        ESP32_CAN_TX_GPIO, ESP32_CAN_RX_GPIO, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = ESP32_CAN_RX_QUEUE_SIZE;
    g_config.tx_queue_len = ESP32_CAN_TX_QUEUE_SIZE;
    
    /* Install driver */
    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI install failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    /* Start driver */
    ret = twai_start();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "TWAI start failed: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return false;
    }
    
    ESP_LOGI(TAG, "CAN initialized (TX: GPIO%d, RX: GPIO%d, %dkbit/s)",
             ESP32_CAN_TX_GPIO, ESP32_CAN_RX_GPIO, ESP32_CAN_BITRATE);
    
    s_hw_initialized = true;
    return true;
}

void esp32_can_bl_hw_deinit(void)
{
    if (s_hw_initialized)
    {
        twai_stop();
        twai_driver_uninstall();
        s_hw_initialized = false;
    }
}

const can_bl_hal_t* esp32_can_bl_get_hal(void)
{
    return &s_esp32_hal;
}

void esp32_can_bootloader_run(void)
{
    /* Initialize hardware (only once) */
    if (!esp32_can_bl_hw_init())
    {
        ESP_LOGE(TAG, "Hardware init failed");
        return;
    }
    
    /* Initialize bootloader context (only once) */
    static bool context_initialized = false;
    if (!context_initialized)
    {
        can_bl_init(&s_bl_context);
        context_initialized = true;
        ESP_LOGI(TAG, "CAN Bootloader started, waiting for commands...");
    }
    
    /* Run one iteration of the bootloader task */
    can_bl_task(&s_bl_context, &s_esp32_hal);
}

/* Legacy API compatibility */
void bootloader_CAN_Tasks(void)
{
    esp32_can_bootloader_run();
}