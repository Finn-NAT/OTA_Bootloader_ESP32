/* GPIO Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#define GPIO_OUTPUT_IO_0    19
#define GPIO_OUTPUT_PIN_SEL  (1ULL<<GPIO_OUTPUT_IO_0)

#define GPIO_INPUT_IO_0     20
#define GPIO_INPUT_PIN_SEL  (1ULL<<GPIO_INPUT_IO_0)

// Button is configured with pull-up, so pressed = 0
#define BUTTON_PRESSED_LEVEL 0

static const char *TAG = "factory_rollback";


static void gpio_task_example(void* arg)
{
    int last_level = gpio_get_level(GPIO_INPUT_IO_0);
    int stable_level = last_level;
    int stable_cnt = 0;

    for (;;) {
        const int level = gpio_get_level(GPIO_INPUT_IO_0);

        // Simple debounce: require N consecutive reads
        if (level == stable_level) {
            stable_cnt++;
        } else {
            stable_level = level;
            stable_cnt = 0;
        }

        // Check for falling edge (unpressed->pressed), after debounce
        if (stable_cnt >= 20 && stable_level != last_level) {
            last_level = stable_level;
            if (stable_level == BUTTON_PRESSED_LEVEL) {
                const esp_partition_t *running = esp_ota_get_running_partition();
                const esp_partition_t *factory = esp_partition_find_first(
                    ESP_PARTITION_TYPE_APP,
                    ESP_PARTITION_SUBTYPE_APP_FACTORY,
                    NULL);

                if (!running) {
                    ESP_LOGE(TAG, "Running partition is NULL (unexpected)");
                    continue;
                }

                ESP_LOGI(TAG, "Button pressed. Running: label=%s subtype=0x%02x offset=0x%08x",
                         running->label, running->subtype, (unsigned)running->address);

                if (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
                    ESP_LOGI(TAG, "Already running factory. Do nothing.");
                    continue;
                }

                if (!factory) {
                    ESP_LOGE(TAG, "No factory app partition found. Can't rollback.");
                    continue;
                }

                ESP_LOGW(TAG, "Switching boot partition to factory (label=%s offset=0x%08x) and restarting...",
                         factory->label, (unsigned)factory->address);

                esp_err_t err = esp_ota_set_boot_partition(factory);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
                    continue;
                }

                vTaskDelay(pdMS_TO_TICKS(1000));
                printf("Restarting to factory partition...\n");
                vTaskDelay(pdMS_TO_TICKS(1000));
                fflush(stdout);
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // while (1)
    // {
    //     vTaskDelay(pdMS_TO_TICKS(2000));
    // }
    
}

void app_main(void)
{
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 4096, NULL, 10, NULL);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ESP_LOGI(TAG, "Booted. Running: label=%s subtype=0x%02x offset=0x%08x",
                 running->label, running->subtype, (unsigned)running->address);
    } else {
        ESP_LOGW(TAG, "Booted, but running partition is NULL");
    }

    int cnt = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(GPIO_OUTPUT_IO_0, cnt++ % 2);
    }
}
