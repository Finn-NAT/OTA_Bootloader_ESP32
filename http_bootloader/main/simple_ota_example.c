
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

#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"

#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "driver/gpio.h"

#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "http_led";

#ifndef HASH_LEN
#define HASH_LEN 32
#endif

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

#define OTA_URL_SIZE 256

#define X_FIRMWARE_UPGRADE_URL "http://192.168.5.95:8070/firmware/app.bin"

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void simple_ota_example_task(void)
{
	while (1) {
    ESP_LOGI(TAG, "Starting OTA example task");
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
    esp_netif_t *netif = get_example_netif_from_desc(bind_interface_name);
    if (netif == NULL) {
        ESP_LOGE(TAG, "Can't find netif from interface description");
        abort();
    }
    struct ifreq ifr;
    esp_netif_get_netif_impl_name(netif, ifr.ifr_name);
    ESP_LOGI(TAG, "Bind interface name is %s", ifr.ifr_name);
#endif
    esp_http_client_config_t config = {
        .url = X_FIRMWARE_UPGRADE_URL,
#ifdef CONFIG_EXAMPLE_USE_CERT_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#else
        .cert_pem = (char *)server_cert_pem_start,
#endif /* CONFIG_EXAMPLE_USE_CERT_BUNDLE */
        .event_handler = _http_event_handler,
        .keep_alive_enable = true,
#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_BIND_IF
        .if_name = &ifr,
#endif
#if CONFIG_EXAMPLE_TLS_DYN_BUF_RX_STATIC
        /* This part applies static buffer strategy for rx dynamic buffer.
         * This is to avoid frequent allocation and deallocation of dynamic buffer.
         */
        .tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC,
#endif /* CONFIG_EXAMPLE_TLS_DYN_BUF_RX_STATIC */
    };

#ifdef CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL_FROM_STDIN
    char url_buf[OTA_URL_SIZE];
    if (strcmp(config.url, "FROM_STDIN") == 0) {
        example_configure_stdin_stdout();
        fgets(url_buf, OTA_URL_SIZE, stdin);
        int len = strlen(url_buf);
        url_buf[len - 1] = '\0';
        config.url = url_buf;
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong firmware upgrade image url");
        abort();
    }
#endif

#ifdef CONFIG_EXAMPLE_SKIP_COMMON_NAME_CHECK
    config.skip_cert_common_name_check = true;
#endif

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Firmware upgrade failed");
    }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

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

#define EXAMPLE_STATUS_PERIOD_MS 1000

/* This project treats CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL as a *base URL*:
 *   http://<ip>:<port>
 * Endpoints are appended in code:
 *   GET  <base>/led
 *   POST <base>/count
 */

#define EXAMPLE_UPDATE_PATH "/update"
#define EXAMPLE_STATUS_PATH "/status"

/* Bootloader/app status endpoint. If you want a different path, change it here. */
#define BOOTLOADER_STATUS_PATH EXAMPLE_STATUS_PATH

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

ota_bootloader_state_t current_bootloader_state = OTA_FIRMWARE_RUNNING;

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

static void http_status_task(void *pv)
{
	const char *base = (const char *)pv;
	char url[192];
	build_url(url, sizeof(url), base, EXAMPLE_STATUS_PATH);
	ESP_LOGI(TAG, "Status endpoint: %s", url);

	while (1) {
		int http_status = -1;
		/* Report that firmware/app is running. Change this based on your state machine if needed. */
		esp_err_t err = http_post_status(url, current_bootloader_state, &http_status);
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

static bool parse_update_bootloader_status(const char *body, ota_bootloader_state_t *out_state)
{
	if (body == NULL || out_state == NULL) {
		return false;
	}

	const char *key = "\"bootloader_status\"";
	const char *p = strstr(body, key);
	if (p == NULL) {
		return false;
	}
	p += strlen(key);

	/* Skip whitespace, ':' and other separators */
	while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) {
		p++;
	}

	/* Optional quote (in case server sends it as a string) */
	bool quoted = false;
	if (*p == '"') {
		quoted = true;
		p++;
	}

	char *endptr = NULL;
	long v = strtol(p, &endptr, 10);
	if (endptr == p) {
		return false; /* no digits */
	}

	if (quoted) {
		while (*endptr && (*endptr == ' ' || *endptr == '\t')) {
			endptr++;
		}
		if (*endptr != '"') {
			return false;
		}
	}

	if (v == -1) {
		*out_state = OTA_BOOTLOADER_ERROR;
		return true;
	}
	if (v == 0) {
		*out_state = OTA_BOOTLOADER_RUNNING;
		return true;
	}
	if (v == 1) {
		*out_state = OTA_FIRMWARE_RUNNING;
		return true;
	}

	return false;
}

static bool http_get_update_bootloader_status(void *pv)
{
	const char *base = (const char *)pv;
	char url[192];
	build_url(url, sizeof(url), base, EXAMPLE_STATUS_PATH);
	ESP_LOGI(TAG, "Status endpoint: %s", url);

	while (1) {
		char body[256];
		int status = -1;
		esp_err_t err = http_get_body(url, body, sizeof(body), &status);
		if (err == ESP_OK && status >= 200 && status < 300) {
			ota_bootloader_state_t state;
			if (parse_update_bootloader_status(body, &state)) {
				if (state == OTA_BOOTLOADER_RUNNING) {
					printf("Double checked: bootloader is running (body='%s')\n", body);
					return true;
				}
			} else {
				ESP_LOGW(TAG, "Unrecognized body='%s' (expect on/off)", body);
			}
		} else {
			ESP_LOGW(TAG, "HTTP status=%d, err=%s", status, esp_err_to_name(err));
		}
		return false;
	}
}

/* Parses response body. Accepts:
 * - "on" / "off" (case-insensitive)
 * - "1" / "0"
 * - JSON-like: {"update":"on"} or {"on":true} (naive substring match)
 */
static bool parse_update_firmware_state(const char *body, bool *out_on)
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
	if (strstr(tmp, "\"on\"") || strstr(tmp, "update:on") || strstr(tmp, "\"update\":\"on\"")) {
		*out_on = true;
		return true;
	}
	if (strstr(tmp, "\"off\"") || strstr(tmp, "update:off") || strstr(tmp, "\"update\":\"off\"")) {
		*out_on = false;
		return true;
	}

	return false;
}

static void http_update_firmware_task(void *pv)
{
	bool one_time = false;
	const char *base = (const char *)pv;
	char url[192];
	build_url(url, sizeof(url), base, EXAMPLE_UPDATE_PATH);
	ESP_LOGI(TAG, "Status endpoint: %s", url);

	while (1) {
		char body[256];
		int status = -1;
		esp_err_t err = http_get_body(url, body, sizeof(body), &status);
		if (err == ESP_OK && status >= 200 && status < 300) {
			bool on_update = false;
			if (parse_update_firmware_state(body, &on_update)) {
				if (on_update && !one_time) {
					ESP_LOGI(TAG, "UPDATE_FIRMWARE -> %s (body='%s')", on_update ? "ON" : "OFF", body);
					current_bootloader_state = OTA_BOOTLOADER_RUNNING;
					while (!http_get_update_bootloader_status((void *)CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL)) {
						ESP_LOGI(TAG, "Waiting for bootloader to take over...");
						vTaskDelay(pdMS_TO_TICKS(1000));
					}
					led_set(on_update);
					printf("Bootloader has taken over. LED is now %s\n", on_update ? "ON" : "OFF");
					one_time = true;
					simple_ota_example_task();
				}
			} else {
				ESP_LOGW(TAG, "Unrecognized body='%s' (expect on/off)", body);
			}
		} else {
			ESP_LOGW(TAG, "HTTP status=%d, err=%s", status, esp_err_to_name(err));
		}

		vTaskDelay(pdMS_TO_TICKS(EXAMPLE_POLL_PERIOD_MS));
	}
}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s %s", label, hash_print);
}

static void get_sha256_of_partitions(void)
{
    uint8_t sha_256[HASH_LEN] = { 0 };
    esp_partition_t partition;

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");
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
	led_set(false);

	get_sha256_of_partitions();

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(example_connect());

	ESP_LOGI(TAG, "Base URL: %s", CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL);
	xTaskCreate(http_status_task, "http_status_task", 4096, (void *)CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL, 5, NULL);
	xTaskCreate(http_update_firmware_task, "http_update_firmware_task", 4096, (void *)CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL, 5, NULL);
}

