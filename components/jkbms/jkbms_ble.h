/*
 * jkbms_ble.h
 *
 * BLE central that connects to up to CONFIG_JKBMS_NUM_DEVICES JK BMS units
 * and keeps their latest device and cell info.
 */

#ifndef COMPONENTS_JKBMS_JKBMS_BLE_H_
#define COMPONENTS_JKBMS_JKBMS_BLE_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "jkbms_proto.h"

#define JKBMS_DEVICES_MAX   CONFIG_JKBMS_NUM_DEVICES


typedef enum {
    JKBMS_LINK_UNASSIGNED = 0,   // no MAC configured or selected yet
    JKBMS_LINK_SEARCHING,        // waiting for the device to show up in a scan
    JKBMS_LINK_CONNECTING,
    JKBMS_LINK_CONNECTED,        // GATT link up, waiting for data
    JKBMS_LINK_STREAMING,        // cell info frames are arriving
} jkbms_link_t;


typedef struct {
    jkbms_link_t link;
    uint8_t mac[6];
    bool mac_known;
    int rssi;
    jkbms_proto_t proto;
    bool info_valid;
    jkbms_device_info_t info;
    bool cells_valid;
    jkbms_cell_info_t cells;
    int64_t cells_updated_us;    // esp_timer_get_time() of the last cell info frame
    bool settings_valid;
    jkbms_settings_t settings;
    bool logbook_valid;
    uint32_t logbook_total;      // number of events stored in the BMS
    uint8_t logbook_count;       // number of events in `logbook`, oldest first
    jkbms_log_entry_t logbook[JKBMS_LOGBOOK_FRAME_ENTRIES];
} jkbms_status_t;


typedef struct {
    uint8_t mac[6];
    char name[24];
    int rssi;
    int64_t seen_us;
} jkbms_found_t;


/**
 * Initializes NVS-independent BLE stack parts and starts scanning.
 * NVS must be initialized beforehand.
 */
esp_err_t jkbms_ble_init(void);

/**
 * Copies the latest status of the BMS with the given index.
 */
bool jkbms_ble_get_status(int idx, jkbms_status_t *status);

/**
 * Keeps scanning for JK BMS advertisers even when all assigned devices
 * are connected, so that jkbms_ble_get_found() stays up to date.
 */
void jkbms_ble_set_discovery(bool enable);

/**
 * Lists JK BMS advertisers seen within max_age_us, strongest signal first.
 */
int jkbms_ble_get_found(jkbms_found_t *list, int max, int64_t max_age_us);

/**
 * Asks the BMS for its event logbook on the next data exchange.
 */
void jkbms_ble_request_logbook(int idx);

/**
 * Assigns a MAC address to an unassigned slot, saves it to NVS so that it is
 * restored on the next boot, and starts connecting to it.
 */
bool jkbms_ble_assign(int idx, const uint8_t mac[6]);

/**
 * Erases the BMS selection saved by jkbms_ble_assign().
 */
void jkbms_ble_forget_saved(void);

const char* jkbms_link_name(jkbms_link_t link);


#ifdef __cplusplus
}
#endif

#endif /* COMPONENTS_JKBMS_JKBMS_BLE_H_ */
