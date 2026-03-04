# DIY LED Spot Controller

DIY replacement of 10x 220VAC LED spots with custom COB LED + ESP32 controller units.
Each spot has isolated AC/DC conversion, PWM dimming, NTC thermal monitoring, and ESP-NOW wireless control — no WiFi router required.

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
| Microcontroller | ESP32-C3 SuperMini | Final. WROVER for prototyping |
| LED driver | PT4115 | Constant-current buck driver, DIM pin PWM control |
| Rsense | 0.33Ω 1W | Sets Iout ≈ 303mA (Iout = 0.1 / Rsense) |
| Flyback diode | 1N4007 | Freewheeling diode across COB (buck topology) |
| Thermal sensor | NTC 10kΩ 0402 | On COB PCB, B=3950 |
| NTC divider resistor | 5.1kΩ | Fixed resistor to 3.3V |
| Noise filter cap | 100nF ceramic | On ADC signal wire to GND |
| Interconnect cable | 24 AWG silicone stranded | Between controller and COB module |
| Connector | JST-PH 2.0mm 2-pin | For easy disconnect |

### Why NTC over DS18B20
Space constraint — the COB PCB is too small for a DS18B20 probe.
NTC 0402 SMD fits flush. Cable runs back to ESP32 ADC with noise filtering.

### NTC Wiring

```
3.3V
 │
[NTC 10kΩ]
 │
 ├──► ADC GPIO (GPIO34 on WROVER / GPIO3 on C3)
 │                          │
 │                        [100nF]
 │                          │
[5.1kΩ fixed]              GND
 │
GND
```

- NTC to 3.3V; 5.1kΩ fixed resistor to GND
- 100nF cap on the ADC signal node to GND for cable noise filtering
- 32x ADC oversampling in firmware
- ADC init: dummy `analogRead` required before `analogSetPinAttenuation(ADC_11db)` on ESP32 Arduino core

**Expected ADC value at 25°C room temperature: ~1375** (out of 4095)

### PT4115 Wiring
```
24V rail ──► PT4115 VIN
             PT4115 SW ──► inductor ──► COB+ ──► COB- ──► PT4115 CS ──► 0.33Ω ──► GND
ESP32 GPIO10 ──► PT4115 DIM   (PWM 10kHz)
```

### Full Per-Spot Schematic (simplified)
```
220VAC ──► HLK-PM24 ──► 24V rail ──► PT4115 VIN
                │                    PT4115 SW ──► L ──► COB+ ──► COB- ──► 0.33Ω ──► GND
                └──► Buck (15V→3.3V) ──► ESP32-C3
                                              │
                                         GPIO10 (PWM) ──► PT4115 DIM
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
typedef struct __attribute__((packed)) {
    uint8_t spot_id;     // 0xFF = broadcast all
    uint8_t brightness;  // 0–255
    uint8_t command;     // CMD_* values below
} esp_now_cmd_t;

// Spot → Master
typedef struct __attribute__((packed)) {
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
| 0x01 | CMD_SET_BRIGHTNESS | Set brightness 0–255 |
| 0x02 | CMD_TURN_ON | Turn on (with optional brightness, 300ms fade) |
| 0x03 | CMD_TURN_OFF | Turn off (500ms fade) |
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

## Status LED (GPIO8)

| State | Pattern |
|---|---|
| NORMAL | 1 s on / 2 s off |
| THROTTLING | Solid ON |
| CRITICAL | 100 ms on / 100 ms off (rapid) |
| NTC fault | CRITICAL pattern (floating/disconnected sensor) |

---

## GPIO Pin Mapping

### ESP32 WROVER (Prototype)
| Function | GPIO | Notes |
|---|---|---|
| PWM → MOSFET gate | GPIO18 | 10kHz, 8-bit LEDC |
| NTC ADC | GPIO34 | Input-only ADC1 channel |
| Status LED | GPIO2 | Onboard LED |

### ESP32-C3 SuperMini (Final)
| Function | GPIO | Notes |
|---|---|---|
| PWM → MOSFET gate | GPIO10 | LEDC capable, no ADC, no strapping conflict |
| NTC ADC | GPIO3 | ADC1_CH3 — safe with ESP-NOW/WiFi active |
| Status LED | GPIO8 | Onboard blue LED, active LOW |

> All pin assignments are `#define` in `config.h` — porting is just changing those values.

---

## Firmware File Structure

```
spot_firmware/
├── platformio.ini
└── src/
    ├── main.cpp          # setup(), loop(), command handler, thermal policy, status LED
    ├── config.h          # All pin defines, thresholds, timing, packet structs — edit to port
    ├── thermal.h/.cpp    # NTC read (32x oversampling), Steinhart-Hart, throttle logic
    ├── dimming.h/.cpp    # LEDC init, setBrightness(), fadeTo()
    └── espnow_manager.h/.cpp  # ESP-NOW init, peer registration, send/receive
```

### Key config.h values to update per unit
```c
#define SPOT_ID  0x01   // Unique per spot: 0x01–0x0A
static const uint8_t MASTER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Real master MAC
```

### PlatformIO commands
```bash
cd spot_firmware
pio run                          # compile
pio run --target upload          # compile + flash
pio device monitor               # serial monitor at 115200
```

### Known hardware quirk
On ESP32 Arduino core, `analogSetPinAttenuation()` only takes effect after the ADC channel has been initialised. Always call a dummy `analogRead(pin)` before setting attenuation, otherwise the ADC reads 0 regardless of input voltage.

---

## PCB & Manufacturing

- Design tool: **EasyEDA** (directly linked to JLCPCB + LCSC)
- Manufacturer: **JLCPCB**
- Strategy: Design 1 unit → validate → order qty 10
- Assembly: Hybrid — JLCPCB SMT for passives, hand solder HLK-PM15 + ESP32 + MOSFET

### Cost Estimate (10 units)
| Scenario | Total |
|---|---|
| Full DIY hand soldered | ~$70–80 |
| Hybrid JLCPCB SMT + hand solder | ~$100–110 |
| Full turnkey JLCPCB assembly | ~$150–180 |

---

## Validation Workflow

1. Validate NTC voltage divider + RC filter in **Falstad** (falstad.com/circuit)
2. Prototype firmware on **WROVER** before flashing C3 SuperMini units
3. Draw schematic + PCB layout in **EasyEDA**
4. Export Gerber + BOM → order on **JLCPCB × 10**

---

## Next Steps

- [x] Write spot node firmware
- [x] Write master controller firmware (TTGO LoRa32 + OLED)
- [x] Port spot firmware to ESP32-C3 SuperMini (pin defines + board updated in platformio.ini)
- [ ] Draw schematic in EasyEDA
- [ ] PCB layout → JLCPCB quote
- [ ] Print test enclosures in PLA
- [ ] Validate thermal performance on first assembled unit
