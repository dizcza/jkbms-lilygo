/*
 * webserver.h
 *
 * Firmware update page on port 80 at http://espbms.local/ (mDNS), borrowed
 * from esp32-sdpsensor:
 *   GET  /                 the update page with an upload button and progress bar
 *   GET  /info             running firmware as JSON
 *   GET  /progress         bytes of the running upload written so far, as JSON
 *   POST /upload_firmware  the raw .bin, written to the other OTA slot
 */

#ifndef MAIN_WEBSERVER_H_
#define MAIN_WEBSERVER_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"

#define WEBSERVER_HOSTNAME  "espbms"     // mDNS: http://espbms.local/

esp_err_t webserver_start(void);

/**
 * True while a firmware upload is being written.
 */
bool webserver_ota_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_WEBSERVER_H_ */
