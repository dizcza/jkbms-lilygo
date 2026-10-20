/*
 * console.h
 */

#ifndef MAIN_CONSOLE_H_
#define MAIN_CONSOLE_H_

#ifdef __cplusplus
 extern "C" {
#endif

/**
 * Starts the development console if CONFIG_UI_SCREENSHOT_CONSOLE is set.
 */
void console_init(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_CONSOLE_H_ */
