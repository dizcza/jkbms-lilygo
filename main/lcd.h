/*
 * lcd.h
 *
 * ST7789V 135x240 IPS panel of the LILYGO TTGO T-Display, driven by LVGL.
 */

#ifndef MAIN_LCD_H_
#define MAIN_LCD_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include <stdbool.h>
#include "lvgl.h"

#define LCD_H_RES    135
#define LCD_V_RES    240

lv_display_t* lcd_init(void);
void lcd_backlight(bool on);

/**
 * Turns the backlight and the panel off and keeps the backlight pin low
 * during deep sleep.
 */
void lcd_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_LCD_H_ */
