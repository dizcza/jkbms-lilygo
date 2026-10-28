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
#include "esp_app_format.h"
#include "esp_err.h"

#define WEBSERVER_HOSTNAME  "espbms"     // mDNS: http://espbms.local/

// the image header, the first segment header and the app description
#define WEBSERVER_IMAGE_HEADER_SIZE \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

esp_err_t webserver_start(void);

/**
 * True while a firmware upload is being written.
 */
bool webserver_ota_active(void);

/**
 * Takes the right to write a firmware, so that a web upload and a download
 * from the dashboard (remote.c) never run at once. False when taken.
 */
bool webserver_ota_claim(void);

void webserver_ota_release(void);

/**
 * Checks the first WEBSERVER_IMAGE_HEADER_SIZE bytes of a firmware file.
 * Returns NULL for an app image of this project and chip, an error otherwise.
 */
const char* webserver_check_image(const char *data);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_WEBSERVER_H_ */
