/*
 * console.c
 *
 * Development console to drive the UI and take screenshots, see
 * CONFIG_UI_SCREENSHOT_CONSOLE and tools/screenshots.py.
 */

#include "console.h"

#include "sdkconfig.h"

#if CONFIG_UI_SCREENSHOT_CONSOLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

#include "buttons.h"
#include "ui.h"


static void console_task(void *args) {
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    while (1) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        switch (c) {
        case 'n':
            ui_on_button(BUTTON_NEXT);
            break;
        case 'p':
            ui_on_button(BUTTON_PREV);
            break;
        case 'L':
            ui_on_button(BUTTON_PREV_LONG);
            break;
        case 's':
            ui_screenshot();
            break;
        case 'a':
            ui_debug_anonymize(true);
            break;
        default:
            break;
        }
    }
}


void console_init(void) {
    xTaskCreate(console_task, "console", 8192, NULL, 4, NULL);
}

#else

void console_init(void) {
}

#endif
