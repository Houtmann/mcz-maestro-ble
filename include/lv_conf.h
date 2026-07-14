// lv_conf.h -- minimal LVGL 8 configuration for the CYD.
// Options that are not set get their defaults via lv_conf_internal.h.
#pragma once

#define LV_COLOR_DEPTH        16
#define LV_COLOR_16_SWAP      0        // byte swap is handled by tft.pushColors(..., true)

// Memory from the system heap (malloc) instead of a static .bss pool -> saves ~40 KB .bss,
// important alongside BLE+WiFi. (ESP32-WROOM without PSRAM.)
#define LV_MEM_CUSTOM         1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc
#define LV_MEM_SIZE           (40U * 1024U)

// Tick/Refresh
#define LV_DISP_DEF_REFR_PERIOD  20
#define LV_INDEV_DEF_READ_PERIOD 20
#define LV_TICK_CUSTOM        0        // we call lv_tick_inc() ourselves

// No LVGL log (saves flash/RAM)
#define LV_USE_LOG            0

// Fonts for a readable UI
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_16

// Widgets are on by default in LVGL 8; explicitly lock in the ones the UI needs:
#define LV_USE_ARC       1
#define LV_USE_BTN       1
#define LV_USE_BTNMATRIX 1
#define LV_USE_LABEL     1
#define LV_USE_DROPDOWN  1
#define LV_USE_LIST      1
#define LV_USE_SLIDER    1
#define LV_USE_SWITCH    1
#define LV_USE_TEXTAREA  1
#define LV_USE_KEYBOARD  1
#define LV_USE_MSGBOX    1
#define LV_USE_SPINNER   1
