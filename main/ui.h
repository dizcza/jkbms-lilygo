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
 * Button handler: step -1 for the left button, +1 for the right one.
 * Picker: left moves the cursor, right selects. Monitor: previous/next page.
 */
void ui_on_button(int step);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_UI_H_ */
