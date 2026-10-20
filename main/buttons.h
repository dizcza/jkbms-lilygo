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

/**
 * Called from the buttons task: -1 for the left button, +1 for the right one,
 * 0 when both buttons are held for 2 seconds.
 */
typedef void (*buttons_cb_t)(int step);

void buttons_init(buttons_cb_t callback);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_BUTTONS_H_ */
