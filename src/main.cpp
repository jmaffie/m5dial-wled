#include <Arduino.h>
#include <M5Dial.h>
#include <math.h>
#include "ui.h"
#include "wled.h"

static const int BRIGHTNESS_STEP = 5;
static const int BRIGHTNESS_MIN  = 0;
static const int BRIGHTNESS_MAX  = 255;

// Map linear encoder position (0-255) to perceptual brightness for WLED.
// log curve: equal perceived steps across the full range.
static uint8_t perceptual_brightness(int linear) {
    if (linear <= 0)   return 0;
    if (linear >= 255) return 255;
    return (uint8_t)(255.0f * powf(linear / 255.0f, 1.7f));
}

static long  last_encoder   = 0;
static int   brightness     = 128;
static int   touch_x        = 0;
static int   touch_y        = 0;

// Display brightness fade
static float    disp_bri           = 128.0f;
static float    disp_bri_target    = 128.0f;
static float    disp_bri_start     = 128.0f;
static uint32_t disp_fade_start_ms = 0;
static const uint32_t DISP_FADE_MS = 300;

// Screen sleep after inactivity
static uint32_t last_activity_ms   = 0;
static bool     display_sleeping   = false;
static const uint32_t SLEEP_MS     = 60000;  // 1 minute
static bool     prev_pressed       = false;  // file-scope so sleep block can sync it

static void start_fade(float target) {
    disp_bri_start     = disp_bri;
    disp_bri_target    = target;
    disp_fade_start_ms = millis();
}

void setup() {
    auto cfg = M5.config();
    M5Dial.begin(cfg, true, false);
    Serial.begin(115200);
    M5Dial.Display.setBrightness(128);

    ui_init();
    wled_init();
    ui_set_brightness(brightness);
    last_activity_ms = millis();
}

void loop() {
    M5Dial.update();

    // --- Read all inputs first ---
    long encoder     = M5Dial.Encoder.read();
    bool btn_pressed = M5Dial.BtnA.wasPressed();

    // Direct FT5x06 register read — bypasses Touch_Class 4ms throttle entirely.
    // FT5x06 at I2C address 0x38, port I2C_NUM_1 (M5Dial.In_I2C).
    // Registers: 0x02=touch count, 0x03=XH, 0x04=XL, 0x05=YH, 0x06=YL
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

    bool any_activity = (encoder != last_encoder) || btn_pressed || now_pressed;

    // --- Sleep / wake ---
    if (display_sleeping) {
        if (any_activity) {
            display_sleeping = false;
            last_activity_ms = millis();
            start_fade(wled_is_on() ? 128.0f : 20.0f);
        }
        // Consume inputs so they don't fire on the first awake frame
        last_encoder = encoder;
        prev_pressed = now_pressed;
        // Keep LVGL ticking and brightness animating while asleep, skip input processing
        if (disp_bri != disp_bri_target) {
            float t = (float)(millis() - disp_fade_start_ms) / DISP_FADE_MS;
            if (t >= 1.0f) disp_bri = disp_bri_target;
            else           disp_bri = disp_bri_start * powf(fmaxf(0.001f, disp_bri_target / disp_bri_start), t);
            M5Dial.Display.setBrightness((uint8_t)disp_bri);
        }
        ui_update();
        return;
    }

    if (any_activity)
        last_activity_ms = millis();

    // --- Encoder → brightness (main/color/presets) or scroll (effects) ---
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
            wled_set_brightness(perceptual_brightness(brightness));
        }
    }

    // --- Encoder button ---
    if (btn_pressed) {
        if (ui_current_screen() == UI_SCREEN_COLOR)
            ui_goto_main();
        else if (ui_current_screen() == UI_SCREEN_EFFECTS)
            wled_set_effect(ui_get_effects_selection());
        else if (ui_current_screen() == UI_SCREEN_PRESETS)
            wled_set_preset(ui_get_presets_selection_id());
        else
            wled_toggle();
    }

    // --- Dim/restore display when WLED power state changes ---
    if (wled_on_changed())
        start_fade(wled_is_on() ? 128.0f : 20.0f);

    // --- Animate display brightness fade ---
    if (disp_bri != disp_bri_target) {
        float t = (float)(millis() - disp_fade_start_ms) / DISP_FADE_MS;
        if (t >= 1.0f) {
            disp_bri = disp_bri_target;
        } else {
            // Exponential interpolation — equal perceived brightness steps
            disp_bri = disp_bri_start * powf(fmaxf(0.001f, disp_bri_target / disp_bri_start), t);
        }
        M5Dial.Display.setBrightness((uint8_t)disp_bri);
    }

    // --- Sync arc color whenever WLED display color changes ---
    if (wled_color_changed()) {
        uint8_t r, g, b;
        wled_get_display_color(&r, &g, &b);
        ui_set_arc_color(r, g, b);
    }

    ui_update();
    ui_set_touch(touch_x, touch_y, now_pressed);

    bool rising_edge = now_pressed && !prev_pressed;
    prev_pressed = now_pressed;

    // --- Color wheel — runs every loop while touching so dragging works ---
    // Wheel center = display center (120,120), radius 94px (188/2).
    // Hue = angle, saturation = distance/radius, value always 100%.
    if (ui_current_screen() == UI_SCREEN_COLOR && now_pressed) {
        const float CX = 120.0f, CY = 120.0f, R = 94.0f;
        float dx = touch_x - CX, dy = touch_y - CY;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= R) {
            float hue = atan2f(dy, dx) * (180.0f / M_PI);
            if (hue < 0.0f) hue += 360.0f;
            float sat = dist / R;
            lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)(sat * 100.0f), 100);
            lv_color32_t c32; c32.full = lv_color_to32(c);
            wled_store_color(c32.ch.red, c32.ch.green, c32.ch.blue);
            wled_set_color();
        }
    }

    // --- Touch hit-testing on rising edge ---
    // Coordinate-based button handling — bypasses broken LVGL event routing.
    // Wedge buttons: quadrant within inner circle (r < 95px from center).
    // Back button on sub-screens: 90×35, bottom-mid, 20px up → (75–165, 185–220)
    if (rising_edge) {
        int px = touch_x, py = touch_y;
        switch (ui_current_screen()) {
            case UI_SCREEN_MAIN: {
                float tdx = px - 120.0f, tdy = py - 120.0f;
                if (tdx * tdx + tdy * tdy < 95.0f * 95.0f) {
                    if      (tdx < 0 && tdy < 0)
                        wled_set_white();
                    else if (tdx >= 0 && tdy < 0) {
                        wled_set_color();
                        ui_goto_color();
                    }
                    else if (tdx < 0 && tdy >= 0)
                        ui_goto_presets();
                    else
                        ui_goto_effects();
                }
                break;
            }
            case UI_SCREEN_COLOR:
                break;  // BtnA handles back; no touch-based back button
            default:
                if (px >= 75 && px <= 165 && py >= 185 && py <= 220)
                    ui_goto_main();
                break;
        }
    }

    // --- Sleep timeout ---
    if (millis() - last_activity_ms > SLEEP_MS) {
        display_sleeping = true;
        start_fade(0.0f);
    }
}
