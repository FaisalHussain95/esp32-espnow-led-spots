#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <HTTPUpdate.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

// ─── PMK (loaded from NVS on boot) ────────────────────────────────────────────
static uint8_t _pmk[16] = {};

// Required slave firmware version — always matches compiled FW_VERSION
static const uint8_t _required_fw_version = FW_VERSION;

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
static volatile bool    _status_available   = false;
static volatile bool    _ota_fail_available = false;

static esp_now_status_t  _status_buffer;

// Runtime MAC table: index 0 = spot 0x01, populated on HELLO
static uint8_t g_spot_macs[NUM_SPOTS][6];

typedef struct {
    uint8_t mac[6];
    uint8_t fw_version;
    uint8_t spot_id;
} hello_event_t;

// Ring buffer for HELLO events — sized to absorb a full simultaneous boot burst.
#define HELLO_RING_SIZE 16
static hello_event_t     _hello_ring[HELLO_RING_SIZE];
static volatile uint8_t  _hello_head = 0;  // ISR writes here
static volatile uint8_t  _hello_tail = 0;  // loop reads here

typedef struct {
    uint8_t spot_id;
    uint8_t attempt;
} ota_fail_event_t;
static ota_fail_event_t _ota_fail_buffer;

// Last known state per spot (index 0 = spot 0x01)
static esp_now_status_t g_spot_state[NUM_SPOTS] = {};

// ─── Forward declarations ──────────────────────────────────────────────────────
static void addPeer(const uint8_t *mac);
static void sendCommand(uint8_t spot_id, uint8_t command, uint8_t brightness, uint16_t param = 0);
static void uart2_send_handshake(uint8_t spot_id, uint8_t fw_version);

// ─── ESP-NOW callbacks ────────────────────────────────────────────────────────
static void onDataReceive(const uint8_t *mac, const uint8_t *data, int len) {
    if (len < (int)sizeof(espnow_header_t)) return;

    const espnow_header_t *hdr = (const espnow_header_t *)data;

    switch (hdr->msg_type) {
        case MSG_STATUS: {
            if (len < (int)sizeof(espnow_status_packet_t)) return;
            const espnow_status_packet_t *pkt = (const espnow_status_packet_t *)data;
            memcpy((void *)&_status_buffer, &pkt->status, sizeof(esp_now_status_t));
            _status_available = true;
            break;
        }
        case MSG_HELLO: {
            uint8_t next = (_hello_head + 1) % HELLO_RING_SIZE;
            if (next != _hello_tail) {  // drop if full (shouldn't happen with 8 slots)
                memcpy(_hello_ring[_hello_head].mac, hdr->mac, 6);
                _hello_ring[_hello_head].fw_version = hdr->fw_version;
                _hello_ring[_hello_head].spot_id    = hdr->attempt;
                _hello_head = next;
            }
            break;
        }
        case MSG_OTA_FAILED: {
            if (len < (int)sizeof(espnow_status_packet_t)) {
                // Just header — use mac to find spot_id
                // spot_id is encoded in attempt field (repurposed as spot_id here)
                _ota_fail_buffer.spot_id = hdr->attempt;
                _ota_fail_buffer.attempt = 0;
            } else {
                const espnow_status_packet_t *pkt = (const espnow_status_packet_t *)data;
                _ota_fail_buffer.spot_id = pkt->status.spot_id;
                _ota_fail_buffer.attempt = hdr->attempt;
            }
            _ota_fail_available = true;
            break;
        }
        default:
            break;
    }
}

static void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac; (void)status;
}

// ─── Provisioning: PMK from Kconfig → NVS ────────────────────────────────────
static bool hexCharToByte(char c, uint8_t &out) {
    if (c >= '0' && c <= '9') { out = c - '0';        return true; }
    if (c >= 'a' && c <= 'f') { out = c - 'a' + 10;   return true; }
    if (c >= 'A' && c <= 'F') { out = c - 'A' + 10;   return true; }
    return false;
}

static void provisioning_init() {
    Preferences prefs;

#if defined(CONFIG_PROV_FORCE_RESET) && CONFIG_PROV_FORCE_RESET
    // Force-overwrite NVS — use when changing the PMK after first flash.
    // Set -DCONFIG_PROV_FORCE_RESET=1 in platformio.ini build_flags, flash once,
    // then remove the flag and reflash to return to normal NVS-first behaviour.
    Serial.println("[PROV] FORCE RESET — clearing NVS credentials.");
    prefs.begin("espnow", false); prefs.clear(); prefs.end();
#endif

    prefs.begin("espnow", false);

    // Check if PMK already stored
    if (prefs.getBytesLength("pmk") == 16) {
        prefs.getBytes("pmk", _pmk, 16);
        Serial.println("[PROV] PMK loaded from NVS.");
    } else {
        // Write PMK from build_flags CONFIG_ESPNOW_PMK (32 hex chars → 16 bytes)
        const char *hex = CONFIG_ESPNOW_PMK;
        size_t hexlen = strlen(hex);
        if (hexlen != 32) {
            Serial.println("[ERR] CONFIG_ESPNOW_PMK must be 32 hex chars — check build_flags");
            while (true) delay(1000);
        }
        for (int i = 0; i < 16; i++) {
            uint8_t hi, lo;
            if (!hexCharToByte(hex[i * 2], hi) || !hexCharToByte(hex[i * 2 + 1], lo)) {
                Serial.println("[ERR] CONFIG_ESPNOW_PMK contains invalid hex — check build_flags");
                while (true) delay(1000);
            }
            _pmk[i] = (hi << 4) | lo;
        }
        prefs.putBytes("pmk", _pmk, 16);
        Serial.println("[PROV] PMK written to NVS from build_flags.");
    }
    prefs.end();
    Serial.print("[PROV] PMK: ");
    for (int i = 0; i < 16; i++) Serial.printf("%02X", _pmk[i]);
    Serial.println();

    // ── WiFi credentials (for OTA HTTP) ───────────────────────────────────────
    prefs.begin("wifi", false);
    if (!prefs.isKey("ssid")) {
        prefs.putString("ssid",     CONFIG_WIFI_SSID);
        prefs.putString("password", CONFIG_WIFI_PASSWORD);
        Serial.println("[PROV] WiFi credentials written to NVS from build_flags.");
    } else {
        Serial.println("[PROV] WiFi credentials loaded from NVS.");
    }
    prefs.end();

    Serial.printf("[PROV] Required slave fw_version: %d\n", _required_fw_version);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void addPeer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;

    // Check if this is the broadcast peer — broadcast cannot be encrypted
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) { is_broadcast = false; break; }
    }
    if (is_broadcast) {
        peer.encrypt = false;
    } else {
        peer.encrypt = true;
        memcpy(peer.lmk, _pmk, 16);  // use PMK as LMK (shared key approach)
    }
    esp_now_add_peer(&peer);
}

static void buildCmdPacket(espnow_cmd_packet_t &pkt, uint8_t spot_id, uint8_t command, uint8_t brightness, uint16_t param = 0) {
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.msg_type   = MSG_CMD;
    pkt.header.fw_version = FW_VERSION;
    pkt.header.attempt    = 0;
    // Leave mac[] zero — master MAC not needed by spots
    pkt.cmd.spot_id    = spot_id;
    pkt.cmd.command    = command;
    pkt.cmd.brightness = brightness;
    pkt.cmd.param      = param;
}

static void sendCommand(uint8_t spot_id, uint8_t command, uint8_t brightness, uint16_t param) {
    espnow_cmd_packet_t pkt;
    buildCmdPacket(pkt, spot_id, command, brightness, param);

    if (spot_id == 0xFF) {
        esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
    } else if (spot_id >= 0x01 && spot_id <= NUM_SPOTS) {
        const uint8_t *mac = g_spot_macs[spot_id - 1];
        if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0) {
            Serial.printf("[ERR] Spot 0x%02X MAC not known yet — awaiting HELLO\n", spot_id);
            return;
        }
        esp_now_send(mac, (uint8_t *)&pkt, sizeof(pkt));
    } else {
        Serial.printf("[ERR] Invalid spot ID: 0x%02X\n", spot_id);
    }
}

static void sendWhois() {
    espnow_header_t pkt = {};
    pkt.msg_type   = MSG_WHOIS;
    pkt.fw_version = FW_VERSION;
    esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
    Serial.println("[BOOT] WHOIS broadcast — waiting for spots to re-announce");
}

static void sendOtaBroadcast(uint8_t target_version) {
    espnow_ota_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.msg_type      = MSG_OTA_NOW;
    pkt.header.fw_version    = FW_VERSION;
    pkt.header.attempt       = 0;
    pkt.target_version       = target_version;
    esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
    Serial.printf("[OTA] Broadcast OTA_NOW → target_version=%d\n", target_version);
}

static void sendAck(const uint8_t *mac) {
    espnow_header_t ack = {};
    ack.msg_type   = MSG_ACK;
    ack.fw_version = FW_VERSION;
    esp_now_send(mac, (uint8_t *)&ack, sizeof(ack));
}

static void sendReject(const uint8_t *mac, uint8_t target_version) {
    espnow_ota_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.msg_type   = MSG_REJECT;
    pkt.header.fw_version = FW_VERSION;
    pkt.target_version    = target_version;
    esp_now_send(mac, (uint8_t *)&pkt, sizeof(pkt));
}

// ─── HELLO handler (called from loop, not ISR) ────────────────────────────────
static void handleHello(const uint8_t *mac, uint8_t spot_fw_version, uint8_t spot_id) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.printf("[HELLO] spot=0x%02X  MAC=%s  fw=%d  required=%d\n",
                  spot_id, mac_str, spot_fw_version, _required_fw_version);

    // Dynamically register peer with encryption if not already known
    if (!esp_now_is_peer_exist(mac)) {
        addPeer(mac);
        Serial.printf("[HELLO] New peer registered: %s\n", mac_str);
    }

    // Store spot_id → MAC mapping in RAM
    if (spot_id >= 0x01 && spot_id <= NUM_SPOTS) {
        memcpy(g_spot_macs[spot_id - 1], mac, 6);
        g_spot_state[spot_id - 1].spot_id = spot_id;
    }

    if (spot_fw_version >= _required_fw_version) {
        sendAck(mac);
        Serial.printf("[HELLO] → ACK (fw ok)\n");
    } else {
        sendReject(mac, _required_fw_version);
        Serial.printf("[HELLO] → REJECT + OTA target=%d\n", _required_fw_version);
    }

    // Notify bridge (UART2) of handshake
    uart2_send_handshake(spot_id, spot_fw_version);
}

// ─── OLED display ─────────────────────────────────────────────────────────────
// Shows the last received status for each known spot, 1 row per spot.
// Layout (128×64, 8px rows):
//   Row 0: "MASTER  LED SPOTS"
//   Row 1: separator line
//   Row 2..7: "S1 ON  255 24.3C!"  (one spot per row, up to 6)

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
            (s.thermal_state == STATE_PULSE)      ? "*" :
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
        oled.print("Waiting for spots...");
    }

    oled.display();
}

// ─── OTA (self-update from GitHub releases) ───────────────────────────────────
// URL: https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v<N>/master_ttgo_v<N>.bin
#define MASTER_OTA_URL_FMT \
    "https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v%d/master_ttgo_v%d.bin"

static void espnow_reinit() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERR] ESP-NOW re-init failed after OTA");
        return;
    }
    esp_now_set_pmk(_pmk);
    esp_now_register_recv_cb(onDataReceive);
    esp_now_register_send_cb(onDataSent);
    addPeer(BROADCAST_MAC);
    // Re-register any known spot peers
    for (int i = 0; i < NUM_SPOTS; i++) {
        if (g_spot_macs[i][0] || g_spot_macs[i][1] || g_spot_macs[i][2]) {
            addPeer(g_spot_macs[i]);
        }
    }
    Serial.println("[ESPNOW] Re-initialised after OTA attempt");
    sendWhois();
}

static void master_ota_start(uint8_t target_version) {
    Serial.printf("[OTA] Master self-update to v%d\n", target_version);

    // Read WiFi creds from NVS
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid     = prefs.getString("ssid",     "");
    String password = prefs.getString("password", "");
    prefs.end();

    if (ssid.isEmpty()) {
        Serial.println("[OTA] No WiFi credentials in NVS — cannot self-update");
        espnow_reinit();
        return;
    }

    // Temporarily stop ESP-NOW and connect to WiFi
    esp_now_deinit();
    WiFi.mode(WIFI_STA);

    char url[200];
    snprintf(url, sizeof(url), MASTER_OTA_URL_FMT, target_version, target_version);

    httpUpdate.setLedPin(LED_BUILTIN, LOW);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    for (int attempt = 1; attempt <= 5; attempt++) {
        Serial.printf("[OTA] Attempt %d/5\n", attempt);

        // Connect (or reconnect) to WiFi each attempt
        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[OTA] Connecting to WiFi '%s'", ssid.c_str());
            WiFi.begin(ssid.c_str(), password.c_str());
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
                delay(500);
                Serial.print(".");
            }
            Serial.println();
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[OTA] WiFi connect failed — SSID='%s' password='%s'\n",
                          ssid.c_str(), password.c_str());
            if (attempt < 5) {
                Serial.println("[OTA] Retrying in 10s...");
                delay(10000);
            }
            continue;
        }

        // Force Cloudflare DNS
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                    IPAddress(1, 1, 1, 1), IPAddress(1, 0, 0, 1));

        WiFiClientSecure client;
        client.setInsecure();
        client.setHandshakeTimeout(30);
        t_httpUpdate_return ret = httpUpdate.update(client, url);

        switch (ret) {
            case HTTP_UPDATE_OK:
                Serial.println("[OTA] Update OK — rebooting");
                return;
            case HTTP_UPDATE_NO_UPDATES:
                Serial.println("[OTA] No update available");
                espnow_reinit();
                return;
            case HTTP_UPDATE_FAILED:
                Serial.printf("[OTA] Attempt %d failed: %s\n", attempt,
                              httpUpdate.getLastErrorString().c_str());
                if (attempt < 5) {
                    Serial.println("[OTA] Retrying in 10s...");
                    delay(10000);
                }
                break;
        }
    }

    Serial.println("[OTA] All attempts failed — staying on current firmware.");
    espnow_reinit();
}

// ─── UART2 bridge (to WiFi ESP32-B) ──────────────────────────────────────────
// Frame format (command, 6 bytes):  0xAA 0x01 spot_id brightness command 0x55
// Frame format (status,  9 bytes):  0xAA 0x02 spot_id brightness temp_hi temp_lo thermal_state is_on 0x55
//   temperature encoded as int16 in 0.1°C units (e.g. 24.3°C → 243)
// Frame format (version, 4 bytes):  0xAA 0x03 target_version 0x55
#define UART2_START     0xAA
#define UART2_END       0x55
#define UART2_CMD       0x01
#define UART2_STATUS    0x02
#define UART2_VERSION   0x03
#define UART2_HANDSHAKE 0x04

// Frame: 0xAA 0x04 spot_id fw_version 0x55
static void uart2_send_handshake(uint8_t spot_id, uint8_t fw_version) {
    uint8_t frame[5] = { UART2_START, UART2_HANDSHAKE, spot_id, fw_version, UART2_END };
    Serial2.write(frame, 5);
}

static void uart2_send_status(const esp_now_status_t &s) {
    int16_t temp_raw = (int16_t)(s.temperature * 10.0f);
    uint8_t frame[9];
    frame[0] = UART2_START;
    frame[1] = UART2_STATUS;
    frame[2] = s.spot_id;
    frame[3] = s.brightness;
    frame[4] = (uint8_t)(temp_raw >> 8);
    frame[5] = (uint8_t)(temp_raw & 0xFF);
    frame[6] = s.thermal_state;
    frame[7] = s.is_on ? 1 : 0;
    frame[8] = UART2_END;
    Serial2.write(frame, 9);
}

static uint8_t _u2buf[8];
static uint8_t _u2len = 0;

static void uart2_poll() {
    while (Serial2.available()) {
        uint8_t b = (uint8_t)Serial2.read();

        if (_u2len == 0) {
            if (b == UART2_START) _u2buf[_u2len++] = b;
            continue;
        }
        _u2buf[_u2len++] = b;

        if (_u2len == 4 && _u2buf[1] == UART2_VERSION) {
            // Version frame: 0xAA 0x03 target_version 0x55
            if (_u2buf[3] == UART2_END) {
                uint8_t target = _u2buf[2];
                Serial.printf("[UART2] VERSION target=%d (master FW=%d)\n", target, FW_VERSION);
                if (FW_VERSION < target) {
                    master_ota_start(target);
                } else {
                    sendWhois();
                }
            }
            _u2len = 0;
        } else if (_u2len == 6) {
            // Command frame: 0xAA 0x01 spot_id bri cmd 0x55
            if (_u2buf[1] == UART2_CMD && _u2buf[5] == UART2_END) {
                uint8_t spot_id = _u2buf[2];
                uint8_t bri     = _u2buf[3];
                uint8_t command = _u2buf[4];
                Serial.printf("[UART2] CMD spot=0x%02X cmd=0x%02X bri=%d\n", spot_id, command, bri);
                sendCommand(spot_id, command, bri);
            }
            _u2len = 0;
        } else if (_u2len >= sizeof(_u2buf)) {
            _u2len = 0;  // overrun — discard
        }
    }
}

// ─── Serial command parser ────────────────────────────────────────────────────
static void printHelp() {
    Serial.println("Commands:");
    Serial.println("  on      <spot|all> [bri]        Turn on (bri=0..255, default 255)");
    Serial.println("  off     <spot|all>              Turn off");
    Serial.println("  dim     <spot|all> <bri>        Set brightness 0..255");
    Serial.println("  pulse   <spot|all> [bri] [ms]   Pulse up then down (default bri=100, ms=500)");
    Serial.println("  status  <spot|all>              Request status reply");
    Serial.println("  version <N>                     Self-OTA if master outdated, then WHOIS → spots update");
    Serial.println("  help                            This message");
    Serial.println("  <spot>: 1-254  or  all");
}

static uint8_t parseSpotId(const char *tok) {
    if (strcasecmp(tok, "all") == 0) return 0xFF;
    int n = atoi(tok);
    if (n >= 1 && n <= NUM_SPOTS) return (uint8_t)n;
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

    if (strcasecmp(tok, "pulse") == 0) {
        char *id_tok  = strtok(nullptr, " \t\r\n");
        if (!id_tok) { Serial.println("[ERR] Usage: pulse <spot|all> [bri] [ms]"); return; }
        uint8_t spot = parseSpotId(id_tok);
        if (!spot) { Serial.println("[ERR] Bad spot ID"); return; }
        char *bri_tok = strtok(nullptr, " \t\r\n");
        char *ms_tok  = strtok(nullptr, " \t\r\n");
        uint8_t bri   = bri_tok ? (uint8_t)atoi(bri_tok) : 100;
        uint16_t dur  = ms_tok  ? (uint16_t)atoi(ms_tok)  : 500;
        Serial.printf("[CMD] PULSE  spot=0x%02X  bri=%d  ms=%d\n", spot, bri, dur);
        sendCommand(spot, CMD_PULSE, bri, dur);
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

    if (strcasecmp(tok, "version") == 0) {
        char *ver_tok = strtok(nullptr, " \t\r\n");
        if (!ver_tok) { Serial.println("[ERR] Usage: version <N>"); return; }
        uint8_t target = (uint8_t)atoi(ver_tok);
        if (target == 0) { Serial.println("[ERR] Version must be >= 1"); return; }
        Serial.printf("[CMD] VERSION target=%d (master FW=%d)\n", target, FW_VERSION);
        if (FW_VERSION < target) {
            master_ota_start(target);
        } else {
            sendWhois();
        }
        return;
    }

    Serial.printf("[ERR] Unknown command: '%s'  (type 'help')\n", tok);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disable brownout reset
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, PIN_UART2_RX, PIN_UART2_TX);
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

    // Load PMK from NVS (write from Kconfig on first boot)
    provisioning_init();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[BOOT] Master — MAC: %s  CH: %d\n",
                  WiFi.macAddress().c_str(), WiFi.channel());

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ERR] esp_now_init failed — halting");
        while (true) delay(1000);
    }

    // Set shared PMK for encrypted communication
    if (esp_now_set_pmk(_pmk) != ESP_OK) {
        Serial.println("[WARN] esp_now_set_pmk failed");
    }

    esp_now_register_recv_cb(onDataReceive);
    esp_now_register_send_cb(onDataSent);

    // Broadcast peer (unencrypted — needed for OTA broadcast)
    addPeer(BROADCAST_MAC);

    // Spots register dynamically on HELLO — no pre-registration needed.

    memset(g_spot_state, 0, sizeof(g_spot_state));
    memset(g_spot_macs,  0, sizeof(g_spot_macs));

    updateOLED();
    sendWhois();
    Serial.println("[BOOT] Ready.  Type 'help' for commands.");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
static char    _line_buf[64];
static uint8_t _line_len = 0;

void loop() {
    // 1. Handle HELLO from spots (dynamic peer registration + version check)
    while (_hello_tail != _hello_head) {
        hello_event_t ev = _hello_ring[_hello_tail];
        _hello_tail = (_hello_tail + 1) % HELLO_RING_SIZE;
        handleHello(ev.mac, ev.fw_version, ev.spot_id);
    }

    // 2. Process received status packets
    if (_status_available) {
        _status_available = false;
        esp_now_status_t s;
        memcpy(&s, (const void *)&_status_buffer, sizeof(s));

        if (s.spot_id >= 0x01 && s.spot_id <= NUM_SPOTS) {
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
            (s.thermal_state == STATE_PULSE)      ? "pulse" :
            (s.thermal_state == THERMAL_CRITICAL) ? "CRITICAL" :
            (s.thermal_state == THERMAL_THROTTLE) ? "THROTTLE" : "normal";
        Serial.printf("[STATUS] spot=0x%02X  %s  bri=%d  temp=%.1f°C  state=%s\n",
                      s.spot_id, s.is_on ? "ON " : "OFF",
                      s.brightness, s.temperature, state_str);

        uart2_send_status(s);
        updateOLED();
    }

    // 3. Log OTA failure notifications from spots
    if (_ota_fail_available) {
        _ota_fail_available = false;
        Serial.printf("[OTA] Spot 0x%02X reported OTA failure (attempt %d)\n",
                      _ota_fail_buffer.spot_id, _ota_fail_buffer.attempt);
    }

    // 4. Poll UART2 for incoming frames from WiFi ESP32-B
    uart2_poll();

    // 5. Accumulate serial input, process on newline
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
