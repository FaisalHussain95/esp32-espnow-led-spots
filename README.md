# DIY LED Spot Controller

DIY replacement of 10× 220VAC LED spots with custom COB LED + ESP32 controller units.
Each spot has an isolated AC/DC converter, constant-current LED driving via PT4115, NTC thermal monitoring,
and encrypted ESP-NOW wireless control — no WiFi router required for the spots.

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
         │ UART2 binary frames (115200 baud)
         │ TX=17 → RX=35 (TTGO)
         │ RX=16 ← TX=13 (TTGO)
┌────────┴──────────┐
│  Master Node      │  TTGO LoRa32 V1.x
│  (ESP32-A / TTGO) │  ESP-NOW only + SSD1306 OLED
└────────┬──────────┘
         │ ESP-NOW (PMK encrypted, 2.4GHz)
         │
    ┌────┴────┐
    │         │
  Spot 1   Spot 2 … Spot 10
 ESP32-C3  ESP32-C3  ESP32-C3
 SuperMini SuperMini SuperMini
```

The master (TTGO) runs ESP-NOW only — no WiFi.
The WiFi bridge handles MQTT/HA and translates commands to/from UART2 binary frames.
This separation avoids ESP-NOW peer drops caused by WiFi channel switching.

---

## Firmware Structure

```
spot_firmware/          — One binary per spot (ESP32-C3 SuperMini)
├── platformio.ini
└── src/
    ├── main.cpp              # Setup, loop, command handler, thermal policy, status LED
    ├── config.h              # Pin defines, thresholds, SPOT_ID, packet structs — edit per unit
    ├── thermal.h/.cpp        # NTC read (32× oversampling), Steinhart-Hart, throttle logic
    ├── dimming.h/.cpp        # LEDC init, setBrightness(), fadeTo()
    ├── espnow_manager.h/.cpp # ESP-NOW init, PMK encryption, HELLO handshake, send/receive
    ├── provisioning.h/.cpp   # PMK + WiFi creds from build_flags → NVS (first boot only)
    └── ota.h/.cpp            # HTTP OTA update from GitHub release URL (5 retries)

master_firmware/        — One binary for the master node (TTGO LoRa32)
├── platformio.ini
└── src/
    ├── main.cpp              # ESP-NOW master, OLED display, serial CLI, UART2 bridge
    └── config.h              # Spot MAC table, packet structs, UART2 pin defines

wifi_bridge_firmware/   — One binary for the WiFi bridge (generic ESP32)
├── platformio.ini
└── src/
    ├── main.cpp              # WiFi + MQTT ↔ UART2 bridge, HA MQTT Discovery
    └── config.h              # UART2 pins, MQTT topic prefix, frame constants
```

**Porting a spot to a new unit:** only change `SPOT_ID` (0x01–0x0A) and `MASTER_MAC[]` in `spot_firmware/src/config.h`.

---

## Hardware (per spot)

### Power Chain
```
220VAC ──► HLK-PM24 ──► 24VDC ──► PT4115 (CC driver) ──► COB LED (5W)
                    │
                    └──► MP1584 buck ──► 3.3VDC ──► ESP32-C3 SuperMini
                                                          │
                                                     GPIO10 (PWM 10kHz)
                                                          │
                                                     PT4115 DIM pin
```

The HLK-PM24 provides an isolated 24V rail.
The PT4115 is a constant-current buck LED driver — current is set by Rsense, not firmware.
PWM on the DIM pin dims the LED by modulating the driver enable signal.

### Components

| Component | Value / Model | Purpose |
|---|---|---|
| AC/DC converter | HLK-PM24 | 220VAC → 24VDC, 1A, isolated |
| Buck converter | MP1584 module | 24V → 3.3V for ESP32 |
| Microcontroller | ESP32-C3 SuperMini | Control + ESP-NOW wireless |
| LED driver | PT4115 | Constant-current buck driver, DIM pin PWM |
| Current sense | 0.33Ω 1W | Iout = 0.1V / 0.33Ω ≈ 303mA |
| Freewheeling diode | 1N4007 | Across COB LED (buck topology) |
| Thermal sensor | NTC 10kΩ 0402, B=3950 | Mounted on COB PCB |
| NTC divider | 5.1kΩ fixed to GND | NTC on 3.3V side |
| ADC filter | 100nF ceramic to GND | On ADC signal wire, noise filtering |

### PT4115 Wiring
```
24V ──► PT4115 VIN
        PT4115 SW ──► inductor ──► COB+ ──► COB- ──► PT4115 CS ──► 0.33Ω ──► GND
ESP32 GPIO10 ──► PT4115 DIM   (PWM 10kHz, 0–100% duty = 0–full current)
```

### NTC Wiring
```
3.3V
 │
[NTC 10kΩ]          ← mounted on COB PCB, wired back to ESP32 via cable
 │
 ├──► GPIO3 (ADC1_CH3)
 │         │
 │       [100nF] ──► GND    ← on ESP32 PCB end, filters cable noise
 │
[5.1kΩ]
 │
GND
```

- NTC on the 3.3V side; formula: `r_ntc = R_pullup × (ADC_MAX − adc) / adc`
- 32× ADC oversampling + Steinhart-Hart Beta equation
- **ADC quirk (ESP32 Arduino core):** call a dummy `analogRead(pin)` before `analogSetPinAttenuation()` — otherwise attenuation doesn't apply and the ADC reads 0

### GPIO Pin Mapping

#### ESP32-C3 SuperMini (final)
| Function | GPIO | Notes |
|---|---|---|
| PWM → PT4115 DIM | GPIO10 | LEDC 10kHz 8-bit, no ADC, no strapping conflict |
| NTC ADC | GPIO3 | ADC1_CH3 — safe with ESP-NOW active (ADC1 only) |
| Status LED | GPIO8 | Onboard blue LED, **active LOW** |

#### ESP32 WROVER (prototype only)
| Function | GPIO | Notes |
|---|---|---|
| PWM → PT4115 DIM | GPIO18 | LEDC 10kHz 8-bit |
| NTC ADC | GPIO34 | Input-only ADC1 channel |
| Status LED | GPIO2 | Onboard LED, active HIGH |

---

## Thermal Protection

### PWM limits
| Constant | Value | Reason |
|---|---|---|
| `PWM_MAX` | 100 | Hardware ceiling — driving above 100 overloads the heatsink (measured 54°C at PWM=100 full load) |
| `PWM_MIN_FLOOR` | 20 | Minimum allowed in throttle/critical states |

The master can send brightness 0–255, but the spot clamps the output to `PWM_MAX` (100) before passing it to the LEDC peripheral.

### Thermal states

| Temperature | State | Action |
|---|---|---|
| < 60°C | NORMAL | Full PWM authority (capped at `PWM_MAX`=100) |
| 60–75°C | THROTTLING | Linear ramp-down from requested brightness to `PWM_MIN_FLOOR` (20) |
| 75–85°C | THROTTLING | Hard floor: output clamped to `PWM_MIN_FLOOR` (20) |
| > 85°C | CRITICAL | Clamped to `PWM_MIN_FLOOR` + immediate ESP-NOW alert to master |

### Status LED patterns (GPIO8, active LOW)
| State | Pattern |
|---|---|
| NORMAL | 1s on / 2s off |
| THROTTLING | Solid ON |
| CRITICAL | 100ms on / 100ms off (rapid blink) |
| NTC fault | CRITICAL pattern |

---

## ESP-NOW Protocol

### Encryption
All unicast packets are PMK-encrypted (AES-128).
The PMK is set in `platformio.ini` `build_flags`, written to NVS on first boot, and survives OTA flashes.
It is never stored in the released binary.

```
esp_now_set_pmk(pmk)     ← called on both master and spots
peer.encrypt = true      ← on all unicast peers
peer.lmk = pmk           ← PMK used as LMK — single shared key, no 6-peer hardware limit
```

Broadcast peer (FF:FF:FF:FF:FF:FF) is unencrypted — required for OTA broadcast to reach all spots.

### Packet Format
Every packet starts with a 9-byte header:

```c
typedef struct __attribute__((packed)) {
    uint8_t msg_type;    // MSG_* value
    uint8_t fw_version;  // sender's firmware version
    uint8_t attempt;     // OTA retry count (0 for non-OTA)
    uint8_t mac[6];      // sender MAC (for dynamic peer registration)
} espnow_header_t;
```

Full packet types:

| Packet type | Structure |
|---|---|
| `espnow_cmd_packet_t` | `espnow_header_t` + `esp_now_cmd_t` |
| `espnow_status_packet_t` | `espnow_header_t` + `esp_now_status_t` |
| `espnow_ota_packet_t` | `espnow_header_t` + `uint8_t target_version` |

### Message Types
| Value | Name | Direction | Description |
|---|---|---|---|
| 0x01 | MSG_HELLO | Spot → Master | Boot announce — fw_version + MAC |
| 0x02 | MSG_ACK | Master → Spot | Version OK, proceed normally |
| 0x03 | MSG_REJECT | Master → Spot | Version outdated, OTA incoming |
| 0x04 | MSG_OTA_NOW | Master → All | Broadcast OTA trigger with target version |
| 0x05 | MSG_OTA_FAILED | Spot → Master | OTA attempt failed |
| 0x06 | MSG_WHOIS | Master → All | Broadcast on master boot — spots re-send MSG_HELLO |
| 0x10 | MSG_CMD | Master → Spot | LED command |
| 0x11 | MSG_STATUS | Spot → Master | Periodic status report |

### LED Commands (inside MSG_CMD)
| Value | Name | Description |
|---|---|---|
| 0x01 | CMD_SET_BRIGHTNESS | Set brightness 0–255 |
| 0x02 | CMD_TURN_ON | Turn on (with brightness, 300ms fade) |
| 0x03 | CMD_TURN_OFF | Turn off (500ms fade) |
| 0x04 | CMD_REQUEST_STATUS | Force immediate status reply |
| 0x05 | CMD_PULSE | Looping pulse (param = duration in ms, 0 = 500ms default) |

### Boot Handshake
```
Spot boots
  → provisioning_init(): PMK + WiFi loaded from NVS (written from build_flags on first boot)
  → espnow_init(): set PMK, register master as encrypted peer, send MSG_HELLO

Master receives MSG_HELLO from spot:
  → if MAC not yet registered: addPeer() with encryption
  → if spot.fw_version >= required_fw_version: send MSG_ACK
  → else: send MSG_REJECT + broadcast MSG_OTA_NOW

Spot receives MSG_ACK        → normal operation
Spot receives MSG_REJECT / MSG_OTA_NOW → ota_start(target_version)
```

---

## OTA Updates

Spot nodes fetch firmware over HTTP from GitHub releases. Master and WiFi bridge must be flashed manually via USB.

### OTA URL
```
https://github.com/FaisalHussain95/esp32-espnow-led-spots/releases/download/v<N>/slave_c3_supermini_v<N>.bin
```

### OTA sequence
```
1. Spot connects to WiFi (SSID/password from NVS)
2. HTTP GET firmware .bin from GitHub releases URL
3. HTTPUpdate.update() flashes new firmware to inactive OTA slot
4. ESP.restart() — boots into new firmware
5. On failure: send MSG_OTA_FAILED, wait 10s, retry
6. After 5 failures: stay on current firmware, log error
```

### Triggering OTA

**Via serial (master TTGO):**
```
version 2
```

**Via MQTT (Home Assistant):**
```
Topic:   homeassistant/led_spots/ota/set
Payload: {"version":2}
```

---

## UART2 Bridge

Binary frame protocol between TTGO master (ESP32-A) and WiFi bridge (ESP32-B).

### Pin connections
| TTGO Master (ESP32-A) | WiFi Bridge (ESP32-B) |
|---|---|
| GPIO13 TX | GPIO16 RX |
| GPIO35 RX | GPIO17 TX |
| GND | GND |

### Frame format (115200 baud, 8N1)

| Frame | Size | Bytes |
|---|---|---|
| Command (HA → spot) | 6 | `0xAA 0x01 spot_id brightness command 0x55` |
| Status (spot → HA) | 9 | `0xAA 0x02 spot_id brightness temp_hi temp_lo thermal_state is_on 0x55` |
| Version/OTA (HA → master) | 4 | `0xAA 0x03 target_version 0x55` |

Temperature in STATUS frames is `int16` in 0.1°C units, big-endian (e.g. 24.3°C = 243 = `0x00 0xF3`).

---

## Home Assistant Integration

The WiFi bridge publishes MQTT Discovery payloads on connect — spots appear automatically as light entities in HA.

| Entity | MQTT Topic |
|---|---|
| Light command | `homeassistant/led_spots/<id>/set` |
| Light state | `homeassistant/led_spots/<id>/state` |
| HA Discovery config | `homeassistant/light/led_spot_<id>/config` (retained) |
| OTA trigger | `homeassistant/led_spots/ota/set` |

**Command payload:** `{"state":"ON","brightness":200}`

**State payload:** `{"state":"ON","brightness":200,"temperature":24.3,"thermal_state":"normal"}`

---

## Master Serial CLI

Connect to TTGO on 115200 baud:

```
on    <spot|all> [bri]        Turn on (brightness 0–255, default 255)
off   <spot|all>              Turn off
dim   <spot|all> <bri>        Set brightness 0–255
pulse <spot|all> [bri] [ms]   Pulse loop (default bri=100, ms=500)
status <spot|all>              Request immediate status reply
version <N>                    Set required fw version, broadcast OTA to all spots
help                           List commands
```

Examples:
```
on all         → turn on all spots at full brightness
dim 3 128      → set spot 3 to ~50% brightness
version 2      → push OTA firmware v2 to all spots
```

---

## Provisioning (First-Time Setup)

Credentials are injected into all three `platformio.ini` files by the `inject_credentials.js` script.
On first boot they are written to NVS (flash, separate partition from firmware) and survive all subsequent OTA updates.

### 1. Copy and fill in `.env`

```bash
cp .env.exemple .env
```

```ini
# .env
PMK_INPUT=your-secret-passphrase   # hashed to a 32-char hex PMK

WIFI_SSID=YourNetwork              # used only for OTA HTTP downloads
WIFI_PASSWORD=YourPassword

MQTT_BROKER_IP=192.168.1.x         # WiFi bridge only — leave blank if unused
MQTT_PORT=1883
MQTT_USER=
MQTT_PASSWORD=
```

### 2. Inject credentials into all platformio.ini files

```bash
node --env-file=.env inject_credentials.js
```

This updates `build_flags` in `spot_firmware/`, `master_firmware/`, and `wifi_bridge_firmware/` in one shot.

### 3. Flash

```bash
cd spot_firmware        && pio run --target upload
cd master_firmware      && pio run --target upload
cd wifi_bridge_firmware && pio run --target upload
```

> **PMK must be identical** on all spots and the master.
> WiFi credentials on spots are used only for OTA HTTP downloads — not for normal operation.

> **To change credentials after first flash:** add `-DCONFIG_PROV_FORCE_RESET=1` to `build_flags`, flash once (NVS is cleared and rewritten), then remove the flag and reflash.

---

## GitHub Actions CI

Pushing a git tag builds all three firmware targets and creates a GitHub release with `.bin` files attached.

```bash
git tag v2
git push origin v2
# Builds: slave_c3_supermini_v2.bin  master_ttgo_v2.bin  wifi_bridge_v2.bin
# Creates GitHub release with all three files
```

The spot OTA URL is constructed automatically from the tag version number.

> Release binaries are built with placeholder credentials. Set your own PMK and WiFi credentials in `platformio.ini` before flashing.

---

## Heatsink

Selected: **25×25mm pin-grid, 6×6 array, 13mm tall** (~19°C/W).

| Heatsink | θsa (°C/W) | T_junction @25°C ambient | T_junction @50°C ambient |
|---|---|---|---|
| 20×20×6mm flat | ~102 | ~131°C ✗ | ~156°C ✗ |
| 20×20×10mm flat | ~83 | ~112°C ✓ | ~137°C ✗ |
| **25×25 pin-grid 6×6** | **~19** | **~49°C ✓✓** | **~74°C ✓✓** |

- COB max junction temp: 125°C
- Heat at junction at 5W: ~1W (remainder is light)
- **51°C margin at 50°C ambient ✓✓**

Thermal chain: `P_heat → [θjc ~4°C/W] → [θcs paste ~0.3°C/W] → [θsa ~19°C/W] → ambient`

---

## Enclosures (3D Printed)

| Part | Material | Reason |
|---|---|---|
| COB + heatsink housing | PC (Polycarbonate) | Near heat source, ~110–120°C rated |
| Controller side (ESP32 + PCB) | PLA | Thermally isolated from COB side |
| Cable routing clips | PLA / PETG | Ambient temperature only |

PC print settings: nozzle 260–280°C, bed 90–100°C, cooling fan off, dry filament at 80°C for 4–6h before printing.

---

## PCB & Manufacturing

- Design tool: **KiCad** — project at `kicad/diy_led_spot/`
- Gerbers exported to `kicad/diy_led_spot/` (ready for JLCPCB upload)
- Strategy: design 1 unit → validate → order ×10
- Assembly: hybrid — JLCPCB SMT for passives, hand-solder HLK-PM24 + ESP32-C3

### KiCad Project Files
```
kicad/diy_led_spot/
├── diy_led_spot.kicad_sch    # Schematic
├── diy_led_spot.kicad_pcb    # PCB layout
├── diy_led_spot.kicad_pro    # Project file
└── diy_led_spot_step5-*.gbr  # Exported Gerbers (F_Cu, B_Cu, Mask, Silkscreen, Edge_Cuts)
```

### Cost Estimate (10 units)
| Scenario | Estimated Total |
|---|---|
| Full DIY hand-soldered | ~$70–80 |
| Hybrid JLCPCB SMT + hand-solder | ~$100–110 |

---

## NVS Layout

| Firmware | Namespace | Key | Type | Content |
|---|---|---|---|---|
| Spot | `espnow` | `pmk` | bytes (16) | Shared PMK |
| Spot | `wifi` | `ssid` | string | WiFi SSID (OTA only) |
| Spot | `wifi` | `password` | string | WiFi password (OTA only) |
| Master | `espnow` | `pmk` | bytes (16) | Shared PMK |

---

## Validation Workflow

1. Validate NTC divider + RC filter in **Falstad** (falstad.com/circuit)
2. Prototype firmware on **WROVER** before committing to ESP32-C3 SuperMini boards
3. Schematic + PCB layout in **KiCad** (`kicad/diy_led_spot/`)
4. Export Gerbers → upload to **JLCPCB**, order prototype → validate → order ×10

---

## Next Steps

- [x] Write spot node firmware (dimming, thermal protection, ESP-NOW)
- [x] Write master controller firmware (TTGO LoRa32 + OLED + serial CLI)
- [x] Port spot firmware to ESP32-C3 SuperMini
- [x] Add WiFi bridge (UART2 ↔ MQTT) for Home Assistant integration
- [x] ESP-NOW PMK encryption + boot handshake protocol
- [x] OTA update system (HTTP from GitHub releases, 5-retry)
- [x] GitHub Actions CI (auto-build + release on git tag push)
- [x] KiCad schematic + PCB layout (`kicad/diy_led_spot/`)
- [x] Gerbers exported (ready for JLCPCB)
- [ ] Order prototype PCBs → validate
- [ ] Order ×10 production run
- [ ] Print test enclosures (PLA prototype, then PC for COB side)
- [ ] Validate thermal performance on first assembled unit
- [ ] Active cooling: 5V 25mm fan via MT3608 boost converter, EN pin on GPIO4 or GPIO5 — fan ON above 50°C, OFF below 45°C (hysteresis). Parts on order from AliExpress.
