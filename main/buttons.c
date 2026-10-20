/*
 * buttons.c
 */

#include "buttons.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"

#define BUTTON_LEFT_GPIO       0
#define BUTTON_RIGHT_GPIO      35     // input only, external pull-up on the board

#define BUTTON_POLL_MS         10
#define BUTTON_DEBOUNCE_MS     30
#define BUTTON_REPEAT_DELAY_MS 500
#define BUTTON_REPEAT_MS       150
#define BUTTON_BOTH_HOLD_MS    2000

typedef struct {
    int gpio;
    int step;
    int pressed_ms;           // 0 if released
    int next_repeat_ms;
} button_t;

static button_t m_buttons[] = {
    { .gpio = BUTTON_LEFT_GPIO, .step = -1 },
    { .gpio = BUTTON_RIGHT_GPIO, .step = +1 },
};


static void buttons_task(void *args) {
    buttons_cb_t callback = (buttons_cb_t) args;
    int both_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        const bool both = gpio_get_level(BUTTON_LEFT_GPIO) == 0 && gpio_get_level(BUTTON_RIGHT_GPIO) == 0;
        both_ms = both ? both_ms + BUTTON_POLL_MS : 0;
        if (both_ms == BUTTON_BOTH_HOLD_MS) {
            callback(0);
        }
        for (int i = 0; i < sizeof(m_buttons) / sizeof(m_buttons[0]); i++) {
            button_t *btn = &m_buttons[i];
            if (gpio_get_level(btn->gpio) != 0) {
                btn->pressed_ms = 0;
                continue;
            }
            btn->pressed_ms += BUTTON_POLL_MS;
            if (both) {
                // no auto-repeat while both buttons are held
                btn->next_repeat_ms = btn->pressed_ms + BUTTON_REPEAT_MS;
            }
            if (btn->pressed_ms == BUTTON_DEBOUNCE_MS) {
                callback(btn->step);
                btn->next_repeat_ms = BUTTON_REPEAT_DELAY_MS;
            } else if (btn->pressed_ms > BUTTON_DEBOUNCE_MS && btn->pressed_ms >= btn->next_repeat_ms) {
                callback(btn->step);
                btn->next_repeat_ms += BUTTON_REPEAT_MS;
            }
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
    xTaskCreate(buttons_task, "buttons", 8192, (void*) callback, 5, NULL);
}
