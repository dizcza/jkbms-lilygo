/*
 * main.c
 *
 * JK BMS monitor for the LILYGO TTGO T-Display (ESP32 + ST7789V 135x240).
 * Connects to JK BMS units over BLE and shows the pack summary and
 * per-cell voltage and resistance. Left/right buttons switch pages.
 * Always on: the status is uploaded over Wi-Fi every minute, and the
 * firmware can be updated from the board's web page.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include <stdlib.h>
#include <string.h>

#include "buttons.h"
#include "console.h"
#include "jkbms_ble.h"
#include "lcd.h"
#include "report.h"
#include "ui.h"
#include "uploader.h"
#include "webserver.h"

#define UI_REFRESH_MS    500

#define FIRST_EXTRAS_WAIT_US  (10 * 1000000LL)
#define FIRST_WIFI_WAIT_US    (30 * 1000000LL)  // several networks are tried one by one
#define FALLBACK_PERIOD_US    ((CONFIG_UPLOAD_PERIOD_S + 60) * 1000000LL)

static const char *TAG = "main";


static void nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}


/**
 * A new firmware counts as working once it is back on Wi-Fi, where it can be
 * updated again. If it resets before that, the bootloader rolls back.
 */
static void on_wifi_connected(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK
            && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware is on Wi-Fi: marking it valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}


/**
 * True once per new stream of any BMS, i.e. after every (re)connection.
 */
static bool stream_started(void) {
    static jkbms_link_t links[JKBMS_DEVICES_MAX];
    static jkbms_status_t st;  // large: keep it off the stack
    bool started = false;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_ble_get_status(i, &st);
        started |= st.link == JKBMS_LINK_STREAMING && links[i] != JKBMS_LINK_STREAMING;
        links[i] = st.link;
    }
    return started;
}


/**
 * True when the set of chosen BMS differs from the last call (and on the
 * first call): a newly chosen BMS gets a full first report again.
 */
static bool selection_changed(void) {
    static uint8_t macs[JKBMS_DEVICES_MAX][6];
    static bool first_call = true;
    static jkbms_status_t st;
    bool changed = first_call;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        uint8_t mac[6] = { 0 };
        if (jkbms_ble_get_status(i, &st) && st.link != JKBMS_LINK_UNASSIGNED) {
            memcpy(mac, st.mac, 6);
        }
        changed |= memcmp(mac, macs[i], 6) != 0;
        memcpy(macs[i], mac, 6);
    }
    first_call = false;
    return changed;
}


/**
 * True when every streaming BMS has delivered its settings and logbook.
 */
static bool extras_ready(void) {
    static jkbms_status_t st;
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_ble_get_status(i, &st);
        if (st.link == JKBMS_LINK_STREAMING && !(st.settings_valid && st.logbook_valid)) {
            return false;
        }
    }
    return true;
}


/**
 * Builds and POSTs the report. `full` adds the settings. Returns
 * ESP_ERR_NOT_FINISHED when skipped during a firmware update.
 */
static esp_err_t upload(uint32_t count, bool full) {
    if (webserver_ota_active()) {
        ESP_LOGI(TAG, "Firmware update in progress, upload skipped");
        return ESP_ERR_NOT_FINISHED;
    }
    const bool with_extras = full || (count % CONFIG_UPLOAD_EXTRAS_EVERY) == 1 || CONFIG_UPLOAD_EXTRAS_EVERY == 1;
    char *json = report_build(count, with_extras);
    if (json == NULL) {
        ESP_LOGW(TAG, "No memory for the report: %u bytes free, largest block %u",
                 (unsigned) esp_get_free_heap_size(), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Upload #%lu, %u bytes free", (unsigned long) count, (unsigned) esp_get_free_heap_size());
    const esp_err_t err = uploader_send(json);
    if (err == ESP_OK) {
        report_mark_sent();
    }
    free(json);
    if (with_extras && count > 1) {
        // new logbook events go out with the next uploads
        for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
            jkbms_ble_request_logbook(i);
        }
    }
    return err;
}


void app_main(void) {
    nvs_init();
    lv_display_t *disp = lcd_init();
    // reconnect for fresh data so that a new stream starts about once per upload period;
    // the BMS streams for about 30 s and beeps on every connection
    jkbms_ble_set_refresh_period_s(CONFIG_UPLOAD_PERIOD_S > 30 ? CONFIG_UPLOAD_PERIOD_S - 30 : CONFIG_UPLOAD_PERIOD_S);
    ESP_ERROR_CHECK(jkbms_ble_init());
    ui_init(disp);
    ui_refresh();
    lcd_backlight(true);

    buttons_init(ui_on_button);
    console_init();
    if (uploader_start(on_wifi_connected) == ESP_OK) {
        webserver_start();
    }
    ESP_LOGI(TAG, "Monitoring %d JK BMS", JKBMS_DEVICES_MAX);

    uint32_t uploads = 0;
    int64_t last_upload_us = 0;
    int64_t due_us = -1;            // a new stream started: upload
    bool first_sent = false;        // the full report of the chosen BMS was delivered
    bool retry_on_wifi = false;     // the first report failed without Wi-Fi: send once connected
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
        ui_refresh();
        if (CONFIG_UPLOAD_PERIOD_S == 0) {
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (selection_changed()) {
            first_sent = false;
            retry_on_wifi = false;
            report_reset();
        }
        if (stream_started() && due_us < 0) {
            due_us = now;
        }
        if (retry_on_wifi && !first_sent && due_us < 0 && uploader_connected()) {
            due_us = now;
        }
        if (due_us >= 0) {
            // the first report waits a few seconds for the settings and the logbook,
            // and for Wi-Fi, which may still be trying the networks
            const bool extras = extras_ready() || now - due_us > FIRST_EXTRAS_WAIT_US;
            const bool wifi = uploader_connected() || now - due_us > FIRST_WIFI_WAIT_US;
            if (first_sent || (extras && wifi)) {
                const esp_err_t err = upload(++uploads, !first_sent);
                if (!first_sent && err != ESP_ERR_NOT_FINISHED) {
                    // a green check mark once, or a red cross until it succeeds
                    ui_show_upload_result(err == ESP_OK);
                    first_sent = err == ESP_OK;
                    // Wi-Fi was down or dropped during the upload: resend once it is back
                    retry_on_wifi = err != ESP_OK && (err == ESP_ERR_INVALID_STATE || !uploader_connected());
                }
                last_upload_us = now;
                due_us = -1;
            }
        } else if (now - last_upload_us >= FALLBACK_PERIOD_US) {
            // no BMS streams: still report, so the dashboard shows it offline
            upload(++uploads, false);
            last_upload_us = now;
        }
    }
}
