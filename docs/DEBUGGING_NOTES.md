# LiFi-Auth: Debugging Notes & Known Issues

## Analog Front-End Bring-Up (March 2026)

### Problem: GP27 stuck at 1 (all HIGH) in raw_bit_monitor

**Symptoms:**
- `rx_monitor.py` shows uninterrupted `1111111111...`
- `% HIGH` always 100%
- Changing light level had no effect

**Diagnosis steps taken:**

1. **Confirmed MCP4725 found via I2C** — `MCP4725 OK — threshold ~0.80V`
2. **Confirmed GP27 actively driven** (not floating) via pull-up/pull-down test — both pull-up=200 and pull-dn=200 HIGH → comparator actively outputting HIGH
3. **Confirmed DAC NOT reaching comparator** via DAC toggle test (0V ↔ 3.3V, no GP27 change) — later found this was because the DAC IS wired to TLV IN+ but the OPA signal wasn't reaching TLV IN-
4. **Confirmed OPA swings correctly** — multimeter on OPA output: ~0V dark, ~2V bright
5. **Found the actual bug:** 47Ω from OPA to TLV IN- was tapped at the **OPA inverting input** (2.8V virtual ground, fixed) instead of the **OPA output** (swinging 0-2V). Both nodes are legs of the 470kΩ feedback resistor — easy to confuse.

**Root cause:** The OPA's feedback resistor (470kΩ) has two legs:
- **Output side:** swings 0-2V with light (the correct tap point for 47Ω → TLV IN-)
- **Inverting input side:** fixed at ~2.8V (virtual ground, wrong tap point)

The 5pF feedback cap is in parallel with the 470kΩ, so both legs are visible in the same physical area — the wrong leg was used.

**Fix:** Move the 47Ω resistor connection to the OPA output leg (the one that swings).

---

### raw_bit_monitor startup window issue

The original firmware had `sleep_ms(10000)` (10s) before printing, then `sleep_ms(5000)` before bit flood — 15 seconds of silence. Users would miss all startup diagnostics.

**Fix in current firmware:** Reduced to `sleep_ms(3000)`, repeats DAC status and pull-up/pull-down test every 10 loops so it's always visible.

---

### MCP4725 EEPROM default (1.64V) too high

If the Pico fails to communicate with MCP4725, the threshold stays at the EEPROM value of ~1.64V. OPA peaks at ~2V with bright light. This is just barely enough margin — any slight misalignment or weaker light source will keep OPA below 1.64V and the comparator will never trip.

**Fix:** Firmware now programs DAC to 0.8V on every boot. EEPROM default is a fallback risk, not a working state.

---

## TLV3501 Pinout (TLV3501AIDBVR — 6-pin SOT-23)

| Pin | Function |
|-----|----------|
| 1 | IN- (inverting input) |
| 2 | GND |
| 3 | IN+ (non-inverting input) |
| 4 | VCC |
| 5 | OUT |
| 6 | SHDN (active LOW — tie HIGH to enable) |

**Note:** SHDN is ACTIVE LOW. Tying pin 6 HIGH enables the comparator. Floating SHDN is also fine (internal pull-up to VCC).

---

## MCP4725 DAC Commands (fast write format)

For 12-bit value `V`:
```c
uint8_t cmd[2] = { (V >> 8) & 0x0F, V & 0xFF };
i2c_write_timeout_us(I2C_PORT, addr, cmd, 2, false, 50000);
```

Key voltage values:
| Voltage | DAC value | cmd bytes |
|---------|-----------|-----------|
| 0V | 0x000 | `{ 0x00, 0x00 }` |
| 0.8V | 0x3E1 | `{ 0x03, 0xE1 }` |
| 1.0V | 0x4EC | `{ 0x04, 0xEC }` |
| 1.65V | 0x7FF | `{ 0x07, 0xFF }` |
| 3.3V | 0xFFF | `{ 0x0F, 0xFF }` |

---

## rx_monitor.py Connection Timing

The Pico takes 1-3 seconds after USB connect to enumerate as a serial device. `rx_monitor.py` handles this with an auto-reconnect loop. However:

- `raw_bit_monitor.uf2` has a 3s startup sleep — if you start `rx_monitor.py` after flashing, you'll catch most of the startup output
- The script sets `s.dtr = True` to assert DTR and prompt the Pico's `stdio_usb` to start outputting

---

## PIO Clock Divider for 1 Mbps

```c
// lifi_rx.pio: 8 PIO cycles per bit
float div = clock_get_hz(clk_sys) / (BAUD_RATE * 8.0f);
// At 150 MHz sys clock: div = 150,000,000 / 8,000,000 = 18.75
// At 125 MHz sys clock: div = 125,000,000 / 8,000,000 = 15.625
```

RP2350 (Pico 2) default sys clock: 150 MHz.
RP2040 (original Pico) default sys clock: 125 MHz.

---

## I2C on Pico 2 (RP2350) for MCP4725

```c
#define I2C_PORT  i2c1
#define SDA_PIN   2    // GP2 = physical pin 4
#define SCL_PIN   3    // GP3 = physical pin 5
i2c_init(I2C_PORT, 10 * 1000);  // 10 kHz (slow, reliable for jumper wires)
gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
gpio_pull_up(SDA_PIN);
gpio_pull_up(SCL_PIN);
```

MCP4725A1 address: 0x62 (A1 variant with A0=GND). Code scans 0x60-0x63 to find it.

---

## Flashing Pico 2 from WSL

After building, flash by opening Windows Explorer to the UF2 file and dragging to the Pico mass storage device. From WSL:

```
explorer.exe \\wsl$\Ubuntu\home\josef\projects\lifi-auth\receiver_pico\build\<firmware>.uf2
```

Then hold BOOTSEL on Pico 2 while connecting USB (or double-tap BOOTSEL), and drag the UF2 onto the RPI-RP2 drive.
