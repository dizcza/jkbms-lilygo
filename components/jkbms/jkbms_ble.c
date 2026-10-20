/*
 * jkbms_ble.c
 *
 * Bluedroid GATT client for JK BMS (service 0xFFE0, characteristic 0xFFE1).
 * Connection flow: connect -> enable notifications -> a BMS that streamed a
 * moment ago continues by itself; otherwise device info (0x97) followed by
 * cell info (0x96) starts the stream (0x96 alone does not). The BMS beeps on
 * every command, so the settings and the logbook are requested only when
 * asked for, at most once per connection.
 */

#include "jkbms_ble.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "nvs.h"
#include <sys/param.h>


#define JKBMS_SERVICE_UUID          0xFFE0
#define JKBMS_CHARACTERISTIC_UUID   0xFFE1
#define JKBMS_COMPANY_ID            0x0B65
#define JKBMS_LOCAL_MTU             517
#define JKBMS_APP_ID                0

#define SCAN_DURATION_S             10
#define CONNECT_TIMEOUT_US          (40 * 1000000LL)
#define PASSIVE_WAIT_US             (2 * 1000000LL)     // the BMS often streams without a request
#define INFO_REQUEST_ATTEMPTS       2
#define REQUEST_PERIOD_US           (3 * 1000000LL)
#define SETTINGS_WAIT_US            (3 * 1000000LL)     // settings often come by themselves
#define STREAM_STALL_US             (3 * 1000000LL)
#define LINK_DEAD_US                (60 * 1000000LL)
#define NOT_FOUND_US                (15 * 1000000LL)
#define NO_DATA_US                  (20 * 1000000LL)
#define FOUND_MAX                   8
#define NVS_NAMESPACE               "jkbms"


typedef struct {
    jkbms_status_t st;                 // shared with jkbms_ble_get_status()
    esp_ble_addr_type_t addr_type;
    uint16_t conn_id;
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t write_handle;
    uint16_t notify_handle;
    bool notify_ready;
    int64_t notify_ready_us;
    uint8_t info_requests;
    bool info_this_link;               // device info received on this connection
    bool settings_requested;           // on this connection
    bool logbook_requested;            // on this connection
    bool logbook_wanted;
    int64_t link_since_us;
    int64_t last_request_us;
    jkbms_assembler_t assembler;
} jkbms_slot_t;


static jkbms_slot_t m_slots[JKBMS_DEVICES_MAX];
static SemaphoreHandle_t m_lock = NULL;
static esp_gatt_if_t m_gattc_if = ESP_GATT_IF_NONE;
static bool m_scan_params_set = false;
static bool m_scanning = false;
static int m_connecting_idx = -1;
static bool m_discovery = false;
static bool m_fetch_extras = true;
static int64_t m_refresh_us = STREAM_STALL_US;   // 0: never reconnect for fresh data

// device info survives deep sleep, so that it is requested once per power-on
typedef struct {
    bool valid;
    uint8_t mac[6];
    jkbms_proto_t proto;
    jkbms_device_info_t info;
    int64_t info_time_s;
} info_cache_t;

static RTC_DATA_ATTR info_cache_t m_info_cache[JKBMS_DEVICES_MAX];
static jkbms_found_t m_found[FOUND_MAX];
static const char *TAG = "jkbms";

static esp_ble_scan_params_t m_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30,
    .scan_duplicate         = BLE_SCAN_DUPLICATE_DISABLE
};


#define MACSTR_BDA "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC2STR_BDA(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]


static void slot_set_link(jkbms_slot_t *slot, jkbms_link_t link) {
    slot->st.link = link;
    slot->link_since_us = esp_timer_get_time();
}


static jkbms_slot_t* slot_by_conn_id(uint16_t conn_id) {
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_slot_t *slot = &m_slots[i];
        if (slot->st.link >= JKBMS_LINK_CONNECTED && slot->conn_id == conn_id) {
            return slot;
        }
    }
    return NULL;
}


static jkbms_slot_t* slot_by_bda(const uint8_t *bda) {
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_slot_t *slot = &m_slots[i];
        if (slot->st.mac_known && memcmp(slot->st.mac, bda, 6) == 0) {
            return slot;
        }
    }
    return NULL;
}


static bool parse_mac(const char *str, uint8_t mac[6]) {
    unsigned int b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t) b[i];
    }
    return true;
}


static void slot_reset_link(jkbms_slot_t *slot);


/*---------------------------------------------------------------
        Persistent BMS selection
---------------------------------------------------------------*/

static void nvs_mac_key(int idx, char key[8]) {
    snprintf(key, 8, "mac%d", idx);
}


static bool nvs_load_mac(int idx, uint8_t mac[6]) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char key[8];
    nvs_mac_key(idx, key);
    size_t size = 6;
    const bool ok = nvs_get_blob(handle, key, mac, &size) == ESP_OK && size == 6;
    nvs_close(handle);
    return ok;
}


static void nvs_save_mac(int idx, const uint8_t mac[6]) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        char key[8];
        nvs_mac_key(idx, key);
        err = nvs_set_blob(handle, key, mac, 6);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[BMS %d] saving MAC failed: %s", idx + 1, esp_err_to_name(err));
    }
}


void jkbms_ble_forget_saved(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        // the namespace holds other settings too: erase the MACs only
        for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
            char key[8];
            nvs_mac_key(i, key);
            nvs_erase_key(handle, key);
        }
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGI(TAG, "saved BMS selection erased");
}


void jkbms_ble_unassign_all(void) {
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_slot_t *slot = &m_slots[i];
        if (slot->st.link >= JKBMS_LINK_CONNECTING) {
            esp_ble_gap_disconnect(slot->st.mac);
        }
        slot_reset_link(slot);
        memset(&slot->st, 0, sizeof(slot->st));
        slot_set_link(slot, JKBMS_LINK_UNASSIGNED);
    }
    m_connecting_idx = -1;
    xSemaphoreGiveRecursive(m_lock);
}


static void slot_send_command(jkbms_slot_t *slot, uint8_t command) {
    uint8_t frame[JKBMS_COMMAND_SIZE];
    jkbms_build_command(command, frame);
    esp_err_t err = esp_ble_gattc_write_char(m_gattc_if, slot->conn_id, slot->write_handle, sizeof(frame), frame,
                                             ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[" MACSTR_BDA "] write 0x%02X failed: %s", MAC2STR_BDA(slot->st.mac), command, esp_err_to_name(err));
    }
    slot->last_request_us = esp_timer_get_time();
}


/**
 * Resets the GATT session of a slot. The device info and the last cell info
 * are kept so that the UI can still show them as stale.
 */
static void slot_reset_link(jkbms_slot_t *slot) {
    // Bluedroid drops the notification registration together with the link
    slot->svc_start = slot->svc_end = 0;
    slot->write_handle = slot->notify_handle = 0;
    slot->notify_ready = false;
    slot->info_requests = 0;
    slot->info_this_link = false;
    slot->settings_requested = false;
    slot->logbook_requested = false;
    slot->assembler.len = 0;
    slot_set_link(slot, slot->st.mac_known ? JKBMS_LINK_SEARCHING : JKBMS_LINK_UNASSIGNED);
}


/*---------------------------------------------------------------
        GAP: scanning
---------------------------------------------------------------*/

static bool adv_is_jkbms(uint8_t *adv, uint16_t adv_len, char *name, size_t name_size) {
    uint8_t len = 0;
    uint8_t *data = esp_ble_resolve_adv_data_by_type(adv, adv_len, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    if (data == NULL) {
        data = esp_ble_resolve_adv_data_by_type(adv, adv_len, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
    }
    name[0] = '\0';
    if (data != NULL) {
        len = (len < name_size - 1) ? len : name_size - 1;
        memcpy(name, data, len);
        name[len] = '\0';
    }
    if (name[0] != '\0' && strncmp(name, CONFIG_JKBMS_NAME_PREFIX, strlen(CONFIG_JKBMS_NAME_PREFIX)) == 0) {
        return true;
    }
    // manufacturer data with JK company ID 0x0B65, followed by 0x88 0xA0 and the MAC
    data = esp_ble_resolve_adv_data_by_type(adv, adv_len, ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &len);
    if (data != NULL && len >= 2 && (data[0] | (data[1] << 8)) == JKBMS_COMPANY_ID) {
        return true;
    }
    const esp_ble_adv_data_type uuid_types[] = { ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_BLE_AD_TYPE_16SRV_PART };
    for (int t = 0; t < 2; t++) {
        data = esp_ble_resolve_adv_data_by_type(adv, adv_len, uuid_types[t], &len);
        for (int i = 0; data != NULL && i + 1 < len; i += 2) {
            if ((data[i] | (data[i + 1] << 8)) == JKBMS_SERVICE_UUID) {
                return true;
            }
        }
    }
    return false;
}


/**
 * Returns the index of the slot that should connect to the advertiser or -1.
 */
static int slot_for_advertiser(const uint8_t *bda) {
    jkbms_slot_t *slot = slot_by_bda(bda);
    if (slot != NULL && slot->st.link == JKBMS_LINK_SEARCHING) {
        return (int) (slot - m_slots);
    }
    return -1;
}


static void found_update(const uint8_t *bda, const char *name, int rssi) {
    jkbms_found_t *entry = NULL;
    jkbms_found_t *oldest = &m_found[0];
    for (int i = 0; i < FOUND_MAX; i++) {
        if (m_found[i].seen_us != 0 && memcmp(m_found[i].mac, bda, 6) == 0) {
            entry = &m_found[i];
            break;
        }
        if (m_found[i].seen_us < oldest->seen_us) {
            oldest = &m_found[i];
        }
    }
    if (entry == NULL) {
        entry = oldest;
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->mac, bda, 6);
        ESP_LOGI(TAG, "JK BMS candidate " MACSTR_BDA " '%s' rssi %d dBm", MAC2STR_BDA(bda), name, rssi);
    }
    if (name[0] != '\0') {
        strlcpy(entry->name, name, sizeof(entry->name));
    }
    entry->rssi = rssi;
    entry->seen_us = esp_timer_get_time();
}


static void connect_slot(int idx, esp_ble_gap_cb_param_t *param) {
    jkbms_slot_t *slot = &m_slots[idx];
    memcpy(slot->st.mac, param->scan_rst.bda, 6);
    slot->st.mac_known = true;
    slot->st.rssi = param->scan_rst.rssi;
    slot->addr_type = param->scan_rst.ble_addr_type;
    slot_set_link(slot, JKBMS_LINK_CONNECTING);
    m_connecting_idx = idx;

    if (m_scanning) {
        esp_ble_gap_stop_scanning();
        m_scanning = false;
    }

    esp_ble_gatt_creat_conn_params_t conn_params = { 0 };
    memcpy(conn_params.remote_bda, slot->st.mac, 6);
    conn_params.remote_addr_type = slot->addr_type;
    conn_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    conn_params.is_direct = true;
    conn_params.is_aux = false;
    conn_params.phy_mask = 0x0;
    esp_err_t err = esp_ble_gattc_enh_open(m_gattc_if, &conn_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[BMS %d] open failed: %s", idx + 1, esp_err_to_name(err));
        m_connecting_idx = -1;
        slot_reset_link(slot);
    }
}


static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        m_scan_params_set = (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS);
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Scan start failed, status 0x%x", param->scan_start_cmpl.status);
            m_scanning = false;
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        m_scanning = false;
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            m_scanning = false;
            break;
        }
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || m_connecting_idx >= 0) {
            break;
        }
        char name[32];
        const uint16_t adv_len = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
        if (adv_is_jkbms(param->scan_rst.ble_adv, adv_len, name, sizeof(name))) {
            found_update(param->scan_rst.bda, name, param->scan_rst.rssi);
        }
        const int idx = slot_for_advertiser(param->scan_rst.bda);
        if (idx >= 0) {
            ESP_LOGI(TAG, "[BMS %d] found " MACSTR_BDA " '%s' rssi %d dBm, connecting", idx + 1,
                     MAC2STR_BDA(param->scan_rst.bda), name, param->scan_rst.rssi);
            connect_slot(idx, param);
        }
        break;
    }
    default:
        break;
    }
    xSemaphoreGiveRecursive(m_lock);
}


/*---------------------------------------------------------------
        GATT client
---------------------------------------------------------------*/

static void on_search_complete(jkbms_slot_t *slot) {
    const int idx = slot - m_slots;
    if (slot->svc_start == 0) {
        ESP_LOGW(TAG, "[BMS %d] " MACSTR_BDA " has no 0xFFE0 service, not a JK BMS", idx + 1, MAC2STR_BDA(slot->st.mac));
        slot->st.error = JKBMS_ERROR_NOT_JK;
        esp_ble_gap_disconnect(slot->st.mac);
        return;
    }

    esp_gattc_char_elem_t chars[4];
    uint16_t count = sizeof(chars) / sizeof(chars[0]);
    esp_bt_uuid_t char_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = JKBMS_CHARACTERISTIC_UUID };
    esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(m_gattc_if, slot->conn_id, slot->svc_start,
                                                              slot->svc_end, char_uuid, chars, &count);
    if (status != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "[BMS %d] characteristic 0xFFE1 not found", idx + 1);
        slot->st.error = JKBMS_ERROR_NOT_JK;
        esp_ble_gap_disconnect(slot->st.mac);
        return;
    }

    // Old BLE module: a single 0xFFE1 characteristic for write and notify.
    // New BLE module: two 0xFFE1 characteristics (handle 0x03 write, 0x05 notify).
    for (int i = 0; i < count; i++) {
        const esp_gatt_char_prop_t prop = chars[i].properties;
        if (slot->write_handle == 0 && (prop & (ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_WRITE))) {
            slot->write_handle = chars[i].char_handle;
        }
        if (slot->notify_handle == 0 && (prop & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
            slot->notify_handle = chars[i].char_handle;
        }
    }
    if (slot->write_handle == 0) {
        slot->write_handle = chars[0].char_handle;
    }
    if (slot->notify_handle == 0) {
        slot->notify_handle = chars[count - 1].char_handle;
    }
    ESP_LOGI(TAG, "[BMS %d] write handle 0x%x, notify handle 0x%x", idx + 1, slot->write_handle, slot->notify_handle);

    esp_ble_gattc_register_for_notify(m_gattc_if, slot->st.mac, slot->notify_handle);

    esp_gattc_descr_elem_t descr;
    count = 1;
    esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG };
    status = esp_ble_gattc_get_descr_by_char_handle(m_gattc_if, slot->conn_id, slot->notify_handle, cccd_uuid,
                                                    &descr, &count);
    if (status == ESP_GATT_OK && count > 0) {
        uint8_t notify_en[2] = { 0x01, 0x00 };
        esp_ble_gattc_write_char_descr(m_gattc_if, slot->conn_id, descr.handle, sizeof(notify_en), notify_en,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    } else {
        // no CCCD: notifications are always on
        slot->notify_ready = true;
        slot->notify_ready_us = esp_timer_get_time();
    }
}


static void on_frame(jkbms_slot_t *slot, const uint8_t *frame) {
    const int idx = slot - m_slots;
    switch (frame[4]) {
    case JKBMS_FRAME_TYPE_DEVICE_INFO:
        jkbms_decode_device_info(frame, &slot->st.info);
        slot->st.info_valid = true;
        slot->st.info_time_s = time(NULL);
        slot->info_this_link = true;
        slot->st.proto = jkbms_proto_from_device_info(&slot->st.info);
        m_info_cache[idx] = (info_cache_t) { .valid = true, .proto = slot->st.proto, .info = slot->st.info,
                                             .info_time_s = slot->st.info_time_s };
        memcpy(m_info_cache[idx].mac, slot->st.mac, 6);
        if (slot->st.link != JKBMS_LINK_STREAMING) {
            slot_send_command(slot, JKBMS_COMMAND_CELL_INFO);
        }
        ESP_LOGI(TAG, "[BMS %d] %s '%s' hw %s sw %s, power on count %lu, protocol %s", idx + 1,
                 slot->st.info.model, slot->st.info.name, slot->st.info.hw_version, slot->st.info.sw_version,
                 (unsigned long) slot->st.info.power_on_count, jkbms_proto_name(slot->st.proto));
        break;
    case JKBMS_FRAME_TYPE_CELL_INFO:
        if (slot->st.proto == JKBMS_PROTO_UNKNOWN) {
            slot->st.proto = jkbms_proto_guess(frame);
            ESP_LOGW(TAG, "[BMS %d] no device info, guessed protocol %s", idx + 1, jkbms_proto_name(slot->st.proto));
        }
        jkbms_decode_cell_info(frame, slot->st.proto, &slot->st.cells);
        slot->st.cells_valid = true;
        slot->st.cells_updated_us = esp_timer_get_time();
        if (slot->st.link != JKBMS_LINK_STREAMING) {
            ESP_LOGI(TAG, "[BMS %d] streaming: %u cells, %.2f V", idx + 1, slot->st.cells.cell_count,
                     slot->st.cells.total_voltage);
            slot_set_link(slot, JKBMS_LINK_STREAMING);
            slot->st.error = JKBMS_ERROR_NONE;
        }
        break;
    case JKBMS_FRAME_TYPE_LOGBOOK:
        slot->st.logbook_count = jkbms_decode_logbook(frame, &slot->st.logbook_total, slot->st.logbook,
                                                      JKBMS_LOGBOOK_FRAME_ENTRIES);
        slot->st.logbook_valid = true;
        slot->logbook_wanted = false;
        ESP_LOGI(TAG, "[BMS %d] logbook: %u of %lu events", idx + 1, slot->st.logbook_count,
                 (unsigned long) slot->st.logbook_total);
        break;
    case JKBMS_FRAME_TYPE_SETTINGS: {
        jkbms_proto_t proto = slot->st.proto;
        if (proto == JKBMS_PROTO_UNKNOWN) {
            // the settings frame arrives before the first cell info frame
            proto = JKBMS_PROTO_JK02_32S;
        }
        slot->st.settings_valid = jkbms_decode_settings(frame, proto, &slot->st.settings);
        ESP_LOGI(TAG, "[BMS %d] settings: %u cells, %.0f Ah, UVP %.3f V, discharge %s", idx + 1,
                 slot->st.settings.cell_count, slot->st.settings.capacity, slot->st.settings.cell_uvp,
                 slot->st.settings.discharge_enabled ? "on" : "off");
        break;
    }
    default:
        ESP_LOGD(TAG, "[BMS %d] unsupported frame type 0x%02X", idx + 1, frame[4]);
        break;
    }
}


static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            m_gattc_if = gattc_if;
            esp_ble_gap_set_scan_params(&m_scan_params);
        } else {
            ESP_LOGE(TAG, "GATTC app register failed, status %d", param->reg.status);
        }
        return;
    }
    if (gattc_if != m_gattc_if) {
        return;
    }

    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    jkbms_slot_t *slot = NULL;
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        slot = slot_by_bda(param->open.remote_bda);
        if (slot == NULL) {
            break;
        }
        if (slot - m_slots == m_connecting_idx) {
            m_connecting_idx = -1;
        }
        if (param->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "[BMS %d] connect failed, status 0x%x", (int) (slot - m_slots) + 1, param->open.status);
            slot->st.error = JKBMS_ERROR_CONNECT;
            slot_reset_link(slot);
            break;
        }
        ESP_LOGI(TAG, "[BMS %d] connected, conn_id %u", (int) (slot - m_slots) + 1, param->open.conn_id);
        slot->conn_id = param->open.conn_id;
        slot_set_link(slot, JKBMS_LINK_CONNECTED);
        const int idx = slot - m_slots;
        if (!slot->st.info_valid && m_info_cache[idx].valid && memcmp(m_info_cache[idx].mac, slot->st.mac, 6) == 0) {
            slot->st.info = m_info_cache[idx].info;
            slot->st.info_time_s = m_info_cache[idx].info_time_s;
            slot->st.proto = m_info_cache[idx].proto;
            slot->st.info_valid = true;
        }
        // JK BMS notifications are longer than the default 23-byte MTU allows
        esp_ble_gattc_send_mtu_req(m_gattc_if, slot->conn_id);
        esp_bt_uuid_t svc_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = JKBMS_SERVICE_UUID };
        esp_ble_gattc_search_service(m_gattc_if, slot->conn_id, &svc_uuid);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        slot = slot_by_conn_id(param->cfg_mtu.conn_id);
        if (slot != NULL) {
            ESP_LOGI(TAG, "[BMS %d] MTU %u, status %d", (int) (slot - m_slots) + 1, param->cfg_mtu.mtu,
                     param->cfg_mtu.status);
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        slot = slot_by_conn_id(param->search_res.conn_id);
        if (slot != NULL && param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16
                && param->search_res.srvc_id.uuid.uuid.uuid16 == JKBMS_SERVICE_UUID) {
            slot->svc_start = param->search_res.start_handle;
            slot->svc_end = param->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        slot = slot_by_conn_id(param->search_cmpl.conn_id);
        if (slot != NULL) {
            on_search_complete(slot);
        }
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "register for notify failed, handle 0x%x, status %d", param->reg_for_notify.handle,
                     param->reg_for_notify.status);
        }
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        slot = slot_by_conn_id(param->write.conn_id);
        if (slot == NULL) {
            break;
        }
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "[BMS %d] enabling notifications failed, status %d", (int) (slot - m_slots) + 1,
                     param->write.status);
        }
        slot->notify_ready = true;
        slot->notify_ready_us = esp_timer_get_time();
        break;
    case ESP_GATTC_NOTIFY_EVT: {
        slot = slot_by_conn_id(param->notify.conn_id);
        if (slot == NULL || param->notify.handle != slot->notify_handle) {
            break;
        }
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->notify.value, param->notify.value_len, ESP_LOG_VERBOSE);
        const uint8_t *frame = jkbms_assemble(&slot->assembler, param->notify.value, param->notify.value_len);
        if (frame != NULL) {
            on_frame(slot, frame);
        }
        break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
        slot = slot_by_bda(param->disconnect.remote_bda);
        if (slot != NULL) {
            ESP_LOGW(TAG, "[BMS %d] disconnected, reason 0x%x", (int) (slot - m_slots) + 1, param->disconnect.reason);
            if (slot - m_slots == m_connecting_idx) {
                m_connecting_idx = -1;
            }
            slot_reset_link(slot);
        }
        break;
    default:
        break;
    }
    xSemaphoreGiveRecursive(m_lock);
}


/*---------------------------------------------------------------
        Supervisor: scanning, requests and timeouts
---------------------------------------------------------------*/

/**
 * Requests the device info, settings and logbook once the stream runs, each
 * at most once per connection and only if missing or asked for.
 */
static void slot_request_extras(jkbms_slot_t *slot, int64_t now) {
    if (now - slot->last_request_us < REQUEST_PERIOD_US) {
        return;
    }
    // the logbook needs the current BMS uptime from the device info
    const bool need_info = !slot->st.info_valid
            || (!slot->info_this_link && (slot->logbook_wanted || (m_fetch_extras && !slot->st.logbook_valid)));
    if (need_info && slot->info_requests < INFO_REQUEST_ATTEMPTS) {
        slot_send_command(slot, JKBMS_COMMAND_DEVICE_INFO);
        slot->info_requests++;
    } else if (m_fetch_extras && !slot->st.settings_valid && !slot->settings_requested
               && now - slot->link_since_us > SETTINGS_WAIT_US) {
        // the BMS sends its settings frame when (re)starting the stream
        slot_send_command(slot, JKBMS_COMMAND_CELL_INFO);
        slot->settings_requested = true;
    } else if ((slot->logbook_wanted || (m_fetch_extras && !slot->st.logbook_valid)) && !slot->logbook_requested
               && slot->info_this_link) {
        slot_send_command(slot, JKBMS_COMMAND_LOGBOOK);
        slot->logbook_requested = true;
    }
}


static void jkbms_supervisor_task(void *args) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        bool searching = false;
        for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
            jkbms_slot_t *slot = &m_slots[i];
            const int64_t in_state = now - slot->link_since_us;
            const int64_t since_request = now - slot->last_request_us;
            switch (slot->st.link) {
            case JKBMS_LINK_UNASSIGNED:
                break;
            case JKBMS_LINK_SEARCHING:
                searching = true;
                if (in_state > NOT_FOUND_US) {
                    slot->st.error = JKBMS_ERROR_NOT_FOUND;
                }
                break;
            case JKBMS_LINK_CONNECTING:
                // Bluedroid reports a failed OPEN_EVT after its own timeout; this is a safety net
                if (in_state > CONNECT_TIMEOUT_US) {
                    ESP_LOGW(TAG, "[BMS %d] connect timeout", i + 1);
                    slot->st.error = JKBMS_ERROR_CONNECT;
                    esp_ble_gap_disconnect(slot->st.mac);
                    m_connecting_idx = -1;
                    slot_reset_link(slot);
                }
                break;
            case JKBMS_LINK_CONNECTED:
                if (in_state > NO_DATA_US && slot->st.error == JKBMS_ERROR_NONE) {
                    slot->st.error = JKBMS_ERROR_NO_DATA;
                }
                if (in_state > LINK_DEAD_US) {
                    ESP_LOGW(TAG, "[BMS %d] no data, reconnecting", i + 1);
                    esp_ble_gap_disconnect(slot->st.mac);
                } else if (!slot->notify_ready || now - slot->notify_ready_us < PASSIVE_WAIT_US) {
                    // waiting for service discovery, then for the BMS to stream by itself
                } else if (since_request < REQUEST_PERIOD_US) {
                    // waiting for a response
                } else if (!slot->info_this_link && slot->info_requests < INFO_REQUEST_ATTEMPTS) {
                    // 0x96 starts the stream only after 0x97
                    slot_send_command(slot, JKBMS_COMMAND_DEVICE_INFO);
                    slot->info_requests++;
                } else {
                    slot_send_command(slot, JKBMS_COMMAND_CELL_INFO);
                }
                break;
            case JKBMS_LINK_STREAMING:
                slot_request_extras(slot, now);
                // The BMS stops streaming about 30 s after connecting and ignores
                // further requests; a fresh connection restarts the stream.
                if (m_refresh_us > 0 && now - slot->st.cells_updated_us > MAX(m_refresh_us, STREAM_STALL_US)) {
                    ESP_LOGI(TAG, "[BMS %d] reconnecting for fresh data", i + 1);
                    esp_ble_gap_disconnect(slot->st.mac);
                    slot_set_link(slot, JKBMS_LINK_CONNECTED);
                }
                break;
            }
        }
        if ((searching || m_discovery) && m_scan_params_set && !m_scanning && m_connecting_idx < 0) {
            if (esp_ble_gap_start_scanning(SCAN_DURATION_S) == ESP_OK) {
                m_scanning = true;
            }
        }
        xSemaphoreGiveRecursive(m_lock);
    }
}


esp_err_t jkbms_ble_init(void) {
    m_lock = xSemaphoreCreateRecursiveMutex();
    if (m_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *macs[] = {
        CONFIG_JKBMS_MAC_1,
#if CONFIG_JKBMS_NUM_DEVICES >= 2
        CONFIG_JKBMS_MAC_2,
#endif
#if CONFIG_JKBMS_NUM_DEVICES >= 3
        CONFIG_JKBMS_MAC_3,
#endif
    };
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_slot_t *slot = &m_slots[i];
        memset(slot, 0, sizeof(*slot));
        if (macs[i][0] != '\0') {
            if (parse_mac(macs[i], slot->st.mac)) {
                slot->st.mac_known = true;
                ESP_LOGI(TAG, "[BMS %d] MAC " MACSTR_BDA, i + 1, MAC2STR_BDA(slot->st.mac));
            } else {
                ESP_LOGE(TAG, "[BMS %d] invalid MAC '%s'", i + 1, macs[i]);
            }
        } else if (nvs_load_mac(i, slot->st.mac)) {
            slot->st.mac_known = true;
            ESP_LOGI(TAG, "[BMS %d] saved MAC " MACSTR_BDA, i + 1, MAC2STR_BDA(slot->st.mac));
        }
        slot_set_link(slot, slot->st.mac_known ? JKBMS_LINK_SEARCHING : JKBMS_LINK_UNASSIGNED);
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(JKBMS_LOCAL_MTU));
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(JKBMS_APP_ID));

    if (xTaskCreate(jkbms_supervisor_task, "jkbms", 3072, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}


bool jkbms_ble_get_status(int idx, jkbms_status_t *status) {
    if (idx < 0 || idx >= JKBMS_DEVICES_MAX || m_lock == NULL) {
        return false;
    }
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    memcpy(status, &m_slots[idx].st, sizeof(*status));
    xSemaphoreGiveRecursive(m_lock);
    return true;
}


void jkbms_ble_set_refresh_period_s(int period_s) {
    m_refresh_us = period_s * 1000000LL;
}


void jkbms_ble_set_fetch_extras(bool enable) {
    m_fetch_extras = enable;
}


void jkbms_ble_set_discovery(bool enable) {
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    m_discovery = enable;
    xSemaphoreGiveRecursive(m_lock);
}


int jkbms_ble_get_found(jkbms_found_t *list, int max, int64_t max_age_us) {
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    int count = 0;
    for (int i = 0; i < FOUND_MAX && count < max; i++) {
        if (m_found[i].seen_us != 0 && now - m_found[i].seen_us <= max_age_us) {
            list[count++] = m_found[i];
        }
    }
    xSemaphoreGiveRecursive(m_lock);
    // strongest signal first
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && list[j].rssi > list[j - 1].rssi; j--) {
            jkbms_found_t tmp = list[j];
            list[j] = list[j - 1];
            list[j - 1] = tmp;
        }
    }
    return count;
}


void jkbms_ble_request_logbook(int idx) {
    if (idx < 0 || idx >= JKBMS_DEVICES_MAX) {
        return;
    }
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    jkbms_slot_t *slot = &m_slots[idx];
    slot->logbook_wanted = true;
    slot->logbook_requested = false;
    xSemaphoreGiveRecursive(m_lock);
}


bool jkbms_ble_assign(int idx, const uint8_t mac[6]) {
    if (idx < 0 || idx >= JKBMS_DEVICES_MAX) {
        return false;
    }
    xSemaphoreTakeRecursive(m_lock, portMAX_DELAY);
    jkbms_slot_t *slot = &m_slots[idx];
    const bool ok = (slot->st.link == JKBMS_LINK_UNASSIGNED);
    if (ok) {
        memcpy(slot->st.mac, mac, 6);
        slot->st.mac_known = true;
        slot_set_link(slot, JKBMS_LINK_SEARCHING);
        nvs_save_mac(idx, mac);
        ESP_LOGI(TAG, "[BMS %d] assigned " MACSTR_BDA, idx + 1, MAC2STR_BDA(mac));
    }
    xSemaphoreGiveRecursive(m_lock);
    return ok;
}


uint32_t jkbms_bms_now_s(const jkbms_status_t *status) {
    if (!status->info_valid) {
        return 0;
    }
    const int64_t elapsed = time(NULL) - status->info_time_s;
    return status->info.uptime_s + (elapsed > 0 ? (uint32_t) elapsed : 0);
}


const char* jkbms_link_name(jkbms_link_t link) {
    switch (link) {
    case JKBMS_LINK_UNASSIGNED:
        return "unassigned";
    case JKBMS_LINK_SEARCHING:
        return "searching";
    case JKBMS_LINK_CONNECTING:
        return "connecting";
    case JKBMS_LINK_CONNECTED:
        return "connected";
    case JKBMS_LINK_STREAMING:
        return "online";
    default:
        return "?";
    }
}
