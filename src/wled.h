#pragma once
#include <stdint.h>

void wled_init();
bool wled_connected();
void wled_toggle();
void wled_set_brightness(uint8_t brightness);
void wled_set_power(bool on);
void wled_send_raw(const char *json);
void wled_set_white();
void wled_set_color();
void wled_store_color(uint8_t r, uint8_t g, uint8_t b);
bool wled_color_changed();
void wled_get_display_color(uint8_t *r, uint8_t *g, uint8_t *b);
bool wled_is_on();
bool wled_on_changed();

// Effects list — fetched from WLED on boot
void        wled_fetch_effects();
int         wled_get_effects_count();
const char *wled_get_effect_name(int index);
int         wled_get_current_fx();
void        wled_set_effect(int fx_index);

// Presets list — fetched from WLED on boot
void        wled_fetch_presets();
int         wled_get_presets_count();
const char *wled_get_preset_name(int index);
int         wled_get_preset_id(int index);
int         wled_get_current_ps();
void        wled_set_preset(int preset_id);
