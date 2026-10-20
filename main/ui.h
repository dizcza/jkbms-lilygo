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
 * Shows the first-upload result for 10 s over everything: a green check mark
 * with "Відправлено" when sent, a red cross when not. A button closes it.
 */
void ui_show_upload_result(bool ok);

/**
 * Shows the firmware update progress over everything, 0..100 %.
 * A negative value hides it.
 */
void ui_ota_progress(int percent);

/**
 * Development helpers, see CONFIG_UI_SCREENSHOT_CONSOLE.
 */
void ui_debug_anonymize(bool enable);
void ui_screenshot(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UI_H_ */
