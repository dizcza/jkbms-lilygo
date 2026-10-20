/*
 * report.h
 *
 * Status of all assigned BMS as JSON for the dashboard (web/app.py).
 */

#ifndef MAIN_REPORT_H_
#define MAIN_REPORT_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * Returns a JSON string to be freed with free(). Settings are included only
 * when `with_extras` is set; logbook events whenever there are new ones.
 */
char* report_build(uint32_t wake, bool with_extras);

/**
 * The last built report was delivered: its logbook events are not sent again.
 */
void report_mark_sent(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_REPORT_H_ */
