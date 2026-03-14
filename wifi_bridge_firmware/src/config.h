#pragma once

// ─── WiFi + MQTT credentials (set via: pio run --target menuconfig) ───────────
#define WIFI_SSID        CONFIG_WIFI_SSID
#define WIFI_PASSWORD    CONFIG_WIFI_PASSWORD
#define MQTT_BROKER_IP   CONFIG_MQTT_BROKER_IP
#define MQTT_PORT        CONFIG_MQTT_PORT
#define MQTT_USER        CONFIG_MQTT_USER
#define MQTT_PASSWORD    CONFIG_MQTT_PASSWORD

// ─── UART2 pins (connect to TTGO LoRa32) ──────────────────────────────────────
// ESP32-B GPIO_TX → TTGO GPIO35 (RX)
// ESP32-B GPIO_RX ← TTGO GPIO13 (TX)
// Choose any free GPIOs on the generic ESP32 dev board:
#define PIN_UART2_TX  17
#define PIN_UART2_RX  16

// ─── MQTT topic prefix ────────────────────────────────────────────────────────
// Command:  homeassistant/led_spots/<id>/set    (subscribed)
// State:    homeassistant/led_spots/<id>/state  (published)
// Discovery: homeassistant/light/led_spot_<id>/config
#define MQTT_TOPIC_PREFIX  "homeassistant/led_spots"
#define MQTT_CLIENT_ID     "led-spots-bridge"
#define NUM_SPOTS          10

// ─── Firmware Version ─────────────────────────────────────────────────────────
#define FW_VERSION  3

// ─── UART2 frame bytes (must match master_firmware) ───────────────────────────
#define UART2_START    0xAA
#define UART2_END      0x55
#define UART2_CMD      0x01
#define UART2_STATUS   0x02
#define UART2_VERSION  0x03

// ─── ESP-NOW command codes (must match spot_firmware/config.h) ────────────────
#define CMD_SET_BRIGHTNESS  0x01
#define CMD_TURN_ON         0x02
#define CMD_TURN_OFF        0x03
#define CMD_REQUEST_STATUS  0x04

// ─── Thermal states ───────────────────────────────────────────────────────────
#define THERMAL_NORMAL    0
#define THERMAL_THROTTLE  1
#define THERMAL_CRITICAL  2
