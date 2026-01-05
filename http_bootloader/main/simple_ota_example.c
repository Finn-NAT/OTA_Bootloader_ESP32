
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

#define EXAMPLE_POLL_PERIOD_MS 1000

#define EXAMPLE_COUNT_PERIOD_MS 1000

#define EXAMPLE_STATUS_PERIOD_MS 500

/* This project treats CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL as a *base URL*:
 *   http://<ip>:<port>
 * Endpoints are appended in code:
 *   GET  <base>/led
 *   POST <base>/count
 */

#define EXAMPLE_LED_PATH "/led"
#define EXAMPLE_COUNT_PATH "/count"
#define EXAMPLE_STATUS_PATH "/status"

/* Bootloader/app status endpoint. If you want a different path, change it here. */
#define BOOTLOADER_STATUS_PATH EXAMPLE_STATUS_PATH

static void build_url(char *out, size_t out_size, const char *base, const char *path)
{
	if (out == NULL || out_size == 0) {
		return;
	}
	out[0] = 0;
	if (base == NULL || path == NULL) {
		return;
	}

	size_t blen = strlen(base);
	bool base_has_slash = (blen > 0 && base[blen - 1] == '/');
	bool path_has_slash = (path[0] == '/');

	if (base_has_slash && path_has_slash) {
		/* avoid double slash */
		snprintf(out, out_size, "%.*s%s", (int)(blen - 1), base, path);
	} else if (!base_has_slash && !path_has_slash) {
		/* ensure single slash */
		snprintf(out, out_size, "%s/%s", base, path);
	} else {
		snprintf(out, out_size, "%s%s", base, path);
	}
}

typedef enum {
	OTA_BOOTLOADER_ERROR = -1,
	OTA_BOOTLOADER_RUNNING = 0,
	OTA_FIRMWARE_RUNNING = 1,
} ota_bootloader_state_t;

static esp_err_t http_post_status(const char *url, ota_bootloader_state_t status, int *out_status)
{
	if (url == NULL || url[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	char body[64];
	snprintf(body, sizeof(body), "{\"bootloader_status\":%u}", (unsigned)status);

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
	int http_status = esp_http_client_get_status_code(client);
	if (out_status) {
		*out_status = http_status;
	}
	ESP_LOGI(TAG, "POST bootloader_status=%u -> http=%d", (unsigned)status, http_status);

	/* Drain response body (optional) */
	char resp[64];
	while (esp_http_client_read(client, resp, sizeof(resp)) > 0) {
		;
	}

	esp_http_client_close(client);
	esp_http_client_cleanup(client);

	return ESP_OK;
}

// static void SetFirmwareStatus(ota_bootloader_state_t status)
// {
// 	const char *base = (const char *)pv;
// 	char url[192];
// 	build_url(url, sizeof(url), base, EXAMPLE_STATUS_PATH);
// 	ESP_LOGI(TAG, "Status endpoint: %s", url);

// 	while (1) {
// 		int http_status = -1;
// 		/* Report that firmware/app is running. Change this based on your state machine if needed. */
// 		esp_err_t err = http_post_status(url, OTA_FIRMWARE_RUNNING, &http_status);
// 		if (err != ESP_OK || http_status < 200 || http_status >= 300) {
// 			ESP_LOGW(TAG, "POST status failed: %s (http=%d)", esp_err_to_name(err), http_status);
// 		}
// 		vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_PERIOD_MS));
// 	}
// }


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

static void http_status_task(void *pv)
{
	const char *base = (const char *)pv;
	char url[192];
	build_url(url, sizeof(url), base, EXAMPLE_STATUS_PATH);
	ESP_LOGI(TAG, "Status endpoint: %s", url);

	while (1) {
		int http_status = -1;
		/* Report that firmware/app is running. Change this based on your state machine if needed. */
		esp_err_t err = http_post_status(url, OTA_FIRMWARE_RUNNING, &http_status);
		if (err != ESP_OK || http_status < 200 || http_status >= 300) {
			ESP_LOGW(TAG, "POST status failed: %s (http=%d)", esp_err_to_name(err), http_status);
		}
		vTaskDelay(pdMS_TO_TICKS(EXAMPLE_STATUS_PERIOD_MS));
	}
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
	ESP_LOGI(TAG, "HTTP status=%d, content_length=%lld \n", status, (long long)content_length);

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

	ESP_LOGI(TAG, "Base URL: %s", CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL);
	xTaskCreate(http_status_task, "http_status_task", 4096, (void *)CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL, 5, NULL);
}

