#include "ui.h"
#include "wled.h"
#include <M5Dial.h>
#include <math.h>

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[240 * 20];

/* --- Display driver --- */

static void disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    M5Dial.Display.startWrite();
    M5Dial.Display.setAddrWindow(area->x1, area->y1, w, h);
    M5Dial.Display.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
    M5Dial.Display.endWrite();
    lv_disp_flush_ready(disp);
}

/* --- Screens (forward declared for touch_read) --- */
static lv_obj_t *main_screen;

/* --- Touch input driver --- */

static lv_indev_data_t touch_data;

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    *data = touch_data;
    static lv_indev_state_t last_state = LV_INDEV_STATE_RELEASED;
    if (data->state != last_state) {
        Serial.printf("[indev] %s x=%d y=%d\n",
            data->state == LV_INDEV_STATE_PRESSED ? "PRESSED" : "RELEASED",
            data->point.x, data->point.y);
        last_state = data->state;
    }
}

/* --- Screens --- */
static lv_obj_t *color_screen;
static lv_obj_t *presets_screen;
static lv_obj_t *effects_screen;

/* --- Main screen widgets --- */
static lv_obj_t *brightness_arc;
static lv_obj_t *pct_label;

/* --- Color screen widgets --- */
static lv_obj_t *color_canvas;
static lv_obj_t *color_arc;

/* --- Presets screen widgets --- */
static lv_obj_t *pre_labels[5];
static int        presets_cursor = 0;

/* --- Effects screen widgets --- */
static lv_obj_t *eff_labels[5];   // prev2, prev1, selected, next1, next2
static int        effects_cursor = 0;
static lv_obj_t  *eff_highlight;
static lv_obj_t  *eff_back_btn;
static lv_obj_t  *pre_highlight;
static lv_obj_t  *pre_back_btn;

// Current accent color (matches arc indicator, updated via ui_set_arc_color)
static lv_color_t accent_color;

// Color wheel pixel buffer — 188×188 RGB565, generated once at boot (~71 KB DRAM).
// 188px diameter sits inside the arc inner edge (arc inner radius = 95px, wheel radius = 94px).
static lv_color_t colorwheel_pixels[188 * 188];

/* --- Main screen icon buffers (44×44 px each) --- */
static lv_color_t white_icon_buf[44 * 44];
static lv_color_t rainbow_icon_buf[44 * 44];
static lv_color_t bookmark_icon_buf[44 * 44];
static lv_color_t star_icon_buf[44 * 44];

/* --- Wedge buttons canvas (200×200) --- */
static lv_color_t buttons_canvas_buf[200 * 200];

/* --- Icon generators --- */

// Ray-casting point-in-polygon test.
static bool point_in_poly(const int *px, const int *py, int n, int x, int y) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((py[i] > y) != (py[j] > y)) {
            float xint = (float)(px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i];
            if ((float)x < xint) inside = !inside;
        }
    }
    return inside;
}

// White filled circle on dark button background.
static void generate_white_icon() {
    const int W = 44, H = 44, cx = 22, cy = 22, R2 = 18 * 18;
    lv_color_t bg = lv_color_hex(0x1E1E1E);
    lv_color_t fg = lv_color_white();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int dx = x - cx, dy = y - cy;
            white_icon_buf[y * W + x] = (dx*dx + dy*dy <= R2) ? fg : bg;
        }
}

// Rainbow gradient circle — hue=angle, sat=distance, val=100%.
static void generate_rainbow_icon() {
    const int W = 44, H = 44, cx = 22, cy = 22;
    const float R = 18.0f;
    lv_color_t bg = lv_color_hex(0x1E1E1E);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx*dx + dy*dy);
            if (r > R) { rainbow_icon_buf[y * W + x] = bg; continue; }
            float hue = atan2f(dy, dx) * (180.0f / M_PI);
            if (hue < 0.0f) hue += 360.0f;
            rainbow_icon_buf[y * W + x] =
                lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)(r / R * 100.0f), 100);
        }
}

// Bookmark with diagonal gradient: orange-red (TL) to dark red (BR).
static void generate_bookmark_icon() {
    const int W = 44, H = 44;
    const int px[] = { 11, 33, 33, 22, 11 };
    const int py[] = {  5,  5, 38, 29, 38 };
    lv_color_t bg = lv_color_hex(0x1E1E1E);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!point_in_poly(px, py, 5, x, y)) {
                bookmark_icon_buf[y * W + x] = bg;
                continue;
            }
            float t = (float)(x + y) / (float)(W + H - 2);
            uint8_t r = (uint8_t)(0xFF - t * (0xFF - 0x88));
            uint8_t g = (uint8_t)(0x55 - t * (0x55 - 0x11));
            uint8_t b = (uint8_t)(0x22 - t * (0x22 - 0x11));
            bookmark_icon_buf[y * W + x] = lv_color_make(r, g, b);
        }
    }
}

// 4-point star with radial gradient: bright yellow center to orange edges.
static void generate_star_icon() {
    const int W = 44, H = 44;
    const int px[] = { 22,  28, 40, 28, 22, 16,  4, 16 };
    const int py[] = {  4,  16, 22, 28, 40, 28, 22, 16 };
    const float MAX_R = 18.0f;
    lv_color_t bg = lv_color_hex(0x1E1E1E);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (!point_in_poly(px, py, 8, x, y)) {
                star_icon_buf[y * W + x] = bg;
                continue;
            }
            float dx = x - 22.0f, dy = y - 22.0f;
            float t = sqrtf(dx*dx + dy*dy) / MAX_R;
            if (t > 1.0f) t = 1.0f;
            uint8_t g = (uint8_t)(0xFF - t * (0xFF - 0x88));
            uint8_t b = (uint8_t)(0x44 - t * 0x44);
            star_icon_buf[y * W + x] = lv_color_make(0xFF, g, b);
        }
    }
}

/* --- Wedge buttons canvas renderer ---
   200×200, centered on the 240×240 display (offset 20,20).
   Canvas center = (100,100). Inner circle radius = 95px (arc outer 110 − 15px width).
   Inner circle: 4 pie-wedge quadrants with icons.
   Outer area: screen bg so arc ring shows through when arc is rendered on top.
*/
static void draw_buttons_canvas() {
    const int W = 200, H = 200;
    const int CX = 100, CY = 100;
    const float BTN_R2 = 95.0f * 95.0f;

    lv_color_t col_bg  = lv_color_hex(0x111111);
    lv_color_t col_btn = lv_color_hex(0x1E1E1E);

    // Icon top-left corners in canvas coords.
    // Icon size 44×44, centers pushed ±38px from canvas center (100,100).
    // → icon TL = center - 22: (62-22)=40 and (138-22)=116.
    // q=0 TL(dx<0,dy<0)=white  q=1 TR(dx≥0,dy<0)=rainbow
    // q=2 BL(dx<0,dy≥0)=bookmark  q=3 BR(dx≥0,dy≥0)=star
    const int icon_x0[4] = { 40, 116,  40, 116 };
    const int icon_y0[4] = { 40,  40, 116, 116 };
    lv_color_t *icon_buf[4] = { white_icon_buf, rainbow_icon_buf,
                                 bookmark_icon_buf, star_icon_buf };

    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            float dx = px - CX, dy = py - CY;
            float r2 = dx * dx + dy * dy;

            if (r2 >= BTN_R2) {
                buttons_canvas_buf[py * W + px] = col_bg;
                continue;
            }
            // Thin dividing cross between quadrants
            if (fabsf(dx) < 1.5f || fabsf(dy) < 1.5f) {
                buttons_canvas_buf[py * W + px] = col_bg;
                continue;
            }
            int q = (dx >= 0.0f ? 1 : 0) | (dy >= 0.0f ? 2 : 0);
            int ix = px - icon_x0[q];
            int iy = py - icon_y0[q];
            if (ix >= 0 && ix < 44 && iy >= 0 && iy < 44)
                buttons_canvas_buf[py * W + px] = icon_buf[q][iy * 44 + ix];
            else
                buttons_canvas_buf[py * W + px] = col_btn;
        }
    }
}

/* --- Helpers --- */

// Icon-only button: 60×60, shows a 36×36 canvas from icon_buf.
static lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_color_t *icon_buf,
                                int x_ofs, int y_ofs, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 60, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *canvas = lv_canvas_create(btn);
    lv_canvas_set_buffer(canvas, icon_buf, 36, 36, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(canvas, 36, 36);
    lv_obj_center(canvas);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(canvas, 0, 0);
    lv_obj_set_style_pad_all(canvas, 0, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    return btn;
}

static lv_obj_t *make_grid_btn(lv_obj_t *parent, const char *line1, const char *line2,
                                int x_ofs, int y_ofs, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 60, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, x_ofs, y_ofs);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x333333), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *lbl1 = lv_label_create(btn);
    lv_label_set_text(lbl1, line1);
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl1, LV_ALIGN_CENTER, 0, line2 ? -9 : 0);

    if (line2) {
        lv_obj_t *lbl2 = lv_label_create(btn);
        lv_label_set_text(lbl2, line2);
        lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl2, lv_color_hex(0x888888), 0);
        lv_obj_align(lbl2, LV_ALIGN_CENTER, 0, 9);
    }

    return btn;
}

static lv_obj_t *make_sub_screen(const char *title, lv_event_cb_t back_cb, lv_obj_t **out_back_btn = nullptr) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *back_btn = lv_btn_create(scr);
    lv_obj_set_size(back_btn, 90, 35);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(back_btn, accent_color, 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "< Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0x111111), 0);
    lv_obj_center(back_lbl);

    if (out_back_btn) *out_back_btn = back_btn;
    return scr;
}

/* --- Button callbacks --- */

static void cb_back(lv_event_t *e) {
    lv_disp_load_scr(main_screen);
}

static void cb_debug_screen(lv_event_t *e) {
    Serial.println("[LVGL] main_screen received event");
}

static void cb_white(lv_event_t *e) {
    Serial.println("[LVGL] White button clicked");
    wled_send_raw("{\"on\":true,\"seg\":[{\"col\":[[0,0,0,128]]}]}");
}

static void cb_color(lv_event_t *e) {
    // Turn off white, restore RGB mode (color screen will set colour)
    wled_send_raw("{\"seg\":[{\"col\":[[255,255,255,0]]}]}");
    lv_scr_load(color_screen);
}

static void cb_presets(lv_event_t *e) {
    lv_scr_load(presets_screen);
}

static void cb_effects(lv_event_t *e) {
    lv_scr_load(effects_screen);
}

/* --- Screen builders --- */

static void build_main_screen() {
    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(main_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(main_screen, 0, 0);
    lv_obj_clear_flag(main_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(main_screen, cb_debug_screen, LV_EVENT_PRESSED, NULL);

    /* Wedge buttons canvas — created FIRST so arc renders on top of it */
    generate_white_icon();
    generate_rainbow_icon();
    generate_bookmark_icon();
    generate_star_icon();
    draw_buttons_canvas();

    lv_obj_t *btn_canvas = lv_canvas_create(main_screen);
    lv_canvas_set_buffer(btn_canvas, buttons_canvas_buf, 200, 200, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(btn_canvas, 200, 200);
    lv_obj_center(btn_canvas);
    lv_obj_set_style_border_width(btn_canvas, 0, 0);
    lv_obj_set_style_pad_all(btn_canvas, 0, 0);
    lv_obj_clear_flag(btn_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Brightness arc — created after canvas so it renders on top */
    brightness_arc = lv_arc_create(main_screen);
    lv_obj_set_size(brightness_arc, 220, 220);
    lv_obj_center(brightness_arc);
    lv_arc_set_rotation(brightness_arc, 135);
    lv_arc_set_bg_angles(brightness_arc, 0, 270);
    lv_arc_set_range(brightness_arc, 0, 255);
    lv_arc_set_value(brightness_arc, 128);
    lv_obj_clear_flag(brightness_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(brightness_arc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(brightness_arc, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(brightness_arc, lv_color_hex(0xFFEECC), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(brightness_arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(brightness_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_arc, 0, LV_PART_KNOB);

    /* Brightness % label — bottom center, rendered on top */
    pct_label = lv_label_create(main_screen);
    lv_obj_set_style_text_font(pct_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pct_label, lv_color_hex(0x666666), 0);
    lv_obj_align(pct_label, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// Render the HSV color wheel into colorwheel_pixels[].
// Hue = angle, saturation = distance from center, value = 1.0 always.
// Pixels outside the circle radius are set to black.
static void generate_colorwheel() {
    const int W = 188, H = 188;
    const int cx = W / 2, cy = H / 2;
    const float R = W / 2.0f;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float dx = x - cx, dy = y - cy;
            float r = sqrtf(dx * dx + dy * dy);
            if (r > R) {
                colorwheel_pixels[y * W + x] = lv_color_black();
                continue;
            }
            float hue = atan2f(dy, dx) * (180.0f / M_PI);
            if (hue < 0.0f) hue += 360.0f;
            float sat = r / R;
            // lv_color_hsv_to_rgb: h 0-360, s 0-100, v 0-100
            colorwheel_pixels[y * W + x] =
                lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)(sat * 100.0f), 100);
        }
    }
}

static void build_color_screen() {
    generate_colorwheel();

    color_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(color_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(color_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(color_screen, 0, 0);
    lv_obj_clear_flag(color_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Color wheel canvas — drawn FIRST so the arc renders on top of it.
    // 196×196 exactly fills to the arc inner edge (arc inner radius = 98px = canvas radius).
    color_canvas = lv_canvas_create(color_screen);
    lv_canvas_set_buffer(color_canvas, colorwheel_pixels, 188, 188, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(color_canvas, 188, 188);
    lv_obj_center(color_canvas);
    lv_obj_set_style_bg_opa(color_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(color_canvas, 0, 0);
    lv_obj_set_style_pad_all(color_canvas, 0, 0);
    lv_obj_clear_flag(color_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Brightness arc — created AFTER canvas so it renders on top, covering the canvas rim.
    color_arc = lv_arc_create(color_screen);
    lv_obj_set_size(color_arc, 220, 220);
    lv_obj_center(color_arc);
    lv_arc_set_rotation(color_arc, 135);
    lv_arc_set_bg_angles(color_arc, 0, 270);
    lv_arc_set_range(color_arc, 0, 255);
    lv_arc_set_value(color_arc, 128);
    lv_obj_clear_flag(color_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(color_arc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(color_arc, 15, LV_PART_MAIN);
    lv_obj_set_style_arc_color(color_arc, lv_color_hex(0xFFEECC), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(color_arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(color_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(color_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(color_arc, 0, LV_PART_KNOB);

    // Back arrow — bottom of screen, indicates BtnA navigates back.
    lv_obj_t *back_lbl = lv_label_create(color_screen);
    lv_label_set_text(back_lbl, "<");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(back_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
}

static void refresh_preset_labels() {
    int count = wled_get_presets_count();
    for (int i = 0; i < 5; i++) {
        int idx = presets_cursor + (i - 2);
        const char *name;
        if (count == 0)                   name = (i == 2) ? "Loading..." : "";
        else if (idx < 0 || idx >= count) name = "";
        else                              name = wled_get_preset_name(idx);
        lv_label_set_text(pre_labels[i], name);
        lv_obj_set_style_text_color(pre_labels[i],
            lv_color_hex(i == 2 ? 0x000000 : 0xFFFFFF), 0);
    }
}

static void build_presets_screen() {
    presets_screen = make_sub_screen("Presets", cb_back, &pre_back_btn);

    pre_highlight = lv_obj_create(presets_screen);
    lv_obj_t *hl = pre_highlight;
    lv_obj_set_size(hl, 200, 22);
    lv_obj_align(hl, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(hl, accent_color, 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hl, 4, 0);
    lv_obj_set_style_border_width(hl, 0, 0);
    lv_obj_clear_flag(hl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    const int y_base[] = { 62, 87, 112, 137, 162 };
    for (int i = 0; i < 5; i++) {
        pre_labels[i] = lv_label_create(presets_screen);
        lv_obj_set_style_text_font(pre_labels[i], &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(pre_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(pre_labels[i], 190, 22);
        lv_obj_align(pre_labels[i], LV_ALIGN_TOP_MID, 0, y_base[i]);
    }

    refresh_preset_labels();
}

// Update the 5 visible effect name labels from the current cursor position.
static void refresh_effects_labels() {
    int count = wled_get_effects_count();
    for (int i = 0; i < 5; i++) {
        int idx = effects_cursor + (i - 2);
        const char *name;
        if (count == 0)                   name = (i == 2) ? "Loading..." : "";
        else if (idx < 0 || idx >= count) name = "";
        else                              name = wled_get_effect_name(idx);
        lv_label_set_text(eff_labels[i], name);
        lv_obj_set_style_text_color(eff_labels[i],
            lv_color_hex(i == 2 ? 0x000000 : 0xFFFFFF), 0);
    }
}

static void build_effects_screen() {
    effects_screen = make_sub_screen("Effects", cb_back, &eff_back_btn);

    eff_highlight = lv_obj_create(effects_screen);
    lv_obj_t *hl = eff_highlight;
    lv_obj_set_size(hl, 200, 22);
    lv_obj_align(hl, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_bg_color(hl, accent_color, 0);
    lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hl, 4, 0);
    lv_obj_set_style_border_width(hl, 0, 0);
    lv_obj_clear_flag(hl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    const int y_base[] = { 62, 87, 112, 137, 162 };
    for (int i = 0; i < 5; i++) {
        eff_labels[i] = lv_label_create(effects_screen);
        lv_obj_set_style_text_font(eff_labels[i], &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(eff_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(eff_labels[i], 190, 22);
        lv_obj_align(eff_labels[i], LV_ALIGN_TOP_MID, 0, y_base[i]);
    }

    refresh_effects_labels();
}

/* --- Public API --- */

void ui_init() {
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 240;
    disp_drv.ver_res  = 240;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read;
    lv_indev_drv_register(&indev_drv);

    accent_color = lv_color_hex(0xFFEECC);  // matches initial arc indicator color
    build_main_screen();
    build_color_screen();
    build_presets_screen();
    build_effects_screen();

    lv_disp_load_scr(main_screen);
}

void ui_update() {
    lv_timer_handler();
}

void ui_set_touch(int x, int y, bool pressed) {
    touch_data.point.x = x;
    touch_data.point.y = y;
    touch_data.state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed) Serial.printf("[touch] x=%d y=%d scr_is_main=%d\n", x, y, lv_scr_act() == main_screen);
}

void ui_set_brightness(int value) {
    int pct = (value * 100) / 255;
    lv_arc_set_value(brightness_arc, value);
    lv_arc_set_value(color_arc, value);
    lv_label_set_text_fmt(pct_label, "%d%%", pct);
    lv_refr_now(NULL);
}

void ui_set_arc_color(uint8_t r, uint8_t g, uint8_t b) {
    accent_color = lv_color_make(r, g, b);
    lv_obj_set_style_arc_color(brightness_arc, accent_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(color_arc,      accent_color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(eff_highlight,   accent_color, 0);
    lv_obj_set_style_bg_color(pre_highlight,   accent_color, 0);
    lv_obj_set_style_bg_color(eff_back_btn,    accent_color, 0);
    lv_obj_set_style_bg_color(pre_back_btn,    accent_color, 0);
    lv_refr_now(NULL);
}



ui_screen_t ui_current_screen() {
    lv_obj_t *a = lv_scr_act();
    if (a == color_screen)   return UI_SCREEN_COLOR;
    if (a == presets_screen) return UI_SCREEN_PRESETS;
    if (a == effects_screen) return UI_SCREEN_EFFECTS;
    return UI_SCREEN_MAIN;
}

void ui_goto_main()  { lv_disp_load_scr(main_screen);  lv_refr_now(NULL); }
void ui_goto_color() { lv_disp_load_scr(color_screen); lv_refr_now(NULL); }

void ui_goto_presets() {
    int count = wled_get_presets_count();
    int cur = wled_get_current_ps();
    presets_cursor = 0;
    for (int i = 0; i < count; i++) {
        if (wled_get_preset_id(i) == cur) { presets_cursor = i; break; }
    }
    refresh_preset_labels();
    lv_disp_load_scr(presets_screen);
    lv_refr_now(NULL);
}

void ui_scroll_presets(int delta) {
    int count = wled_get_presets_count();
    if (count == 0) return;
    presets_cursor = constrain(presets_cursor + delta, 0, count - 1);
    refresh_preset_labels();
    lv_obj_invalidate(presets_screen);
    lv_refr_now(NULL);
}

int ui_get_presets_selection_id() {
    return wled_get_preset_id(presets_cursor);
}

void ui_goto_effects() {
    // Sync cursor to currently active effect when entering the screen.
    effects_cursor = constrain(wled_get_current_fx(), 0,
                               wled_get_effects_count() > 0 ? wled_get_effects_count() - 1 : 0);
    refresh_effects_labels();
    lv_disp_load_scr(effects_screen);
    lv_refr_now(NULL);
}

void ui_scroll_effects(int delta) {
    int count = wled_get_effects_count();
    if (count == 0) return;
    effects_cursor = constrain(effects_cursor + delta, 0, count - 1);
    refresh_effects_labels();
    lv_obj_invalidate(effects_screen);
    lv_refr_now(NULL);
}

int ui_get_effects_selection() {
    return effects_cursor;
}

void ui_set_bg_color(uint32_t hex) {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

