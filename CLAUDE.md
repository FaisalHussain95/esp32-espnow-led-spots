# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32-C3 controller units. Each spot has an isolated AC/DC converter, PWM dimming via MOSFET, NTC thermal monitoring, and ESP-NOW wireless control (no WiFi router required).

## Firmware Development

### Toolchain
- **IDE:** Arduino IDE or PlatformIO (not yet configured — no build files exist yet)
- **Target MCU:** ESP32-C3 SuperMini (final), ESP32 WROVER (prototype/testing)
- **Simulation:** [Wokwi](https://wokwi.com) for ESP32-C3 firmware + ESP-NOW before flashing real hardware

### Planned Firmware Structure

```
spot_firmware/
└── main/
    ├── main.ino           # Setup, main loop, command handler, thermal policy
    ├── config.h           # All pin defines, thresholds, SPOT_ID — edit to port per unit
    ├── thermal.h          # NTC reading, Steinhart-Hart conversion, throttle logic
    ├── dimming.h          # PWM setup, fadeTo, brightness control
    └── espnow_manager.h   # ESP-NOW init, packet structs, send/receive handlers
```

**Porting to a new unit:** only `config.h` needs changing — update `SPOT_ID` (0x01–0x0A) and `MASTER_MAC[]`.

## Architecture

### Power Chain (per spot)
```
220VAC → HLK-PM15 → 15VDC → COB LED (via IRLZ44N MOSFET, PWM)
                   → MP1584 buck → 3.3VDC → ESP32-C3
```

### GPIO Pin Mapping

| Function | WROVER (prototype) | C3 SuperMini (final) |
|---|---|---|
| PWM → MOSFET gate | GPIO18 (10kHz) | GPIO3 |
| NTC ADC | GPIO34 (input-only) | GPIO1 |
| Status LED | GPIO2 | — |

### NTC Temperature Sensing
- Voltage divider: 10kΩ pullup at ESP32 end, NTC 10kΩ 0402 SMD (B=3950) at COB end
- 10kΩ series resistor at ESP32 ADC pin; 100nF cap at sensor end for cable noise filtering
- Firmware: 32x ADC oversampling + Steinhart-Hart equation

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
| AC/DC | HLK-PM15 | 220VAC → 15VDC, 1A, isolated |
| Buck | MP1584 module | 15V → 3.3V for ESP32 |
| MCU | ESP32-C3 SuperMini | Control + wireless |
| MOSFET | IRLZ44N | Logic-level N-ch, PWM-driven LED |
| Gate resistor | 100Ω | Between GPIO and MOSFET gate |
| Flyback diode | 1N4007 | Across COB LED |
| Heatsink | 25×25mm pin-grid 6×6, 13mm | θsa ~19°C/W — gives 51°C margin at 50°C ambient |

### MOSFET Wiring
```
15V → COB+ → COB- → MOSFET Drain
                     MOSFET Source → GND
ESP32 GPIO → 100Ω → MOSFET Gate
```

## PCB & Manufacturing

- **Design tool:** EasyEDA (linked to JLCPCB + LCSC)
- **Strategy:** Design 1 unit, validate, order ×10
- **Assembly:** Hybrid — JLCPCB SMT for passives; hand-solder HLK-PM15, ESP32, MOSFET

## Validation Workflow

1. Validate NTC divider + RC filter in **Falstad** (falstad.com/circuit)
2. Test firmware + ESP-NOW in **Wokwi** (wokwi.com) before touching hardware
3. Prototype on **WROVER** before flashing ESP32-C3 units
4. Draw schematic + PCB in **EasyEDA** → export Gerber + BOM → JLCPCB
