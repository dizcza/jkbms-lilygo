/*
 * uploader.h
 *
 * Keeps the board on a known Wi-Fi network and POSTs the status JSON to the
 * dashboard.
 */

#ifndef MAIN_UPLOADER_H_
#define MAIN_UPLOADER_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * Starts Wi-Fi and keeps it connected in the background. `on_connected`
 * (optional) is called after each successful join.
 */
esp_err_t uploader_start(void (*on_connected)(void));

/**
 * POSTs `json` to ENV_JKBMS_UPLOAD_URL and copies the reply (the dashboard's
 * commands) to `resp` when given. Returns ESP_ERR_INVALID_STATE right away
 * without Wi-Fi.
 */
esp_err_t uploader_send(const char *json, char *resp, size_t resp_size);

/**
 * POSTs `body` to `url` with the API token; the reply goes to `resp` when given.
 */
esp_err_t uploader_post(const char *url, const char *content_type, const char *body, size_t len,
                        char *resp, size_t resp_size);

/**
 * URL of another dashboard endpoint next to the upload URL, e.g. "firmware"
 * for http://host/api/firmware. False without an upload URL.
 */
bool uploader_api_url(const char *name, char *url, size_t url_size);

/**
 * Signal strength of the joined network in dBm, 0 when offline.
 */
int uploader_wifi_rssi(void);

/**
 * True while connected to a Wi-Fi network with an IP address.
 */
bool uploader_connected(void);

/**
 * Copies the joined network name and the IP address. False when offline.
 */
bool uploader_get_network(char *ssid, size_t ssid_size, char *ip, size_t ip_size);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UPLOADER_H_ */
