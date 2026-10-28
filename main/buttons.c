/*
 * buttons.c
 */

#include "buttons.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define BUTTON_LEFT_GPIO       0
#define BUTTON_RIGHT_GPIO      35     // input only, external pull-up on the board

#define BUTTON_POLL_MS         10
#define BUTTON_DEBOUNCE_MS     30
#define BUTTON_LONG_MS         1500
#define BUTTON_RIGHT_LONG_MS   5000


static void buttons_task(void *args) {
    buttons_cb_t callback = (buttons_cb_t) args;
    int left_ms = 0;
    int right_ms = 0;
    // ignore the press that woke the board up until both buttons are released
    bool armed = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        if (!armed) {
            armed = gpio_get_level(BUTTON_LEFT_GPIO) != 0 && gpio_get_level(BUTTON_RIGHT_GPIO) != 0;
            continue;
        }

        // left: short press fires on release, so that it can become a long press
        if (gpio_get_level(BUTTON_LEFT_GPIO) == 0) {
            left_ms += BUTTON_POLL_MS;
            if (left_ms == BUTTON_LONG_MS) {
                callback(BUTTON_PREV_LONG);
            }
        } else {
            if (left_ms >= BUTTON_DEBOUNCE_MS && left_ms < BUTTON_LONG_MS) {
                callback(BUTTON_PREV);
            }
            left_ms = 0;
        }

        // right: fires on press
        if (gpio_get_level(BUTTON_RIGHT_GPIO) == 0) {
            right_ms += BUTTON_POLL_MS;
            if (right_ms == BUTTON_DEBOUNCE_MS) {
                callback(BUTTON_NEXT);
            } else if (right_ms == BUTTON_RIGHT_LONG_MS) {
                callback(BUTTON_NEXT_LONG);
            }
        } else {
            right_ms = 0;
        }
    }
}


void buttons_init(buttons_cb_t callback) {
    // GPIO34..39 have no internal pull-ups
    const gpio_config_t config_left = {
        .pin_bit_mask = 1ULL << BUTTON_LEFT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t config_right = {
        .pin_bit_mask = 1ULL << BUTTON_RIGHT_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&config_left));
    ESP_ERROR_CHECK(gpio_config(&config_right));
    // the callback renders the UI, which needs a large stack
    xTaskCreate(buttons_task, "buttons", 6144, (void*) callback, 5, NULL);
}
