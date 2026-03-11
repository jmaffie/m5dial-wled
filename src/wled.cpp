#include "wled.h"
#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>

static WebSocketsClient ws;
static volatile bool ws_connected    = false;
static volatile int  pending_bri     = -1;
static volatile bool pending_toggle  = false;
static char          pending_raw[128] = {0};
static volatile bool has_pending_raw  = false;
static TaskHandle_t  wled_task_handle = NULL;

// Last-used RGB color (used by wled_set_color to restore at full value).
// Default: warm amber. Updated by wled_store_color() when user picks a color.
static uint8_t stored_r = 255, stored_g = 100, stored_b = 0;

// Current display color for the UI arc — tracks what the LEDs are actually showing.
// White mode uses warm white (0xFFEECC). Updated by set_white/set_color and WebSocket sync.
static volatile uint8_t display_r = 255, display_g = 238, display_b = 204;
static volatile bool     color_dirty = true;  // true on boot so arc initialises

// Effects list — fetched from WLED on boot via HTTP /json/effects
#define MAX_EFFECTS          200
#define MAX_EFFECT_NAME_LEN   32
static char effect_storage[MAX_EFFECTS][MAX_EFFECT_NAME_LEN];
static int  effect_count = 0;
static volatile int current_fx = 0;

// Presets list — fetched from WLED on boot via HTTP /json/presets
#define MAX_PRESETS          50
#define MAX_PRESET_NAME_LEN  32
static char preset_storage[MAX_PRESETS][MAX_PRESET_NAME_LEN];
static int  preset_ids[MAX_PRESETS];
static int  preset_count = 0;
static volatile int current_ps = -1;

// On/off state — parsed from WebSocket state messages
static volatile bool wled_on_state = true;
static volatile bool on_dirty      = true;  // true on boot so UI initialises

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            ws_connected = true;
            Serial.println("[WLED] WebSocket connected");
            break;
        case WStype_DISCONNECTED:
            ws_connected = false;
            Serial.println("[WLED] WebSocket disconnected");
            break;
        case WStype_TEXT: {
            // WLED broadcasts state JSON on connect and after each change.
            const char *msg = (char *)payload;

            // Parse "col":[[r,g,b,w],...] from segment 0.
            const char *p = strstr(msg, "\"col\":[[");
            if (p) {
                int r, g, b, w = 0;
                int n = sscanf(p, "\"col\":[[%d,%d,%d,%d", &r, &g, &b, &w);
                if (n >= 3) {
                    if (r | g | b) {
                        // Color mode — remember hue and set display to full-value color
                        stored_r = r; stored_g = g; stored_b = b;
                        uint8_t m = max(r, max(g, b));
                        display_r = (m > 0) ? r * 255 / m : 0;
                        display_g = (m > 0) ? g * 255 / m : 0;
                        display_b = (m > 0) ? b * 255 / m : 0;
                        Serial.printf("[WLED] color synced: %d,%d,%d\n", r, g, b);
                    } else if (w > 0) {
                        // White mode — arc shows warm white
                        display_r = 255; display_g = 238; display_b = 204;
                    }
                    color_dirty = true;
                }
            }

            // Parse current effect index "fx": from segment 0.
            const char *fx_p = strstr(msg, "\"fx\":");
            if (fx_p) {
                int fx;
                if (sscanf(fx_p, "\"fx\":%d", &fx) == 1)
                    current_fx = fx;
            }

            // Parse current preset "ps": from top-level state.
            const char *ps_p = strstr(msg, "\"ps\":");
            if (ps_p) {
                int ps;
                if (sscanf(ps_p, "\"ps\":%d", &ps) == 1)
                    current_ps = ps;
            }

            // Parse on/off state "on": from top-level state.
            const char *on_p = strstr(msg, "\"on\":");
            if (on_p) {
                bool was = wled_on_state;
                if      (strncmp(on_p + 5, "true",  4) == 0) wled_on_state = true;
                else if (strncmp(on_p + 5, "false", 5) == 0) wled_on_state = false;
                if (wled_on_state != was) on_dirty = true;
            }
            break;
        }
        default:
            break;
    }
}

static void ws_send(const char *msg) {
    if (ws_connected) {
        ws.sendTXT(msg);
        Serial.printf("[WLED] sent: %s\n", msg);
    }
}

static void wled_task(void *param) {
    wled_task_handle = xTaskGetCurrentTaskHandle();

    ws.begin(WLED_HOST, 80, "/ws");
    ws.onEvent(ws_event);
    ws.setReconnectInterval(3000);

    while (true) {
        ws.loop();

        // Wait for notification, then flush pending commands
        if (xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(10)) == pdTRUE || pending_bri >= 0 || pending_toggle) {
            bool tog = pending_toggle;
            int  bri = pending_bri;
            pending_toggle = false;
            pending_bri    = -1;

            if (tog) ws_send("{\"on\":\"t\"}");
            if (bri >= 0) {
                char msg[24];
                snprintf(msg, sizeof(msg), "{\"bri\":%d}", bri);
                ws_send(msg);
            }
            if (has_pending_raw) {
                ws_send(pending_raw);
                has_pending_raw = false;
            }
        }
    }
}

void wled_fetch_effects() {
    HTTPClient http;
    char url[64];
    snprintf(url, sizeof(url), "http://%s/json/effects", WLED_HOST);
    http.begin(url);
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        const char *p = body.c_str();
        effect_count = 0;
        // Response is a bare JSON array: ["Solid","Blink",...].
        // Walk all quoted strings — the only strings in this response are effect names.
        while ((p = strchr(p, '"')) != NULL && effect_count < MAX_EFFECTS) {
            p++;  // skip opening quote
            const char *end = strchr(p, '"');
            if (!end) break;
            int len = end - p;
            if (len >= MAX_EFFECT_NAME_LEN) len = MAX_EFFECT_NAME_LEN - 1;
            memcpy(effect_storage[effect_count], p, len);
            effect_storage[effect_count][len] = '\0';
            effect_count++;
            p = end + 1;
        }
        Serial.printf("[WLED] Loaded %d effects\n", effect_count);
    } else {
        Serial.printf("[WLED] Effects fetch failed: %d\n", code);
    }
    http.end();
}

int wled_get_effects_count() { return effect_count; }

const char *wled_get_effect_name(int index) {
    if (index < 0 || index >= effect_count) return "";
    return effect_storage[index];
}

int wled_get_current_fx() { return current_fx; }

void wled_set_effect(int fx_index) {
    char msg[32];
    snprintf(msg, sizeof(msg), "{\"seg\":[{\"fx\":%d}]}", fx_index);
    wled_send_raw(msg);
}

void wled_fetch_presets() {
    // Correct endpoint is /presets.json (not /json/presets which returns error:4).
    // Format: {"0":{},"1":{...,"seg":[{"n":"",..."col":...}],"n":"White"},...}
    // Preset objects contain nested segment objects with their own "n":"" fields.
    // A depth-tracking forward parser distinguishes top-level "n" (depth 1) from
    // segment "n" (depth 2+), avoiding false matches from the backward-scan approach.
    HTTPClient http;
    char url[64];
    snprintf(url, sizeof(url), "http://%s/presets.json", WLED_HOST);
    http.begin(url);
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        const char *p = body.c_str();
        preset_count = 0;

        while (*p && preset_count < MAX_PRESETS) {
            // Find a numeric string key at the top level: '"' DIGITS '"' ':' '{'
            if (*p != '"') { p++; continue; }
            p++;
            if (*p < '0' || *p > '9') { continue; }

            const char *id_start = p;
            while (*p >= '0' && *p <= '9') p++;
            if (*p != '"' || *(p+1) != ':' || *(p+2) != '{') continue;

            int preset_id = atoi(id_start);
            p += 3;  // skip '":{'

            // Scan the preset object with depth tracking.
            // Extract "n":"VALUE" only at depth 1 (top level of preset object).
            // Any other string or nested {/} is skipped properly.
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '"') {
                    if (depth == 1 && strncmp(p, "\"n\":\"", 5) == 0) {
                        // Preset name at top level — extract it
                        const char *ns = p + 5;
                        const char *ne = ns;
                        while (*ne && *ne != '"') ne++;
                        if (ne > ns) {  // skip empty names (segment "n":"")
                            int len = ne - ns;
                            if (len >= MAX_PRESET_NAME_LEN) len = MAX_PRESET_NAME_LEN - 1;
                            memcpy(preset_storage[preset_count], ns, len);
                            preset_storage[preset_count][len] = '\0';
                            preset_ids[preset_count] = preset_id;
                            preset_count++;
                        }
                        p = *ne ? ne + 1 : ne;
                    } else {
                        // Skip string (key or value we don't need)
                        p++;
                        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
                        if (*p) p++;
                    }
                } else if (*p == '{') { depth++; p++; }
                  else if (*p == '}') { depth--; p++; }
                  else                { p++; }
            }
        }
        Serial.printf("[WLED] Loaded %d presets\n", preset_count);
    } else {
        Serial.printf("[WLED] Presets fetch failed: %d\n", code);
    }
    http.end();
}

int         wled_get_presets_count()        { return preset_count; }
const char *wled_get_preset_name(int index) { return (index >= 0 && index < preset_count) ? preset_storage[index] : ""; }
int         wled_get_preset_id(int index)   { return (index >= 0 && index < preset_count) ? preset_ids[index] : -1; }
int         wled_get_current_ps()           { return current_ps; }

void wled_set_preset(int preset_id) {
    char msg[24];
    snprintf(msg, sizeof(msg), "{\"ps\":%d}", preset_id);
    wled_send_raw(msg);
}

void wled_init() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);
        Serial.printf("\n[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
        wled_fetch_effects();
        wled_fetch_presets();
    } else {
        Serial.println("\n[WiFi] Connection failed.");
    }

    xTaskCreatePinnedToCore(wled_task, "wled", 4096, NULL, 1, NULL, 0);
}

bool wled_connected() {
    return ws_connected;
}

void wled_toggle() {
    pending_toggle = true;
    if (wled_task_handle) xTaskNotify(wled_task_handle, 0, eNoAction);
}

void wled_set_brightness(uint8_t brightness) {
    pending_bri = brightness;
    if (wled_task_handle) xTaskNotify(wled_task_handle, 0, eNoAction);
}

void wled_send_raw(const char *json) {
    strncpy(pending_raw, json, sizeof(pending_raw) - 1);
    has_pending_raw = true;
    if (wled_task_handle) xTaskNotify(wled_task_handle, 0, eNoAction);
}

void wled_set_white() {
    // W=255, RGB=0, effect=Solid (fx:0). Does not touch overall brightness (bri).
    display_r = 255; display_g = 238; display_b = 204;
    color_dirty = true;
    wled_send_raw("{\"seg\":[{\"col\":[[0,0,0,255]],\"fx\":0}]}");
}

void wled_store_color(uint8_t r, uint8_t g, uint8_t b) {
    stored_r = r; stored_g = g; stored_b = b;
}

bool wled_color_changed() {
    if (color_dirty) { color_dirty = false; return true; }
    return false;
}

void wled_get_display_color(uint8_t *r, uint8_t *g, uint8_t *b) {
    *r = display_r; *g = display_g; *b = display_b;
}

void wled_set_color() {
    // Restore stored color at full value (max channel scaled to 255), W=0.
    // Preserves hue and saturation; only sets value to 100%.
    uint8_t r = stored_r, g = stored_g, b = stored_b;
    uint8_t m = max(r, max(g, b));
    if (m > 0 && m < 255) { r = r * 255 / m; g = g * 255 / m; b = b * 255 / m; }
    display_r = r; display_g = g; display_b = b;
    color_dirty = true;
    char msg[48];
    snprintf(msg, sizeof(msg), "{\"seg\":[{\"col\":[[%d,%d,%d,0]]}]}", r, g, b);
    wled_send_raw(msg);
}

bool wled_is_on() { return wled_on_state; }

bool wled_on_changed() {
    if (on_dirty) { on_dirty = false; return true; }
    return false;
}

void wled_set_power(bool on) {
    if (on) {
        pending_toggle = true;
    } else {
        pending_bri = 0;
    }
    if (wled_task_handle) xTaskNotify(wled_task_handle, 0, eNoAction);
}
