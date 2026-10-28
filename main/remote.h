/*
 * remote.h
 *
 * Remote maintenance through the dashboard (web/app.py), for a board that
 * nobody near it can debug or flash: the reply to each status upload may
 * carry a command (assign a BMS, reboot) and the firmware the board should
 * run, which it then downloads from /api/firmware into the other OTA slot.
 */

#ifndef MAIN_REMOTE_H_
#define MAIN_REMOTE_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include "cJSON.h"

/**
 * Adds the firmware, board and BLE diagnostics to the status report.
 */
void remote_add_diag(cJSON *root);

/**
 * Acts on the dashboard's reply to a status upload: a command, a firmware
 * update. Blocks while a firmware update downloads and
 * restarts the board when it succeeds.
 */
void remote_on_reply(const char *reply);

/**
 * Keeps a firmware chosen with remote_boot_previous() (the bootloader would
 * roll it back like a failed update). Call after nvs_flash_init().
 */
void remote_boot_check(void);

/**
 * Restarts into the firmware in the other OTA slot, unless it is missing,
 * broken or was rolled back; returns the reason on screen-ready text then.
 */
const char* remote_boot_previous(void);

/**
 * True while the firmware is being downloaded.
 */
bool remote_ota_active(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_REMOTE_H_ */
