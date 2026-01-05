
/* Simple HTTP client LED control (ESP32-S3)
 *
 * Behavior:
 * - Connects to Wi-Fi/Ethernet using protocol_examples_common (example_connect)
 * - Periodically performs HTTP GET to a URL
 * - Expects response body contains either "on" or "off" (case-insensitive)
 * - Toggles a GPIO LED accordingly
 */

#include <ctype.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "driver/gpio.h"

#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "http_led";

/* 1) LED GPIO: change this to match your board
 * Many ESP32-S3 devkits have an on-board LED on GPIO48, but this varies.
 */
#ifndef EXAMPLE_LED_GPIO
#define EXAMPLE_LED_GPIO 19
#endif

/* 2) Poll URL: reuse the project Kconfig "Firmware upgrade URL" to avoid adding new Kconfig
 * Set it in menuconfig to something like: http://192.168.1.10:8000/led
 */
#ifndef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL
#error "CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL is not set. Please configure it in menuconfig."
#endif

#define EXAMPLE_POLL_PERIOD_MS 2000

/* Optional: send an incrementing counter to server every second.
 * Default endpoint: same host as CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL but path should be /count.
 * Example: http://192.168.5.83:8070/count
 */
#ifndef EXAMPLE_COUNT_URL
#define EXAMPLE_COUNT_URL "http://192.168.5.95:8070/count"
#endif

#define EXAMPLE_COUNT_PERIOD_MS 1000

static void led_init(void)
{
	gpio_config_t io_conf = {
		.pin_bit_mask = 1ULL << EXAMPLE_LED_GPIO,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&io_conf));
	ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_LED_GPIO, 0));
}

static void led_set(bool on)
{
	ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_LED_GPIO, on ? 1 : 0));
}

static void str_trim_inplace(char *s)
{
	if (s == NULL) {
		return;
	}
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || isspace((unsigned char)s[len - 1]))) {
		s[len - 1] = 0;
		len--;
	}
	size_t start = 0;
	while (s[start] && isspace((unsigned char)s[start])) {
		start++;
	}
	if (start > 0) {
		memmove(s, s + start, strlen(s + start) + 1);
	}
}

/* Parses response body. Accepts:
 * - "on" / "off" (case-insensitive)
 * - "1" / "0"
 * - JSON-like: {"led":"on"} or {"on":true} (naive substring match)
 */
static bool parse_led_state(const char *body, bool *out_on)
{
	if (body == NULL || out_on == NULL) {
		return false;
	}

	char tmp[128];
	strncpy(tmp, body, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = 0;
	str_trim_inplace(tmp);
	ESP_LOGD(TAG, "Body after trim: '%s'", tmp);

	// Make lowercase for easy compares
	for (size_t i = 0; tmp[i]; i++) {
		tmp[i] = (char)tolower((unsigned char)tmp[i]);
	}

	if (strcmp(tmp, "on") == 0 || strcmp(tmp, "1") == 0 || strcmp(tmp, "true") == 0) {
		*out_on = true;
		return true;
	}
	if (strcmp(tmp, "off") == 0 || strcmp(tmp, "0") == 0 || strcmp(tmp, "false") == 0) {
		*out_on = false;
		return true;
	}

	// Very simple JSON-ish fallback
	if (strstr(tmp, "\"on\"") || strstr(tmp, "led:on") || strstr(tmp, "\"led\":\"on\"")) {
		*out_on = true;
		return true;
	}
	if (strstr(tmp, "\"off\"") || strstr(tmp, "led:off") || strstr(tmp, "\"led\":\"off\"")) {
		*out_on = false;
		return true;
	}

	return false;
}

static esp_err_t http_get_body(const char *url, char *out_body, size_t out_body_size, int *out_status)
{
	if (url == NULL || out_body == NULL || out_body_size < 2) {
		return ESP_ERR_INVALID_ARG;
	}
	out_body[0] = 0;

	esp_http_client_config_t cfg = {
		.url = url,
		.timeout_ms = 10000,
		.keep_alive_enable = true,
		.disable_auto_redirect = false,
	};

	esp_http_client_handle_t client = esp_http_client_init(&cfg);
	if (client == NULL) {
		return ESP_FAIL;
	}

	esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_GET);
	if (err != ESP_OK) {
		esp_http_client_cleanup(client);
		return err;
	}

	/* Use open/fetch_headers/read loop for reliable body reads.
	 * Some servers + keep-alive + chunked transfer + redirects can yield empty read_response().
	 */
	err = esp_http_client_open(client, 0);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
		esp_http_client_cleanup(client);
		return err;
	}

	int64_t content_length = esp_http_client_fetch_headers(client);
	int status = esp_http_client_get_status_code(client);
	if (out_status) {
		*out_status = status;
	}
	printf("HTTP status=%d, content_length=%lld \n", status, (long long)content_length);

	int total = 0;
	while (total < (int)out_body_size - 1) {
		int r = esp_http_client_read(client, out_body + total, (int)out_body_size - 1 - total);
		if (r > 0) {
			total += r;
			continue;
		}
		if (r == 0) {
			break; // done
		}
		ESP_LOGW(TAG, "esp_http_client_read failed (%d)", r);
		break;
	}
	out_body[total] = 0;

	esp_http_client_close(client);

	esp_http_client_cleanup(client);
	return ESP_OK;
}

static esp_err_t http_post_count(const char *url, uint32_t count, int *out_status)
{
	if (url == NULL || url[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	char body[64];
	snprintf(body, sizeof(body), "{\"count\":%u}", (unsigned)count);

	esp_http_client_config_t cfg = {
		.url = url,
		.timeout_ms = 10000,
		.keep_alive_enable = true,
		.disable_auto_redirect = false,
	};

	esp_http_client_handle_t client = esp_http_client_init(&cfg);
	if (client == NULL) {
		return ESP_FAIL;
	}

	esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_POST);
	if (err != ESP_OK) {
		esp_http_client_cleanup(client);
		return err;
	}

	(void)esp_http_client_set_header(client, "Content-Type", "application/json");
	err = esp_http_client_set_post_field(client, body, (int)strlen(body));
	if (err != ESP_OK) {
		esp_http_client_cleanup(client);
		return err;
	}

	/* Open with write_len so the library knows how much we're sending */
	err = esp_http_client_open(client, (int)strlen(body));
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "POST open failed: %s", esp_err_to_name(err));
		esp_http_client_cleanup(client);
		return err;
	}

	int w = esp_http_client_write(client, body, (int)strlen(body));
	if (w < 0) {
		ESP_LOGW(TAG, "POST write failed (%d)", w);
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ESP_FAIL;
	}

	(void)esp_http_client_fetch_headers(client);
	int status = esp_http_client_get_status_code(client);
	if (out_status) {
		*out_status = status;
	}
	ESP_LOGI(TAG, "POST count=%u -> status=%d", (unsigned)count, status);

	/* Drain response body (optional) */
	char resp[64];
	while (esp_http_client_read(client, resp, sizeof(resp)) > 0) {
		;
	}

	esp_http_client_close(client);
	esp_http_client_cleanup(client);
	return ESP_OK;
}

static void http_count_task(void *pv)
{
	const char *url = (const char *)pv;
	uint32_t count = 0;
	while (1) {
		int status = -1;
		esp_err_t err = http_post_count(url, count++, &status);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "POST count failed: %s (status=%d)", esp_err_to_name(err), status);
		}
		vTaskDelay(pdMS_TO_TICKS(EXAMPLE_COUNT_PERIOD_MS));
	}
}

static void http_led_task(void *pv)
{
	const char *url = (const char *)pv;
	bool last_known = false;
	bool has_state = false;

	while (1) {
		char body[256];
		int status = -1;
		esp_err_t err = http_get_body(url, body, sizeof(body), &status);
		if (err == ESP_OK && status >= 200 && status < 300) {
			bool on = false;
			if (parse_led_state(body, &on)) {
				if (!has_state || on != last_known) {
					ESP_LOGI(TAG, "LED -> %s (body='%s')", on ? "ON" : "OFF", body);
				}
				led_set(on);
				last_known = on;
				has_state = true;
			} else {
				ESP_LOGW(TAG, "Unrecognized body='%s' (expect on/off)", body);
			}
		} else {
			ESP_LOGW(TAG, "HTTP status=%d, err=%s", status, esp_err_to_name(err));
		}

		vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_PERIOD_MS));
	}
}

void app_main(void)
{
	ESP_LOGI(TAG, "Starting HTTP LED demo");
	/* Show DEBUG logs for this component (optional). */
	esp_log_level_set(TAG, ESP_LOG_DEBUG);

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	led_init();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(example_connect());

	ESP_LOGI(TAG, "Polling URL: %s", CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL);
	xTaskCreate(http_led_task, "http_led_task", 4096, (void *)CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL, 5, NULL);
	ESP_LOGI(TAG, "Posting count to: %s", EXAMPLE_COUNT_URL);
	xTaskCreate(http_count_task, "http_count_task", 4096, (void *)EXAMPLE_COUNT_URL, 5, NULL);
}

