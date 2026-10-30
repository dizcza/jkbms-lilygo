/*
 * remote.h
 *
 * Diagnostics of the board for the dashboard, and the switch between the two
 * OTA slots with the right button. The firmware is updated only from the
 * board's own update page (webserver.c); the dashboard sends no commands.
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
 * Keeps a firmware chosen with remote_boot_previous() (the bootloader would
 * roll it back like a failed update). Call after nvs_flash_init().
 */
void remote_boot_check(void);

/**
 * Restarts into the firmware in the other OTA slot, unless it is missing,
 * broken or was rolled back; returns the reason on screen-ready text then.
 */
const char* remote_boot_previous(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_REMOTE_H_ */
