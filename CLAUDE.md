# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32-S2 controller units. Each spot has an isolated AC/DC converter, PWM dimming via MOSFET, NTC thermal monitoring, and ESP-NOW wireless control (no WiFi router required).

## Firmware Development

### Toolchain
- **IDE:** PlatformIO
- **Target MCU:** ESP32-C3 SuperMini (final), ESP32 WROVER (prototype/testing)
- **Master node:** TTGO LoRa32 V1.0/V1.2 (ESP32 + SSD1306 OLED 128×64)

### Firmware Structure

```
spot_firmware/
├── platformio.ini
└── src/
    ├── main.cpp           # Setup, main loop, command handler, thermal policy, status LED
    ├── config.h           # All pin defines, thresholds, SPOT_ID — edit to port per unit
    ├── thermal.h/.cpp     # NTC reading, Steinhart-Hart conversion, throttle logic
    ├── dimming.h/.cpp     # LEDC init, setBrightness(), fadeTo()
    └── espnow_manager.h/.cpp  # ESP-NOW init, peer registration, send/receive

master_firmware/
├── platformio.ini
└── src/
    ├── config.h           # Spot MAC table, packet structs
    └── main.cpp           # OLED display, serial command parser, ESP-NOW master
```

**Porting to a new unit:** only `config.h` needs changing — update `SPOT_ID` (0x01–0x0A) and `MASTER_MAC[]`.

## Architecture

### Power Chain (per spot)
```
220VAC → HLK-PM24 → 24VDC → PT4115 (CC driver, Rsense=0.33Ω) → COB LED
                   → MP1584 buck → 3.3VDC → ESP32-C3 SuperMini
                                                 │
                                            GPIO10 (PWM) → PT4115 DIM pin
```

### GPIO Pin Mapping

| Function | WROVER (prototype) | C3 SuperMini (final) |
|---|---|---|
| PWM → PT4115 DIM | GPIO18 (10kHz) | GPIO10 |
| NTC ADC | GPIO34 (input-only) | GPIO3 (ADC1_CH3) |
| Status LED | GPIO2 | GPIO8 (onboard blue LED, active LOW) |

### NTC Temperature Sensing
- Voltage divider: NTC 10kΩ MF52 (B=3950) to 3.3V, 5.1kΩ fixed resistor to GND
- 100nF cap on the ADC signal wire to GND for cable noise filtering
- ADC init quirk: dummy `analogRead` must be called before `analogSetPinAttenuation(ADC_11db)` on ESP32 Arduino core or attenuation doesn't apply
- Firmware: 32x ADC oversampling + Steinhart-Hart Beta equation

### Thermal Protection Policy

| Temperature | State | Firmware Action |
|---|---|---|
| < 60°C | NORMAL | Full PWM authority |
| 60–75°C | THROTTLING | Linear brightness reduction |
| 75–85°C | THROTTLING | Minimum brightness floor (PWM=20) |
| > 85°C | CRITICAL | Cut to minimum + immediate ESP-NOW alert to master |

### ESP-NOW Protocol

**Topology:** 1 master node ↔ 10 spot nodes (broadcast or unicast, peer-to-peer 2.4GHz)

```c
// Master → Spot
typedef struct {
    uint8_t spot_id;    // 0xFF = broadcast all
    uint8_t brightness; // 0–255
    uint8_t command;    // 0x01=SET_BRIGHTNESS, 0x02=TURN_ON, 0x03=TURN_OFF, 0x04=REQUEST_STATUS
} esp_now_cmd_t;

// Spot → Master
typedef struct {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state; // 0=normal, 1=throttling, 2=critical
    bool    is_on;
} esp_now_status_t;
```

## Hardware Reference

### Key Components (per spot)
| Component | Value/Model | Purpose |
|---|---|---|
| AC/DC | HLK-PM24 | 220VAC → 24VDC, 1A, isolated |
| Buck | MP1584 module | 24V → 3.3V for ESP32 |
| MCU | ESP32-C3 SuperMini | Control + wireless, WiFi + BLE 5.0, native USB, RISC-V |
| LED driver | PT4115 | Constant-current buck driver, DIM pin PWM control |
| Rsense | 0.33Ω 1W | Sets Iout = 0.1 / 0.33 ≈ 303mA (≈ 0.1/Rsense formula) |
| Flyback diode | 1N4007 | Across COB LED (freewheeling diode for buck) |
| Heatsink | 25×25mm pin-grid 6×6, 13mm | θsa ~19°C/W — gives 51°C margin at 50°C ambient |

### PT4115 Wiring
```
15V → PT4115 VIN
PT4115 SW → inductor → COB+ → COB- → PT4115 CS → Rsense (0.33Ω) → GND
ESP32 GPIO10 → PT4115 DIM   (PWM 10kHz, 0–100% duty = 0–full current)
```

## PCB & Manufacturing

- **Design tool:** EasyEDA (linked to JLCPCB + LCSC)
- **Strategy:** Design 1 unit, validate, order ×10
- **Assembly:** Hybrid — JLCPCB SMT for passives; hand-solder HLK-PM24, ESP32, PT4115

## Validation Workflow

1. Validate NTC divider + RC filter in **Falstad** (falstad.com/circuit)
2. Prototype on **WROVER** before flashing ESP32-C3 SuperMini units
3. Draw schematic + PCB in **EasyEDA** → export Gerber + BOM → JLCPCB
