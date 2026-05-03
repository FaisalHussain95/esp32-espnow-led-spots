# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32-C3 controller units. Each spot has an isolated AC/DC converter, PWM dimming via PT4115 LED driver, NTC thermal monitoring, active fan cooling, and ESP-NOW wireless control.

## Build & Flash Commands

```bash
# Spot node (ESP32-C3 SuperMini, COM10)
cd spot_firmware && pio run --target upload

# Master (ESP32-C3 SuperMini, COM8)
cd master_firmware && pio run --target upload

# WiFi bridge (generic ESP32 — set upload_port in platformio.ini first)
cd wifi_bridge_firmware && pio run --target upload

# Monitor serial output
cd <firmware_dir> && pio run --target monitor

# Inject PMK + WiFi credentials before flashing
node inject_credentials.js   # reads .env, writes to all three platformio.ini files
```

Before creating a git tag, always build all three firmwares to verify they compile.

## Credentials & Provisioning

Credentials (PMK, WiFi SSID/password) are injected via `inject_credentials.js` from `.env` into `platformio.ini` build_flags as hex strings. `inject_credentials.js` derives the 32-hex-char PMK from `PMK_INPUT` via MD5. On first boot, firmware writes them to NVS. Subsequent flashes (including OTA) read from NVS, so credentials survive updates.

To force re-provisioning from build_flags: add `-DCONFIG_PROV_FORCE_RESET=1` to build_flags.

NVS namespaces: `espnow` (PMK), `spot` (spot_id), `wifi` (ssid/password).

**Porting to a new spot unit:** set `-DCONFIG_SPOT_ID=0x0N` in `platformio.ini` build_flags; credential provisioning is automatic.

## 3-Node System Architecture

```
Home Assistant (MQTT)
        ↕ WiFi
[wifi_bridge_firmware] (ESP32-C3 SuperMini, GPIO6 TX / GPIO7 RX)
        ↕ UART2 binary frames (115200 baud)
[master_firmware] (ESP32-C3 SuperMini, GPIO6 TX / GPIO7 RX)
        ↕ ESP-NOW encrypted (PMK AES-128, WiFi ch11)
[spot_firmware × N] (ESP32-C3 SuperMini, up to 254 spots)
```

### Firmware Modules

```
spot_firmware/src/
├── main.cpp           # Loop: ESP-NOW commands, OTA trigger, pulse, thermal, fan, status LED
├── config.h           # Pins, thresholds, packet structs, SPOT_ID (set via build_flag)
├── thermal.h/.cpp     # NTC readout (Steinhart-Hart), throttle logic
├── dimming.h/.cpp     # LEDC init, setBrightness(), fadeTo()
├── espnow_manager.h/.cpp  # Init, HELLO handshake, send/receive, master-reachability
├── provisioning.h/.cpp    # NVS read/write for PMK, WiFi creds, spot_id
└── ota.h/.cpp         # WiFi connect + HTTPUpdate from GitHub releases

master_firmware/src/
├── config.h           # Packet structs, UART2 pins, spot registry
└── main.cpp           # ESP-NOW master, OLED, serial CLI, UART2 bridge, OTA self-update

wifi_bridge_firmware/src/
├── config.h           # MQTT topics, UART2 pins, WiFi creds
└── main.cpp           # MQTT↔UART2 translation, HA discovery, OTA self-update
```

## ESP-NOW Protocol

**Encryption:** PMK shared across all nodes (AES-128). Broadcast peer is unencrypted (required for OTA). Unicast peers use PMK as LMK.

**Boot handshake:** Spot broadcasts MSG_HELLO → Master sends MSG_ACK (fw_version OK) or MSG_REJECT (triggers OTA). Master broadcasts MSG_WHOIS on its own reboot to rediscover all spots.

**Packet header** (prepended to all payloads):
```c
typedef struct __attribute__((packed)) {
    uint8_t msg_type;    // MSG_* below
    uint8_t fw_version;  // sender's firmware version
    uint8_t attempt;     // OTA retry count; reused as spot_id in MSG_HELLO
    uint8_t mac[6];      // sender MAC address
} espnow_header_t;
```

**Message types:** MSG_HELLO(0x01), MSG_ACK(0x02), MSG_REJECT(0x03), MSG_OTA_NOW(0x04), MSG_OTA_FAILED(0x05), MSG_WHOIS(0x06), MSG_CMD(0x10), MSG_STATUS(0x11)

**Commands (CMD field):** CMD_SET_BRIGHTNESS(0x01), CMD_TURN_ON(0x02), CMD_TURN_OFF(0x03), CMD_REQUEST_STATUS(0x04), CMD_PULSE(0x05)

**Spot → Master status** (10s period + after every command):
```c
typedef struct __attribute__((packed)) {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state;  // 0=normal, 1=throttle, 2=critical
    bool    is_on;
} esp_now_status_t;
```

**Packet structures must be kept in sync** between `spot_firmware/src/config.h` and `master_firmware/src/config.h` — they share an identical wire format.

## UART2 Binary Bridge Protocol

Master (ESP32-C3 SuperMini, GPIO6 TX / GPIO7 RX) ↔ WiFi bridge (ESP32-C3 SuperMini, GPIO6 TX / GPIO7 RX), 115200 baud.

| Frame | Bytes | Layout |
|---|---|---|
| Command | 6 | `0xAA 0x01 spot_id brightness command 0x55` |
| Status | 9 | `0xAA 0x02 spot_id bri temp_hi temp_lo thermal_state is_on 0x55` |
| OTA version | 4 | `0xAA 0x03 target_version 0x55` |

Temperature is encoded as `int16 = (float * 10)`, split into `temp_hi / temp_lo` bytes.

## OTA Updates

Firmware binaries are published as GitHub release assets and fetched via `HTTPUpdate`. OTA is triggered by the master (MSG_OTA_NOW broadcast) when a spot's `fw_version` < `required_fw_version`. Spots attempt WiFi connect (5× retry, 10s each), fetch from GitHub, then re-init ESP-NOW on failure.

Binary URL pattern: `https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v<N>/slave_c3_supermini_v<N>.bin`

`FW_VERSION` is defined in `config.h` — all three firmwares share the same version number and must be kept in sync when releasing.

## Hardware Reference

### Power Chain (per spot)
```
220VAC → HLK-PM24 → 24VDC → PT4115 (CC driver, Rsense=0.33Ω) → COB LED
                   → MP1584 buck → 3.3VDC → ESP32-C3 SuperMini
                                                 │
                                            GPIO10 (PWM 10kHz) → PT4115 DIM pin
                   → MT3608 boost (5V, EN=GPIO5) → Fan (via AO3400 MOSFET, gate=GPIO4)
```

### GPIO Pin Mapping

| Function | WROVER (prototype) | C3 SuperMini (final) |
|---|---|---|
| PWM → PT4115 DIM | GPIO18 | GPIO10 |
| NTC ADC | GPIO34 (input-only) | GPIO3 (ADC1_CH3) |
| Status LED | GPIO2 (active HIGH) | GPIO8 (onboard blue, active LOW) |
| Fan PWM (AO3400 gate) | — | GPIO4 |
| Fan enable (MT3608 EN) | — | GPIO5 |

### Fan Control
Fan turns on ≥ 45°C, off ≤ 40°C (hysteresis). MT3608 boost rail is enabled 2ms before driving the MOSFET gate. On NTC fault (open/short), fan is forced ON at full speed.

### NTC Temperature Sensing
- Divider: NTC 10kΩ MF52 (B=3950) on 3.3V side, 5.1kΩ to GND → `r_ntc = R_pullup * (ADC_MAX - adc) / adc`
- 100nF cap on ADC signal wire to GND for noise filtering
- ADC init quirk: must call dummy `analogRead(pin)` before `analogSetPinAttenuation()` or attenuation doesn't apply
- 32× oversampling + Steinhart-Hart Beta equation

### Thermal Protection Policy

| Temperature | State | Action |
|---|---|---|
| < 60°C | NORMAL | Full PWM authority |
| 60–75°C | THROTTLING | Linear brightness reduction |
| 75–85°C | THROTTLING | Hard floor PWM=20 |
| > 85°C | CRITICAL | PWM=20 + immediate ESP-NOW alert to master |

PWM ceiling: `PWM_MAX=160` (~5W operating point); `PWM_MIN_FLOOR=20`.

Status LED patterns: heartbeat (1s on / 2s off) = normal; solid = throttling; rapid blink (100ms) = critical.

### Key Platform Quirks

- **LEDC API (Arduino-ESP32 v3.x):** `ledcAttach(pin, freq, bits)` + `ledcWrite(pin, val)` — no channel args
- **ESP32-C3 USB:** Native USB-Serial-JTAG — no CH340/CP2102; `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` required

## Serial CLIs

### Master CLI (via monitor on COM8)
```
on  <spot|all> [bri]        — turn on (default bri=255)
off <spot|all>              — turn off
dim <spot|all> <bri>        — set brightness 0–255
pulse <spot|all> [bri] [ms] — pulsing effect
status <spot|all>           — request status reply
version <N>                 — trigger OTA to version N
help                        — print command list
```

### Spot CLI (via monitor on COM10, for bench testing without master)
```
on [brightness]                        — turn on
off                                    — turn off
dim <0-255>                            — set brightness
pulse [peak] [ms]                      — start pulse loop
status                                 — print current state
prov <id>|<ssid>|<password>|<pmk_hex> — write NVS credentials and reboot
```

## PCB & Manufacturing

- **Design tool:** KiCad (files in `kicad/diy_led_spot/`)
- **Strategy:** Design 1 unit, validate, order ×10 via JLCPCB
- **Assembly:** JLCPCB SMT for passives; hand-solder HLK-PM24, ESP32, PT4115
