# LiFi-Auth: Hardware & Analog Front-End

## Sender Hardware (RP2040 Pico)

### LED TX Pins

| GPIO | LED Color | PIO Pin Index |
|------|-----------|--------------|
| GP6  | White     | 0 |
| GP7  | Green     | 1 |
| GP8  | Blue      | 2 |
| GP9  | Red       | 3 |

All 4 pins transmit the same data simultaneously via `lifi_multi_tx.pio`. Individual channels can be masked off via `set_led_mask(mask)` where each bit controls one channel (bits: W G B R).

### Other Sender Pins

- **UART0** (GP0/GP1): Debug output
- **UART1** (GP4/GP5): Secondary RX for key reception
- **USB**: stdio (commands, plaintext message input, status)

---

## Receiver Hardware (RP2350 Pico 2)

### Signal Path: Light → GPIO

```
LED (sender)
  ↓ photons
Photodiode (reverse-biased)
  ↓ photocurrent
OPA / TIA (Transimpedance Amplifier)
  → Feedback: 470kΩ || 5pF (resistor and cap in parallel)
  → OPA output swings: ~0V (dark) → ~2V (bright)
  ↓
TLV3501 Comparator
  → IN- (pin 1): OPA output via 47Ω series resistor
  → IN+ (pin 3): MCP4725 DAC output (~0.8V threshold) via 2.2kΩ + 4.7nF RC filter
  → SHDN (pin 6): tied HIGH (enabled — SHDN is active LOW)
  → OUT (pin 5): comparator digital output → 33Ω → GP27
  ↓
GP27 (Pico 2 GPIO input)
  → PIO RX state machine (lifi_rx.pio)
```

### Critical Wiring Note

The 47Ω resistor from OPA to TLV IN- must connect to the **OPA output node** (the swinging leg of the 470kΩ feedback resistor). The other leg of the 470kΩ is the OPA inverting input (virtual ground, fixed at ~2.8V) — connecting the 47Ω there is a common mistake that results in IN- being stuck at 2.8V regardless of light.

### TLV3501 Comparator (6-pin SOT-23, TLV3501AIDBVR)

| Pin | Function | Connection |
|-----|----------|-----------|
| 1   | IN- (inverting) | OPA output via 47Ω |
| 2   | GND | Ground |
| 3   | IN+ (non-inverting) | MCP4725 Vout via 2.2kΩ + 4.7nF |
| 4   | VCC | 3.3V |
| 5   | OUT | 33Ω → GP27 |
| 6   | SHDN | 3.3V (HIGH = enabled) |

**Comparator behavior:**
- Dark (OPA ~0V) → IN- < IN+ (0.8V) → OUT = HIGH (1) = idle
- Light (OPA ~2V) → IN- > IN+ (0.8V) → OUT = LOW (0) = data bit

### MCP4725 DAC (MCP4725A1T-E/CH, SOT-23-6)

Provides the comparator threshold voltage via I2C.

| Pin | Function |
|-----|----------|
| 1   | Vout (DAC output → 2.2kΩ → TLV IN+) |
| 2/3 | GND / SCL |
| 4/5 | SDA / VDD |
| 6   | A0 (address bit) |

- **I2C port:** I2C1 on Pico 2
- **SDA:** GP2 (physical pin 4)
- **SCL:** GP3 (physical pin 5)
- **I2C address:** 0x62 (A1 variant with A0=GND)
- **DAC value for 0.8V threshold:** `{ 0x03, 0xE1 }` (fast write, DAC=0x3E1=993, (993/4095)×3.3V ≈ 0.80V)
- **EEPROM default:** 1.64V (too high for 2V signal swing — always override via I2C on boot)

### RC Filter on TLV IN+

```
MCP4725 Vout → [2.2kΩ] → ┬── TLV IN+ (pin 3)
                           └── [4.7nF] → GND
```

- **Purpose:** Stabilize the DAC threshold, filter switching noise
- **Time constant:** τ = 2.2kΩ × 4.7nF = 10.34 µs
- **Cutoff:** ~15 kHz (well above DAC settling, well below 1 Mbps signal)
- The DAC sets a DC level; the cap settles in <100 µs

### Receiver Pico 2 GPIO Summary

| GPIO | Function |
|------|----------|
| GP2  | I2C1 SDA → MCP4725 |
| GP3  | I2C1 SCL → MCP4725 |
| GP27 | Digital RX input (TLV OUT via 33Ω) |

---

## Signal Level Summary

| Condition | OPA Output | TLV IN- | TLV IN+ | TLV OUT | GP27 |
|-----------|-----------|---------|---------|---------|------|
| Dark (no light) | ~0 mV | ~0 mV | 0.80V | HIGH (3.3V) | 1 |
| Bright constant light | ~2.0V | ~2.0V | 0.80V | LOW (0V) | 0 |
| Idle (no transmission) | ~0 mV | ~0 mV | 0.80V | HIGH | 1 |
| Receiving data | toggling | toggling | 0.80V | toggling | toggling |

---

## Analog Front-End Bring-Up Procedure

1. Flash `raw_bit_monitor.uf2` to Pico 2
2. Run `python3 rx_monitor.py` immediately after flashing (3s window before startup)
3. Firmware will:
   - Scan I2C bus for MCP4725
   - Program DAC to 0.8V threshold
   - Run pull-up/pull-down test on GP27 to detect floating vs. driven
   - Sweep DAC 0→3.3V to find OPA floor voltage
   - Run DAC toggle test (0V ↔ 3.3V) to verify DAC reaches TLV IN+
   - Print `% HIGH` every 10 loops
4. Expected output once wired correctly: ~50% HIGH at idle (ambient), toggles with sender transmitting

### Diagnostic Commands (in `lifi_pico2_rx` firmware)

Send via `rx_monitor.py` or any serial terminal at 115200 baud:
- `raw on` / `raw off` — print every received byte as hex
- `status` — show pin, baud, mode, message count
- `pintest` — sample GP27 for 3s and count transitions
