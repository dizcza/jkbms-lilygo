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
 * POSTs `json` to ENV_JKBMS_UPLOAD_URL. Fails right away without Wi-Fi.
 */
esp_err_t uploader_send(const char *json);

/**
 * Copies the joined network name and the IP address. False when offline.
 */
bool uploader_get_network(char *ssid, size_t ssid_size, char *ip, size_t ip_size);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UPLOADER_H_ */
