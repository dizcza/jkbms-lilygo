/*
 * remote.c
 */

#include "remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "jkbms_ble.h"
#include "ui.h"
#include "uploader.h"
#include "webserver.h"

#define NVS_NAMESPACE       "jkbms"
#define NVS_KEY_CMD_ID      "cmd_id"
#define NVS_KEY_OTA_ELF     "ota_elf"       // the firmware being tried and how often
#define NVS_KEY_OTA_TRIES   "ota_tries"
#define NVS_KEY_SWITCHED    "switched"      // the right button chose the running firmware
#define SEEN_MAX_AGE_US     (5 * 60 * 1000000LL)
#define SEEN_REPORTED       6
#define OTA_ATTEMPTS        3                   // per firmware, across restarts
#define OTA_TIMEOUT_MS      15000
#define OTA_CHUNK           2048
#define OTA_MIN_BLOCK       (12 * 1024)         // the HTTP client and the chunk buffer must fit

extern const char *const build_time;   // generated, see main/build_time.cmake

static const char *TAG = "remote";


static uint32_t m_cmd_id;               // last executed command, saved in NVS
static char m_ota_error[64];
static volatile bool m_ota_active;


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


static void nvs_load_cmd_id(void);


void remote_add_diag(cJSON *root) {
    nvs_load_cmd_id();
    const esp_app_desc_t *app = esp_app_get_description();
    char elf[65];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    cJSON *fw = cJSON_AddObjectToObject(root, "fw");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "built", build_time);
    cJSON_AddStringToObject(fw, "elf", elf);
    cJSON_AddStringToObject(fw, "idf", app->idf_ver);
    cJSON_AddStringToObject(fw, "partition", esp_ota_get_running_partition()->label);
    if (m_ota_error[0] != '\0') {
        cJSON_AddStringToObject(fw, "ota_error", m_ota_error);
    }

    cJSON *diag = cJSON_AddObjectToObject(root, "diag");
    cJSON_AddNumberToObject(diag, "uptime", (double) (esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(diag, "reset", reset_reason_name(esp_reset_reason()));
    cJSON_AddNumberToObject(diag, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(diag, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(diag, "heap_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(diag, "wifi_rssi", uploader_wifi_rssi());
    cJSON_AddNumberToObject(diag, "cmd_id", m_cmd_id);

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


/*---------------------------------------------------------------
        Commands
---------------------------------------------------------------*/

static void nvs_save_cmd_id(uint32_t id) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, NVS_KEY_CMD_ID, id);
        nvs_commit(handle);
        nvs_close(handle);
    }
}


static void nvs_load_cmd_id(void) {
    static bool loaded = false;
    nvs_handle_t handle;
    if (!loaded && nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u32(handle, NVS_KEY_CMD_ID, &m_cmd_id);
        nvs_close(handle);
    }
    loaded = true;
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


/**
 * {"id": 7, "name": "assign", "slot": 1, "mac": "C8:47:80:12:34:56"} (an empty
 * MAC unassigns the slot) or {"id": 8, "name": "reboot"}. Each id runs once.
 */
static void run_command(const cJSON *cmd) {
    const cJSON *id = cJSON_GetObjectItem(cmd, "id");
    const cJSON *name = cJSON_GetObjectItem(cmd, "name");
    if (!cJSON_IsNumber(id) || !cJSON_IsString(name) || (uint32_t) id->valuedouble <= m_cmd_id) {
        return;
    }
    m_cmd_id = (uint32_t) id->valuedouble;
    nvs_save_cmd_id(m_cmd_id);
    ESP_LOGW(TAG, "Command %lu: %s", (unsigned long) m_cmd_id, name->valuestring);

    if (strcmp(name->valuestring, "assign") == 0) {
        const cJSON *slot = cJSON_GetObjectItem(cmd, "slot");
        const cJSON *mac_str = cJSON_GetObjectItem(cmd, "mac");
        uint8_t mac[6] = { 0 };
        if (!cJSON_IsNumber(slot) || !cJSON_IsString(mac_str)
                || (mac_str->valuestring[0] != '\0' && !parse_mac(mac_str->valuestring, mac))) {
            ESP_LOGW(TAG, "assign: bad arguments");
            return;
        }
        jkbms_ble_set_mac(slot->valueint - 1, mac);
    } else if (strcmp(name->valuestring, "reboot") == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGW(TAG, "Unknown command");
    }
}


/*---------------------------------------------------------------
        Firmware update from the dashboard
---------------------------------------------------------------*/

static int nvs_load_ota_attempts(const char *elf) {
    nvs_handle_t handle;
    char saved[17] = "";
    size_t size = sizeof(saved);
    uint8_t attempts = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_str(handle, NVS_KEY_OTA_ELF, saved, &size);
        nvs_get_u8(handle, NVS_KEY_OTA_TRIES, &attempts);
        nvs_close(handle);
    }
    return strncasecmp(saved, elf, 16) == 0 ? attempts : 0;
}


static void nvs_save_ota_attempts(const char *elf, int attempts) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        char prefix[17];
        strlcpy(prefix, elf, sizeof(prefix));
        nvs_set_str(handle, NVS_KEY_OTA_ELF, prefix);
        nvs_set_u8(handle, NVS_KEY_OTA_TRIES, (uint8_t) attempts);
        nvs_commit(handle);
        nvs_close(handle);
    }
}


static bool hex_to_bytes(const char *hex, uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned int b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
            return false;
        }
        bytes[i] = b;
    }
    return true;
}


/**
 * True when `elf` already ran and was rolled back: it is not tried again.
 */
static bool was_rolled_back(const char *elf) {
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    uint8_t sha[32];
    return invalid != NULL && esp_ota_get_partition_description(invalid, &desc) == ESP_OK
           && hex_to_bytes(elf, sha, sizeof(sha)) && memcmp(desc.app_elf_sha256, sha, sizeof(sha)) == 0;
}


static const char* ota_download(const char *url) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        return "no OTA partition";
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    char *buf = malloc(OTA_CHUNK);
    if (client == NULL || buf == NULL) {
        esp_http_client_cleanup(client);
        free(buf);
        return "no memory";
    }
#ifdef ENV_JKBMS_API_TOKEN
    esp_http_client_set_header(client, "Authorization", "Bearer " ENV_JKBMS_API_TOKEN);
#endif
    const char *error = NULL;
    esp_ota_handle_t handle = 0;
    int64_t total = 0;
    size_t received = 0;
    size_t header_len = 0;
    int shown = -1;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        error = "connection failed";
    } else if ((total = esp_http_client_fetch_headers(client)) <= 0 || esp_http_client_get_status_code(client) != 200) {
        error = "HTTP error";
    } else if (total < WEBSERVER_IMAGE_HEADER_SIZE || total > partition->size) {
        error = "wrong firmware size";
    }
    while (error == NULL && received < total) {
        // the first chunk is collected until the header is complete
        const size_t offset = (handle == 0) ? header_len : 0;
        const size_t want = (handle == 0) ? WEBSERVER_IMAGE_HEADER_SIZE - header_len : OTA_CHUNK;
        const int len = esp_http_client_read(client, buf + offset, MIN(want, total - received));
        if (len <= 0) {
            error = "download interrupted";
            break;
        }
        received += len;
        esp_err_t err;
        if (handle == 0) {
            header_len += len;
            if (header_len < WEBSERVER_IMAGE_HEADER_SIZE) {
                continue;
            }
            error = webserver_check_image(buf);
            if (error != NULL) {
                break;
            }
            ESP_LOGI(TAG, "Downloading %lld bytes into %s", total, partition->label);
            err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                handle = 0;
                error = "esp_ota_begin failed";
                break;
            }
            err = esp_ota_write(handle, buf, header_len);
        } else {
            err = esp_ota_write(handle, buf, len);
        }
        if (err != ESP_OK) {
            error = "flash write failed";
            break;
        }
        const int percent = (int) (100 * received / total);
        if (percent != shown) {
            ui_ota_progress(percent);
            shown = percent;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);

    if (error == NULL) {
        const esp_err_t err = esp_ota_end(handle);
        handle = 0;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            error = "image corrupted";
        } else if (err != ESP_OK) {
            error = "esp_ota_end failed";
        } else if (esp_ota_set_boot_partition(partition) != ESP_OK) {
            error = "cannot set boot partition";
        }
    }
    if (handle != 0) {
        esp_ota_abort(handle);
    }
    return error;
}


/**
 * {"elf": "<SHA-256 of the ELF>", "url": "firmware"}: downloads the firmware
 * unless it is running already, was rolled back, or failed too often.
 */
static void update_firmware(const cJSON *ota) {
    const cJSON *elf = cJSON_GetObjectItem(ota, "elf");
    const cJSON *name = cJSON_GetObjectItem(ota, "url");
    if (!cJSON_IsString(elf) || strlen(elf->valuestring) != 64 || !cJSON_IsString(name)) {
        return;
    }
    char running[65];
    esp_app_get_elf_sha256(running, sizeof(running));
    if (strcasecmp(running, elf->valuestring) == 0) {
        return;
    }
    if (was_rolled_back(elf->valuestring)) {
        snprintf(m_ota_error, sizeof(m_ota_error), "rolled back %.8s", elf->valuestring);
        return;
    }
    // a failed update restarts the board: the attempts are counted in NVS
    const int attempts = nvs_load_ota_attempts(elf->valuestring);
    if (attempts >= OTA_ATTEMPTS) {
        snprintf(m_ota_error, sizeof(m_ota_error), "gave up on %.8s after %d attempts", elf->valuestring, attempts);
        return;
    }
    char url[128];
    if (!uploader_api_url(name->valuestring, url, sizeof(url)) || !webserver_ota_claim()) {
        return;
    }
    // Bluetooth is off now (webserver_ota_claim): only a restart brings the monitor back
    m_ota_active = true;
    nvs_save_ota_attempts(elf->valuestring, attempts + 1);
    ESP_LOGW(TAG, "Firmware update to %.8s from %s, attempt %d, %u bytes free, largest block %u", elf->valuestring,
             url, attempts + 1, (unsigned) esp_get_free_heap_size(),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    const char *error = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < OTA_MIN_BLOCK ? "low memory"
                                                                                           : ota_download(url);
    if (error != NULL) {
        ESP_LOGW(TAG, "Firmware update failed: %s, %u bytes free, restarting", error,
                 (unsigned) esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGW(TAG, "Firmware updated, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
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
    if (webserver_ota_active() || m_ota_active) {
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


bool remote_ota_active(void) {
    return m_ota_active;
}


void remote_on_reply(const char *reply) {
    nvs_load_cmd_id();
    cJSON *root = cJSON_Parse(reply);
    if (root == NULL) {
        return;
    }
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsObject(cmd)) {
        run_command(cmd);
    }
    const cJSON *ota = cJSON_GetObjectItem(root, "ota");
    if (cJSON_IsObject(ota)) {
        update_firmware(ota);
    }
    cJSON_Delete(root);
}
