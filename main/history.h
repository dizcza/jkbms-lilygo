/*
 * history.h
 *
 * BMS readings recorded to a flash ring buffer (the "history" partition), so
 * that a drained battery can be inspected after the fact without Wi-Fi.
 */

#ifndef MAIN_HISTORY_H_
#define MAIN_HISTORY_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jkbms_ble.h"

#define HISTORY_FLAG_CHARGING     0x01
#define HISTORY_FLAG_DISCHARGING  0x02
#define HISTORY_FLAG_BALANCING    0x04

typedef struct __attribute__((packed)) {
    uint32_t seq;              // increasing record number
    uint32_t time_s;           // BMS uptime, see jkbms_bms_now_s()
    uint16_t voltage_cv;       // pack voltage, 10 mV
    int16_t current_ca;        // 10 mA, negative = discharging
    uint8_t slot;              // BMS index
    uint8_t soc;               // %
    uint8_t flags;             // HISTORY_FLAG_*
    uint8_t min_cell;          // 1-based
    uint8_t max_cell;          // 1-based
    uint8_t reserved;
    uint16_t min_mv;
    uint16_t max_mv;
    uint32_t errors;           // BMS alarm bits
    uint8_t padding[6];
} history_record_t;

/**
 * Returns false to stop the iteration.
 */
typedef bool (*history_cb_t)(const history_record_t *record, void *ctx);

esp_err_t history_init(void);

/**
 * Records the current cell info of the BMS in `slot`.
 */
void history_add(int slot, const jkbms_status_t *status);

/**
 * Calls `callback` for the records, newest first.
 */
void history_foreach(history_cb_t callback, void *ctx);

/**
 * Sequence number of the newest record, 0 if there is none.
 */
uint32_t history_last_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_HISTORY_H_ */
