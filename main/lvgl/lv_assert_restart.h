/*
 * lv_assert_restart.h
 *
 * Included by lv_assert.h (CONFIG_LV_ASSERT_HANDLER_INCLUDE): a failed LVGL
 * assertion, e.g. out of memory, restarts the board instead of hanging in
 * while(1) with a frozen screen.
 */

#include <stdlib.h>

#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();
