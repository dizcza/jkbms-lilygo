/*
 * main.c
 *
 * JK BMS monitor for the LILYGO TTGO T-Display (ESP32 + ST7789V 135x240).
 * Connects to JK BMS units over BLE and shows the pack summary and
 * per-cell voltage and resistance. Left/right buttons switch pages.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "buttons.h"
#include "jkbms_ble.h"
#include "lcd.h"
#include "ui.h"

#define UI_REFRESH_MS    500

static const char *TAG = "main";


static void nvs_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}


void app_main(void) {
    nvs_init();

    lv_display_t *disp = lcd_init();
    ESP_ERROR_CHECK(jkbms_ble_init());
    ui_init(disp);
    ui_refresh();
    lcd_backlight(true);

    buttons_init(ui_on_button);
    ESP_LOGI(TAG, "Monitoring %d JK BMS", JKBMS_DEVICES_MAX);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
        ui_refresh();
    }
}
