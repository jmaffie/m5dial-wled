# Touch Input Troubleshooting

**Status: RESOLVED**

Touch buttons respond correctly on direct press. See the Resolution section at the bottom
for the final working approach.

---

## What Works

- Touch hardware (CST816S) reports correct X/Y coordinates
- Serial confirms `[touch] press x,y` fires on contact
- Rotary encoder, encoder button, brightness arc, screen rendering all work correctly
- WLED commands work when triggered via direct function calls

---

## Symptom History

### Symptom 1 — LVGL callbacks never fire
LVGL buttons (`LV_EVENT_CLICKED` / `LV_EVENT_PRESSED`) never triggered callbacks
even though touch coordinates were correct and the correct screen was active
(`lv_scr_act() == main_screen` confirmed via serial).

**Things tried:**
- Cleared `LV_OBJ_FLAG_SCROLLABLE` on `main_screen` and all sub-screens — no change
- Cleared `LV_OBJ_FLAG_CLICKABLE` on the brightness arc — no change
- Changed button callbacks from `LV_EVENT_CLICKED` to `LV_EVENT_PRESSED` — no change
- Used `lv_disp_load_scr()` (immediate) instead of `lv_scr_load()` — screen confirmed active, no change
- Added `[indev]` debug print inside `touch_read` callback — could not verify output (serial monitor fails with `termios.error: (25, Inappropriate ioctl for device)` when launched non-interactively)

**Conclusion:** LVGL event routing is broken. Root cause unknown. Bypassed in favor of direct coordinate handling.

---

### Symptom 2 — Touch only registers on next encoder turn

After bypassing LVGL events, direct coordinate checking via `t.wasPressed()` was added
to `main.cpp`. This produced a new symptom: touching the screen did nothing, but the
PREVIOUS touch was registered the next time the encoder was turned.

**Analysis:**
The CST816S touch IC fires a brief interrupt pulse when touched (single-pulse mode,
configured by M5Unified via `ONCE_WLP=1` in IrqCtl register 0xFA). `M5Dial.update()`
only reads touch data when the INT pin is asserted. If `update()` is called after
the ~1ms pulse has already cleared, the touch is missed entirely.

The encoder turn coincidentally causes the touch to be caught because it creates
additional processing time, during which the ISR flag remains readable.

**Things tried:**

1. **Write 0x40 to CST816S register 0xFA** to clear `ONCE_WLP` (change from single-pulse
   to hold-low-while-touching mode):
   ```cpp
   M5Dial.In_I2C.writeRegister8(0x15, 0xFA, 0x40, 400000UL);
   ```
   No change — the write may have failed (wrong device or M5Unified re-initialises the IC).

2. **Direct I2C polling** — bypass M5Unified entirely, read CST816S register 0x01
   (finger count) + 0x02–0x05 (XY) every loop via `M5Dial.In_I2C.readRegister()`:
   ```cpp
   uint8_t tbuf[5] = {};
   if (M5Dial.In_I2C.readRegister(0x15, 0x01, tbuf, 5, 400000UL)) { ... }
   ```
   Resulted in **no response at all** — readRegister likely fails silently (wrong I2C
   address, wrong bus instance, or wrong M5Unified API signature).

3. **Second `M5Dial.update()` call** right before the touch read (after encoder/button
   processing), to give a second window to catch the interrupt:
   ```cpp
   M5Dial.update();  // at top of loop — encoder + button
   // ... encoder + button handling ...
   M5Dial.update();  // second call — fresh touch attempt
   auto t = M5Dial.Touch.getDetail();
   ```
   This is the **current state** of the firmware. Effect unknown — session ended here.

---

## Current Code State (main.cpp loop)

```cpp
void loop() {
    M5Dial.update();

    // Encoder → brightness
    long encoder = M5Dial.Encoder.read();
    if (encoder != last_encoder) {
        int delta = (encoder > last_encoder) ? BRIGHTNESS_STEP : -BRIGHTNESS_STEP;
        brightness = constrain(brightness + delta, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        last_encoder = encoder;
        ui_set_brightness(brightness);
        wled_set_brightness(brightness);
    }

    // Encoder button → toggle
    if (M5Dial.BtnA.wasPressed()) { wled_toggle(); }

    // Second update for fresh touch data
    M5Dial.update();

    auto t = M5Dial.Touch.getDetail();
    bool now_pressed = t.isPressed();
    if (now_pressed) { touch_x = t.x; touch_y = t.y; }
    ui_set_touch(touch_x, touch_y, now_pressed);

    // Manual edge detection (rising edge = touch-down event)
    static bool prev_pressed = false;
    bool rising_edge = now_pressed && !prev_pressed;
    prev_pressed = now_pressed;

    if (rising_edge) {
        int px = touch_x, py = touch_y;
        switch (ui_current_screen()) {
            case UI_SCREEN_MAIN:
                if      (px >= 57 && px <= 117 && py >= 57  && py <= 117)
                    wled_send_raw("{\"on\":true,\"seg\":[{\"col\":[[0,0,0,128]]}]}");
                else if (px >= 123 && px <= 183 && py >= 57  && py <= 117) {
                    wled_send_raw("{\"seg\":[{\"col\":[[255,255,255,0]]}]}");
                    ui_goto_color();
                }
                else if (px >= 57  && px <= 117 && py >= 123 && py <= 183)
                    ui_goto_presets();
                else if (px >= 123 && px <= 183 && py >= 123 && py <= 183)
                    ui_goto_effects();
                break;
            default:  // sub-screens
                if (px >= 75 && px <= 165 && py >= 185 && py <= 220)
                    ui_goto_main();
                break;
        }
    }

    ui_update();
    delay(5);
}
```

---

## Recommended Next Steps (priority order)

### 1. Read serial output (highest priority)

The serial monitor fails non-interactively (`termios` error). Open it in a real terminal:
```bash
screen /dev/ttyACM0 115200
# or
cat /dev/ttyACM0
```
Then tap a button and look for:
- `[touch] press x,y` — confirms hardware detection
- `[indev] PRESSED x=... y=...` — confirms LVGL input driver sees the event
- `[LVGL] ...` — confirms button callback fired (currently dead since we bypassed LVGL)

This will immediately show whether `isPressed()` returns true during touch.

### 2. Verify CST816S I2C address and INT pin

The I2C address 0x15 and INT pin used in our attempts may be wrong. To verify:
```cpp
// In setup(), scan I2C bus to find actual touch IC address
for (uint8_t addr = 1; addr < 127; addr++) {
    M5Dial.In_I2C.writeRegister8(addr, 0x00, 0x00, 400000UL);
    // Log which addresses ACK
}
```
Or check M5Unified source: `~/.platformio/packages/framework-arduinoespressif32/...`
or `~/.platformio/libdeps/m5dial/M5Unified/src/utility/`

The INT pin GPIO number is also needed for option 3 below.

### 3. Hardware interrupt on CST816S INT pin

Attach an Arduino `attachInterrupt()` to the CST816S INT pin. In the ISR, set a flag.
In the main loop, check the flag and call `M5Dial.Touch.getDetail()`. This guarantees
we never miss the touch pulse:
```cpp
volatile bool touch_flag = false;
void IRAM_ATTR touch_isr() { touch_flag = true; }

// In setup():
pinMode(TOUCH_INT_PIN, INPUT);
attachInterrupt(digitalPinToInterrupt(TOUCH_INT_PIN), touch_isr, FALLING);

// In loop():
if (touch_flag) {
    touch_flag = false;
    auto t = M5Dial.Touch.getDetail();
    // handle touch
}
```
Find `TOUCH_INT_PIN` in M5Unified source or the M5Dial schematic.

### 4. Configure CST816S continuous mode via correct API

M5Unified may re-initialise the IC after our write. Try writing AFTER `M5Dial.begin()`
and verify with a readback. Also try value `0x60` (EN_TOUCH | EN_CHANGE) or `0x70`
(EN_TOUCH | EN_CHANGE | EN_MOTION) instead of `0x40`.

---

## Button Layout Reference

Buttons are 60×60 px, centered at ±33 px from screen center (120, 120):

| Button  | Center   | Hit zone (x, y)         |
|---------|----------|--------------------------|
| White   | 87, 87   | x: 57–117, y: 57–117    |
| Color   | 153, 87  | x: 123–183, y: 57–117   |
| Presets | 87, 153  | x: 57–117, y: 123–183   |
| Effects | 153, 153 | x: 123–183, y: 123–183  |

Back button (sub-screens): 90×35 px, bottom-center, 20 px from edge → x: 75–165, y: 185–220

---

## Resolution

### Root cause 1 — Wrong touch IC assumption (CST816S vs FT5x06)

All early attempts wrote to I2C address 0x15 (CST816S). The actual touch IC is
**FT5x06 at address 0x38**, confirmed by reading M5GFX source:
`~/.platformio/libdeps/m5dial/M5GFX/src/M5GFX.cpp`.

Hardware config (from M5GFX.cpp):
- I2C address: `0x38`
- I2C port: `I2C_NUM_1` (M5Dial.In_I2C)
- SDA: GPIO 11, SCL: GPIO 12
- INT pin: GPIO 14

### Root cause 2 — Touch_Class 4ms throttle

`Touch_Class::TOUCH_MIN_UPDATE_MSEC = 4` — M5Unified discards any `M5Dial.update()`
call less than 4ms after the previous one. The GPIO polling loop (100µs iterations
calling `M5Dial.update()`) was silently discarded on most iterations.

### Root cause 3 — Stale FT5x06 register data (final fix)

The FT5x06 holds INT (GPIO 14) LOW for the entire touch duration. When the finger
lifts, INT goes HIGH — but the IC's internal registers still report `count = 1`
(stale data from the last touch).

Direct I2C reads without checking the INT pin saw this stale count and kept
`now_pressed = true` after lift. This caused `prev_pressed` to get stuck at `true`,
so `rising_edge = (now_pressed && !prev_pressed)` never fired on the next press.

### Working approach (current firmware)

1. **Bypass Touch_Class entirely** — read FT5x06 registers directly via
   `M5Dial.In_I2C.readRegister(0x38, 0x02, tbuf, 5, 400000UL)`.
   Registers: `0x02`=count, `0x03`=XH, `0x04`=XL, `0x05`=YH, `0x06`=YL.

2. **Gate reads on INT pin** — only trust register data when `digitalRead(14) == LOW`.
   When INT is HIGH, treat as not-touched regardless of register content:

   ```cpp
   bool now_pressed = false;
   if (digitalRead(14) == LOW) {
       uint8_t tbuf[5] = {};
       if (M5Dial.In_I2C.readRegister(0x38, 0x02, tbuf, 5, 400000UL)) {
           uint8_t count = tbuf[0] & 0x0F;
           if (count > 0 && count <= 5) {
               now_pressed = true;
               touch_x = ((tbuf[1] & 0x0F) << 8) | tbuf[2];
               touch_y = ((tbuf[3] & 0x0F) << 8) | tbuf[4];
           }
       }
   }
   ```

3. **Manual rising-edge detection** in main loop — LVGL event routing is still broken,
   so button actions are triggered by coordinate hit-testing on the rising edge of
   `now_pressed`.

4. **`lv_refr_now(NULL)` after `lv_disp_load_scr()`** in all `ui_goto_*` functions —
   screen navigation renders immediately rather than waiting up to 30ms for the LVGL
   refresh timer.
