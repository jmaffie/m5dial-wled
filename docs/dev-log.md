# Development Log

## 2026-03-09 — Phase 1: Hardware Validation

### Environment Setup
- OS: Ubuntu Linux (6.17.0)
- Toolchain: PlatformIO installed via official installer to `~/.platformio/penv/`
- PlatformIO added to PATH in `~/.bashrc`
- `python3.12-venv` required for PlatformIO installer
- User added to `dialout` group for serial port access
- PlatformIO udev rules installed at `/etc/udev/rules.d/99-platformio-udev.rules`

### Hardware
- M5 Stack Dial connected via USB
- Detected as `/dev/ttyACM0` — Espressif USB JTAG/serial debug unit (VID 303A:1001)

### Board Configuration
- Initial board `esp32s3box` caused display to not initialize (wrong flash size 16MB vs actual 8MB, wrong pin variant)
- Fixed by switching to `m5stack-stamps3` which correctly matches the ESP32-S3, 8MB flash, and USB config
- `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` required for native USB serial

### Display
- Display (GC9A01) requires explicit `setBrightness()` call before drawing — defaults to 0 (off)
- Working call sequence:
  ```cpp
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);  // (config, enable_encoder, enable_rfid)
  M5Dial.Display.setBrightness(128);
  M5Dial.Display.fillScreen(TFT_BLUE);
  ```
- Screen confirmed rendering solid blue

### Library
- `m5stack/M5Dial @ 1.0.3` pulls in `M5Unified` and `M5GFX` automatically

### Current platformio.ini
```ini
[env:m5dial]
platform = espressif32
board = m5stack-stamps3
framework = arduino
monitor_speed = 115200
upload_speed = 921600

lib_deps =
    m5stack/M5Dial

build_flags =
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
```

### Status
Phase 1 complete. Device uploads and display works.

---

## 2026-03-09 — LVGL Integration

### Setup
- Added `lvgl/lvgl @ ^8.3.0` to `lib_deps` — resolved to 8.4.0
- Added `-DLV_CONF_INCLUDE_SIMPLE` build flag so LVGL finds `lv_conf.h` in `src/`
- Created `src/lv_conf.h` with key settings:
  - `LV_COLOR_DEPTH 16` (RGB565 to match M5GFX)
  - `LV_COLOR_16_SWAP 0`
  - `LV_MEM_SIZE 64KB`
  - `LV_TICK_CUSTOM 1` using `millis()` — no separate tick ISR needed
  - Enabled: arc, button, label, slider, switch, textarea
  - Disabled unused widgets to save flash

### Display Driver
- M5GFX used as LVGL flush backend via `writePixels()` + `setAddrWindow()`
- Draw buffer: `240 * 20` pixels (line buffer approach)
- `lv_timer_handler()` called in `loop()` with 5ms delay

### Status
LVGL rendering confirmed working. Blue screen rendered via LVGL. Ready for Phase 2 (input).

---

## 2026-03-09 — Phase 2: Input Validation

### Code Refactor
- Split monolithic `main.cpp` into three modules:
  - `src/ui.cpp` — LVGL display driver and widget management
  - `src/wled.cpp` — WLED API stub (WiFi/HTTP to be implemented in Phase 3)
  - `src/main.cpp` — IO loop only; reads hardware, calls ui/wled functions

### Encoder Button
- Accessed via `M5Dial.BtnA.wasPressed()` / `wasReleased()` / `isPressed()`
- Requires `M5Dial.update()` called each loop
- Confirmed working

### Rotary Encoder
- Read via `M5Dial.Encoder.read()` — returns a cumulative `long`
- Direction detected by comparing to previous value
- CW rotation increments, CCW decrements
- Button press resets count to 0 and resyncs `last_encoder`
- Confirmed working

### Touch Screen (CST816S)
- Read via `M5Dial.Touch.getDetail()` returning x, y, `isPressed()`
- LVGL touch input driver registered (`LV_INDEV_TYPE_POINTER`) fed from `ui_set_touch()`
- Coordinates displayed live on screen
- Confirmed working

### LVGL Touch Driver Pattern
```cpp
static lv_indev_data_t touch_data;

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    *data = touch_data;
}
// registered as LV_INDEV_TYPE_POINTER in ui_init()
// updated each loop via ui_set_touch(x, y, pressed)
```

### Status
All hardware inputs confirmed working. Phase 2 complete. Next: Phase 3 WiFi + WLED API.

---

## 2026-03-09 — Phase 3: WiFi + WLED API

### WiFi
- Connected to `<WIFI_SSID>` via `WiFi.begin()`
- `WiFi.setSleep(false)` required — default power saving adds 100–200ms packet latency
- Device IP: `<DEVICE_IP>`
- WLED IP: `<WLED_IP>`

### Secrets Management
- `src/secrets.h` stores WiFi credentials and WLED host — gitignored
- `src/secrets.h.example` committed as a template for GitHub

### WLED Communication — Iteration History

**Attempt 1: Blocking HTTPClient**
- Each `wled_set_brightness()` call opened a new TCP connection and blocked the loop
- Result: encoder missed ticks, display unresponsive, 200–500ms lag

**Attempt 2: HTTPClient on FreeRTOS Core 0**
- Moved HTTP calls to a dedicated task pinned to Core 0
- Display became responsive immediately
- WLED still laggy — root cause: new TCP handshake per request even on LAN

**Attempt 3: WebSocket (confirmed working)**
- Switched to `links2004/WebSockets @ ^2.4.0`
- Persistent connection to WLED at `ws://<WLED_IP>/ws`
- Same JSON format as HTTP API (`{"bri":128}`, `{"on":"t"}`)
- No TCP handshake per command — feels like a physical dimmer
- Task uses `xTaskNotifyWait` (10ms timeout) + `xTaskNotify` from main for immediate wake

### Architecture
- **Core 1** (Arduino loop): encoder → `ui_set_brightness()` + `wled_set_brightness()` instantly
- **Core 0** (wled_task): maintains WebSocket, sends pending commands on notification
- `wled_set_brightness()` / `wled_toggle()` are non-blocking — just set volatile vars and notify task

### Key Pattern
```cpp
// Non-blocking call from Core 1
void wled_set_brightness(uint8_t brightness) {
    pending_bri = brightness;
    if (wled_task_handle) xTaskNotify(wled_task_handle, 0, eNoAction);
}

// Task on Core 0
ws.begin(WLED_HOST, 80, "/ws");
ws.onEvent(ws_event);
xTaskNotifyWait(...);  // wakes immediately on notify, 10ms timeout otherwise
ws.sendTXT(msg);
```

### Status
Phase 3 complete. Brightness control and toggle confirmed responsive. Ready for Phase 4 UI.

---

## 2026-03-09 — Phase 4: UI Screens + Touch — COMPLETE

### UI Built
- Main screen: brightness arc (220×220) + 2×2 button grid (60×60, ±33 px offset)
- Sub-screens: Color Selection (complete), Effects (complete), Presets (placeholder)
- Screen navigation via `lv_disp_load_scr()` + `lv_refr_now(NULL)` (immediate render)
- `lv_font_montserrat_32` — required `-DLV_FONT_MONTSERRAT_32=1` in build_flags
  (lv_conf.h change alone insufficient due to library build cache)

### Touch Input — RESOLVED
See `docs/touch-troubleshooting.md` for full root-cause analysis.
Final approach: direct FT5x06 I2C register read gated on GPIO 14 INT pin.
LVGL event routing abandoned; all button actions use coordinate hit-testing in `main.cpp`.

### Current Architecture
- Touch actions handled by coordinate zones in `main.cpp` (not LVGL events)
- Screen navigation exposed via `ui_goto_*()` / `ui_current_screen()` in `ui.cpp`
- `ui_screen_t` enum: `UI_SCREEN_MAIN`, `UI_SCREEN_COLOR`, `UI_SCREEN_PRESETS`, `UI_SCREEN_EFFECTS`

### Status
**RESOLVED.** Touch buttons respond correctly on direct press.
See `docs/touch-troubleshooting.md` for full root-cause analysis and working approach.

---

## 2026-03-10 — Phase 4 Complete: Touch Fixed

### Root Causes Identified and Fixed

**Wrong touch IC (CST816S → FT5x06):**
All early attempts targeted I2C address 0x15 (CST816S). The actual IC is FT5x06 at
0x38 on I2C port 1 (M5Dial.In_I2C), confirmed by reading M5GFX source.

**Touch_Class 4ms throttle:**
`Touch_Class::TOUCH_MIN_UPDATE_MSEC = 4` silently discards `M5Dial.update()` calls
less than 4ms apart. The GPIO polling loop trying to call `update()` every 100µs was
completely ineffective.

**Stale FT5x06 register data (final fix):**
The FT5x06 holds INT (GPIO 14) LOW while actively touched. When finger lifts, INT
goes HIGH but registers still report `count = 1`. Without gating reads on INT state,
`now_pressed` stayed true after lift, `prev_pressed` got stuck at true, and
`rising_edge` never fired on the next press. The encoder turn happened to call
`M5Dial.update()` which reset the IC state — explaining the "only responds after
encoder turn" symptom.

### Final Architecture

- **Touch read**: Direct `M5Dial.In_I2C.readRegister(0x38, 0x02, ...)` every loop,
  gated on `digitalRead(14) == LOW`.
- **Button actions**: Rising-edge hit-testing in `main.cpp` (LVGL event routing
  remains broken and abandoned).
- **Screen navigation**: `lv_disp_load_scr()` + `lv_refr_now(NULL)` for immediate render.
- **No delays** in main loop.

### Status
Phase 4 complete. All UI buttons and screen navigation working correctly.

---

## 2026-03-10 — Effects Screen

### Implementation

**Effects list fetch:**
- On boot (in `wled_init()`, after WiFi connects): HTTP GET `http://<WLED_HOST>/json/effects`
- Response is a bare JSON array: `["Solid","Blink","Breathe",...]`
- Parsed by walking quoted strings; stored in `effect_storage[200][32]` (static, no heap)
- 187 effects loaded in testing

**WebSocket state sync:**
- `"fx":N` parsed from incoming WLED state messages (same handler as color sync)
- Stored in `current_fx` — used to initialise cursor when entering the effects screen

**UI — scroll wheel (`ui.cpp`):**
- 5 individual LVGL labels at y = 62, 87, 112, 137, 162 (25 px spacing)
- Colors: very dim (0x444444) / dim (0x888888) / **white (0xFFFFFF)** / dim / very dim
- Blue highlight rect (0x1A3A6A, 200×22 px) sits behind the center label
- Labels use fixed size (`lv_obj_set_size(190, 20)`) — required for `lv_obj_invalidate`
  to correctly track dirty areas when text changes
- `refresh_effects_labels()` updates all 5 labels from `effects_cursor` each tick

**Input routing (`main.cpp`):**
- Encoder rotation on effects screen → `ui_scroll_effects(±1)` (not brightness)
- Encoder button (BtnA) on effects screen → `wled_set_effect(ui_get_effects_selection())`
  sends `{"seg":[{"fx":N}]}`; does **not** navigate away
- Back button (touch, bottom center) → returns to main screen

**White Only button:**
- Sends `{"seg":[{"col":[[0,0,0,255]],"fx":0}]}` — sets W=255, RGB=0, and resets
  effect to Solid in a single message

### Key Rendering Fix
LVGL labels did not repaint when text changed until `lv_obj_invalidate(effects_screen)`
was added before `lv_refr_now(NULL)` in `ui_scroll_effects`. Root cause: with
variable-height labels (height = LV_SIZE_CONTENT), LVGL defers size recalculation
and the dirty area tracking was incomplete. Fixed by setting explicit fixed size on
each label (`lv_obj_set_size`) and force-invalidating the parent screen.

### Status
Effects screen complete. Scrolls through live WLED effect list; BtnA applies effect.

---

## 2026-03-10 — Presets Screen

### Implementation

Identical scroll-wheel UI to the effects screen. Key differences from effects:

**Endpoint:** `/presets.json` (not `/json/presets` — that returns `{"error":4}` on this WLED version)

**JSON format:**
```json
{"0":{},"1":{...,"seg":[{"n":"",..."col":...}],"n":"White"},"2":{...,"n":"Green"}}
```
- Preset 0 is always an empty placeholder; skipped automatically
- Each preset object contains nested `"seg"` objects that also have `"n":""` fields
- The actual preset name is at the **top level** of the preset object (depth 1)

**Parser — depth-tracking forward scan (`wled_fetch_presets`):**
Backward-search approach fails because walking back from `"n":"Name"` hits
inner segment `{` braces first. Solution: forward scan with brace-depth tracking.
`"n":"VALUE"` is only extracted at depth 1 (top-level of each preset object).
Nested segment `"n":""` fields (depth 2+) are silently skipped. Empty names are
also skipped.

**Preset IDs:**
Unlike effects (sequential array indices), preset IDs are arbitrary integers stored
as JSON object keys. `preset_ids[]` maps list index → WLED ID. `wled_set_preset(id)`
sends `{"ps":ID}`. `wled_get_current_ps()` tracks the active preset from WebSocket
`"ps":N` state messages and is used to initialise the cursor on screen entry.

**White Only button:**
Also resets the effect to Solid (`"fx":0`) in the same message as the white channel:
`{"seg":[{"col":[[0,0,0,255]],"fx":0}]}`.

### Status
Presets screen complete. Scrolls through live WLED preset list; BtnA applies preset.

---

## 2026-03-10 — UI Appearance: Icon Buttons + Power Indicator

### Icon Buttons (main screen 2×2 grid)

Text labels replaced with pixel-rendered 36×36 icons drawn into LVGL canvases,
centred inside the existing 60×60 button widgets.

| Button | Icon | Technique |
|---|---|---|
| White Only | White filled circle | Distance from centre ≤ 14px |
| Color | Rainbow gradient circle | HSV: hue=angle, sat=distance/R, val=100% |
| Presets | Red bookmark | Point-in-polygon fill, 5-vertex polygon |
| Effects | Yellow 4-point star | Point-in-polygon fill, 8-vertex polygon |

**Pixel render approach (`ui.cpp`):**
- One `static lv_color_t buf[36*36]` per icon — generated once in `build_main_screen()`.
- Non-icon pixels set to button background colour (0x1E1E1E) so no transparency needed.
- `point_in_poly()` helper uses ray-casting for bookmark and star fills.
- Icon vertex coordinates:
  - **Bookmark** (5 pts): `(9,4),(27,4),(27,31),(18,24),(9,31)` — rectangle with V-notch at bottom
  - **Star** (8 pts, alternating outer r≈15 / inner r≈7): `(18,3),(23,13),(33,18),(23,23),(18,33),(13,23),(3,18),(13,13)`

**Canvas setup (per button):**
```cpp
lv_obj_t *canvas = lv_canvas_create(btn);
lv_canvas_set_buffer(canvas, icon_buf, 36, 36, LV_IMG_CF_TRUE_COLOR);
lv_obj_center(canvas);
lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
```

### Power State Indicator

Small `"1"` / `"0"` label at bottom-centre of main screen (`LV_ALIGN_BOTTOM_MID, 0, -8`).

- **1** = on, green (0x00CC44)
- **0** = off, dim grey (0x666666)

**State tracking (`wled.cpp`):**
- `static volatile bool wled_on_state` + `on_dirty` flag, initialised true on boot.
- WebSocket `WStype_TEXT` handler parses `"on":true` / `"on":false` from WLED state push.
- `wled_is_on()` and `wled_on_changed()` expose state (same dirty-flag pattern as color).

**Sync in `main.cpp`:**
```cpp
if (wled_on_changed())
    ui_set_on_state(wled_is_on());
```

### Status
Icon buttons and power indicator complete. UI appearance refinement deferred to next session.
