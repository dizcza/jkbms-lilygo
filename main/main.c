/*
 * main.c
 *
 * JK BMS monitor for the LILYGO TTGO T-Display (ESP32 + ST7789V 135x240).
 * Connects to JK BMS units over BLE and shows the pack summary and
 * per-cell voltage and resistance. Left/right buttons switch pages.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include <stdlib.h>

#include "esp_attr.h"
#include "esp_timer.h"

#include "buttons.h"
#include "console.h"
#include "history.h"
#include "jkbms_ble.h"
#include "lcd.h"
#include "report.h"
#include "ui.h"
#include "uploader.h"

#define UI_REFRESH_MS    500

#define WAKEUP_GPIO_LEFT    GPIO_NUM_0
#define WAKEUP_GPIO_RIGHT   GPIO_NUM_35    // external pull-up on the board

#define REPORT_BLE_WAIT_US  (25 * 1000000LL)
#define REPORT_MIN_SLEEP_US (5 * 1000000LL)
#define REPORT_EXTRAS_WAIT_US (8 * 1000000LL)

static RTC_DATA_ATTR uint32_t m_wake_count;

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
 * Sleeps until a button is pressed (display on) or the upload timer fires
 * (display stays off). The board boots from scratch on wakeup.
 */
static void deep_sleep(int64_t sleep_us) {
    // GPIO0 and GPIO35 are RTC GPIOs; ext0 and ext1 can each wake on one low pin
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(WAKEUP_GPIO_RIGHT, 0));
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << WAKEUP_GPIO_LEFT, ESP_EXT1_WAKEUP_ALL_LOW));
    rtc_gpio_pullup_en(WAKEUP_GPIO_LEFT);
    rtc_gpio_pulldown_dis(WAKEUP_GPIO_LEFT);
    if (sleep_us > 0) {
        esp_sleep_enable_timer_wakeup(sleep_us);
    }
    esp_deep_sleep_start();
}


/**
 * Writes the current readings of all BMS with data to the flash history.
 * `recorded` (optional) marks the BMS already recorded, to record each once.
 */
static void record_history(bool *recorded) {
    static jkbms_status_t st;  // large: keep it off the stack
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        if ((recorded != NULL && recorded[i]) || !jkbms_ble_get_status(i, &st) || !st.cells_valid) {
            continue;
        }
        history_add(i, &st);
        if (recorded != NULL) {
            recorded[i] = true;
        }
    }
}


/**
 * True when every assigned BMS has delivered what the report needs, or
 * clearly will not.
 */
static bool report_ready(bool with_extras) {
    static jkbms_status_t st;  // large: keep it off the stack
    for (int i = 0; i < JKBMS_DEVICES_MAX; i++) {
        jkbms_ble_get_status(i, &st);
        if (st.link == JKBMS_LINK_UNASSIGNED || st.error == JKBMS_ERROR_NOT_FOUND
                || st.error == JKBMS_ERROR_NOT_JK) {
            continue;
        }
        if (!st.cells_valid || (with_extras && !(st.settings_valid && st.logbook_valid))) {
            return false;
        }
    }
    return true;
}


/**
 * Timer wakeup: reads the BMS and uploads the status with the display off.
 */
static void report_and_sleep(void) {
    const int64_t start = esp_timer_get_time();
    m_wake_count++;
    const bool with_extras = (m_wake_count % CONFIG_UPLOAD_EXTRAS_EVERY) == 1 || CONFIG_UPLOAD_EXTRAS_EVERY == 1;
    ESP_LOGI(TAG, "Upload wakeup #%lu%s", (unsigned long) m_wake_count, with_extras ? " with settings and logbook" : "");

    jkbms_ble_set_fetch_extras(with_extras);
    ESP_ERROR_CHECK(jkbms_ble_init());
    while (esp_timer_get_time() - start < REPORT_BLE_WAIT_US && !report_ready(false)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    // the settings and the logbook are nice to have: do not wait long for them
    const int64_t cells_us = esp_timer_get_time();
    while (with_extras && esp_timer_get_time() - cells_us < REPORT_EXTRAS_WAIT_US && !report_ready(true)) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    record_history(NULL);
    char *json = report_build(m_wake_count, with_extras);
    if (json != NULL) {
        uploader_send(json);
        free(json);
    }

    int64_t sleep_us = CONFIG_UPLOAD_PERIOD_S * 1000000LL - (esp_timer_get_time() - start);
    if (sleep_us < REPORT_MIN_SLEEP_US) {
        sleep_us = REPORT_MIN_SLEEP_US;
    }
    ESP_LOGI(TAG, "Awake for %lld ms, sleeping", (esp_timer_get_time() - start) / 1000);
    // the display was put to sleep before; keep the backlight pin held low
    gpio_deep_sleep_hold_en();
    deep_sleep(sleep_us);
}


void app_main(void) {
    nvs_init();
    history_init();
    if (CONFIG_UPLOAD_PERIOD_S > 0 && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        report_and_sleep();
    }

    lv_display_t *disp = lcd_init();
    // read the BMS once per wakeup: the BMS beeps on every new connection
    jkbms_ble_set_reconnect_on_stall(false);
    ESP_ERROR_CHECK(jkbms_ble_init());
    ui_init(disp);
    ui_refresh();
    lcd_backlight(true);

    buttons_init(ui_on_button);
    console_init();
    ESP_LOGI(TAG, "Monitoring %d JK BMS", JKBMS_DEVICES_MAX);

    bool recorded[JKBMS_DEVICES_MAX] = { 0 };  // once per wakeup
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
        ui_refresh();
        record_history(recorded);
        if (ui_should_sleep() && ui_show_sleep_screen()) {
            ESP_LOGI(TAG, "Idle, entering deep sleep");
            lcd_sleep();
            deep_sleep(CONFIG_UPLOAD_PERIOD_S * 1000000LL);
        }
    }
}
