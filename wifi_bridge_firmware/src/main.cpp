#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include "config.h"

// ─── MQTT + WiFi clients ──────────────────────────────────────────────────────
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

// ─── UART2 receive buffer ─────────────────────────────────────────────────────
static uint8_t _u2buf[12];
static uint8_t _u2len = 0;

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void wifi_connect() {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_max_tx_power(52);  // 13 dBm — reduces heat on C3 SuperMini
    Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WIFI] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

static void mqtt_publish_discovery(uint8_t spot_id) {
    char config_topic[64];
    snprintf(config_topic, sizeof(config_topic),
             "homeassistant/light/led_spot_%d/config", spot_id);

    char state_topic[64];
    snprintf(state_topic, sizeof(state_topic),
             "%s/%d/state", MQTT_TOPIC_PREFIX, spot_id);

    char cmd_topic[64];
    snprintf(cmd_topic, sizeof(cmd_topic),
             "%s/%d/set", MQTT_TOPIC_PREFIX, spot_id);

    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"LED Spot %d\","
        "\"unique_id\":\"led_spot_%d\","
        "\"schema\":\"json\","
        "\"state_topic\":\"%s\","
        "\"command_topic\":\"%s\","
        "\"brightness\":true,"
        "\"brightness_scale\":255,"
        "\"availability_topic\":\"%s/%d/state\","
        "\"availability_template\":\"{{ 'online' }}\","
        "\"json_attributes_topic\":\"%s\","
        "\"device\":{"
            "\"identifiers\":[\"led_spots\"],"
            "\"name\":\"DIY LED Spots\","
            "\"model\":\"ESP32-C3 + PT4115\","
            "\"manufacturer\":\"DIY\""
        "}"
        "}",
        spot_id, spot_id,
        state_topic, cmd_topic,
        MQTT_TOPIC_PREFIX, spot_id,
        state_topic);

    mqtt.publish(config_topic, payload, true);  // retained
}

static void mqtt_publish_discovery_all() {
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"LED Spots (All)\","
        "\"unique_id\":\"led_spot_all\","
        "\"schema\":\"json\","
        "\"state_topic\":\"%s/all/state\","
        "\"command_topic\":\"%s/all/set\","
        "\"brightness\":true,"
        "\"brightness_scale\":255,"
        "\"device\":{"
            "\"identifiers\":[\"led_spots\"],"
            "\"name\":\"DIY LED Spots\","
            "\"model\":\"ESP32-C3 + PT4115\","
            "\"manufacturer\":\"DIY\""
        "}"
        "}",
        MQTT_TOPIC_PREFIX, MQTT_TOPIC_PREFIX);

    mqtt.publish("homeassistant/light/led_spot_all/config", payload, true);
}

static void mqtt_subscribe_all() {
    char topic[64];
    for (int i = 1; i <= NUM_SPOTS; i++) {
        snprintf(topic, sizeof(topic), "%s/%d/set", MQTT_TOPIC_PREFIX, i);
        mqtt.subscribe(topic);
    }
    mqtt.subscribe(MQTT_TOPIC_PREFIX "/all/set");
    mqtt.subscribe(MQTT_TOPIC_PREFIX "/ota/set");
}

static void mqtt_connect() {
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] Connecting to %s:%d...", MQTT_BROKER_IP, MQTT_PORT);
        bool ok = (strlen(MQTT_USER) > 0)
            ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
            : mqtt.connect(MQTT_CLIENT_ID);

        if (ok) {
            Serial.println(" connected.");
            mqtt_subscribe_all();
            for (int i = 1; i <= NUM_SPOTS; i++)
                mqtt_publish_discovery(i);
            mqtt_publish_discovery_all();
        } else {
            Serial.printf(" failed (rc=%d), retrying in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

// ─── UART2 → MQTT (status frames from TTGO) ──────────────────────────────────
// Status frame: 0xAA 0x02 spot_id brightness temp_hi temp_lo thermal_state is_on 0x55 (9 bytes)
static void uart2_process_status(const uint8_t *frame) {
    uint8_t spot_id      = frame[2];
    uint8_t brightness   = frame[3];
    int16_t temp_raw     = ((int16_t)frame[4] << 8) | frame[5];
    uint8_t thermal_state = frame[6];
    bool    is_on        = frame[7] != 0;

    float temperature = temp_raw / 10.0f;

    const char *state_str =
        (thermal_state == THERMAL_CRITICAL) ? "critical" :
        (thermal_state == THERMAL_THROTTLE) ? "throttling" : "normal";

    char state_topic[64];
    snprintf(state_topic, sizeof(state_topic), "%s/%d/state", MQTT_TOPIC_PREFIX, spot_id);

    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"state\":\"%s\",\"brightness\":%d,\"temperature\":%.1f,\"thermal_state\":\"%s\"}",
        is_on ? "ON" : "OFF", brightness, temperature, state_str);

    mqtt.publish(state_topic, payload);
    Serial.printf("[UART2→MQTT] spot=%d %s bri=%d temp=%.1f°C %s\n",
                  spot_id, is_on ? "ON" : "OFF", brightness, temperature, state_str);
}

static void uart2_poll() {
    while (Serial1.available()) {
        uint8_t b = (uint8_t)Serial1.read();

        if (_u2len == 0) {
            if (b == UART2_START) _u2buf[_u2len++] = b;
            continue;
        }
        _u2buf[_u2len++] = b;

        // Status frame is 9 bytes: START TYPE SPOT BRI TEMP_HI TEMP_LO THERMAL IS_ON END
        if (_u2len == 9) {
            if (_u2buf[1] == UART2_STATUS && _u2buf[8] == UART2_END) {
                uart2_process_status(_u2buf);
            }
            _u2len = 0;
        } else if (_u2len >= sizeof(_u2buf)) {
            _u2len = 0;  // overrun — discard
        }
    }
}

// ─── MQTT → UART2 (commands from HA) ─────────────────────────────────────────
// Expected payload: {"state":"ON","brightness":200}
static void uart2_send_command(uint8_t spot_id, uint8_t command, uint8_t brightness) {
    uint8_t frame[6] = {UART2_START, UART2_CMD, spot_id, brightness, command, UART2_END};
    Serial1.write(frame, sizeof(frame));
    Serial.printf("[MQTT→UART2] spot=%d cmd=0x%02X bri=%d\n", spot_id, command, brightness);
}

static void uart2_send_version(uint8_t target_version) {
    uint8_t frame[4] = {UART2_START, UART2_VERSION, target_version, UART2_END};
    Serial1.write(frame, sizeof(frame));
    Serial.printf("[UART2] VERSION frame sent → master (target=%d)\n", target_version);
}

// ─── Self-OTA ─────────────────────────────────────────────────────────────────
// URL: https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v<N>/wifi_bridge_v<N>.bin
#define BRIDGE_OTA_URL_FMT \
    "https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v%d/wifi_bridge_v%d.bin"

static void bridge_ota_start(uint8_t target_version) {
    Serial.printf("[OTA] Bridge self-update to v%d\n", target_version);

    char url[200];
    snprintf(url, sizeof(url), BRIDGE_OTA_URL_FMT, target_version, target_version);

    WiFiClientSecure client;
    client.setInsecure();  // GitHub redirects to CDN; skip cert validation

    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Serial.printf("[OTA] Update failed: %s\n", httpUpdate.getLastErrorString().c_str());
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Serial.println("[OTA] No update available");
            break;
        case HTTP_UPDATE_OK:
            Serial.println("[OTA] Update OK — rebooting");
            // httpUpdate triggers ESP.restart() automatically
            break;
    }
}

static void onMqttMessage(char *topic, byte *payload, unsigned int length) {
    // Null-terminate payload for string ops
    char msg[128];
    unsigned int len = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
    memcpy(msg, payload, len);
    msg[len] = '\0';

    // OTA topic: homeassistant/led_spots/ota/set  payload: {"version":2}
    if (strstr(topic, "/ota/set") != nullptr) {
        char *ver_ptr = strstr(msg, "\"version\":");
        if (ver_ptr) {
            uint8_t target = (uint8_t)atoi(ver_ptr + 10);
            if (target >= 1) {
                if (FW_VERSION < target) {
                    // Self-OTA first. On reboot, setup() sends VERSION frame to master,
                    // master self-OTAs if needed, then broadcasts WHOIS to trigger spots.
                    bridge_ota_start(target);
                } else {
                    // Bridge already up to date — forward version frame to master directly.
                    uart2_send_version(target);
                }
            }
        }
        return;
    }

    // Spot command topic: homeassistant/led_spots/<id>/set  or  .../all/set
    bool is_all = (strstr(topic, "/all/set") != nullptr);
    uint8_t spot_id = 0xFF;

    if (!is_all) {
        char topic_copy[64];
        strncpy(topic_copy, topic, sizeof(topic_copy) - 1);
        topic_copy[sizeof(topic_copy) - 1] = '\0';
        char *seg = strtok(topic_copy, "/");
        char *prev = nullptr;
        while (seg) { prev = seg; seg = strtok(nullptr, "/"); }
        int id = prev ? atoi(prev) : 0;
        if (id < 1 || id > NUM_SPOTS) return;
        spot_id = (uint8_t)id;
    }

    // Parse JSON: {"state":"ON","brightness":200}
    bool turn_on  = strstr(msg, "\"ON\"")  != nullptr;
    bool turn_off = strstr(msg, "\"OFF\"") != nullptr;

    uint8_t brightness = 255;
    char *bri_ptr = strstr(msg, "\"brightness\":");
    if (bri_ptr) brightness = (uint8_t)atoi(bri_ptr + 13);

    if (turn_off) {
        uart2_send_command(spot_id, CMD_TURN_OFF, 0);
    } else if (turn_on) {
        uart2_send_command(spot_id, CMD_TURN_ON, brightness);
    } else if (bri_ptr) {
        uart2_send_command(spot_id, CMD_SET_BRIGHTNESS, brightness);
    }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial1.begin(115200, SERIAL_8N1, PIN_UART1_RX, PIN_UART1_TX);

    Serial.println("[BOOT] WiFi Bridge starting...");

    wifi_connect();

    mqtt.setServer(MQTT_BROKER_IP, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(512);

    mqtt_connect();

    // Send current firmware version to master on every boot.
    // Master compares against its own FW_VERSION and self-OTAs if needed,
    // then broadcasts WHOIS to check spot versions.
    uart2_send_version(FW_VERSION);

    Serial.println("[BOOT] Ready.");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
void loop() {
    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Lost connection — reconnecting...");
        wifi_connect();
    }

    // Reconnect MQTT if dropped
    if (!mqtt.connected()) {
        mqtt_connect();
    }

    mqtt.loop();
    uart2_poll();
}
