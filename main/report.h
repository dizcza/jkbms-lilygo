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
 * Returns a JSON string to be freed with free(). Settings and the logbook
 * are included only when `with_extras` is set.
 */
char* report_build(uint32_t wake, bool with_extras);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_REPORT_H_ */
