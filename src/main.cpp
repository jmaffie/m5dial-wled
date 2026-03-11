#include <Arduino.h>
#include <M5Dial.h>
#include <math.h>
#include "ui.h"
#include "wled.h"

static const int BRIGHTNESS_STEP = 5;
static const int BRIGHTNESS_MIN  = 0;
static const int BRIGHTNESS_MAX  = 255;

static long last_encoder = 0;
static int  brightness   = 128;
static int  touch_x      = 0;
static int  touch_y      = 0;

void setup() {
    auto cfg = M5.config();
    M5Dial.begin(cfg, true, false);
    Serial.begin(115200);
    M5Dial.Display.setBrightness(128);

    ui_init();
    wled_init();
    ui_set_brightness(brightness);
}

void loop() {
    M5Dial.update();

    // Encoder → brightness (main/color/presets) or scroll (effects)
    long encoder = M5Dial.Encoder.read();
    if (encoder != last_encoder) {
        int delta = (encoder > last_encoder) ? 1 : -1;
        last_encoder = encoder;
        if (ui_current_screen() == UI_SCREEN_EFFECTS) {
            ui_scroll_effects(delta);
        } else if (ui_current_screen() == UI_SCREEN_PRESETS) {
            ui_scroll_presets(delta);
        } else {
            brightness = constrain(brightness + delta * BRIGHTNESS_STEP, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
            ui_set_brightness(brightness);
            wled_set_brightness(brightness);
        }
    }

    // Encoder button — back on color screen, select effect on effects screen, toggle elsewhere
    if (M5Dial.BtnA.wasPressed()) {
        if (ui_current_screen() == UI_SCREEN_COLOR)
            ui_goto_main();
        else if (ui_current_screen() == UI_SCREEN_EFFECTS)
            wled_set_effect(ui_get_effects_selection());
        else if (ui_current_screen() == UI_SCREEN_PRESETS)
            wled_set_preset(ui_get_presets_selection_id());
        else
            wled_toggle();
    }

    // Sync power indicator whenever on/off state changes via WebSocket
    if (wled_on_changed())
        ui_set_on_state(wled_is_on());

    // Sync arc color whenever WLED display color changes (button press or WebSocket state)
    if (wled_color_changed()) {
        uint8_t r, g, b;
        wled_get_display_color(&r, &g, &b);
        ui_set_arc_color(r, g, b);
    }

    ui_update();

    // Direct FT5x06 register read — bypasses Touch_Class 4ms throttle entirely.
    // FT5x06 at I2C address 0x38, port I2C_NUM_1 (M5Dial.In_I2C).
    // Registers: 0x02=touch count, 0x03=XH, 0x04=XL, 0x05=YH, 0x06=YL
    // Coordinate-based button handling — bypasses broken LVGL event routing.
    // Buttons: 60×60 px, ±33 px from center (120,120).
    //   White:   (57–117, 57–117)   Color:   (123–183, 57–117)
    //   Presets: (57–117, 123–183)  Effects: (123–183, 123–183)
    // Back button on sub-screens: 90×35, bottom-mid, 20 px up → (75–165, 185–220)
    // Gate I2C read on INT pin (GPIO 14). The FT5x06 holds INT LOW for the
    // entire touch duration. When the finger lifts, INT goes HIGH but the IC
    // still returns count > 0 (stale register data). Without this gate,
    // now_pressed stays true after lift → prev_pressed stays true → rising_edge
    // never fires on the next press.
    bool now_pressed = false;
    static const int FT5_ADDR     = 0x38;
    static const int TOUCH_INT_PIN = 14;
    if (digitalRead(TOUCH_INT_PIN) == LOW) {
        uint8_t tbuf[5] = {};
        if (M5Dial.In_I2C.readRegister(FT5_ADDR, 0x02, tbuf, 5, 400000UL)) {
            uint8_t count = tbuf[0] & 0x0F;
            if (count > 0 && count <= 5) {
                now_pressed = true;
                touch_x = ((tbuf[1] & 0x0F) << 8) | tbuf[2];
                touch_y = ((tbuf[3] & 0x0F) << 8) | tbuf[4];
            }
        }
    }
    ui_set_touch(touch_x, touch_y, now_pressed);

    static bool prev_pressed = false;
    bool rising_edge = now_pressed && !prev_pressed;
    prev_pressed = now_pressed;

    // Color wheel — runs every loop while touching so dragging works.
    // Wheel center = display center (120,120), radius 94px (188/2).
    // Hue = angle, saturation = distance/radius, value always 100%.
    if (ui_current_screen() == UI_SCREEN_COLOR && now_pressed) {
        const float CX = 120.0f, CY = 120.0f, R = 98.0f;
        float dx = touch_x - CX, dy = touch_y - CY;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= R) {
            float hue = atan2f(dy, dx) * (180.0f / M_PI);
            if (hue < 0.0f) hue += 360.0f;
            float sat = dist / R;
            // lv_color_hsv_to_rgb: h 0-360, s 0-100, v 0-100
            lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)(sat * 100.0f), 100);
            lv_color32_t c32; c32.full = lv_color_to32(c);
            wled_store_color(c32.ch.red, c32.ch.green, c32.ch.blue);
            wled_set_color();
        }
    }

    if (rising_edge) {
        int px = touch_x, py = touch_y;
        switch (ui_current_screen()) {
            case UI_SCREEN_MAIN:
                if      (px >= 57 && px <= 117 && py >= 57  && py <= 117)
                    wled_set_white();
                else if (px >= 123 && px <= 183 && py >= 57  && py <= 117) {
                    wled_set_color();
                    ui_goto_color();
                }
                else if (px >= 57  && px <= 117 && py >= 123 && py <= 183)
                    ui_goto_presets();
                else if (px >= 123 && px <= 183 && py >= 123 && py <= 183)
                    ui_goto_effects();
                break;
            case UI_SCREEN_COLOR:
                break;  // BtnA handles back; no touch-based back button
            default:
                if (px >= 75 && px <= 165 && py >= 185 && py <= 220)
                    ui_goto_main();
                break;
        }
    }
}
