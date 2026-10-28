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


typedef enum {
    JKBMS_ERROR_NONE = 0,
    JKBMS_ERROR_NOT_FOUND,       // not advertising: off, out of range or the phone app is connected
    JKBMS_ERROR_CONNECT,         // the BLE connection failed
    JKBMS_ERROR_NOT_JK,          // connected, but the device has no JK BMS service
    JKBMS_ERROR_NO_DATA,         // connected, but the BMS does not send data
} jkbms_error_t;


typedef struct {
    jkbms_link_t link;
    jkbms_error_t error;         // why the link is not streaming; cleared once data arrives
    uint8_t mac[6];
    bool mac_known;
    int rssi;
    jkbms_proto_t proto;
    bool info_valid;
    jkbms_device_info_t info;
    int64_t info_time_s;         // board time(NULL) when `info` was received; runs through deep sleep
    bool cells_valid;
    jkbms_cell_info_t cells;
    int64_t cells_updated_us;    // esp_timer_get_time() of the last cell info frame
} jkbms_status_t;


/**
 * The settings and the logbook, read on demand into a buffer of the caller
 * (memory is short: nothing keeps them), see jkbms_ble_fetch_extras().
 */
typedef struct {
    bool want_settings;          // set by the caller, cleared when the settings arrived
    bool want_logbook;
    bool settings_valid;
    jkbms_settings_t settings;
    bool logbook_valid;
    uint32_t logbook_total;      // number of events stored in the BMS
    uint8_t logbook_count;       // number of events in `logbook`, oldest first
    uint32_t logbook_uptime_s;   // BMS uptime when the logbook was read, the time base of its events
    int64_t logbook_read_us;     // esp_timer_get_time() then
    jkbms_log_entry_t logbook[JKBMS_LOGBOOK_FRAME_ENTRIES];
} jkbms_extras_t;


typedef struct {
    uint8_t mac[6];
    char name[24];
    int rssi;
    int64_t seen_us;
} jkbms_found_t;


typedef struct {
    uint8_t mac[6];
    char name[24];
    int rssi;
    uint8_t addr_type;
    bool jk;                     // recognized as a JK BMS
    uint32_t count;              // advertisements received
    int64_t seen_us;
} jkbms_seen_t;


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
 * Disconnects the BMS and turns Bluetooth off for good, freeing its memory
 * and radio time for a firmware update. Only a restart turns it on again.
 */
void jkbms_ble_shutdown(void);

/**
 * Keeps scanning for JK BMS advertisers even when all assigned devices
 * are connected, so that jkbms_ble_get_found() stays up to date.
 */
void jkbms_ble_set_discovery(bool enable);


/**
 * Reconnects when the data is older than `period_s` (default: as soon as
 * the stream stops; 0: never). The BMS stops streaming about 30 s after
 * connecting and beeps on every new connection.
 */
void jkbms_ble_set_refresh_period_s(int period_s);

/**
 * Lists JK BMS advertisers seen within max_age_us, strongest signal first.
 */
int jkbms_ble_get_found(jkbms_found_t *list, int max, int64_t max_age_us);

/**
 * Reads the settings and/or the logbook (want_settings, want_logbook) of a
 * BMS into `extras`, which must stay valid until both arrived (the want_
 * flags are cleared, the _valid flags set) or jkbms_ble_cancel_extras().
 * Each is requested once per connection while the BMS streams; it beeps on
 * every request. False when two other requests are pending already.
 */
bool jkbms_ble_fetch_extras(int idx, jkbms_extras_t *extras);

/**
 * Forgets a request of jkbms_ble_fetch_extras(): `extras` can be freed then.
 */
void jkbms_ble_cancel_extras(jkbms_extras_t *extras);

/**
 * Assigns a MAC address to an unassigned slot, saves it to NVS so that it is
 * restored on the next boot, and starts connecting to it.
 */
bool jkbms_ble_assign(int idx, const uint8_t mac[6]);

/**
 * Replaces the BMS of a slot, whether assigned or not, and saves the choice.
 * A NULL or all-zero MAC unassigns the slot.
 */
bool jkbms_ble_set_mac(int idx, const uint8_t mac[6]);

/**
 * Lists every BLE advertiser seen within max_age_us (JK BMS or not),
 * strongest signal first. For diagnostics.
 */
int jkbms_ble_get_seen(jkbms_seen_t *list, int max, int64_t max_age_us);

/**
 * Erases the BMS selection saved by jkbms_ble_assign().
 */
void jkbms_ble_forget_saved(void);

/**
 * Disconnects all BMS and makes their slots unassigned (and forgets their data).
 */
void jkbms_ble_unassign_all(void);

/**
 * Current BMS uptime in seconds (the time base of the logbook), projected from
 * the device info and the board clock. 0 if the device info is unknown.
 */
uint32_t jkbms_bms_now_s(const jkbms_status_t *status);

const char* jkbms_link_name(jkbms_link_t link);


#ifdef __cplusplus
}
#endif

#endif /* COMPONENTS_JKBMS_JKBMS_BLE_H_ */
