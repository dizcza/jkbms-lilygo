/*
 * report.h
 *
 * Status of all assigned BMS as JSON for the dashboard (web/app.py). The
 * settings and the logbook go in separate, smaller reports: memory is short.
 */

#ifndef MAIN_REPORT_H_
#define MAIN_REPORT_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "jkbms_ble.h"

/**
 * The status report: the board, and per BMS its device info and cell data.
 * A JSON string to be freed with free(), NULL without memory.
 */
char* report_build(uint32_t wake);

/**
 * A partial report with the settings of one BMS, NULL when not assigned.
 */
char* report_build_settings(int slot, const jkbms_settings_t *settings);

/**
 * A partial report with up to 10 logbook events of one BMS that were not
 * delivered yet, oldest first; NULL when there are none. Call
 * report_logbook_sent() once delivered, then again for the next ones.
 */
char* report_build_logbook(int slot, const jkbms_extras_t *extras);

void report_logbook_sent(int slot);

/**
 * A copy of the status report `json` with its age, for a report sent later
 * than it was built; the dashboard dates its values back. Free it with free().
 */
char* report_add_age(const char *json, uint32_t age_s);

/**
 * Forgets which logbook events were delivered: they are all sent again.
 */
void report_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_REPORT_H_ */
