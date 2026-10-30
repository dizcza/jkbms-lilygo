/*
 * remote.c
 */

#include "remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "jkbms_ble.h"
#include "uploader.h"
#include "webserver.h"

#define NVS_NAMESPACE       "jkbms"
#define NVS_KEY_SWITCHED    "switched"      // the right button chose the running firmware
#define SEEN_MAX_AGE_US     (5 * 60 * 1000000LL)
#define SEEN_REPORTED       6

extern const char *const build_time;   // generated, see main/build_time.cmake

static const char *TAG = "remote";




/*---------------------------------------------------------------
        Diagnostics
---------------------------------------------------------------*/

static const char* reset_reason_name(esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "other";
    }
}


static void add_mac(cJSON *object, const char *name, const uint8_t *mac) {
    char str[18];
    snprintf(str, sizeof(str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(object, name, str);
}


void remote_add_diag(cJSON *root) {
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[65];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    cJSON *fw = cJSON_AddObjectToObject(root, "fw");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "built", build_time);
    cJSON_AddStringToObject(fw, "elf", elf);
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    cJSON_AddStringToObject(fw, "partition", esp_ota_get_running_partition()->label);

    cJSON *diag = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddNumberToObject(diag, "uptime", (double) (esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(diag, "reset", reset_reason_name(esp_reset_reason()));
    cJSON_AddNumberToObject(diag, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(diag, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(diag, "heap_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(diag, "wifi_rssi", uploader_wifi_rssi());

    static jkbms_status_t st;  // large: keep it off the stack
    bool streaming = false;
    cJSON *slots = cJSON_AddArrayToObject(diag, "slots");
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        if (!jkbms_ble_get_status(i, &st)) {
            continue;
        }
        cJSON *slot = cJSON_CreateObject();
        cJSON_AddNumberToObject(slot, "slot", i + 1);
        cJSON_AddStringToObject(slot, "link", jkbms_link_name(st.link));
        streaming |= st.link == JKBMS_LINK_STREAMING;
        cJSON_AddNumberToObject(slot, "error", st.error);
        if (st.mac_known) {
            add_mac(slot, "mac", st.mac);
            cJSON_AddNumberToObject(slot, "rssi", st.rssi);
        }
        cJSON_AddItemToArray(slots, slot);
    }

    if (streaming) {
        return;  // the BLE list helps to find a BMS; memory is short while one streams
    }
    static jkbms_seen_t seen[SEEN_REPORTED];
    const int n = jkbms_ble_get_seen(seen, SEEN_REPORTED, SEEN_MAX_AGE_US);
    const int64_t now = esp_timer_get_time();
    cJSON *list = cJSON_AddArrayToObject(diag, "ble");
    for (int i = 0; i < n; i++) {
        cJSON *dev = cJSON_CreateObject();
        add_mac(dev, "mac", seen[i].mac);
        cJSON_AddStringToObject(dev, "name", seen[i].name);
        cJSON_AddNumberToObject(dev, "rssi", seen[i].rssi);
        cJSON_AddBoolToObject(dev, "jk", seen[i].jk);
        cJSON_AddNumberToObject(dev, "addr_type", seen[i].addr_type);
        cJSON_AddNumberToObject(dev, "count", seen[i].count);
        cJSON_AddNumberToObject(dev, "age", (double) ((now - seen[i].seen_us) / 1000000));
        cJSON_AddItemToArray(list, dev);
    }
}


void remote_boot_check(void) {
    nvs_handle_t handle;
    uint8_t switched = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_get_u8(handle, NVS_KEY_SWITCHED, &switched);
    if (switched) {
        // chosen by hand, not a new update: no rollback when it restarts before reaching the dashboard
        esp_ota_mark_app_valid_cancel_rollback();
        nvs_erase_key(handle, NVS_KEY_SWITCHED);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Firmware chosen with the button: kept");
    }
    nvs_close(handle);
}


const char* remote_boot_previous(void) {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (other == NULL || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        ESP_LOGW(TAG, "No previous firmware");
        return "No previous\nfirmware";
    }
    if (esp_ota_get_state_partition(other, &state) == ESP_OK
            && (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED)) {
        ESP_LOGW(TAG, "Previous firmware %s on %s was rolled back", desc.version, other->label);
        return "Previous firmware\ndid not work";
    }
    if (webserver_ota_active()) {
        return "Firmware update\nis running";
    }
    // checks the whole image: a slot half-written by a failed update is refused
    const esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Previous firmware on %s is broken: %s", other->label, esp_err_to_name(err));
        return "Previous firmware\nis broken";
    }
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_SWITCHED, 1);
        nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGW(TAG, "Restarting into the previous firmware %s on %s", desc.version, other->label);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return NULL;
}
