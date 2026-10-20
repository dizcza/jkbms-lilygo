/*
 * ui.h
 */

#ifndef MAIN_UI_H_
#define MAIN_UI_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include "lvgl.h"

void ui_init(lv_display_t *disp);

/**
 * Re-reads the BMS status and redraws the current page.
 */
void ui_refresh(void);

/**
 * Button handler, see BUTTON_* in buttons.h.
 * Picker: left moves the cursor, right selects. Monitor: previous/next page.
 * Holding left forgets the saved BMS and restarts into the picker.
 */
void ui_on_button(int step);

/**
 * True when the board has been idle long enough to go to deep sleep.
 */
bool ui_should_sleep(void);

/**
 * Shows the sleep screen for 2 seconds. Returns false if a button press
 * cancelled it, true if the board should go to sleep now.
 */
bool ui_show_sleep_screen(void);

/**
 * Development helpers, see CONFIG_UI_SCREENSHOT_CONSOLE.
 */
void ui_debug_sleep_screen(bool show);
void ui_debug_anonymize(bool enable);
void ui_screenshot(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UI_H_ */
