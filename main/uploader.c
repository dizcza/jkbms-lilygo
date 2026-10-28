/*
 * uploader.c
 *
 * Wi-Fi networks come from build-time secrets: the end user's networks first,
 * then the developer's home networks. Like esp32-sdpsensor, the networks are
 * tried one by one and the one that worked is saved to NVS and tried first
 * next time. Wi-Fi stays connected; a background task rejoins after a drop.
 */

#include "uploader.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#define WIFI_CONNECT_TIMEOUT_MS   10000
#define NVS_NAMESPACE             "jkbms"
#define NVS_KEY_NETWORK           "wifi_net"
#define HTTP_TIMEOUT_MS           5000
#define RETRY_DELAY_MS            30000

#define BIT_CONNECTED   BIT0
#define BIT_FAILED      BIT1

typedef struct {
    const char *ssid;
    const char *password;
} wifi_network_t;

static const wifi_network_t m_networks[] = {
#ifdef ENV_JKBMS_WIFI_SSID
    { ENV_JKBMS_WIFI_SSID, ENV_JKBMS_WIFI_PASS },
#endif
#ifdef ENV_JKBMS_WIFI_SSID1
    { ENV_JKBMS_WIFI_SSID1, ENV_JKBMS_WIFI_PASS1 },
#endif
#ifdef ENV_WIFI_STA_SSID2
    { ENV_WIFI_STA_SSID2, ENV_WIFI_STA_PASS2 },
#endif
};
#define NETWORKS_COUNT  (sizeof(m_networks) / sizeof(m_networks[0]))

static EventGroupHandle_t m_events;
static int m_network = -1;      // index of the joined network
static char m_ip[16];
static void (*m_on_connected)(void);
static const char *TAG = "upload";


static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(m_events, BIT_CONNECTED);
        xEventGroupSetBits(m_events, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        snprintf(m_ip, sizeof(m_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(m_events, BIT_CONNECTED);
    }
}


static bool wifi_join(int network) {
    wifi_config_t config = { 0 };
    strlcpy((char*) config.sta.ssid, m_networks[network].ssid, sizeof(config.sta.ssid));
    strlcpy((char*) config.sta.password, m_networks[network].password, sizeof(config.sta.password));
    xEventGroupClearBits(m_events, BIT_CONNECTED | BIT_FAILED);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_connect();
    const EventBits_t bits = xEventGroupWaitBits(m_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & BIT_CONNECTED) {
        m_network = network;
        ESP_LOGI(TAG, "Connected to '%s', IP %s", m_networks[network].ssid, m_ip);
        return true;
    }
    esp_wifi_disconnect();
    return false;
}


static int nvs_load_network(void) {
    nvs_handle_t handle;
    uint8_t network = UINT8_MAX;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, NVS_KEY_NETWORK, &network);
        nvs_close(handle);
    }
    return network < NETWORKS_COUNT ? network : -1;
}


static void nvs_save_network(int network) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_NETWORK, (uint8_t) network);
        nvs_commit(handle);
        nvs_close(handle);
    }
}


/**
 * Tries the saved network first, then all networks in order.
 */
static bool wifi_join_any(void) {
    const int saved = nvs_load_network();
    if (saved >= 0 && wifi_join(saved)) {
        return true;
    }
    for (int n = 0; n < NETWORKS_COUNT; n++) {
        if (n == saved) {
            continue;
        }
        ESP_LOGI(TAG, "Trying '%s'", m_networks[n].ssid);
        if (wifi_join(n)) {
            nvs_save_network(n);
            return true;
        }
    }
    ESP_LOGW(TAG, "None of %d Wi-Fi networks is reachable", (int) NETWORKS_COUNT);
    return false;
}


/**
 * Keeps Wi-Fi connected: joins a network, waits for a drop, joins again.
 */
static void wifi_task(void *args) {
    while (1) {
        if (!wifi_join_any()) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        if (m_on_connected != NULL) {
            m_on_connected();
        }
        xEventGroupWaitBits(m_events, BIT_FAILED, pdFALSE, pdFALSE, portMAX_DELAY);
        ESP_LOGW(TAG, "Wi-Fi disconnected");
        m_network = -1;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}


esp_err_t uploader_start(void (*on_connected)(void)) {
    if (NETWORKS_COUNT == 0) {
        ESP_LOGW(TAG, "No Wi-Fi network configured");
        return ESP_ERR_INVALID_STATE;
    }
    m_on_connected = on_connected;
    m_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    if (xTaskCreate(wifi_task, "wifi", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


bool uploader_connected(void) {
    return m_events != NULL && (xEventGroupGetBits(m_events) & BIT_CONNECTED);
}


bool uploader_get_network(char *ssid, size_t ssid_size, char *ip, size_t ip_size) {
    const int network = m_network;
    if (m_events == NULL || network < 0 || !(xEventGroupGetBits(m_events) & BIT_CONNECTED)) {
        return false;
    }
    strlcpy(ssid, m_networks[network].ssid, ssid_size);
    strlcpy(ip, m_ip, ip_size);
    return true;
}


int uploader_wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (!uploader_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}


bool uploader_api_url(const char *name, char *url, size_t url_size) {
#ifdef ENV_JKBMS_UPLOAD_URL
    // the upload URL ends with /api/status: the other endpoints are next to it
    const char *slash = strrchr(ENV_JKBMS_UPLOAD_URL, '/');
    if (slash == NULL) {
        return false;
    }
    snprintf(url, url_size, "%.*s/%s", (int) (slash - ENV_JKBMS_UPLOAD_URL), ENV_JKBMS_UPLOAD_URL, name);
    return true;
#else
    return false;
#endif
}


esp_err_t uploader_post(const char *url, const char *content_type, const char *body, size_t len,
                        char *resp, size_t resp_size) {
#ifndef ENV_JKBMS_API_TOKEN
    ESP_LOGW(TAG, "No API token, upload skipped");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (resp != NULL && resp_size > 0) {
        resp[0] = '\0';
    }
    if (!uploader_connected()) {
        ESP_LOGW(TAG, "Wi-Fi is not connected, upload skipped");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Authorization", "Bearer " ENV_JKBMS_API_TOKEN);
    int status = -1;
    esp_err_t err = esp_http_client_open(client, len);
    size_t written = 0;
    while (err == ESP_OK && written < len) {
        const int n = esp_http_client_write(client, body + written, len - written);
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        written += n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (resp != NULL && resp_size > 1) {
            const int n = esp_http_client_read_response(client, resp, resp_size - 1);
            resp[n > 0 ? n : 0] = '\0';
        }
        if (status != 200) {
            err = ESP_FAIL;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "POST %u bytes to %s: %s, HTTP %d", (unsigned) len, url, esp_err_to_name(err), status);
    return err;
#endif
}


esp_err_t uploader_send(const char *json, char *resp, size_t resp_size) {
#ifndef ENV_JKBMS_UPLOAD_URL
    ESP_LOGW(TAG, "No upload URL, upload skipped");
    return ESP_ERR_NOT_SUPPORTED;
#else
    return uploader_post(ENV_JKBMS_UPLOAD_URL, "application/json", json, strlen(json), resp, resp_size);
#endif
}
