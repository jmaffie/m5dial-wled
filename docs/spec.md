# M5 Dial WLED Controller — Project Specification

## Overview

A physical rotary controller built on the M5 Stack Dial that communicates with a WLED instance over WiFi. The device provides tactile control of lighting via a rotary encoder and round touchscreen display, replacing the need for a phone app for common adjustments.

---

## Hardware

| Component | Details |
|---|---|
| MCU | ESP32-S3 (M5 Stack Dial) |
| Display | GC9A01 1.2" round LCD, 240x240 |
| Input | Rotary encoder with push button |
| Connectivity | WiFi (2.4GHz) |
| USB | Native USB via ESP32-S3 (USB JTAG/CDC, VID 303A:1001) |
| Flash | 8MB |

---

## Goals

- Control WLED brightness via rotary encoder
- Control WLED color/effect via display UI
- Show current WLED state on the round display
- Button press to toggle on/off
- WiFi connection to local WLED instance via HTTP JSON API

---

## Planned Features

### Phase 1 — Hardware Validation (complete)
- [x] Toolchain setup (PlatformIO + Arduino framework)
- [x] USB upload working
- [x] Display initializes and renders color
- [x] LVGL 8.4.0 integrated and rendering via M5GFX driver

### Phase 2 — Input (complete)
- [x] Rotary encoder reads rotation (CW = up, CCW = down)
- [x] Encoder button press detected (resets count in test)
- [x] Touch screen reads and reports x/y coordinates
- [x] Touch input registered as LVGL pointer input device
- [ ] Map encoder rotation to brightness value
- [ ] Button toggles WLED on/off

### Phase 3 — WiFi + WLED API (complete)
- [x] Connect to WiFi (credentials in gitignored `secrets.h`)
- [x] WebSocket persistent connection to WLED (`/ws` endpoint)
- [x] Brightness control via encoder — non-blocking, FreeRTOS Core 0
- [x] Toggle on/off via button
- [ ] Read current WLED state on boot to sync display
- [ ] Handle connection loss / reconnect gracefully

### Phase 4 — Display UI
- [ ] Show brightness level on screen
- [ ] Show current color/effect name
- [ ] Connection status indicator
- [ ] Menu for selecting effects or colors

### Phase 5 — Polish
- [ ] Persist WiFi credentials and WLED IP in NVS/flash
- [ ] Config mode via USB serial
- [ ] Sleep/wake on inactivity

---

## Code Architecture

### Source Files

| File | Responsibility |
|---|---|
| `src/main.cpp` | Hardware IO — reads encoder, button, touch; calls ui/wled functions |
| `src/ui.h/.cpp` | LVGL display — driver setup, widget creation, screen updates |
| `src/wled.h/.cpp` | WLED API — WiFi connection, HTTP requests (stub in Phase 2) |
| `src/lv_conf.h` | LVGL configuration — color depth, memory, enabled widgets |

### UI API

| Function | Description |
|---|---|
| `ui_init()` | Initialize LVGL, register display and touch drivers, create widgets |
| `ui_update()` | Call `lv_timer_handler()` — must be called every loop |
| `ui_set_bg_color(hex)` | Set screen background color and force repaint |
| `ui_set_count(value)` | Update the center count label |
| `ui_set_touch(x, y, pressed)` | Update touch coordinates label and feed LVGL input driver |

### WLED API (stub)

| Function | Description |
|---|---|
| `wled_init()` | Connect WiFi, fetch initial state |
| `wled_set_brightness(0–255)` | POST brightness to WLED JSON API |
| `wled_set_power(bool)` | POST on/off to WLED JSON API |

---

## WLED API

WLED exposes a JSON API at `http://<ip>/json/state`.

**Get state:**
```
GET http://<wled-ip>/json/state
```

**Set brightness:**
```
POST http://<wled-ip>/json/state
Content-Type: application/json

{"bri": 128}
```

**Toggle on/off:**
```json
{"on": true}
{"on": false}
```

---

## Configuration

| Parameter | Value |
|---|---|
| WLED IP | TBD |
| WiFi SSID | TBD |
| WiFi Password | TBD |

---

## Non-Goals

- No BLE
- No OTA updates (initially)
- No multi-WLED instance support (initially)
