#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

// ─── OLED ─────────────────────────────────────────────────────────────────────
// TTGO LoRa32 V1.0/V1.2: SSD1306 128×64, I2C on SDA=4, SCL=15, RST=16
#define OLED_WIDTH   128
#define OLED_HEIGHT   64
#define OLED_ADDR   0x3C
#define OLED_SDA       4
#define OLED_SCL      15
#define OLED_RST      16

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RST);

// ─── Receive buffer (ISR → loop bridge) ───────────────────────────────────────
static volatile bool    _status_available = false;
static esp_now_status_t _status_buffer;

// Last known state per spot (index 0 = spot 0x01)
static esp_now_status_t g_spot_state[NUM_SPOTS] = {};

// ─── ESP-NOW callbacks ────────────────────────────────────────────────────────
static void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(esp_now_status_t)) return;
    memcpy((void *)&_status_buffer, data, sizeof(esp_now_status_t));
    _status_available = true;
}

static void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac; (void)status;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void addPeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
}

static void sendCommand(uint8_t spot_id, uint8_t command, uint8_t brightness) {
    esp_now_cmd_t cmd;
    cmd.spot_id    = spot_id;
    cmd.command    = command;
    cmd.brightness = brightness;

    if (spot_id == 0xFF) {
        esp_now_send(BROADCAST_MAC, (uint8_t *)&cmd, sizeof(cmd));
    } else if (spot_id >= 0x01 && spot_id <= 0x0A) {
        esp_now_send(SPOT_MACS[spot_id - 1], (uint8_t *)&cmd, sizeof(cmd));
    } else {
        Serial.printf("[ERR] Invalid spot ID: 0x%02X\n", spot_id);
    }
}

// ─── OLED display ─────────────────────────────────────────────────────────────
// Shows the last received status for each known spot, 1 row per spot.
// Layout (128×64, 8px rows):
//   Row 0: "MASTER  <MAC tail>"
//   Row 1: "─────────────────"
//   Row 2..7: "S1 ON  255 24.3C"  (one spot per row, up to 6)

static void updateOLED() {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    // Header
    oled.setCursor(0, 0);
    oled.print("MASTER  LED SPOTS");
    oled.drawLine(0, 9, OLED_WIDTH - 1, 9, SSD1306_WHITE);

    // One row per spot that has reported in
    int row = 0;
    for (int i = 0; i < NUM_SPOTS && row < 6; i++) {
        const esp_now_status_t &s = g_spot_state[i];
        if (s.spot_id == 0) continue;  // Never heard from this spot

        int y = 12 + row * 9;
        oled.setCursor(0, y);

        const char *state_icon =
            (s.thermal_state == THERMAL_CRITICAL) ? "!" :
            (s.thermal_state == THERMAL_THROTTLE) ? "~" : " ";

        // "S1 ON  255 24.3C !"  (fits 128px at size=1, 6px/char = 21 chars)
        char buf[24];
        snprintf(buf, sizeof(buf), "S%d %s %3d %5.1fC%s",
                 s.spot_id,
                 s.is_on ? "ON " : "OFF",
                 s.brightness,
                 s.temperature,
                 state_icon);
        oled.print(buf);
        row++;
    }

    if (row == 0) {
        oled.setCursor(0, 28);
        oled.print("  Waiting for spots...");
    }

    oled.display();
}

// ─── Serial command parser ────────────────────────────────────────────────────
static void printHelp() {
    Serial.println("Commands:");
    Serial.println("  on  <spot|all> [bri]   Turn on (bri=0..255, default 255)");
    Serial.println("  off <spot|all>         Turn off");
    Serial.println("  dim <spot|all> <bri>   Set brightness 0..255");
    Serial.println("  status <spot|all>      Request status reply");
    Serial.println("  help                   This message");
    Serial.println("  <spot>: 1-10  or  all");
}

static uint8_t parseSpotId(const char *tok) {
    if (strcasecmp(tok, "all") == 0) return 0xFF;
    int n = atoi(tok);
    if (n >= 1 && n <= 10) return (uint8_t)n;
    return 0x00;
}

static void processLine(char *line) {
    char *tok = strtok(line, " \t\r\n");
    if (!tok) return;

    if (strcasecmp(tok, "help") == 0) { printHelp(); return; }

    if (strcasecmp(tok, "on") == 0) {
        char *id_tok = strtok(nullptr, " \t\r\n");
        if (!id_tok) { Serial.println("[ERR] Usage: on <spot|all> [bri]"); return; }
        uint8_t spot = parseSpotId(id_tok);
        if (!spot) { Serial.println("[ERR] Bad spot ID"); return; }
        char *bri_tok = strtok(nullptr, " \t\r\n");
        uint8_t bri = bri_tok ? (uint8_t)atoi(bri_tok) : 255;
        Serial.printf("[CMD] TURN_ON  spot=0x%02X  bri=%d\n", spot, bri);
        sendCommand(spot, CMD_TURN_ON, bri);
        return;
    }

    if (strcasecmp(tok, "off") == 0) {
        char *id_tok = strtok(nullptr, " \t\r\n");
        if (!id_tok) { Serial.println("[ERR] Usage: off <spot|all>"); return; }
        uint8_t spot = parseSpotId(id_tok);
        if (!spot) { Serial.println("[ERR] Bad spot ID"); return; }
        Serial.printf("[CMD] TURN_OFF  spot=0x%02X\n", spot);
        sendCommand(spot, CMD_TURN_OFF, 0);
        return;
    }

    if (strcasecmp(tok, "dim") == 0) {
        char *id_tok  = strtok(nullptr, " \t\r\n");
        char *bri_tok = strtok(nullptr, " \t\r\n");
        if (!id_tok || !bri_tok) { Serial.println("[ERR] Usage: dim <spot|all> <bri>"); return; }
        uint8_t spot = parseSpotId(id_tok);
        if (!spot) { Serial.println("[ERR] Bad spot ID"); return; }
        uint8_t bri = (uint8_t)atoi(bri_tok);
        Serial.printf("[CMD] SET_BRIGHTNESS  spot=0x%02X  bri=%d\n", spot, bri);
        sendCommand(spot, CMD_SET_BRIGHTNESS, bri);
        return;
    }

    if (strcasecmp(tok, "status") == 0) {
        char *id_tok = strtok(nullptr, " \t\r\n");
        if (!id_tok) { Serial.println("[ERR] Usage: status <spot|all>"); return; }
        uint8_t spot = parseSpotId(id_tok);
        if (!spot) { Serial.println("[ERR] Bad spot ID"); return; }
        Serial.printf("[CMD] REQUEST_STATUS  spot=0x%02X\n", spot);
        sendCommand(spot, CMD_REQUEST_STATUS, 0);
        return;
    }

    Serial.printf("[ERR] Unknown command: '%s'  (type 'help')\n", tok);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disable brownout reset
    Serial.begin(115200);
    delay(200);

    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(10);
    digitalWrite(OLED_RST, HIGH);

    Wire.begin(OLED_SDA, OLED_SCL);
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[WARN] SSD1306 not found — continuing without OLED");
    } else {
        oled.clearDisplay();
        oled.setTextSize(1);
        oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0, 0);
        oled.print("Booting...");
        oled.display();
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("[BOOT] Master — MAC: %s\n", WiFi.macAddress().c_str());

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERR] esp_now_init failed — halting");
        while (true) delay(1000);
    }

    esp_now_register_recv_cb(onDataReceive);
    esp_now_register_send_cb(onDataSent);

    addPeer(BROADCAST_MAC);

    for (int i = 0; i < NUM_SPOTS; i++) {
        bool is_broadcast = true;
        for (int b = 0; b < 6; b++) {
            if (SPOT_MACS[i][b] != 0xFF) { is_broadcast = false; break; }
        }
        if (!is_broadcast) addPeer(SPOT_MACS[i]);
    }

    memset(g_spot_state, 0, sizeof(g_spot_state));

    updateOLED();
    Serial.println("[BOOT] Ready.  Type 'help' for commands.");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
static char    _line_buf[64];
static uint8_t _line_len = 0;

void loop() {
    // 1. Process received status packets
    if (_status_available) {
        _status_available = false;
        esp_now_status_t s;
        memcpy(&s, (const void *)&_status_buffer, sizeof(s));

        if (s.spot_id >= 0x01 && s.spot_id <= 0x0A) {
            const esp_now_status_t &prev = g_spot_state[s.spot_id - 1];

            // Boot packet: brightness=0, is_on=false, temp=0 — spot just rebooted.
            // Re-send the last known command so it restores its state.
            bool is_boot_packet = (!s.is_on && s.brightness == 0 && s.temperature == 0.0f);
            if (is_boot_packet && prev.spot_id != 0) {
                Serial.printf("[INFO] Spot 0x%02X rebooted — restoring state (%s bri=%d)\n",
                              s.spot_id, prev.is_on ? "ON" : "OFF", prev.brightness);
                if (prev.is_on) {
                    sendCommand(s.spot_id, CMD_TURN_ON, prev.brightness);
                } else {
                    sendCommand(s.spot_id, CMD_TURN_OFF, 0);
                }
            }

            g_spot_state[s.spot_id - 1] = s;
        }

        // Serial log
        const char *state_str =
            (s.thermal_state == THERMAL_CRITICAL) ? "CRITICAL" :
            (s.thermal_state == THERMAL_THROTTLE) ? "THROTTLE" : "normal";
        Serial.printf("[STATUS] spot=0x%02X  %s  bri=%d  temp=%.1f°C  state=%s\n",
                      s.spot_id, s.is_on ? "ON " : "OFF",
                      s.brightness, s.temperature, state_str);

        updateOLED();
    }

    // 2. Accumulate serial input, process on newline
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_line_len > 0) {
                _line_buf[_line_len] = '\0';
                processLine(_line_buf);
                _line_len = 0;
            }
        } else if (_line_len < sizeof(_line_buf) - 1) {
            _line_buf[_line_len++] = c;
        }
    }
}
