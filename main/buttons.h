/*
 * buttons.h
 *
 * T-Display buttons. Holding the board with the USB connector down,
 * GPIO0 is the left button and GPIO35 is the right button.
 */

#ifndef MAIN_BUTTONS_H_
#define MAIN_BUTTONS_H_

#ifdef __cplusplus
 extern "C" {
#endif

#define BUTTON_PREV        (-1)    // left button, short press (fires on release)
#define BUTTON_NEXT        (+1)    // right button press
#define BUTTON_PREV_LONG   (-2)    // left button held for 1.5 s

/**
 * Called from the buttons task with one of the BUTTON_* events.
 */
typedef void (*buttons_cb_t)(int event);

void buttons_init(buttons_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_BUTTONS_H_ */
