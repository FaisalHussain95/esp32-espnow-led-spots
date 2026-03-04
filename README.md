# DIY LED Spot Controller

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32 controller units.
Each spot has isolated AC/DC conversion, constant-current LED driving, NTC thermal monitoring,
and encrypted ESP-NOW wireless control — no WiFi router required for the spots themselves.

---

## System Architecture

```
Home Assistant
     │
     │ MQTT (WiFi)
     │
┌────┴──────────────┐
│  WiFi Bridge      │  Generic ESP32 dev board
│  (ESP32-B)        │  WiFi + MQTT ↔ UART2 bridge
└────────┬──────────┘
         │ UART2 binary frames (TX=17, RX=16)
         │
┌────────┴──────────┐
│  Master Node      │  TTGO LoRa32 V1.x
│  (ESP32-A / TTGO) │  ESP-NOW only + OLED display
└────────┬──────────┘
         │ ESP-NOW (PMK encrypted, 2.4GHz)
         │
    ┌────┴────┐
    │         │
  Spot 1   Spot 2 … Spot 10
 ESP32-C3  ESP32-C3  ESP32-C3
```

The master (TTGO) speaks only ESP-NOW — no WiFi connection.
The WiFi bridge (second ESP32) handles MQTT/HA and translates to/from UART2 frames.
This avoids ESP-NOW peer drops caused by WiFi channel switching.

---

## Firmware Modules

```
spot_firmware/          — One per spot (ESP32-C3 SuperMini)
├── platformio.ini
└── src/
    ├── main.cpp              # Setup, loop, command handler, thermal policy, status LED
    ├── config.h              # Pin defines, thresholds, SPOT_ID, packet structs
    ├── thermal.h/.cpp        # NTC read (32× oversampling), Steinhart-Hart, throttle logic
    ├── dimming.h/.cpp        # LEDC init, setBrightness(), fadeTo()
    ├── espnow_manager.h/.cpp # ESP-NOW init, PMK encryption, HELLO handshake, send/receive
    ├── provisioning.h/.cpp   # PMK + WiFi creds from build_flags → NVS (first boot only)
    └── ota.h/.cpp            # HTTP OTA update from GitHub release URL (5 retries)

master_firmware/        — One master node (TTGO LoRa32)
├── platformio.ini
└── src/
    ├── main.cpp              # ESP-NOW master, OLED display, serial CLI, UART2 bridge
    └── config.h              # Spot MAC table, packet structs, pin defines

wifi_bridge_firmware/   — One WiFi bridge (generic ESP32 dev board)
├── platformio.ini
└── src/
    ├── main.cpp              # WiFi+MQTT ↔ UART2 bridge, HA MQTT Discovery
    └── config.h              # UART2 pins, MQTT topic prefix, frame constants
```

---

## Hardware Design

### COB LED Specifications
- Supply current: 50–320mA
- Recommended operating points:
  - 3W → 9–12V
  - 5W → 15–18V
  - 7W → 21–25V
  - 10W → 30–34V
- **Target: 5W max @ 24V, ~303mA (set by Rsense=0.33Ω)**

### Power Chain (per spot)
```
220VAC → HLK-PM24 → 24VDC → PT4115 (CC driver, Rsense=0.33Ω) → COB LED
                   → MP1584 buck → 3.3VDC → ESP32-C3 SuperMini
                                                  │
                                             GPIO10 (PWM 10kHz) → PT4115 DIM pin
```

| Stage | Input | Output | Loss | Efficiency |
|---|---|---|---|---|
| HLK-PM24 (AC/DC) | 220VAC | 24VDC | ~1.5W | ~77% |
| Buck 24V→3.3V (MP1584) | 24V | 3.3V | ~0.4W | ~83% |
| ESP32 active + ESP-NOW TX | 3.3V | — | ~0.3W | — |
| PT4115 driver loss | 24V@303mA | ~5W | ~2.3W | ~68% |
| COB LED at 5W | — | 5W light | ~1W heat | ~83% |
| NTC + voltage divider leakage | — | — | ~0.03W | — |
| **Total per spot from mains** | | | **~9.5W** | |

**All 10 spots: ~90–95W total wall consumption**

### Components (per spot)

| Component | Value/Model | Notes |
|---|---|---|
| AC/DC converter | HLK-PM24 | 220VAC → 24VDC, 1A, isolated |
| Buck converter | MP1584 module | 24V → 3.3V for ESP32 |
| Microcontroller | ESP32-C3 SuperMini | Final target. WROVER for prototyping |
| LED driver | PT4115 | Constant-current buck driver, DIM pin PWM control |
| Rsense | 0.33Ω 1W | Sets Iout ≈ 303mA (Iout = 0.1 / Rsense) |
| Flyback diode | 1N4007 | Freewheeling diode across COB (buck topology) |
| Thermal sensor | NTC 10kΩ 0402 | On COB PCB, B=3950 |
| NTC divider resistor | 5.1kΩ | Fixed resistor to GND |
| Noise filter cap | 100nF ceramic | On ADC signal wire to GND |

### PT4115 Wiring
```
24V rail ──► PT4115 VIN
             PT4115 SW ──► inductor ──► COB+ ──► COB- ──► PT4115 CS ──► 0.33Ω ──► GND
ESP32 GPIO10 ──► PT4115 DIM   (PWM 10kHz)
```

### NTC Wiring
```
3.3V
 │
[NTC 10kΩ]
 │
 ├──► ADC GPIO3 (C3 SuperMini) / GPIO34 (WROVER)
 │                   │
 │                 [100nF]
 │                   │
[5.1kΩ fixed]       GND
 │
GND
```

- 100nF cap on ADC node filters cable noise
- Firmware: 32× oversampling + Steinhart-Hart Beta equation
- ADC quirk: call dummy `analogRead(pin)` before `analogSetPinAttenuation()` — otherwise reads 0

### Full Per-Spot Schematic (simplified)
```
220VAC ──► HLK-PM24 ──► 24V rail ──► PT4115 VIN
                │                    PT4115 SW ──► L ──► COB+ ──► COB- ──► 0.33Ω ──► GND
                └──► Buck (24V→3.3V) ──► ESP32-C3
                                              │
                                         GPIO10 (PWM) ──► PT4115 DIM
```

### GPIO Pin Mapping

#### ESP32-C3 SuperMini (Final)
| Function | GPIO | Notes |
|---|---|---|
| PWM → PT4115 DIM | GPIO10 | LEDC, no ADC, no strapping conflict |
| NTC ADC | GPIO3 | ADC1_CH3 — safe with ESP-NOW active |
| Status LED | GPIO8 | Onboard blue LED, active LOW |

#### ESP32 WROVER (Prototype)
| Function | GPIO | Notes |
|---|---|---|
| PWM → PT4115 DIM | GPIO18 | 10kHz, 8-bit LEDC |
| NTC ADC | GPIO34 | Input-only ADC1 channel |
| Status LED | GPIO2 | Onboard LED |

> All pin assignments are `#define` in `config.h` — porting to a new board only requires changing those values.

---

## ESP-NOW Protocol

### Encryption
All unicast packets are **PMK encrypted** using a 16-byte shared pre-master key.
The PMK is set in `platformio.ini` `build_flags`, written to NVS on first boot, and survives OTA flashes.
It is **never stored in the released binary**.

```
esp_now_set_pmk(pmk)           ← set on both master and spots
peer.encrypt = true            ← on all unicast peers
peer.lmk = pmk                 ← PMK used as LMK (single shared key, no 6-peer limit)
```

The broadcast peer (0xFF×6) is unencrypted — required for OTA broadcast to reach all spots.

### Packet Envelope
Every packet now has a common header:

```c
typedef struct __attribute__((packed)) {
    uint8_t msg_type;    // MSG_* value
    uint8_t fw_version;  // sender's current firmware version
    uint8_t attempt;     // OTA retry count (0 for non-OTA)
    uint8_t mac[6];      // sender MAC (for dynamic peer registration)
} espnow_header_t;
```

Full packet types:
```c
espnow_cmd_packet_t    = espnow_header_t + esp_now_cmd_t
espnow_status_packet_t = espnow_header_t + esp_now_status_t
espnow_ota_packet_t    = espnow_header_t + uint8_t target_version
```

### Message Types
| Value | Name | Direction | Description |
|---|---|---|---|
| 0x01 | MSG_HELLO | Spot → Master | Boot announce with fw_version + MAC |
| 0x02 | MSG_ACK | Master → Spot | Version OK, proceed normally |
| 0x03 | MSG_REJECT | Master → Spot | Version outdated, OTA incoming |
| 0x04 | MSG_OTA_NOW | Master → All | Broadcast OTA trigger with target version |
| 0x05 | MSG_OTA_FAILED | Spot → Master | OTA attempt failed (attempt number in header) |
| 0x10 | MSG_CMD | Master → Spot | Command packet (dim/on/off/status) |
| 0x11 | MSG_STATUS | Spot → Master | Periodic status report |

### Commands (inside MSG_CMD)
| Value | Name | Description |
|---|---|---|
| 0x01 | CMD_SET_BRIGHTNESS | Set brightness 0–255 |
| 0x02 | CMD_TURN_ON | Turn on (with brightness, 300ms fade) |
| 0x03 | CMD_TURN_OFF | Turn off (500ms fade) |
| 0x04 | CMD_REQUEST_STATUS | Force immediate status reply |

### Boot Handshake Flow
```
Spot boots
  → provisioning_init(): load PMK + WiFi creds from NVS (write from build_flags if first boot)
  → espnow_init(): set PMK, add master peer (encrypted), send MSG_HELLO

Master receives MSG_HELLO:
  → if MAC not known: addPeer() dynamically (encrypted)
  → if spot.fw_version >= required_fw_version: send MSG_ACK
  → else: send MSG_REJECT (with target_version) + sendOtaBroadcast()

Spot receives MSG_ACK   → normal operation
Spot receives MSG_REJECT/MSG_OTA_NOW → ota_start(target_version)
```

---

## OTA Update System

Spots update their firmware over HTTP from a GitHub release URL.

### URL format
```
https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v<N>/slave_c3_supermini_v<N>.bin
```

### OTA sequence
```
Master sends MSG_OTA_NOW (broadcast, target_version=N)
  → Each spot:
       1. Connect to WiFi (creds from NVS)
       2. HTTP GET the .bin from GitHub releases
       3. HTTPUpdate.update() — flashes new firmware
       4. ESP.restart() on success
       5. On failure: send MSG_OTA_FAILED + retry up to 5× with 10s delay
       6. After 5 failures: stay on current firmware, log error
```

### Triggering OTA

**Via serial on master:**
```
version 2
```

**Via MQTT (Home Assistant):**
```
Topic:   homeassistant/led_spots/ota/set
Payload: {"version":2}
```

**Via HA UI:** publish the payload above from any MQTT tool or HA automation.

Only **spot nodes** update over OTA. Master and WiFi bridge must be flashed manually via USB.

---

## Provisioning (First-Time Setup)

Credentials are set by editing `platformio.ini` `build_flags` before the first flash.
They are written to NVS on first boot and survive all subsequent OTA flashes (NVS is a separate flash partition).

### 1. Spot firmware — edit `spot_firmware/platformio.ini`
```ini
build_flags =
    -DCONFIG_ESPNOW_PMK=\"aabbccddeeff00112233445566778899\"  ; 32 hex chars = 16 bytes
    -DCONFIG_WIFI_SSID=\"YourSSID\"       ; used only for OTA HTTP, not normal operation
    -DCONFIG_WIFI_PASSWORD=\"YourPassword\"
```

### 2. Master firmware — edit `master_firmware/platformio.ini`
```ini
build_flags =
    -DBOARD_HAS_PSRAM=0
    -DCONFIG_ESPNOW_PMK=\"aabbccddeeff00112233445566778899\"  ; must be identical to spots!
```

### 3. WiFi bridge firmware — edit `wifi_bridge_firmware/platformio.ini`
```ini
build_flags =
    -DCONFIG_WIFI_SSID=\"YourSSID\"
    -DCONFIG_WIFI_PASSWORD=\"YourPassword\"
    -DCONFIG_MQTT_BROKER_IP=\"192.168.1.x\"
    -DCONFIG_MQTT_PORT=1883
    -DCONFIG_MQTT_USER=\"\"            ; leave empty if no auth
    -DCONFIG_MQTT_PASSWORD=\"\"
```

Then flash each board:
```bash
cd spot_firmware      && pio run --target upload
cd master_firmware    && pio run --target upload
cd wifi_bridge_firmware && pio run --target upload
```

> The PMK must be **identical** on all spots and the master. WiFi credentials on spots are used only during OTA HTTP downloads.

> **Changing credentials after first flash:** Add `-DCONFIG_PROV_FORCE_RESET=1` to `build_flags`, flash once (this clears and rewrites NVS), then remove the flag and reflash to return to normal behaviour.

---

## UART2 Bridge (Master ↔ WiFi Bridge)

Binary frame protocol between TTGO and ESP32-B over UART2 (115200 baud).

| Frame | Bytes | Format |
|---|---|---|
| CMD (HA→spot) | 6 | `0xAA 0x01 spot_id brightness command 0x55` |
| STATUS (spot→HA) | 9 | `0xAA 0x02 spot_id brightness temp_hi temp_lo thermal_state is_on 0x55` |
| VERSION (HA→master) | 4 | `0xAA 0x03 target_version 0x55` |

Temperature in STATUS is encoded as `int16` in 0.1°C units (e.g. 24.3°C → 243, big-endian).

### UART2 Pin Connections
| TTGO (Master) | ESP32-B (WiFi Bridge) |
|---|---|
| GPIO13 (TX) | GPIO16 (RX) |
| GPIO35 (RX) | GPIO17 (TX) |
| GND | GND |

---

## Home Assistant Integration

The WiFi bridge publishes **MQTT Discovery** payloads on connect, so spots appear automatically as lights in HA.

| Entity | MQTT Topic |
|---|---|
| Light state | `homeassistant/led_spots/<id>/state` |
| Light command | `homeassistant/led_spots/<id>/set` |
| Discovery config | `homeassistant/light/led_spot_<id>/config` (retained) |
| OTA trigger | `homeassistant/led_spots/ota/set` |

State payload: `{"state":"ON","brightness":200,"temperature":24.3,"thermal_state":"normal"}`

Command payload: `{"state":"ON","brightness":200}`

---

## Thermal Protection Policy

| Temperature | State | Firmware Action |
|---|---|---|
| < 60°C | NORMAL | Full PWM authority |
| 60–75°C | THROTTLING | Linear brightness reduction |
| 75–85°C | THROTTLING | Minimum brightness floor (PWM=20) |
| > 85°C | CRITICAL | Cut to minimum + immediate ESP-NOW alert to master |

### Status LED (GPIO8, active LOW)
| State | Pattern |
|---|---|
| NORMAL | 1 s on / 2 s off |
| THROTTLING | Solid ON |
| CRITICAL | 100 ms on / 100 ms off (rapid blink) |
| NTC fault | CRITICAL pattern |

---

## Master Serial CLI

Connect to the TTGO on 115200 baud:

```
on  <spot|all> [bri]    Turn on (bri default=255)
off <spot|all>          Turn off
dim <spot|all> <bri>    Set brightness 0–255
status <spot|all>       Request status reply
version <N>             Set required fw version, broadcast OTA to all spots
help                    List commands
```

Examples:
```
on all          → broadcast turn on at full brightness
dim 3 128       → set spot 3 to 50% brightness
version 2       → trigger OTA update to fw version 2 on all spots
```

---

## GitHub Actions CI

On every `git tag v*` push, GitHub Actions builds all three firmware targets and creates a GitHub release with the `.bin` files attached.

```bash
git tag v2
git push origin v2
# → builds slave_c3_supermini_v2.bin, master_ttgo_v2.bin, wifi_bridge_v2.bin
# → creates GitHub release with all three files
```

The spot OTA URL is constructed automatically from the tag version — no manual URL management needed.

> **Note:** Release binaries are built with placeholder credentials. Each user must set their own PMK and WiFi credentials in `platformio.ini` `build_flags` before flashing.

---

## Heatsink

### Selected Heatsink
- **Pin grid style**: 25×25mm base (2mm thick) + 6×6 array of aluminum rods, 13mm tall
- Omnidirectional convection — air flows between pins in all directions

### Thermal Calculation Results

| Heatsink | θsa | T_junction @25°C ambient | T_junction @50°C ambient |
|---|---|---|---|
| 20×20×6mm flat | ~102°C/W | ~131°C ✗ | ~156°C ✗ |
| 20×20×10mm flat | ~83°C/W | ~112°C ✓ | ~137°C ✗ |
| **25×25 pin grid 6×6** | **~19°C/W** | **~49°C ✓✓** | **~74°C ✓✓** |

- COB max junction temp: 125°C
- Heat dissipated at junction at 5W: ~1W (rest is light)
- **Selected heatsink gives 51°C margin at 50°C ambient ✓✓**

### Thermal Resistance Model
```
P_heat → [θjc COB ~4°C/W] → [θcs paste ~0.3°C/W] → [θsa heatsink ~19°C/W] → ambient
```

---

## Enclosures (3D Printed)

| Part | Material | Reason |
|---|---|---|
| COB + heatsink side | PC (Polycarbonate) | Near heat, ~110-120°C resistance |
| Controller side (ESP32 + PCB) | PLA | Thermally isolated |
| Cable routing clips | PLA/PETG | Ambient temps only |

**PC print settings:** Nozzle 260–280°C, bed 90–100°C, fan off, dry filament 80°C 4–6h.

---

## PCB & Manufacturing

- Design tool: **EasyEDA** (directly linked to JLCPCB + LCSC)
- Strategy: Design 1 unit → validate → order qty 10
- Assembly: Hybrid — JLCPCB SMT for passives, hand solder HLK-PM24 + ESP32

### Cost Estimate (10 units)
| Scenario | Total |
|---|---|
| Full DIY hand soldered | ~$70–80 |
| Hybrid JLCPCB SMT + hand solder | ~$100–110 |

---

## Validation Workflow

1. Validate NTC divider + RC filter in **Falstad** (falstad.com/circuit)
2. Prototype firmware on **WROVER** before flashing C3 SuperMini units
3. Draw schematic + PCB layout in **EasyEDA**
4. Export Gerber + BOM → order on **JLCPCB × 10**

---

## Next Steps

- [x] Write spot node firmware (dimming, thermal protection, ESP-NOW)
- [x] Write master controller firmware (TTGO LoRa32 + OLED + serial CLI)
- [x] Port spot firmware to ESP32-C3 SuperMini
- [x] Add WiFi bridge (MQTT ↔ UART2) for Home Assistant integration
- [x] ESP-NOW PMK encryption + boot handshake
- [x] OTA update system (HTTP from GitHub releases)
- [x] GitHub Actions CI (auto-build + release on tag push)
- [ ] Draw schematic in EasyEDA
- [ ] PCB layout → JLCPCB quote
- [ ] Print test enclosures in PLA
- [ ] Validate thermal performance on first assembled unit
