"""
M5 Dial WLED Controller — Integration Tests
============================================
Requires:
  - M5 Dial connected via USB (/dev/ttyACM0)
  - WLED device reachable on the network
  - pip install -r requirements.txt

Run:
  python test/test_integration.py

Config is read from src/secrets.h automatically.
"""

import re
import sys
import time
import threading
import unittest
import requests
import serial

# ---------------------------------------------------------------------------
# Config — parsed from src/secrets.h so it stays in sync with the firmware
# ---------------------------------------------------------------------------

def parse_secrets(path="../src/secrets.h"):
    values = {}
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r'#define\s+(\w+)\s+"([^"]+)"', line)
                if m:
                    values[m.group(1)] = m.group(2)
    except FileNotFoundError:
        sys.exit(f"ERROR: {path} not found. Copy secrets.h.example and fill in credentials.")
    return values

secrets   = parse_secrets()
WLED_HOST = secrets.get("WLED_HOST", "")
SERIAL_PORT = "/dev/ttyACM0"
SERIAL_BAUD = 115200

if not WLED_HOST:
    sys.exit("ERROR: WLED_HOST not found in secrets.h")

WLED_URL = f"http://{WLED_HOST}/json/state"


# ---------------------------------------------------------------------------
# Serial monitor — collects lines in background thread
# ---------------------------------------------------------------------------

class SerialMonitor:
    def __init__(self, port, baud):
        self.lines = []         # list of (timestamp, line)
        self._lock = threading.Lock()
        self._ser = serial.Serial(port, baud, timeout=1)
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        while True:
            try:
                raw = self._ser.readline()
                line = raw.decode("utf-8", errors="ignore").strip()
                if line:
                    with self._lock:
                        self.lines.append((time.time(), line))
            except Exception:
                break

    def wait_for(self, pattern, timeout=15, after=None):
        """
        Wait up to `timeout` seconds for a line containing `pattern`.
        If `after` is a float timestamp, only consider lines received after that time.
        Returns the matching line, or None on timeout.
        """
        deadline = time.time() + timeout
        since = after or 0.0
        while time.time() < deadline:
            with self._lock:
                for ts, line in self.lines:
                    if ts >= since and pattern in line:
                        return line
            time.sleep(0.05)
        return None

    def mark(self):
        """Return current time as a marker — pass to wait_for(after=) to
        ignore lines that arrived before the action under test."""
        return time.time()

    def reset_device(self):
        """Pulse DTR to reset the ESP32, then wait briefly for boot."""
        self._ser.setDTR(False)
        time.sleep(0.1)
        self._ser.setDTR(True)
        time.sleep(0.5)

    def close(self):
        self._ser.close()


# ---------------------------------------------------------------------------
# WLED helpers
# ---------------------------------------------------------------------------

def wled_get():
    return requests.get(WLED_URL, timeout=5).json()

def wled_set(**kwargs):
    requests.post(WLED_URL, json=kwargs, timeout=5)
    time.sleep(0.3)  # give WLED time to apply and push WebSocket state


def wled_get_effects():
    return requests.get(f"http://{WLED_HOST}/json/effects", timeout=5).json()

def wled_get_presets():
    return requests.get(f"http://{WLED_HOST}/presets.json", timeout=5).json()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

mon = None  # set in setUpModule


def setUpModule():
    global mon
    print(f"\nOpening serial port {SERIAL_PORT} at {SERIAL_BAUD} baud...")
    try:
        mon = SerialMonitor(SERIAL_PORT, SERIAL_BAUD)
    except serial.SerialException as e:
        sys.exit(f"ERROR: Could not open serial port: {e}")
    print(f"WLED host: {WLED_HOST}")


def tearDownModule():
    if mon:
        mon.close()


class TestBoot(unittest.TestCase):
    """Verify the device boots and connects successfully."""

    @classmethod
    def setUpClass(cls):
        print("\n[Boot] Resetting device...")
        cls.boot_time = time.time()
        mon.reset_device()

    def test_01_wifi_connects(self):
        line = mon.wait_for("[WiFi] Connected", timeout=20, after=self.boot_time)
        self.assertIsNotNone(line, "WiFi did not connect within 20s")
        print(f"  PASS: {line}")

    def test_02_effects_loaded(self):
        line = mon.wait_for("[WLED] Loaded", timeout=20, after=self.boot_time)
        self.assertIsNotNone(line, "Effects/presets did not load")
        m = re.search(r"Loaded (\d+) effects", line)
        if m:
            count = int(m.group(1))
            self.assertGreater(count, 0, "No effects loaded")
            print(f"  PASS: {count} effects loaded")

    def test_03_presets_loaded(self):
        line = mon.wait_for("[WLED] Loaded", timeout=20, after=self.boot_time)
        # wait for the second "Loaded" line (presets)
        t = self.boot_time
        for _ in range(2):
            line = mon.wait_for("[WLED] Loaded", timeout=20, after=t)
            if line:
                t = time.time() - 14  # slide window forward
        self.assertIsNotNone(line, "Presets did not load")
        print(f"  PASS: {line}")

    def test_04_websocket_connects(self):
        line = mon.wait_for("[WLED] WebSocket connected", timeout=35, after=self.boot_time)
        self.assertIsNotNone(line, "WebSocket did not connect within 35s")
        print(f"  PASS: {line}")


class TestEffectsCount(unittest.TestCase):
    """Effects count on device matches WLED API."""

    def test_count_matches_wled(self):
        wled_effects = wled_get_effects()
        expected = len(wled_effects)

        line = mon.wait_for("[WLED] Loaded", timeout=5)
        self.assertIsNotNone(line, "No effects loaded log found")
        m = re.search(r"Loaded (\d+) effects", line)
        self.assertIsNotNone(m, f"Could not parse count from: {line}")
        actual = int(m.group(1))

        self.assertEqual(actual, expected,
            f"Device loaded {actual} effects but WLED reports {expected}")
        print(f"  PASS: {actual} effects (matches WLED)")


class TestPresetsCount(unittest.TestCase):
    """Preset count on device matches WLED API (skipping empty preset 0)."""

    def test_count_matches_wled(self):
        wled_presets = wled_get_presets()
        # Skip preset "0" (always empty placeholder) and presets with no name
        expected = sum(
            1 for k, v in wled_presets.items()
            if k != "0" and isinstance(v, dict) and v.get("n", "").strip()
        )

        line = mon.wait_for("[WLED] Loaded", timeout=5)
        # Get the presets line specifically
        for _, l in mon.lines:
            if "Loaded" in l and "preset" in l:
                line = l
                break

        self.assertIsNotNone(line, "No presets loaded log found")
        m = re.search(r"Loaded (\d+) preset", line)
        self.assertIsNotNone(m, f"Could not parse count from: {line}")
        actual = int(m.group(1))

        self.assertEqual(actual, expected,
            f"Device loaded {actual} presets but WLED reports {expected}")
        print(f"  PASS: {actual} presets (matches WLED)")


class TestColorSync(unittest.TestCase):
    """WLED color changes are reflected in device serial output."""

    def _set_and_verify(self, r, g, b, desc):
        t = mon.mark()
        wled_set(seg=[{"col": [[r, g, b, 0]]}])
        # WLED may push earlier state (initial sync or tearDown) after the mark.
        # Scan all color syncs received after the mark and accept the first that
        # matches the expected values within ±2 per channel.
        deadline = time.time() + 10
        match_line = None
        while time.time() < deadline:
            with mon._lock:
                candidates = [l for ts, l in mon.lines
                              if ts >= t and "[WLED] color synced" in l]
            for l in candidates:
                m = re.search(r"color synced: (\d+),(\d+),(\d+)", l)
                if m:
                    ar, ag, ab = int(m.group(1)), int(m.group(2)), int(m.group(3))
                    if abs(ar - r) <= 2 and abs(ag - g) <= 2 and abs(ab - b) <= 2:
                        match_line = l
                        break
            if match_line:
                break
            time.sleep(0.05)
        self.assertIsNotNone(match_line, f"No matching color sync after setting {desc}")
        print(f"  PASS: color sync {desc} → {match_line}")

    def test_red(self):
        self._set_and_verify(255, 0, 0, "red")

    def test_green(self):
        self._set_and_verify(0, 255, 0, "green")

    def test_blue(self):
        self._set_and_verify(0, 0, 255, "blue")

    def test_mixed(self):
        self._set_and_verify(128, 64, 200, "mixed")

    def tearDown(self):
        # Restore a neutral color and confirm the device received it before the next test marks
        t = mon.mark()
        wled_set(seg=[{"col": [[255, 140, 0, 0]]}])
        mon.wait_for("[WLED] color synced", timeout=8, after=t)
        time.sleep(0.3)


class TestPowerStateSync(unittest.TestCase):
    """WLED on/off state changes are reflected on the device."""

    def test_power_off_sync(self):
        wled_set(**{"on": True})   # ensure known state
        time.sleep(0.5)
        t = mon.mark()
        wled_set(**{"on": False})
        line = mon.wait_for("on_changed", timeout=8, after=t)
        self.assertIsNotNone(line, "No on_changed event after powering off")
        self.assertIn("on=0", line, f"Expected on=0 in: {line}")
        print(f"  PASS: {line}")

    def test_power_on_sync(self):
        wled_set(**{"on": False})  # ensure known state
        time.sleep(0.5)
        t = mon.mark()
        wled_set(**{"on": True})
        line = mon.wait_for("on_changed", timeout=8, after=t)
        self.assertIsNotNone(line, "No on_changed event after powering on")
        self.assertIn("on=1", line, f"Expected on=1 in: {line}")
        print(f"  PASS: {line}")

    @classmethod
    def tearDownClass(cls):
        wled_set(**{"on": True})   # leave lights on


class TestWledCommandsSent(unittest.TestCase):
    """
    Verify the device sends correctly formatted commands to WLED.
    These tests use the WLED HTTP API to set a known state, then read
    back WLED state after the device would have applied a change.
    Currently covers boot-time state sync; interactive tests (encoder,
    touch) require physical input and are noted for manual verification.
    """

    def test_websocket_state_received_on_connect(self):
        """Device should receive and log WLED state immediately after WebSocket connect."""
        # After boot, color synced should appear shortly after WS connect
        ws_line = mon.wait_for("[WLED] WebSocket connected", timeout=40)
        self.assertIsNotNone(ws_line, "WebSocket connection not seen in log")
        # State push (color or on/off) should follow within 3s
        t = next((ts for ts, l in mon.lines if "[WLED] WebSocket connected" in l), 0)
        color_or_state = mon.wait_for("[WLED] color synced", timeout=5, after=t)
        self.assertIsNotNone(color_or_state,
            "No color sync received after WebSocket connected — state push may have failed")
        print(f"  PASS: state received after connect: {color_or_state}")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    unittest.main(verbosity=2)
