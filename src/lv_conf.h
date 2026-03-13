#if 1 /* Set this to "1" to enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color depth: 1 (1 byte per pixel), 8 (RGB332), 16 (RGB565), 32 (ARGB8888) */
#define LV_COLOR_DEPTH 16

/* Swap the 2 bytes of RGB565 color — needed for M5GFX */
#define LV_COLOR_16_SWAP 0


/* Memory */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64 * 1024U)  /* 64KB */

/* HAL */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Display resolution — M5 Dial GC9A01 */
#define LV_HOR_RES_MAX 240
#define LV_VER_RES_MAX 240

/* Font */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Logging */
#define LV_USE_LOG 0

/* Extra widgets */
#define LV_USE_ARC        1
#define LV_USE_BTN        1
#define LV_USE_CANVAS     1
#define LV_USE_LABEL      1
#define LV_USE_IMG        1
#define LV_USE_LINE       1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1

/* Unused features — keep disabled to save flash */
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_MSGBOX     0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0
#define LV_USE_LIST       1
#define LV_USE_METER      0

#endif /* LV_CONF_H */
#endif /* End of "1" */
