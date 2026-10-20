/*
 * uploader.h
 *
 * Connects to a known Wi-Fi network and POSTs the status JSON to the dashboard.
 */

#ifndef MAIN_UPLOADER_H_
#define MAIN_UPLOADER_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include "esp_err.h"

/**
 * Connects to Wi-Fi, POSTs `json` to ENV_JKBMS_UPLOAD_URL and stops Wi-Fi.
 */
esp_err_t uploader_send(const char *json);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UPLOADER_H_ */
