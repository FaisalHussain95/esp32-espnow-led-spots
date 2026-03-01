# DIY LED Spot Controller — Project Context
> Exported conversation summary for Claude Code

---

## Project Overview

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32 controller units.
Each spot gets a custom PCB with isolated AC/DC conversion, PWM dimming, thermal monitoring,
and ESP-NOW wireless control — no WiFi router required.

---

## Hardware Design

### COB LED Specifications
- Supply current: 50–320mA
- Recommended operating points:
  - 3W → 9–12V
  - 5W → 15–18V
  - 7W → 21–25V
  - 10W → 30–34V
- **Target: 5W max @ 15V, ~333mA**

### Power Chain (per spot)
```
220VAC → HLK-PM15 → 15VDC → COB LED (via MOSFET PWM)
                   → MP1584 buck → 3.3VDC → ESP32-C3
```

| Stage | Input | Output | Loss | Efficiency |
|---|---|---|---|---|
| HLK-PM15 (AC/DC) | 220VAC | 15VDC | ~1.5W | ~77% |
| Buck 15V→3.3V (MP1584) | 15V | 3.3V | ~0.3W | ~85% |
| ESP32-C3 active + ESP-NOW TX | 3.3V | — | ~0.3W | — |
| MOSFET switching loss | — | — | ~0.1W | — |
| COB LED at 5W | 15V@333mA | 5W light | ~1W heat | ~83% |
| NTC + voltage divider leakage | — | — | ~0.03W | — |
| **Total per spot from mains** | | | **~7.2W** | |

**All 10 spots: ~72–75W total wall consumption**

### Components (per spot)

| Component | Value/Model | Notes |
|---|---|---|
| AC/DC converter | HLK-PM15 | 220VAC → 15VDC, 1A, isolated |
| Buck converter | MP1584 module | 15V → 3.3V for ESP32 |
| Microcontroller | ESP32-C3 SuperMini | Final. WROVER for prototyping |
| MOSFET | IRLZ44N | Logic-level N-channel |
| Gate resistor | 100Ω | Between ESP32 GPIO and MOSFET gate |
| Flyback diode | 1N4007 | Across COB |
| Thermal sensor | NTC 10kΩ 0402 | On COB PCB, B=3950 |
| Noise filter cap | 100nF ceramic | At NTC sensor end of cable |
| ADC series resistor | 10kΩ | At ESP32 ADC pin |
| Interconnect cable | 24 AWG silicone stranded | Between controller and COB module |
| Connector | JST-PH 2.0mm 2-pin | For easy disconnect |

### Why NTC over DS18B20
Space constraint — the COB PCB is too small for a DS18B20 probe.
NTC 0402 SMD fits flush. Cable runs back to ESP32 ADC with noise filtering.

### NTC Wiring

```
ESP32 END                        COB PCB END

3.3V
 │
[10kΩ fixed]
 │
 ├──► ADC GPIO (GPIO34 on WROVER)
 │
 └────────── cable ─────────┬── [NTC 10kΩ]──┐
                             │               │
                           [100nF]          GND
                             │
                            GND
```

- Pullup (10kΩ fixed) and series resistor (10kΩ) sit at ESP32 end
- 100nF cap sits at sensor end to filter noise before it travels down cable
- Use 32x oversampling in firmware to reduce ADC noise

### MOSFET Wiring
```
15V rail ──► COB+ ──► COB- ──► MOSFET Drain
                                MOSFET Source ──► GND
ESP32 GPIO ──► 100Ω ──► MOSFET Gate
```

### Full Per-Spot Schematic (simplified)
```
220VAC ──► HLK-PM15 ──► 15V rail ──► COB+ ──► COB- ──► MOSFET Drain
                │                                              │
                └──► Buck (15V→3.3V) ──► ESP32-C3            MOSFET Source ──► GND
                                              │
                                         GPIO (PWM) ──► 100Ω ──► MOSFET Gate
```

---

## Heatsink

### Selected Heatsink
- **Pin grid style**: 25×25mm base (2mm thick) + 6×6 array of aluminum rods, 13mm tall
- Omnidirectional convection — air flows between pins in all directions
- Ideal for confined spot housing

### Thermal Calculation Results

| Heatsink | θsa | T_junction @25°C ambient | T_junction @50°C ambient |
|---|---|---|---|
| 20×20×6mm flat | ~102°C/W | ~131°C ✗ | ~156°C ✗ |
| 20×20×10mm flat | ~83°C/W | ~112°C ✓ | ~137°C ✗ |
| 23mm×10mm round | ~87°C/W | ~116°C ✓ | ~141°C ✗ |
| 40mm disk | ~18°C/W | ~47°C ✓ | ~72°C ✓ |
| **25×25 pin grid 6×6** | **~19°C/W** | **~49°C ✓✓** | **~74°C ✓✓** |

- COB max junction temp: 125°C
- Worst case ambient inside housing: 50°C
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
| Controller side (ESP32 + PCB) | PLA | Thermally isolated, fine for ambient |
| Cable routing clips | PLA/PETG | Ambient temps only |

### Printer: Ender 3 V3 SE with ceramic hotend (300°C capable)

**PC print settings:**
- Nozzle: 260–280°C
- Bed: 90–100°C
- Speed: 30–40mm/s
- Cooling fan: Off or 10–15%
- Dry filament 80°C for 4–6h before printing
- Recommended brand: Polymaker PC-Max

---

## ESP-NOW Communication

### Topology
- One **master node** (dedicated ESP32 or Home Assistant bridge)
- 10 **spot nodes** — each listens for commands, replies with status
- No WiFi router required — peer-to-peer radio

### Packet Structures

```c
// Master → Spot
typedef struct {
    uint8_t spot_id;     // 0xFF = broadcast all
    uint8_t brightness;  // 0-255
    uint8_t command;     // CMD_SET_BRIGHTNESS, CMD_TURN_ON, CMD_TURN_OFF, CMD_REQUEST_STATUS
} esp_now_cmd_t;

// Spot → Master
typedef struct {
    uint8_t spot_id;
    uint8_t brightness;
    float   temperature;
    uint8_t thermal_state;  // 0=normal, 1=throttling, 2=critical
    bool    is_on;
} esp_now_status_t;
```

### Commands
| Value | Name | Description |
|---|---|---|
| 0x01 | CMD_SET_BRIGHTNESS | Set brightness 0-255 |
| 0x02 | CMD_TURN_ON | Turn on (with optional brightness) |
| 0x03 | CMD_TURN_OFF | Turn off with fade |
| 0x04 | CMD_REQUEST_STATUS | Force immediate status reply |

---

## Thermal Protection Policy (Firmware)

| Temperature | State | Action |
|---|---|---|
| < 60°C | NORMAL | Full PWM authority |
| 60–75°C | THROTTLING | Linear brightness reduction |
| 75–85°C | THROTTLING | Minimum brightness floor (PWM=20) |
| > 85°C | CRITICAL | Cut to minimum + immediate ESP-NOW alert to master |

---

## GPIO Pin Mapping

### ESP32 WROVER (Prototype)
| Function | GPIO | Notes |
|---|---|---|
| PWM → MOSFET gate | GPIO18 | 10kHz PWM |
| NTC ADC | GPIO34 | Input-only pin, good for ADC |
| Status LED | GPIO2 | Onboard LED |

### ESP32-C3 SuperMini (Final)
| Function | GPIO | Notes |
|---|---|---|
| PWM → MOSFET gate | GPIO3 | |
| NTC ADC | GPIO1 | Check pinout |
| I2C SDA (if needed) | GPIO8 | |
| I2C SCL (if needed) | GPIO9 | |

> All pin assignments are `#define` in `config.h` — porting is just changing those values.

---

## Firmware File Structure

```
spot_firmware/
└── main/
    ├── main.ino              # Main loop, setup, command handler, thermal policy
    ├── config.h              # All pin defines, thresholds, IDs — edit this to port
    ├── thermal.h             # NTC reading, Steinhart-Hart, thermal throttle logic
    ├── dimming.h             # PWM setup, fadeTo, brightness control
    └── espnow_manager.h      # ESP-NOW init, packet structs, send/receive
```

### Key config.h values to update per unit
```c
#define SPOT_ID       0x01   // Unique per spot: 0x01 to 0x0A
uint8_t MASTER_MAC[]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Update with real MAC
```

---

## PCB & Manufacturing

- Design tool: **EasyEDA** (directly linked to JLCPCB + LCSC)
- Manufacturer: **JLCPCB**
- Strategy: Design 1 unit → validate → order qty 10 (manufacturer handles duplication)
- Assembly: Hybrid — JLCPCB SMT for passives, hand solder HLK-PM15 + ESP32 + MOSFET

### Cost Estimate (10 units)
| Scenario | Total |
|---|---|
| Full DIY hand soldered | ~$70–80 |
| Hybrid JLCPCB SMT + hand solder | ~$100–110 |
| Full turnkey JLCPCB assembly | ~$150–180 |

---

## Simulation & Testing Tools

| Tool | Purpose | URL |
|---|---|---|
| Wokwi | ESP32-C3 firmware + ESP-NOW simulation | wokwi.com |
| Falstad | Analog circuit validation (NTC divider, RC filter, MOSFET) | falstad.com/circuit |
| LTSpice | MOSFET switching waveform, PWM ringing check | Desktop app |
| EasyEDA | Schematic + PCB design → JLCPCB export | easyeda.com |

### Recommended workflow
1. Validate NTC voltage divider + RC filter in **Falstad**
2. Test firmware + ESP-NOW between virtual boards in **Wokwi**
3. Draw schematic + PCB layout in **EasyEDA**
4. Export Gerber + BOM → order on **JLCPCB × 10**
5. Prototype firmware on **WROVER** before flashing C3 units

---

## Next Steps (at time of export)

- [ ] Write master controller firmware
- [ ] Draw schematic in EasyEDA
- [ ] Test NTC circuit in Falstad
- [ ] Test ESP-NOW in Wokwi with spot firmware
- [ ] PCB layout → JLCPCB quote
- [ ] Print test enclosures in PLA
- [ ] Validate thermal performance on first assembled unit
