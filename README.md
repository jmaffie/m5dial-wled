# M5 Dial WLED Controller

A physical rotary controller for [WLED](https://kno.wled.ge/) built on the M5 Stack Dial. Use the dial to control brightness, colors, effects, and presets on your WLED lighting — no phone required.

## Features

- Rotary encoder controls brightness
- Round touchscreen UI with 4 screens: Main, Color, Effects, Presets
- Effects and presets fetched live from your WLED device
- WebSocket connection for low-latency, real-time control
- Power toggle button
- White-only mode (sets W channel, resets effect to Solid)

## Hardware Required

- [M5 Stack Dial](https://shop.m5stack.com/products/m5stack-dial-esp32-s3-smart-rotary-knob-w-1-28-round-touch-screen) (ESP32-S3, 1.28" round display)
- WLED device on the same WiFi network

## Setup

### 1. Install PlatformIO

Install [PlatformIO](https://platformio.org/) via VS Code extension or CLI.

### 2. Clone the repo

```bash
git clone https://github.com/jmaffie/m5dial-wled.git
cd m5dial-wled
```

### 3. Configure credentials

Copy the example secrets file and fill in your details:

```bash
cp src/secrets.h.example src/secrets.h
```

Edit `src/secrets.h`:

```cpp
#define WIFI_SSID     "your_wifi_ssid"
#define WIFI_PASSWORD "your_wifi_password"
#define WLED_HOST     "192.168.x.x"  // IP or hostname of your WLED device
```

### 4. Flash to M5 Dial

Connect the M5 Dial via USB, then:

```bash
pio run --target upload
```

The device should appear as `/dev/ttyACM0` (Linux) or a COM port (Windows).

### 5. Monitor serial output (optional)

```bash
screen /dev/ttyACM0 115200
```

## Project Structure

```
src/
  main.cpp        — Hardware IO loop (encoder, button, touch)
  ui.cpp/h        — LVGL display and screen navigation
  wled.cpp/h      — WebSocket connection and WLED API
  lv_conf.h       — LVGL configuration
  secrets.h       — WiFi + WLED credentials (gitignored)
  secrets.h.example — Template for secrets.h
```

## Dependencies

Managed automatically by PlatformIO:

- [M5Dial](https://github.com/m5stack/M5Dial)
- [LVGL 8.x](https://lvgl.io/)
- [links2004/WebSockets](https://github.com/Links2004/arduinoWebSockets)
