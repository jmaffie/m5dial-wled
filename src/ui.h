#pragma once
#include <lvgl.h>

typedef enum { UI_SCREEN_MAIN, UI_SCREEN_COLOR, UI_SCREEN_PRESETS, UI_SCREEN_EFFECTS } ui_screen_t;

void ui_init();
void ui_update();
void ui_set_bg_color(uint32_t hex);
void ui_set_brightness(int value);
void ui_set_arc_color(uint8_t r, uint8_t g, uint8_t b);
void ui_set_touch(int x, int y, bool pressed);

ui_screen_t ui_current_screen();
void ui_goto_main();
void ui_goto_color();
void ui_goto_presets();
void ui_goto_effects();

void ui_scroll_effects(int delta);
int  ui_get_effects_selection();

void ui_scroll_presets(int delta);
int  ui_get_presets_selection_id();

void ui_set_on_state(bool on);
